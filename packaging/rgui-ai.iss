; Inno Setup script for RGui with the local AI assistant.
;
; Built by 'rgui installer' (see rgui.ps1), which stages the files and
; passes the version:
;
;   ISCC.exe /DAppVersion=0.1.1 /DNumericVersion=0.1.1.0 /DStageDir=... /DOutputDir=... rgui-ai.iss
;
; Needs Inno Setup 7.
;
; Offline builds include both verified models through OfflineModelsInclude.

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
#ifndef AppIdValue
  #define AppIdValue "{{A1E92B8A-2AFD-45A8-BAF7-C1803C90FB17}"
#endif
#ifndef AppNameValue
  #define AppNameValue "RGui AI"
#endif
#ifdef OfflineModelsInclude
  #define InstallerSuffix "-offline"
#else
  #define InstallerSuffix ""
#endif

[Setup]
; Never change AppId: upgrades find the earlier install through it.
AppId={#AppIdValue}
AppName={#AppNameValue}
AppVersion={#AppVersion}
AppVerName={#AppNameValue} {#AppVersion}
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
DefaultDirName={autopf}\{#AppNameValue}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1903: RGui relies on the UTF-8 active code page from then on.
MinVersion=10.0.18362
OutputDir={#OutputDir}
OutputBaseFilename=RGui-AI-{#AppVersion}{#InstallerSuffix}-setup
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
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "\R\etc\Rai.conf,\R\ai\system_prompt.txt,*.gguf,*.gguf.part"; Flags: ignoreversion recursesubdirs createallsubdirs
; ... which an upgrade must not overwrite.
Source: "{#StageDir}\R\etc\Rai.conf"; DestDir: "{app}\R\etc"; Flags: onlyifdoesntexist
Source: "{#StageDir}\R\ai\system_prompt.txt"; DestDir: "{app}\R\ai"; Flags: onlyifdoesntexist

[Dirs]
Name: "{app}\R\ai\models"
Name: "{app}\R\ai\context"

[UninstallDelete]
; The model arrived after installation, so Setup does not know about it.
Type: files; Name: "{app}\R\ai\models\Qwen3.5-4B-Q4_K_M.gguf"
Type: files; Name: "{app}\R\ai\models\Qwen3.5-4B-Q4_K_M.gguf.part"
Type: files; Name: "{app}\R\ai\models\Qwen3.5-4B-mmproj-F16.gguf"
Type: files; Name: "{app}\R\ai\models\Qwen3.5-4B-mmproj-F16.gguf.part"
Type: filesandordirs; Name: "{app}\work\tmp"
; {app}\work itself (the R workspace, history and packages) is left alone.

[Icons]
; Start-R.cmd keeps R's workspace, history and packages inside the install
; folder, so a pendrive install behaves the same on every computer.
Name: "{autoprograms}\{#AppNameValue}"; Filename: "{app}\Start-R.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\R\bin\x64\Rgui.exe"; Flags: runminimized
Name: "{autodesktop}\{#AppNameValue}"; Filename: "{app}\Start-R.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\R\bin\x64\Rgui.exe"; Flags: runminimized; Tasks: desktopicon

[Run]
Filename: "{app}\Start-R.cmd"; WorkingDir: "{app}"; Description: "Start RGui now"; Flags: postinstall nowait skipifsilent runminimized shellexec

#ifdef OfflineModelsInclude
  #include OfflineModelsInclude
#endif
