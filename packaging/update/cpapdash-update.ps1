# SDD-041 D8/D9: install a verified new CpapDash over the current one, and put
# the old one back if the new one does not come up.
#
# Started by the supervisor (desktop/qt TrayShell::onUpdateRequested) from a
# COPY in %TEMP%, because the install folder is what this replaces. By then the
# service has downloaded CpapDashDesktop-Setup.exe and checked it against the
# release manifest, and the supervisor has hashed it again. This script checks
# it a third time, because it is the one that acts.
#
# The installer is per-user (PrivilegesRequired=lowest), so /CURRENTUSER runs it
# without a UAC prompt, and /SUPPRESSMSGBOXES covers the preflight message box.
# Integrity only: the installer is not Authenticode-signed yet (SDD-041 D3).
#
# Every outcome is written to <DataDir>\update\result.json, which the service
# reads on its next start and Settings shows.
param(
    [Parameter(Mandatory = $true)][string]$Pending,
    [Parameter(Mandatory = $true)][string]$InstallDir,
    [Parameter(Mandatory = $true)][string]$DataDir,
    [Parameter(Mandatory = $true)][int]$SupervisorPid,
    [int]$Port = 8893
)

$ErrorActionPreference = 'Continue'
$UpdateDir = Join-Path $DataDir 'update'
New-Item -ItemType Directory -Force -Path $UpdateDir | Out-Null
$ResultPath = Join-Path $UpdateDir 'result.json'
$LogPath = Join-Path $UpdateDir 'update.log'
$Prev = "$InstallDir.previous"
$Version = ''

function Log([string]$msg) {
    Add-Content -Path $LogPath -Value ("{0} {1}" -f (Get-Date).ToUniversalTime().ToString('s'), $msg)
}

function Write-Result([bool]$ok, [string]$step, [string]$message) {
    [ordered]@{
        ok      = $ok
        version = $Version
        step    = $step
        message = $message
        at      = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    } | ConvertTo-Json -Compress | Set-Content -Path $ResultPath -Encoding UTF8
    Log "result: ok=$ok step=$step $message"
}

function Start-Tray {
    Start-Process -FilePath (Join-Path $InstallDir 'CpapDashDesktop.exe') | Out-Null
}

function Stop-Ours {
    # Only processes running out of THIS install; a second copy elsewhere is not ours.
    Get-Process CpapDashDesktop, hms_cpap -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) } |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

# Nothing has been touched yet: say why and leave the install as it is.
function Refuse([string]$step, [string]$message) {
    Write-Result $false $step $message
    Remove-Item -Force $Pending -ErrorAction SilentlyContinue
    Start-Tray
    exit 1
}

# The new version is in place and failed: put the old one back and start it.
function Roll-Back([string]$step, [string]$message) {
    Log "rolling back: $message"
    Stop-Ours
    Start-Sleep -Seconds 2
    if (Test-Path $Prev) {
        # /MIR makes the folder exactly the saved copy, removing files the new
        # version added. Exit codes below 8 are success for robocopy.
        robocopy $Prev $InstallDir /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Log "robocopy restore returned $LASTEXITCODE" }
        # Restored: the copy has done its job. Kept only when the restore
        # failed, because then it is the one good copy left.
        else { Remove-Item -Recurse -Force $Prev -ErrorAction SilentlyContinue }
    }
    Write-Result $false $step $message
    Remove-Item -Force $Pending -ErrorAction SilentlyContinue
    Start-Tray
    exit 1
}

Log "=== cpapdash-update.ps1"
if (-not (Test-Path $Pending)) { Write-Result $false 'verify' "no pending.json at $Pending"; exit 1 }
$p = Get-Content -Raw -Path $Pending | ConvertFrom-Json
$Version = [string]$p.version
$File = [string]$p.file

# 1. Wait for the supervisor, and the service it ran, to be gone.
try { Wait-Process -Id $SupervisorPid -Timeout 60 -ErrorAction Stop } catch { }
if (Get-Process -Id $SupervisorPid -ErrorAction SilentlyContinue) { Refuse 'verify' 'the supervisor did not exit' }
for ($i = 0; $i -lt 60; $i++) {
    $svc = Get-Process hms_cpap -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) }
    if (-not $svc) { break }
    Start-Sleep -Milliseconds 500
}
Stop-Ours

# 2. Verify, again.
if (-not (Test-Path $File)) { Refuse 'verify' 'the download is missing' }
$hash = (Get-FileHash -Algorithm SHA256 -Path $File).Hash.ToLowerInvariant()
if ($hash -ne ([string]$p.sha256).ToLowerInvariant()) { Refuse 'verify' 'the download does not match its checksum' }

# 3. Install: keep a copy of the current install, then run the installer over it.
if (Test-Path $Prev) { Remove-Item -Recurse -Force $Prev }
robocopy $InstallDir $Prev /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Refuse 'install' "the current install could not be copied aside (robocopy $LASTEXITCODE)" }

$setup = Start-Process -FilePath $File -Wait -PassThru -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCANCEL', '/CURRENTUSER',
    "/DIR=`"$InstallDir`"", "/LOG=`"$(Join-Path $UpdateDir 'setup.log')`"")
if ($setup.ExitCode -ne 0) { Roll-Back 'install' "the installer exited with $($setup.ExitCode)" }

# 4. Preflight the new service against this machine's configuration.
$env:HMS_CPAP_DATA_DIR = $DataDir
& (Join-Path $InstallDir 'hms_cpap.exe') --preflight *>> $LogPath
if ($LASTEXITCODE -ne 0) { Roll-Back 'preflight' "the new version's configuration check failed" }

# 5. Start it the way a user would.
try { Start-Tray } catch { Roll-Back 'start' 'the new application would not start' }

# 6. It has to answer, and answer as the NEW version.
for ($i = 0; $i -lt 120; $i++) {
    try {
        $h = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2
        if ([string]$h.version -eq $Version) {
            Write-Result $true 'done' "updated to $Version"
            Remove-Item -Recurse -Force $Prev -ErrorAction SilentlyContinue
            Remove-Item -Force $File, $Pending -ErrorAction SilentlyContinue
            Remove-Item -Force $PSCommandPath -ErrorAction SilentlyContinue
            exit 0
        }
    } catch { }
    Start-Sleep -Seconds 1
}
Roll-Back 'health' "the new version did not answer as $Version within two minutes"
