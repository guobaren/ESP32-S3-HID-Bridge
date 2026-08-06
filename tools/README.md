# 命令行蓝牙验证工具

`connect-ble-hid.ps1` 使用 **Windows PowerShell 5.1** 的 WinRT API 完成本机 BLE HID 的发现、配对与连接，不需要打开 Windows 蓝牙设置界面：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\connect-ble-hid.ps1 `
  -Name 'HidBridge Keyboard Mouse' `
  -ScanSeconds 30
```

工作流程：

1. 枚举本机蓝牙适配器状态；
2. 先从 Windows 已知 BLE 设备中按名称查找；
3. 若未找到，使用 `BluetoothLEAdvertisementWatcher` **主动扫描 BLE 广播和 Scan Response**，避免把 `BluetoothLEDevice.GetDeviceSelector()` 误当作未配对设备扫描；
4. 从广播地址创建 `BluetoothLEDevice`，调用 `PairAsync()`（如尚未配对）；
5. 以未缓存方式枚举 GATT 服务，主动请求本机 BLE 栈建立链路；
6. 输出 `ConnectionStatus`，并在默认情况下保持进程运行以持有连接引用。

可在完成验证后自动退出：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\connect-ble-hid.ps1 `
  -ScanSeconds 30 `
  -DurationSeconds 20
```

注意：`BluetoothLEDevice=Connected` 只证明 WinRT BLE 链路已建立。HOGP 的输入链路仍必须结合：

- `Get-PnpDevice` 中新增的 HID/鼠标设备；
- 固件 `GAP`、加密和 HID 订阅日志；
- 实际鼠标移动和 Windows 光标变化；

三项共同验证，不能用脚本退出码单独替代真实 HID 输入验证。

按已知地址连接（地址来自同一设备会话的扫描日志，可能因随机地址变化而失效）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\connect-ble-hid.ps1 `
  -Address 'E88485461D82' `
  -DurationSeconds 20
```
