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

按已知地址连接（下面是示例占位地址，需替换为同一设备会话的扫描地址；地址可能因随机变化而失效）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\connect-ble-hid.ps1 `
  -Address 'AABBCCDDEEFF' `
  -DurationSeconds 20
```

## G HUB 窗口置顶截图

`capture_window_winapi.py` 通过 WinAPI 置顶指定 Logitech G HUB 顶层窗口，优先调用 `PrintWindow(PW_RENDERFULLCONTENT)` 并保存 PNG。脚本只接受进程名为 `lghub*.exe`、且可执行文件路径位于 `LGHUB` 目录中的窗口；标题按完整字符串匹配（不区分大小写），多个匹配项会报错并要求改用 HWND。

先列出窗口句柄、进程和标题：

```powershell
py -3 .\tools\capture_window_winapi.py --list
```

按句柄截图并置顶：

```powershell
py -3 .\tools\capture_window_winapi.py --hwnd 0x00123456 --output .\artifacts\ghub.png
```

也可使用唯一进程窗口或完整标题；`--show` 会恢复并显示窗口，`--foreground` 会请求切到前台。`--fallback-desktop` 会在 PrintWindow 失败或返回空白时，以桌面 BitBlt 截取该窗口矩形内的可见区域。

`--restore` 用于最小化或跑到屏幕外的窗口：按 `GetWindowPlacement` 记录的**正常尺寸**（`rcNormalPosition`，并夹到可见桌面内）还原并显示，**不会最大化**。还原顺序是 `SetWindowPlacement(showCmd=SW_SHOWNORMAL)` → `ShowWindowAsync(SW_SHOWNOACTIVATE)` → `SetWindowPos(SWP_SHOWWINDOW)`，逐级兜底并检查结果：

```powershell
py -3 .\tools\capture_window_winapi.py --hwnd 0x00123456 --restore --foreground --allow-unpinned --fallback-desktop --output .\artifacts\ghub.png
```

契约与实测（`tools/selftest_capture_restore.py`，用临时记事本窗口，退出码 0 = 三项契约满足）：

- 最小化 → 还原后 `showCmd=1`（正常尺寸）、不再最小化、矩形落在可见桌面内；
- **先前最大化过**再最小化 → 还原回正常尺寸，不会回到最大化；
- 已经可见且未最小化的窗口 → 调用还原无任何副作用（不改矩形、不变最大化）。

只调用 `SetWindowPos` **无法**让窗口离开最小化状态（自测中 `SetWindowPos` 返回成功但窗口仍最小化），所以状态切换必须由 `SetWindowPlacement`/`ShowWindowAsync` 负责。G HUB 进程曾对 `ShowWindow`/`SetWindowPos(HWND_TOPMOST)` 返回 Win32 error 5；若还原被拒，脚本会明确报“窗口仍处于最小化”，需要手动还原。

若 `SetWindowPos(HWND_TOPMOST)` 被 Windows 权限拒绝，可加 `--allow-unpinned --show --foreground --fallback-desktop` 继续截图。脚本会明确报告“未能始终置顶”，但前台显示仍可能成功。

截图限制：PrintWindow 可能无法呈现硬件加速或受保护内容。桌面回退只反映当前屏幕像素，被其他窗口遮挡的区域可能显示遮挡窗口内容，屏幕外区域无法恢复；因此回退结果不保证是纯 G HUB 内容。

最小化（`IsIconic=true`，窗口矩形为 `-32000,-32000`）或位于其他虚拟桌面时脚本会报“窗口完全位于采集范围之外”。这种情况优先改用 `--restore`（见上一节）自动还原；若还原也被系统拒绝（Win32 error 5），需要先在桌面上手动还原 G HUB 窗口再截图。

## 用软件模拟「拔插 USB 口」（需要 UAC 提权）

Windows 上可以用 `Disable-PnpDevice`/`Enable-PnpDevice` 把 PC 能看到的 USB 设备断开再恢复，等价于**数据线**拔插，用于自动化跑 O1–O6 这类「谁先插」的线序测试。

```powershell
# 主流程不是管理员时，用 Start-Process -Verb RunAs 拉起（会弹 UAC，需要你点“是”）
Start-Process pwsh -Verb RunAs -Wait -ArgumentList @(
  '-NoProfile','-ExecutionPolicy','Bypass','-File','tools\Invoke-ElevatedDeviceAction.ps1',
  '-Action','cycle','-Match','VID_046D&PID_C092','-HoldSeconds','6','-ResultPath',"$env:TEMP\usb-cycle.txt")
Get-Content "$env:TEMP\usb-cycle.txt"     # 每步真实结果都在这里
```

`-Action` 可选 `self-check` / `list` / `disable` / `enable` / `cycle`；`-Match` 是 `InstanceId` 的子串（如 `VID_046D&PID_C092`、`VID_1A86&PID_7523`）。

**能模拟与不能模拟的边界（实测结论）**

- 能：PC 侧可见设备的**数据线**拔插——电脑侧板呈现给目标电脑的克隆设备（`VID_046D&PID_C092`）、探测设备（`VID_303A&PID_4005`）、两块板的 CH340 调试口。
- 不能：**VBUS 断电**。PC 端口一般不做单口断电，所以只靠原生 USB 取电的板子不会因此断电重启（角色不会丢），要丢角色仍需 RTS/EN 复位配合。
- 完全不能：**插在鼠标侧板 Host 口上的物理鼠标**。那一侧是板子在当 USB 主机，PC 看不到也管不到它的端口——只能手动拔插，或给固件加测试钩子。
- 非管理员时 `Disable-PnpDevice` 直接失败（本机实测报「常规故障」），所以必须提权；提权脚本把结果写文件，主流程读文件判定，不依赖提权子进程的标准输出。

## 双板统计快照读取

`dual_proxy` 固件保留 RAM 中的累计计数与实时队列快照，通过 UART0 `0x1D` 请求、`0x1E` 分页返回。快照包括恢复事务、输入处理、UART1 与各队列状态，不清零计数、不暂停输入。EXE 的“读取设备统计”入口复用已打开的两块板串口；单板 Python 工具仅用于 EXE 未占用的端口：

```powershell
python .\tools\read_device_stats.py --port COM14
python .\tools\read_device_stats.py --port COM14 --baud 921600 --timeout 4
```

工具在打开串口前把 DTR/RTS 设为低电平，并要求收到计数器页和队列页的完整快照。未知队列指标输出为 `null`。共享帧编码、CRC 与流式解析实现位于 `tools/uart_protocol.py`；`inject_mouse_motion.py`、`vendor_control_probe.py` 等诊断脚本复用此模块。统计协议字段和实际 C 编码 fixture 见 [docs/protocol.md](../docs/protocol.md) 的「双板内存统计快照」及 `tests/fixtures/stats_snapshot_c_frames.txt`。

## 软件鼠标移动延迟测试
`measure_udp_smoothing.py` 测量 UDP 软件输入到本机光标的延迟、5 槽平滑和连续命令重叠效果：向运行 `HidBridge.Host.exe` 的地址发送相对位移，在运行脚本的机器上每 1 ms 采样一次 `GetCursorPos`，输出 CSV、JSON 和两个无依赖 SVG。

链路为 `本脚本 --UDP--> HidBridge.Host.exe --UART0--> 鼠标侧板(M) --UART1--> 电脑侧板(P) --USB--> 本机光标`，因此**只在电脑侧板(P)输出到本机、克隆鼠标已枚举、且 EXE 正在运行时可用**。脚本会先只读查询本机是否存在 `VID_046D&PID_C092` 节点，缺失时直接以退出码 2 结束（`--force` 可跳过）。

```powershell
# 本机拓扑（EXE 与 P 板输出都在本机）
python .\tools\measure_udp_smoothing.py --output-dir .\artifacts\tests\move-latency

# EXE 在另一台机器时：--host 指向那台机器
python .\tools\measure_udp_smoothing.py --host 192.0.2.10 --output-dir .\artifacts\tests\move-latency
```

判定标准（不符合时退出码 1，通过时退出码 0）：

- 每类用例的**首次移动延迟 < 10 ms**（相对该用例第一条命令的发出时刻；只取首条命令，因为连续用例后续命令发出时前一条的平滑槽位仍在到达）；
- **实际位移与预期一致**：单次用例 20px、连续三次用例 60px。

50% 位移耗时、逐命令延迟、每步位移等写入 `summary.json`/CSV/SVG，仅作过程信息。多命令用例偏差 1px 时脚本会提示先检查 Windows 指针加速（设置→鼠标→提高指针精确度）与上一用例残留的平滑槽位。

当前基线（2026-09-25，`artifacts/tests/move-latency-20260925/summary.json`）：连续两次运行均 `[结论] PASS`——第 1 次单次首动 `5.998 ms`、连续首动 `3.999 ms`；第 2 次 `6.999 ms`、`5.0 ms`；两次位移都是 `20px`/`60px` 精确一致。阈值仍为 `< 10 ms`，样本量 2 次，不代表长时稳定性。
