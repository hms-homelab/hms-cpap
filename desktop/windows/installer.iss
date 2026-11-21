; HmsCpap Installer (SDD-005 phase 3; called CpapDash Desktop until 5.6.1).
;
; PER-USER by decision, which is why PrivilegesRequired is lowest and the
; install goes to %LOCALAPPDATA%\Programs rather than Program Files: a CPAP
; patient should not have to clear a UAC prompt to look at their own sleep data,
; and nothing here needs machine-wide rights.
;
; Inno Setup rather than WiX: it produces a clean Add/Remove Programs entry
; without pulling in the WiX toolchain, and the whole thing is one readable file.

#define AppName "HmsCpap Installer"
#define AppPublisher "HMS Homelab"
#define AppExe "HmsCpapInstaller.exe"
; Before 5.6.1 the tray was "CpapDash Desktop" / CpapDashDesktop.exe. That name
; now belongs to a different app. Everything below that names the old one is
; there so a machine installed before upgrades cleanly; see [InstallDelete],
; the second [Files] line for the exe, and [Code].
#define OldAppName "CpapDash Desktop"
#define OldAppExe "CpapDashDesktop.exe"
; SDD-073 D7: the inbound rule for the Sleep Bridge stream, by this name.
#define FirewallRule "HmsCpap Sleep Bridge (TCP 8894)"
; #91: the version is the repo's VERSION file, the one CMake stamps into both
; exes. CI passes it as /DAppVersion; a build without it reads the file
; itself, so no build of this script can carry a version of its own.
#ifndef AppVersion
  #define VersionFile FileOpen(AddBackslash(SourcePath) + "..\..\VERSION")
  #define AppVersion Trim(FileRead(VersionFile))
  #expr FileClose(VersionFile)
#endif

[Setup]
AppId={{8C4F1E2A-7B3D-4A5E-9C1F-6D2E8A0B4F71}
AppName={#AppName}
AppVersion={#AppVersion}
; The title bar and Add/Remove Programs ("HmsCpap Installer version x.y.z")
; and Setup's own file version, all from AppVersion (#91).
AppVerName={#AppName} version {#AppVersion}
VersionInfoVersion={#AppVersion}
VersionInfoProductVersion={#AppVersion}
VersionInfoProductName=HmsCpap
VersionInfoDescription={#AppName} Setup
VersionInfoCompany={#AppPublisher}
AppPublisher={#AppPublisher}
AppSupportURL=https://github.com/hms-homelab/hms-cpap
DefaultDirName={localappdata}\Programs\HMS-CPAP
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Same AppId as CpapDash Desktop, so Inno would reuse the Start menu folder it
; recorded then ("CpapDash Desktop") and put the new shortcuts back into it.
UsePreviousGroup=no
; No elevation: everything this installs lives under the user's own profile.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputBaseFilename=HmsCpapInstaller-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; x64 only, matching the hms_cpap.exe the CI job builds.
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
; The installer had no icon of any kind, so Setup and the Add/Remove Programs
; entry both showed a generic box. Derived from the same logo.png as the tray
; and the app icon (scripts/make-icons.sh).
SetupIconFile=..\qt\resources\cpapdash.ico

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startatlogin"; Description: "Start {#AppName} when I log in"; GroupDescription: "Startup"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts"; Flags: unchecked

[Files]
; The tray shell and the service binary sit side by side, which is exactly the
; layout the release zip already uses, so the shell finds hms_cpap.exe next to
; itself with no configuration.
Source: "..\..\dist-windows\HmsCpapInstaller.exe"; DestDir: "{app}"; Flags: ignoreversion
; The upgrade from a CpapDash Desktop install: the old self-updater, still the
; one running, starts CpapDashDesktop.exe after installing and rolls the whole
; update back if it cannot. So where the old exe was, the new tray goes in under
; that name too; the tray deletes that copy once it runs under its own name.
Source: "..\..\dist-windows\HmsCpapInstaller.exe"; DestDir: "{app}"; DestName: "{#OldAppExe}"; Flags: ignoreversion; Check: OldTrayInstalled
Source: "..\..\dist-windows\hms_cpap.exe";        DestDir: "{app}"; Flags: ignoreversion
; hms-cpap's licence and the Sleep Bridge component's, which hms_cpap.exe links.
Source: "..\..\dist-windows\LICENSE";             DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\dist-windows\LICENSE-sleepbridge"; DestDir: "{app}"; Flags: ignoreversion
; SDD-041 D9: the helper the tray copies to %TEMP% and runs to install an update.
Source: "..\..\packaging\update\cpapdash-update.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\dist-windows\*.dll";               DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\dist-windows\config.example.json";  DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\dist-windows\static\*";            DestDir: "{app}\static"; Flags: ignoreversion recursesubdirs createallsubdirs

; ---------------------------------------------------------------------------
; Qt runtime, shipped WITH the application.
;
; The requirement is that a user installs nothing of their own: no Qt runtime,
; no redistributable, no separate download. windeployqt puts the DLLs beside the
; exe and the plugins into subdirectories, and every one of those directories
; has to be listed here -- the *.dll rule above does NOT recurse.
;
; platforms\qwindows.dll matters most: without it Qt aborts at startup with
; "no Qt platform plugin could be initialized", which is silent at build time
; and fatal on the user's machine. CI asserts windeployqt produced it, and
; windows-desktop-test asserts it survived into the install.
Source: "..\..\dist-windows\platforms\*";          DestDir: "{app}\platforms";          Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\..\dist-windows\styles\*";             DestDir: "{app}\styles";             Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\..\dist-windows\imageformats\*";       DestDir: "{app}\imageformats";       Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\..\dist-windows\iconengines\*";        DestDir: "{app}\iconengines";        Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\..\dist-windows\tls\*";                DestDir: "{app}\tls";                Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\..\dist-windows\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\..\dist-windows\generic\*";            DestDir: "{app}\generic";            Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[InstallDelete]
; The old name's shortcuts: a Start menu folder and a desktop link still saying
; CpapDash Desktop would sit next to the new ones, the confusion the rename is for.
Type: filesandordirs; Name: "{autoprograms}\{#OldAppName}"
Type: files; Name: "{autodesktop}\{#OldAppName}.lnk"

[UninstallDelete]
Type: files; Name: "{app}\{#OldAppExe}"
; #92: what hms_cpap.exe writes into its own folder at run time. The log (and
; its rotations) goes beside the exe when that folder is writable, which a
; per-user install's is. Drogon's upload scratch (tmp\00..FF) is "uploads",
; under the working folder or the document root (static), whichever this
; Drogon resolves it to. None of it is the user's data: that lives in
; %USERPROFILE%\.hms-cpap, which nothing here touches (see the NOTE below).
Type: files;          Name: "{app}\hms-cpap.log*"
Type: files;          Name: "{app}\.hms-cpap-write-test"
Type: filesandordirs; Name: "{app}\uploads"
Type: filesandordirs; Name: "{app}\static\uploads"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Autostart. The value name matches Autostart.cpp so the tray checkbox and the
; installer are talking about the same entry rather than fighting over two. It
; keeps its old name through the rename: nobody sees it, and [Code] repoints an
; existing one at the new exe, so an upgrade leaves one entry, not a dead one.
; uninsdeletevalue is what guarantees an uninstall does not leave a Run entry
; pointing at a deleted executable.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "CpapDashDesktop"; ValueData: """{app}\{#AppExe}"""; \
    Flags: uninsdeletevalue; Tasks: startatlogin
; The same value made by the tray's own "Start at login" (or repointed by an
; upgrade) is not this installer's task, so the line above would leave it,
; pointing at a deleted exe. ValueType none writes nothing; it only deletes on
; uninstall.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: none; ValueName: "CpapDashDesktop"; Flags: uninsdeletevalue

[Run]
; SDD-073 D7: a Sleep Bridge streams to hms_cpap.exe on TCP 8894, and without
; a rule the first listen either shows Windows' prompt or is blocked silently,
; so the bridge's setup test fails with no reason given. One inbound rule for
; the installed exe, that port, private networks only. The web port gets none.
; This installer runs per user (no elevation, above), and a firewall rule
; needs it, so this one command asks for it (the UAC prompt) and only when the
; rule is not there yet: an update, silent or not, finds it and asks nothing.
Filename: "{sys}\netsh.exe"; \
    Parameters: "advfirewall firewall add rule name=""{#FirewallRule}"" dir=in action=allow program=""{app}\hms_cpap.exe"" protocol=TCP localport=8894 profile=private"; \
    Verb: runas; Flags: shellexec runhidden waituntilterminated; \
    StatusMsg: "Allowing the Sleep Bridge through Windows Firewall (private networks)..."; \
    Check: not FirewallRulePresent
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName} now"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop the tray before removing its files, or the uninstaller leaves a running
; process holding port 8893 and a tray icon pointing at nothing.
Filename: "{cmd}"; Parameters: "/C taskkill /IM {#AppExe} /F /T"; Flags: runhidden; RunOnceId: "StopTray"
Filename: "{cmd}"; Parameters: "/C taskkill /IM {#OldAppExe} /F /T"; Flags: runhidden; RunOnceId: "StopOldTray"
Filename: "{cmd}"; Parameters: "/C taskkill /IM hms_cpap.exe /F /T"; Flags: runhidden; RunOnceId: "StopService"
; SDD-073 D7: the rule goes with the program. Not an entry here: an
; [UninstallRun] Check is evaluated while INSTALLING, when the uninstall log is
; written, and on a first install the rule is not there yet ([Run] adds it
; after), so the delete was never recorded and the rule outlived the program
; (#92). CurUninstallStepChanged in [Code] asks at uninstall time instead.

; NOTE, deliberately: nothing here touches %USERPROFILE%\.hms-cpap. That folder
; holds the user's config and their entire therapy database. An uninstaller that
; removes it turns "I am reinstalling" into permanent data loss, and there is no
; way to undo that for someone whose CPAP history is their medical record.

[Code]
// Validate the configuration as the last step of installing, while the user is
// still looking at the installer. Catching a busy port or an unreachable
// database here is far better than letting them find out later from a tray icon
// that never turns green.
//
// CurStepChanged(ssPostInstall) rather than a [Run] entry with a Check function:
// [Run] cannot capture output, and doing the work inside a Check to get at it
// would be a side effect hidden in a predicate.
//
// [Code] is LAST on purpose: Inno treats everything after this header as Pascal,
// so any section placed below would never be parsed.
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'CpapDashDesktop';

var
  // Taken at ssInstall, before any file is copied, so the copy made under the
  // old name cannot make it look as though the old tray had been there.
  OldTrayWasInstalled: Boolean;

function OldTrayInstalled(): Boolean;
begin
  Result := OldTrayWasInstalled;
end;

// SDD-073 D7: whether the Sleep Bridge rule is there. Reading the firewall
// needs no elevation; netsh answers 0 when a rule by that name exists.
function FirewallRulePresent(): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\netsh.exe'),
                 'advfirewall firewall show rule name="{#FirewallRule}"',
                 '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// #92: the uninstall's side of SDD-073 D7. A per-user uninstaller has no
// elevation, and removing a firewall rule needs it, so this one command asks
// (one UAC prompt, the same netsh the install asked for), and only when the
// rule is there. Declined, the uninstall still completes.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep <> usUninstall then Exit;
  if not FirewallRulePresent() then Exit;
  ShellExec('runas', ExpandConstant('{sys}\netsh.exe'),
            'advfirewall firewall delete rule name="{#FirewallRule}"',
            '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Report: AnsiString;
  ReportPath: String;
  Existing: String;
begin
  if CurStep = ssInstall then begin
    OldTrayWasInstalled := FileExists(ExpandConstant('{app}\{#OldAppExe}'));
    Exit;
  end;
  if CurStep <> ssPostInstall then Exit;

  // An entry made by the old tray's own "Start at login" (not this installer's
  // task) still points at CpapDashDesktop.exe, which the new tray deletes:
  // repoint whatever is there at the new exe.
  if RegQueryStringValue(HKCU, RunKey, RunValue, Existing) then
    RegWriteStringValue(HKCU, RunKey, RunValue, '"' + ExpandConstant('{app}\{#AppExe}') + '"');

  ReportPath := ExpandConstant('{tmp}\preflight.txt');

  // cmd /C so stdout can be redirected to a file and read back; Exec on its own
  // yields an exit code and nothing to show anyone.
  if not Exec(ExpandConstant('{cmd}'),
              '/C ""' + ExpandConstant('{app}\hms_cpap.exe') + '" --preflight > "'
                 + ReportPath + '" 2>&1"',
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Exit;   // could not even run the check; the tray will report it on launch

  if ResultCode = 0 then Exit;

  // SuppressibleMsgBox, not MsgBox. Plain MsgBox ignores /SUPPRESSMSGBOXES and
  // shows the dialog anyway, so on a silent install this line waits for a click
  // that is never coming: Setup and Setup.tmp stay alive, producing no output,
  // until something kills them. That is a hang with no message, triggered by
  // the ordinary case of preflight reporting a problem.
  if LoadStringFromFile(ReportPath, Report) then
    SuppressibleMsgBox('HmsCpap is installed, but its configuration needs attention:'#13#10#13#10
           + String(Report) + #13#10
           + 'Fix the item marked FAIL, then start HmsCpap Installer.',
           mbInformation, MB_OK, IDOK)
  else
    SuppressibleMsgBox('HmsCpap is installed, but the configuration check did not pass.'#13#10
           + 'Start HmsCpap Installer for details.', mbInformation, MB_OK, IDOK);
end;
