# Installer

Desto 使用 Inno Setup 6 生成单文件、按用户安装的 x64 安装包。程序目录默认为 `%LocalAppData%\Programs\Desto`，用户数据目录为 `%LocalAppData%\Desto`，二者生命周期分离。简体中文界面使用仓库内 [`languages/ChineseSimplified.isl`](languages/ChineseSimplified.isl)，不依赖构建机额外安装非官方语言包。

```powershell
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File installer\BuildInstaller.ps1
```

升级会替换程序文件，但保留用户配置和 Card 文件。较旧安装包会拒绝覆盖较新版本。卸载默认保留用户数据，并提供明确的“同时删除用户数据”复选框；无论是否保留数据，卸载都会移除 Desto 的登录计划任务和 Run 回退值。

Release 使用静态 MSVC CRT，因此干净 Windows 10 1809 或更高版本不需要单独安装 VC++ Runtime。安装器构建命令会自动读取 `CMakeLists.txt` 的版本号，也支持 `-Version`、`-BuildDirectory` 和 `-OutputDirectory` 覆盖。

本轮验证结果（Windows 11 x64）：

- 安装包名称使用四段版本：`Desto-0.1.0.<build>-win-x64-setup.exe`。`installer/BuildInstaller.ps1` 每次构建自动递增 `.desto-build-number`，并把同一第四位写入 EXE 资源、安装包和设置页“关于”。最终 SHA-256 由发布脚本写入 `SHA256SUMS.txt`，不在文档中固化会随构建变化的值。
- 静默安装退出码 `0`，程序文件完整落盘；`--duration-ms 1500` 启动退出码 `0`，无残留进程。
- 同版本升级保留用户数据；低于当前四段版本的安装包会被拒绝。
- 静默卸载移除程序、登录计划任务和开机启动回退值，默认保留用户数据。

安装包未签名时 Windows SmartScreen 仍可能警告；正式发布需要代码签名证书。
