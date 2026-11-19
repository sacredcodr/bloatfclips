; Inno Setup script for bloatfclips. Build with tools\build_installer.ps1, which compiles the
; Release exe first and passes its path here. Installs per user (no admin prompt) into
; %LOCALAPPDATA%\Programs\bloatfclips, where the app can also write clipper.ini.

#ifndef SourceExe
  #define SourceExe "..\build\bloatfclips.exe"
#endif
#define AppName "bloatfclips"
#define AppExeName "bloatfclips.exe"
#define AppVersion GetStringFileInfo(SourceExe, "ProductVersion")

[Setup]
; Never change AppId: Windows uses it to recognise upgrades and the uninstall entry.
AppId={{BC0CE4AF-5E1D-4A3A-9E2C-AAD5879EC9EA}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=sacredcodr
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
SetupIconFile=..\assets\bloatfclips.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
OutputDir=..\dist
OutputBaseFilename=bloatfclips-setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[CustomMessages]
en.AutoStart=Start {#AppName} automatically when I sign in to Windows (in the tray)
de.AutoStart={#AppName} automatisch beim Anmelden bei Windows starten (im Infobereich)
en.LaunchNow=Start {#AppName} now
de.LaunchNow={#AppName} jetzt starten

[Tasks]
Name: "autostart"; Description: "{cm:AutoStart}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; DestName: "{#AppExeName}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExeName}"" --tray"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchNow}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Settings written by the app. Saved clips live in the user's Videos folder and are kept.
Type: files; Name: "{app}\clipper.ini"

[Code]
const
  WM_CLOSE = $0010;

{ The app keeps running in the tray after its window closes. Ask it to exit through its hidden
  tray window (same as tray > Exit) so files can be replaced or removed, then wait up to 5 s. }
procedure StopRunningApp();
var
  Wnd: HWND;
  I: Integer;
begin
  Wnd := FindWindowByClassName('ClipperTrayHiddenWindow');
  if Wnd = 0 then Exit;
  PostMessage(Wnd, WM_CLOSE, 0, 0);
  for I := 1 to 50 do
  begin
    Sleep(100);
    if FindWindowByClassName('ClipperTrayHiddenWindow') = 0 then Exit;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningApp();
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  StopRunningApp();
  Result := True;
end;
