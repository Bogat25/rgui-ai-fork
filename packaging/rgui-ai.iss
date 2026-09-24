; Inno Setup script for RGui with the local AI assistant.
;
; Built by 'rgui installer' (see rgui.ps1), which stages the files and
; passes the version:
;
;   ISCC.exe /DAppVersion=0.1.1 /DNumericVersion=0.1.1.0 /DStageDir=... /DOutputDir=... rgui-ai.iss
;
; Needs Inno Setup 7.
;
; The model is not in the installer (2.7 GB, and GitHub release assets
; are limited to 2 GB): RGui offers to download it the first time the
; assistant is opened.

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef NumericVersion
  #define NumericVersion "0.0.0.0"
#endif
#ifndef StageDir
  #error Pass /DStageDir=<folder holding R\ and Start-R.cmd>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Never change AppId: upgrades find the earlier install through it.
AppId={{A1E92B8A-2AFD-45A8-BAF7-C1803C90FB17}
AppName=RGui AI
AppVersion={#AppVersion}
AppVerName=RGui AI {#AppVersion}
AppPublisher=RGui AI
AppComments=R 4.6.1 with a local AI assistant for statistics coursework
VersionInfoVersion={#NumericVersion}
VersionInfoProductName=RGui AI
; Per user, never per machine.  The model is downloaded into, and the
; R workspace kept inside, the install folder, which therefore has to be
; writable by the user: Program Files is not.  This also means no
; administrator rights are needed, and any folder can be chosen,
; a pendrive included.
PrivilegesRequired=lowest
DefaultDirName={autopf}\RGui AI
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1903: RGui relies on the UTF-8 active code page from then on.
MinVersion=10.0.18362
OutputDir={#OutputDir}
OutputBaseFilename=RGui-AI-{#AppVersion}-setup
SetupIconFile=..\src\gnuwin32\front-ends\R.ico
UninstallDisplayIcon={app}\R\bin\x64\Rgui.exe
UninstallDisplayName=RGui AI {#AppVersion}
LicenseFile={#StageDir}\R\COPYING
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Close a running RGui before replacing its files.
CloseApplications=yes

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Everything, except the two files people edit ...
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "\R\etc\Rai.conf,\R\ai\system_prompt.txt"; Flags: ignoreversion recursesubdirs createallsubdirs
; ... which an upgrade must not overwrite.
Source: "{#StageDir}\R\etc\Rai.conf"; DestDir: "{app}\R\etc"; Flags: onlyifdoesntexist
Source: "{#StageDir}\R\ai\system_prompt.txt"; DestDir: "{app}\R\ai"; Flags: onlyifdoesntexist

[Dirs]
Name: "{app}\R\ai\models"
Name: "{app}\R\ai\context"

[UninstallDelete]
; The model arrived after installation, so Setup does not know about it.
Type: files; Name: "{app}\R\ai\models\*.gguf"
Type: files; Name: "{app}\R\ai\models\*.gguf.part"
Type: filesandordirs; Name: "{app}\work\tmp"
; {app}\work itself (the R workspace, history and packages) is left alone.

[Icons]
; Start-R.cmd keeps R's workspace, history and packages inside the install
; folder, so a pendrive install behaves the same on every computer.
Name: "{autoprograms}\RGui AI"; Filename: "{app}\Start-R.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\R\bin\x64\Rgui.exe"; Flags: runminimized
Name: "{autodesktop}\RGui AI"; Filename: "{app}\Start-R.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\R\bin\x64\Rgui.exe"; Flags: runminimized; Tasks: desktopicon

[Run]
Filename: "{app}\Start-R.cmd"; WorkingDir: "{app}"; Description: "Start RGui now"; Flags: postinstall nowait skipifsilent runminimized shellexec
