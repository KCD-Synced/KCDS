; The KCDS client installer, built by Dev/tools/release/installer.py with Inno Setup 6:
;   ISCC.exe /DAppVersion=<version> /DSourceDir=<unpacked client folder> /DOutputDir=<dist> ^
;     client-setup.iss
;
; It installs for the signed-in player only (no administrator rights) into
; %LOCALAPPDATA%\Programs\KCDS, where the launcher can update itself and the client in place, and
; adds KCDS to the Start menu (the launcher makes its own desktop shortcut on its first start).
; Installing a newer version over an older one keeps the player's kcds-launcher.json. The
; uninstaller removes the install folder; the player's data in %LOCALAPPDATA%\KCDS (logs, game path,
; crash reports) stays.

#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef SourceDir
  #error SourceDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif

[Setup]
; The same id for every version: a newer installer updates the same install.
AppId={{911DE0A9-3456-48FB-A6B0-AB5CBE4DCED1}
AppName=KCDS
AppVersion={#AppVersion}
AppVerName=KCDS {#AppVersion}
AppPublisher=KCDS
AppPublisherURL=https://kcdsynced.com
DefaultDirName={localappdata}\Programs\KCDS
DefaultGroupName=KCDS
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=KCDS-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\kcds-launcher.exe
UninstallDisplayName=KCDS
; A running launcher or game client is asked to close first.
CloseApplications=yes

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "kcds-launcher.json"; \
  Flags: recursesubdirs ignoreversion
; The player's settings: written once, kept by every later install.
Source: "{#SourceDir}\kcds-launcher.json"; DestDir: "{app}"; Flags: onlyifdoesntexist

[Icons]
Name: "{group}\KCDS"; Filename: "{app}\kcds-launcher.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall KCDS"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\kcds-launcher.exe"; Description: "Start KCDS"; WorkingDir: "{app}"; \
  Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Files the launcher's own updates brought in are the install's too.
Type: filesandordirs; Name: "{app}"
