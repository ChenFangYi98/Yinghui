#ifndef SourceDir
  #define SourceDir "..\dist\Yinghui-Windows-0.4.5-x64"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
[Setup]
#ifdef SmokeTest
AppId={{E729969A-5A89-4E46-A5C8-D925B780124C}
AppName=Yinghui Installer Validation
#else
AppId={{77B380AD-613E-4855-AF18-32A150FA2A83}
AppName=映绘 Yinghui
#endif
AppVersion=0.4.5
AppPublisher=Yinghui
DefaultDirName={localappdata}\Programs\Compositor
#ifdef SmokeTest
DefaultGroupName=Yinghui Installer Validation
#else
DefaultGroupName=映绘 Yinghui
#endif
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19045
OutputDir={#OutputDir}
OutputBaseFilename=Yinghui-Windows-0.4.5-x64-Setup
Compression=lzma2/fast
SolidCompression=no
DiskSpanning=yes
DiskSliceSize=2000000000
WizardStyle=modern
UninstallDisplayIcon={app}\Yinghui.exe
SetupIconFile={#SourceDir}\Yinghui.ico
LicenseFile={#SourceDir}\licenses\Compositor.txt
ChangesAssociations=yes
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut / 创建桌面快捷方式"; Flags: unchecked
Name: "associate"; Description: "Associate .comp projects / 关联 .comp 项目"; Flags: unchecked
[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.safetensors,*.gguf"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\*.safetensors"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs nocompression
Source: "{#SourceDir}\*.gguf"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs nocompression
[InstallDelete]
Type: files; Name: "{app}\Compositor.exe"
#ifndef SmokeTest
Type: files; Name: "{userprograms}\Compositor\Compositor.lnk"
#endif
[Icons]
Name: "{group}\映绘 Yinghui"; Filename: "{app}\Yinghui.exe"
Name: "{autodesktop}\映绘 Yinghui"; Filename: "{app}\Yinghui.exe"; Tasks: desktopicon
[Registry]
Root: HKCU; Subkey: "Software\Classes\.comp\OpenWithProgids"; ValueType: none; ValueName: "Compositor.Project"; Flags: uninsdeletevalue; Tasks: associate
Root: HKCU; Subkey: "Software\Classes\Compositor.Project"; ValueType: string; ValueData: "映绘项目"; Flags: uninsdeletekey; Tasks: associate
Root: HKCU; Subkey: "Software\Classes\Compositor.Project\shell\open\command"; ValueType: string; ValueData: """{app}\Yinghui.exe"" ""%1"""; Tasks: associate
Root: HKCU; Subkey: "Software\Classes\Compositor.Project\shell\open\command"; ValueType: string; ValueData: """{app}\Yinghui.exe"" ""%1"""; Check: IsOurExistingAssociation
[Run]
Filename: "{app}\Yinghui.exe"; Description: "Launch Yinghui / 启动映绘"; Flags: nowait postinstall skipifsilent
[Code]
function IsOurExistingAssociation: Boolean;
var ExistingCommand: String;
begin
  Result := RegQueryStringValue(HKCU, 'Software\Classes\Compositor.Project\shell\open\command', '', ExistingCommand)
    and (CompareText(ExistingCommand, '"' + ExpandConstant('{app}\Compositor.exe') + '" "%1"') = 0);
end;
