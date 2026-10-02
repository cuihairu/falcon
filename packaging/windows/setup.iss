; Falcon Windows 安装器（Inno Setup 6）
;
; 产物：falcon-desktop-setup-<version>.exe（nightly 分发名 falcon-desktop-setup-nightly.exe，
;       匹配 release 上传通配 falcon-desktop-*nightly*）
;
; 构建（CI Package (Windows) 步骤已把载荷暂存到 falcon-desktop/ 目录）：
;   ISCC.exe /DAppVersion=nightly /DStageDir=<abs>\falcon-desktop /O<abs> packaging\windows\setup.iss
;
; 要求：Inno Setup 6.3+（x64compatible 架构别名；GitHub windows-latest 镜像预装 6.x）
; 验证：安装到 Program Files、开始菜单快捷方式与卸载入口、/VERYSILENT 静默
;       安装-启动-卸载 round trip 由 CI smoke 步骤在真 Windows 上走查。

#ifndef AppVersion
  #define AppVersion "nightly"
#endif
#ifndef StageDir
  ; 本地默认：仓库根相对路径（源文件路径相对本 iss 所在目录解析）
  #define StageDir "..\..\build-desktop\bin\Release\falcon-desktop"
#endif

[Setup]
; AppId 固定 GUID：每日覆盖安装合并为同一卸载条目（升级安装语义）
AppId={{8F3A5B7C-2E91-4D64-B1A0-9C7E52D84F16}
AppName=Falcon
AppVersion={#AppVersion}
AppPublisher=Falcon Team
; 装 Program Files（x64 位模式）——需求硬要求，需要管理员提权
DefaultDirName={autopf}\Falcon
DefaultGroupName=Falcon
DisableProgramGroupPage=yes
SetupIconFile=..\..\apps\desktop\resources\windows\falcon.ico
UninstallDisplayIcon={app}\falcon-desktop.exe
OutputBaseFilename=falcon-desktop-setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 升级时目标程序在跑则先关（/SILENT 下自动执行，不弹窗）
CloseApplications=yes

[Types]
Name: "full"; Description: "完整安装"

[Components]
Name: "app"; Description: "Falcon 桌面版"; Types: full; Flags: fixed

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; Flags: unchecked

[Files]
; 载荷 = Package (Windows) 暂存的完整运行时（exe/DLL/qt.conf/plugins/CRT）
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Falcon Desktop"; Filename: "{app}\falcon-desktop.exe"; WorkingDir: "{app}"
; 卸载入口（开始菜单；控制面板/设置卸载条目由 Inno 注册表键自动生成）
Name: "{group}\Uninstall Falcon Desktop"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Falcon Desktop"; Filename: "{app}\falcon-desktop.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\falcon-desktop.exe"; Description: "启动 Falcon(&L)"; Flags: nowait postinstall skipifsilent unchecked

[UninstallDelete]
; 卸载时清理可能的用户运行时残留（日志/缓存；用户数据目录不在 {app} 内）
Type: filesandordirs; Name: "{app}\logs"
