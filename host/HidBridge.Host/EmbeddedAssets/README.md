# EmbeddedAssets

此目录用于存放构建时嵌入 HidBridge.Host.exe 的独立版 esptool.exe（约 14MB）。

- 由 scripts/build-embedded-esptool.ps1 生成，不提交到 git（见根目录 .gitignore）。
- 存在时，csproj 会将其作为 EmbeddedResource 嵌入；远端刷写 API 与设置页本地刷写两个入口都优先调用它，目标机无需安装 Python / ESP-IDF 环境。
- 固件镜像不放在这里：构建时直接从 firmware/build 嵌入（若存在）。
- 首次刷写时程序会把内置 esptool 解压到 %LOCALAPPDATA%/HidBridge/embedded 缓存。
