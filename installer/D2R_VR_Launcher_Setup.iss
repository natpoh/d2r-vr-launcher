; D2R VR Launcher - the installer of the launcher itself (Inno Setup 6). Built by build_launcher_installer.ps1.
;
; Per user, no administrator rights, no UAC prompt: everything goes under %LOCALAPPDATA%\Programs\D2R VR Launcher, two shortcuts,
; and one registry value (HKCU\Software\D2R VR Launcher\InstallDir) that the mods' installers can read to offer "Open the launcher".
; It touches no game folder and no mod: the mods' own installers put themselves into <game>\mods\<Name> and the launcher finds them there.
; What the launcher remembers (%LOCALAPPDATA%\VRModLauncher: settings.json, logs) stays when it is uninstalled - it is the player's, and tiny.

#ifndef LauncherVersion
  #define LauncherVersion "0.3.1"
#endif
#ifndef BuildDir
  #error BuildDir (the folder with D2RVRLauncher.exe and vrmodlauncher.exe) must be given: /DBuildDir=...
#endif

[Setup]
AppId={{B1E3C7A4-5D2F-4E8B-9A60-3C71F2D84E15}
AppName=D2R VR Launcher
AppVersion={#LauncherVersion}
AppPublisher=natpoh
AppPublisherURL=https://github.com/natpoh/d2r-vr-launcher
AppSupportURL=https://github.com/natpoh/d2r-vr-launcher/issues
DefaultDirName={localappdata}\Programs\D2R VR Launcher
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=..\assets\launcher_icon.ico
UninstallDisplayIcon={app}\D2RVRLauncher.exe
UninstallDisplayName=D2R VR Launcher
LicenseFile=LICENSE.txt
OutputDir=Output
OutputBaseFilename=D2R_VR_Launcher_Setup_{#LauncherVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#LauncherVersion}
VersionInfoProductName=D2R VR Launcher

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#BuildDir}\D2RVRLauncher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\vrmodlauncher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE.txt"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\D2R VR Launcher"; Filename: "{app}\D2RVRLauncher.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\D2R VR Launcher"; Filename: "{app}\D2RVRLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\D2R VR Launcher"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\D2R VR Launcher"; ValueType: string; ValueName: "Version"; ValueData: "{#LauncherVersion}"

[Run]
; The finish page offers two ways to start: the install view (the list of D2RLoader and the two VR mods: what is installed, what is newer, install / update) - ticked,
; it is what a player who has just installed the launcher wants - or the plain window. Both start the same program: tick only one of them. A silent run (the mods'
; installers carry this Setup and run it silently) starts nothing.
Filename: "{app}\D2RVRLauncher.exe"; Parameters: "--install-view"; Description: "Choose the mods to install"; Flags: postinstall nowait skipifsilent
Filename: "{app}\D2RVRLauncher.exe"; Description: "Open D2R VR Launcher"; Flags: postinstall nowait skipifsilent unchecked

[Code]
// Another installer carries this Setup (D2R VR Authentic's, D2R VR Unleashed's: docs/INSTALLER_CONTRACT.md) and runs it silently. A silent run never downgrades: when a newer
// launcher is installed it stops without touching anything (exit code 1, which those installers ignore). A run with pages asks nothing and replaces the launcher (a reinstall).
function VersionNewer(A, B: String): Boolean;   // A > B, dotted numbers
var
  I, J, X, Y: Integer;
  PA, PB: String;
begin
  Result := False;
  PA := A + '.';
  PB := B + '.';
  for I := 1 to 4 do
  begin
    J := Pos('.', PA);
    X := StrToIntDef(Copy(PA, 1, J - 1), 0);
    Delete(PA, 1, J);
    J := Pos('.', PB);
    Y := StrToIntDef(Copy(PB, 1, J - 1), 0);
    Delete(PB, 1, J);
    if X > Y then begin Result := True; Exit; end;
    if X < Y then Exit;
  end;
end;

function InitializeSetup: Boolean;
var
  Installed: String;
begin
  Result := True;
  if WizardSilent and RegQueryStringValue(HKCU, 'Software\D2R VR Launcher', 'Version', Installed) then
    if VersionNewer(Installed, '{#LauncherVersion}') then
    begin
      Log('A newer launcher (' + Installed + ') is installed: the silent run of {#LauncherVersion} stops without changing anything.');
      Result := False;
    end;
end;
