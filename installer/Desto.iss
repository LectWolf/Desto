#ifndef MyAppVersion
  #define MyAppVersion "0.1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\\build\\apps\\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\\dist"
#endif

#define MyAppName "Desto"
#define MyAppExeName "Desto.exe"
#define MyAppId "{{A01DC82C-A70F-45B8-BF60-6FD82047D8E4}"
#define MyAppUninstallKey "{A01DC82C-A70F-45B8-BF60-6FD82047D8E4}_is1"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Desto Project
AppPublisherURL=https://github.com/LectWolf/Desto
AppSupportURL=https://github.com/LectWolf/Desto/issues
AppUpdatesURL=https://github.com/LectWolf/Desto/releases
DefaultDirName={localappdata}\Programs\Desto
DefaultGroupName=Desto
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=Desto-{#MyAppVersion}-win-x64-setup
SetupIconFile=..\assets\desto.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
AppMutex=Local\Desto.DesktopHost
MinVersion=10.0.17763
VersionInfoVersion={#MyAppVersion}.0
VersionInfoCompany=Desto Project
VersionInfoDescription=Desto installer
VersionInfoProductName=Desto
VersionInfoProductVersion={#MyAppVersion}

[Languages]
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#SourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; Remove the pre-0.1.0.8 host name during upgrade.
[InstallDelete]
Type: files; Name: "{app}\desto_desktop_host.exe"

[Icons]
Name: "{group}\Desto"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\Desto"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,Desto}"; Flags: nowait postinstall skipifsilent

[Code]
var
  DeleteUserDataCheckBox: TNewCheckBox;

function FindWindow(lpClassName, lpWindowName: String): Integer;
  external 'FindWindowW@user32.dll stdcall';
function SendMessage(hWnd, Msg, wParam, lParam: Integer): Integer;
  external 'SendMessageW@user32.dll stdcall';

function NextVersionPart(var Version: String): Integer;
var
  DotPosition: Integer;
  Part: String;
begin
  DotPosition := Pos('.', Version);
  if DotPosition = 0 then
  begin
    Part := Version;
    Version := '';
  end
  else
  begin
    Part := Copy(Version, 1, DotPosition - 1);
    Delete(Version, 1, DotPosition);
  end;
  Result := StrToIntDef(Part, 0);
end;

function CompareVersions(LeftVersion, RightVersion: String): Integer;
var
  Index: Integer;
  LeftPart: Integer;
  RightPart: Integer;
begin
  Result := 0;
  for Index := 1 to 4 do
  begin
    LeftPart := NextVersionPart(LeftVersion);
    RightPart := NextVersionPart(RightVersion);
    if LeftPart < RightPart then
    begin
      Result := -1;
      Exit;
    end;
    if LeftPart > RightPart then
    begin
      Result := 1;
      Exit;
    end;
  end;
end;

function CloseRunningDesto(): Boolean;
var
  LifecycleWindow: Integer;
  Attempt: Integer;
begin
  Result := True;
  LifecycleWindow := FindWindow('DestoShellLifecycleHost', 'DestoShellLifecycleHost');
  if LifecycleWindow = 0 then Exit;

  SendMessage(LifecycleWindow, $0010 { WM_CLOSE }, 0, 0);
  for Attempt := 1 to 100 do
  begin
    Sleep(100);
    if FindWindow('DestoShellLifecycleHost', 'DestoShellLifecycleHost') = 0 then Exit;
  end;

  MsgBox(
    'Desto 仍在运行，无法安全关闭。请手动退出 Desto 后重试安装。',
    mbError, MB_OK);
  Result := False;
end;

function InitializeSetup(): Boolean;
var
  InstalledVersion: String;
  UninstallKey: String;
begin
  Result := True;
  UninstallKey := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
    '{#MyAppUninstallKey}';
  if RegQueryStringValue(HKCU, UninstallKey, 'DisplayVersion', InstalledVersion) and
     (CompareVersions('{#MyAppVersion}', InstalledVersion) < 0) then
  begin
    MsgBox(
      '已安装较新的 Desto ' + InstalledVersion +
      '。为保护配置，不能直接降级到 {#MyAppVersion}。',
      mbError, MB_OK);
    Result := False;
  end;
  if Result then Result := CloseRunningDesto();
end;

procedure InitializeUninstallProgressForm();
begin
  DeleteUserDataCheckBox := TNewCheckBox.Create(UninstallProgressForm);
  DeleteUserDataCheckBox.Parent := UninstallProgressForm;
  DeleteUserDataCheckBox.Left := UninstallProgressForm.StatusLabel.Left;
  DeleteUserDataCheckBox.Top := UninstallProgressForm.StatusLabel.Top + 42;
  DeleteUserDataCheckBox.Width := UninstallProgressForm.StatusLabel.Width;
  DeleteUserDataCheckBox.Caption :=
    '同时删除用户数据（配置和卡片内容）';
  DeleteUserDataCheckBox.Checked := False;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    RegDeleteValue(
      HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Desto');
    Exec(
      ExpandConstant('{sys}\schtasks.exe'),
      '/Delete /TN "Desto" /F',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
  if (CurUninstallStep = usPostUninstall) and
     DeleteUserDataCheckBox.Checked then
  begin
    DelTree(ExpandConstant('{localappdata}\Desto'), True, True, True);
  end;
end;
