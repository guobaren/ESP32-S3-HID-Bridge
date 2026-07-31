# 项目协作说明

- 面向人的文档、注释和提交说明优先使用中文；协议字段、API 名称和技术标识符保持原样。
- 修改主机端后至少运行：

  ```powershell
  dotnet build .\host\HidBridge.Host\HidBridge.Host.csproj -c Release
  dotnet format .\host\HidBridge.Host\HidBridge.Host.csproj --verify-no-changes --no-restore
  ```

- 修改固件后，在已进入 ESP-IDF 环境的终端运行：

  ```powershell
  Set-Location .\firmware
  idf.py set-target esp32s3
  idf.py build
  ```

- 串口协议发生变化时，同步更新 `docs/protocol.md`、C# 编码器和 C 解析器。
- 任何停止转发、断线或退出路径都必须释放键盘按键和鼠标按钮，避免目标设备卡键。
- 不在日志、配置或示例中加入真实设备序列号等不必要的本机标识。
