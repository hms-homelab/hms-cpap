; CpapDash Desktop installer (SDD-005 phase 3).
;
; PER-USER by decision, which is why PrivilegesRequired is lowest and the
; install goes to %LOCALAPPDATA%\Programs rather than Program Files: a CPAP
; patient should not have to clear a UAC prompt to look at their own sleep data,
; and nothing here needs machine-wide rights.
;
; Inno Setup rather than WiX: it produces a clean Add/Remove Programs entry
; without pulling in the WiX toolchain, and the whole thing is one readable file.

#define AppName "CpapDash Desktop"
#define AppPublisher "HMS Homelab"
#define AppExe "CpapDashDesktop.exe"
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{8C4F1E2A-7B3D-4A5E-9C1F-6D2E8A0B4F71}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppSupportURL=https://github.com/hms-homelab/hms-cpap
DefaultDirName={localappdata}\Programs\HMS-CPAP
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; No elevation: everything this installs lives under the user's own profile.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputBaseFilename=CpapDashDesktop-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; x64 only, matching the hms_cpap.exe the CI job builds.
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startatlogin"; Description: "Start {#AppName} when I log in"; GroupDescription: "Startup"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts"; Flags: unchecked

[Files]
; The tray shell and the service binary sit side by side, which is exactly the
; layout the release zip already uses, so the shell finds hms_cpap.exe next to
; itself with no configuration.
Source: "..\..\dist-windows\CpapDashDesktop.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\dist-windows\hms_cpap.exe";        DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\dist-windows\*.dll";               DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\dist-windows\config.example.json";  DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\dist-windows\static\*";            DestDir: "{app}\static"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Autostart. The value name matches Autostart.cs so the tray checkbox and the
; installer are talking about the same entry rather than fighting over two.
; uninsdeletevalue is what guarantees an uninstall does not leave a Run entry
; pointing at a deleted executable.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "CpapDashDesktop"; ValueData: """{app}\{#AppExe}"""; \
    Flags: uninsdeletevalue; Tasks: startatlogin

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName} now"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop the tray before removing its files, or the uninstaller leaves a running
; process holding port 8893 and a tray icon pointing at nothing.
Filename: "{cmd}"; Parameters: "/C taskkill /IM {#AppExe} /F /T"; Flags: runhidden; RunOnceId: "StopTray"
Filename: "{cmd}"; Parameters: "/C taskkill /IM hms_cpap.exe /F /T"; Flags: runhidden; RunOnceId: "StopService"

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
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Report: AnsiString;
  ReportPath: String;
begin
  if CurStep <> ssPostInstall then Exit;

  ReportPath := ExpandConstant('{tmp}\preflight.txt');

  // cmd /C so stdout can be redirected to a file and read back; Exec on its own
  // yields an exit code and nothing to show anyone.
  if not Exec(ExpandConstant('{cmd}'),
              '/C ""' + ExpandConstant('{app}\hms_cpap.exe') + '" --preflight > "'
                 + ReportPath + '" 2>&1"',
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Exit;   // could not even run the check; the tray will report it on launch

  if ResultCode = 0 then Exit;

  if LoadStringFromFile(ReportPath, Report) then
    MsgBox('CpapDash is installed, but its configuration needs attention:'#13#10#13#10
           + String(Report) + #13#10
           + 'Fix the item marked FAIL, then start CpapDash Desktop.',
           mbInformation, MB_OK)
  else
    MsgBox('CpapDash is installed, but the configuration check did not pass.'#13#10
           + 'Start CpapDash Desktop for details.', mbInformation, MB_OK);
end;
