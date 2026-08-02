# 真实硬件测试

`mouse_20ms_check.py` 通过 UART 协议向已烧录固件发送三组鼠标移动：每 20 ms 分别移动
20、5、1，单组持续 1 秒。脚本会读取固件统计日志，并严格核对每组 50 个有效移动报告、
完整位移、零积压、零提交失败和不超过 40 ms 的完成间隔。

该检查需要 ESP32 的 UART 和 USB HID 目标端同时连接，属于真实硬件端到端测试，不能由
`HidBridge.Host.Checks` 等主机模拟检查替代。

```powershell
python .\tests\hardware\mouse_20ms_check.py --port COM3
```

本机已与 ESP32 BLE HID 配对时，可运行 BLE 专用验收。该用例按设备地址过滤 Windows
Raw Input，逐项核对 50 个原始报告、总位移和最大报告间隔，不使用经过光标加速后的坐标：

```powershell
dotnet run --project .\tests\HidBridge.BleHardware.Checks\HidBridge.BleHardware.Checks.csproj -- --port COM3
```
