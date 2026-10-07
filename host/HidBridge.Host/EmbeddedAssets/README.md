# EmbeddedAssets

此目录用于存放构建时嵌入 HidBridge.Host.exe 的独立版 esptool.exe（约 14MB）。

- 从项目根目录运行 `scripts/build-embedded-esptool.ps1` 生成；产物不提交到 git（见根目录 `.gitignore`）。依赖和完整构建说明见 [开发环境与依赖](../../../docs/dependencies.md)。
- 构建时文件存在，`HidBridge.Host.csproj` 才会把它作为 `EmbeddedResource` 嵌入；远端刷写 API 与设置页本地刷写都优先使用内置工具，目标电脑无需安装 Python / ESP-IDF。
- 固件镜像不放在这里，也不从 `firmware/build` 嵌入。两个刷写入口使用用户选择的 JSON 清单，并从清单所在目录读取镜像；详见[刷写接口说明](../../../docs/firmware-update-api.md)。
- 首次刷写时，程序会把内置 esptool 解压到 `%LOCALAPPDATA%\HidBridge\embedded\esptool\esptool.exe` 缓存。
