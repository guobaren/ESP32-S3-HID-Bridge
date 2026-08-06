# MouseTraceRecorder

在对端 Windows 电脑上记录真实的 Windows Raw Input 鼠标事件，用于比较 BLE 和 USB 的报告节拍、位移、设备来源、光标位置和事件间隔。

## 构建

在本项目目录执行：

```powershell
dotnet build .\tools\MouseTraceRecorder\MouseTraceRecorder.csproj -c Release
```

也可以发布为对端直接运行的目录：

```powershell
dotnet publish .\tools\MouseTraceRecorder\MouseTraceRecorder.csproj -c Release -r win-x64 --self-contained false -o .\artifacts\MouseTraceRecorder
```

## 运行

分别对 BLE 和 USB 做同样的固定路线测试，例如每次记录 30 秒：

```powershell
.\artifacts\MouseTraceRecorder\MouseTraceRecorder.exe --label BLE --seconds 30 --output .\ble-trace.csv
.\artifacts\MouseTraceRecorder\MouseTraceRecorder.exe --label USB --seconds 30 --output .\usb-trace.csv
```

程序运行后，在指定时间内执行相同动作：

1. 静止 3 秒；
2. 匀速从左向右移动 10 秒；
3. 匀速从右向左移动 10 秒；
4. 画 5 秒小圆；
5. 剩余时间快速大范围移动。

每次测试尽量保持 DPI、灵敏度、Windows 鼠标设置和手部动作一致。程序退出后会生成：

- `*.csv`：每个 Raw Input 事件一行，包含高精度单调时间、事件间隔、设备路径、dx/dy、滚轮、按钮和 Windows 光标位置；
- `*.summary.txt`：事件数、累计位移、事件频率、最小/中位数/P95/最大事件间隔。

## 解释边界

这是对端 Windows 的 Raw Input 记录器：它能记录 Windows 收到的鼠标输入和当时的光标位置，但不能直接证明 BLE 空口每个 connection event 的到达时间。应将 CSV/摘要与 ESP32 串口中的 `BLE鼠标统计`、主机端鼠标统计一起分析。USB 和 BLE 必须分别保存文件，不能把一次程序正常退出视为测试通过。
