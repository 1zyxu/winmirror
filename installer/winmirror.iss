#ifndef AppVersion
  #define AppVersion "2.5.4"
#endif

[Setup]
AppId={{8F4A2C10-7B3E-4E91-9A02-574D49520001}
AppName=WinMirror AirPlay
AppVersion={#AppVersion}
AppVerName=WinMirror AirPlay v{#AppVersion}
AppPublisher=karnajitop
AppPublisherURL=https://github.com/1zyxu/winmirror
AppSupportURL=https://github.com/1zyxu/winmirror/releases
DefaultDirName={autopf}\WinMirror
DefaultGroupName=WinMirror AirPlay
OutputDir=..
OutputBaseFilename=WinMirror-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
CloseApplications=yes
CloseApplicationsFilter=winmirror.exe,winmirror_rt.dll
RestartApplications=no
SetupIconFile=..\src\app_icon.ico
UninstallDisplayIcon={app}\winmirror.exe
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany=karnajitop
VersionInfoProductName=WinMirror AirPlay
VersionInfoDescription=WinMirror AirPlay Setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
Type: files; Name: "{app}\*.old.exe"
Type: files; Name: "{app}\*.update.exe"
Type: files; Name: "{app}\*.exe.tmp"
Type: files; Name: "{app}\*.exe.bak"
Type: files; Name: "{app}\*.dll.tmp"
Type: files; Name: "{app}\*.dll.update"
Type: files; Name: "{app}\*.pdb"
Type: files; Name: "{app}\winmirror_update.exe"
Type: files; Name: "{app}\vdd_driver.zip"

[Files]
Source: "..\winmirror.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\winmirror_rt.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\WinMirror AirPlay"; Filename: "{app}\winmirror.exe"
Name: "{group}\Uninstall WinMirror AirPlay"; Filename: "{uninstallexe}"
Name: "{autodesktop}\WinMirror AirPlay"; Filename: "{app}\winmirror.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\winmirror.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated
Filename: "{app}\winmirror.exe"; Description: "{cm:LaunchProgram,WinMirror AirPlay}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\*.old.exe"
Type: files; Name: "{app}\*.update.exe"
Type: files; Name: "{app}\*.tmp"
Type: files; Name: "{app}\*.update"
