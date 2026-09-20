# dual_proxy：双板单鼠标第一阶段固件

这是独立于 `firmware/main` 的实验性 ESP32-S3 工程。两块板刷同一个镜像，固件在启动时先把原生 USB 当作 Device 探测：

- 被电脑枚举：锁定为 `PC_DEVICE`，电脑只看到这一块板提供的一个通用相对鼠标 HID；
- 没有电脑在探测窗口内枚举：卸载 Device，切换为 `MOUSE_HOST`，只监听 USB HID `protocol=2` 的标准鼠标接口；`protocol=0` 的 Logitech/vendor 接口被隔离。

板载 ESP32-S3-DevKitC-1 WS2812B 使用 GPIO48：启动/角色探测、断开、`MOUSE_HOST` 尚未成功启动受支持的 Boot Mouse、或 `PC_DEVICE` 尚未 `tud_mounted()` 时常亮红色；连接后鼠标侧常亮绿色、电脑侧常亮蓝色。软件移动报告在 HID 真正提交成功后短暂熄灭，再恢复蓝色；连续高频输入通过短暂闪烁冷却合并，避免 LED 刷新干扰实时路径。LED 刷新由独立低频任务执行，USB/1 kHz 输入路径只更新状态，不直接做 RMT/LED I/O。

## 运行连接

PC 侧板正常运行时只连接原生 USB-C 数据口：这一根线同时提供板卡供电、让电脑枚举复合 HID+CDC 设备，并承载控制协议。PC 侧板的 UART0/CH340 仅用于刷写和开发日志。鼠标侧板正常运行时不连接电脑，UART0/CH340 也仅用于刷写和开发日志；它需要独立、稳定的板卡和鼠标供电。两板通过 UART1 连接：

```text
PC板 GPIO17 (TX)  ->  鼠标板 GPIO18 (RX)
PC板 GPIO18 (RX)  <-  鼠标板 GPIO17 (TX)
两板 GND 共地
```

不要连接两板的 5V 或 3V3，不要假设鼠标数据口会给鼠标侧板反向供电。鼠标侧板和实体鼠标都需要独立、稳定的供电。当前开发阶段可以分别把两块板的 CH340 接到电脑，但正常使用时不依赖任何 CH340。

## 协议和功能边界

PC 侧板原生 USB 的 CDC ACM 接口使用现有 A5 5A、版本 2 协议：`SessionStart`、`MouseReport`、`Ping`、`ReleaseAll`、`DeviceProbe/DeviceHello`。UART0/CH340 不再解析控制协议，只输出 `ESP_LOG` 并用于刷写调试。CDC 的 RX 回调只通知任务，普通任务负责读取和解析；SessionStart 后软件租约为 1500 ms，CDC 断开、串口关闭、租约超时和 ReleaseAll 只释放软件输入，不清除实体输入。软件按钮和实体按钮分开维护，输出为两者 OR；移动、滚轮和水平滚动分别相加。

PC 侧原生 USB 是一个复合设备（同一 USB 父设备、同一根线）：

| 接口/端点 | 用途 |
|---|---|
| Interface 0 / IN `0x81` | 通用相对鼠标 HID，Report ID 1，16-byte interrupt IN，1 ms |
| Interface 1 / interrupt IN `0x82` | CDC ACM 通知接口 |
| Interface 2 / bulk OUT `0x03`、IN `0x83` | CDC ACM 数据接口，64-byte bulk |

设备描述符使用 `VID:PID=303A:4005`，不同于正式固件的 CDC-only/HID-only 配置；产品字符串为 `Dual Proxy HID+CDC`。该设备只提供通用 HID 描述符，不复制实体 Logitech 的 VID/PID、vendor/HID++ 接口或 Feature 报告，因此不能宣称 Logitech 驱动/G Hub 完整兼容。

板间 UART1 也使用相同的 CRC16 帧封装，使用内部类型：

| 类型 | 含义 |
|---:|---|
| `0x20` | LINK_HELLO：角色、node ID、USB 状态、generation |
| `0x21` | PHYSICAL_MOUSE：接口、Report ID、按钮、X/Y、wheel/pan |
| `0x22` | PHYSICAL_RELEASE：实体源释放 |
| `0x23` | LINK_PING：链路保活 |

UART1 会拒绝相同 node ID 或相同角色的对端；3 个 250 ms 周期无有效帧后清除实体按钮和积累位移。队列溢出、USB HID 断开、角色切换和退出路径都发送安全释放。

本阶段只实现通用 HID 鼠标，不实现 Logitech C092 的原始 VID/PID、第二个 vendor/HID++ 接口、Feature 报告和控制事务。因此不能宣称 Logitech 驱动/G Hub 完整兼容；interface 1 `protocol=0` 仅隔离并计数。

## 构建和刷写

在 ESP-IDF 6.0.2 环境中：

```powershell
Set-Location .\firmware\dual_proxy
. ..\..\scripts\Enter-EspIdf.ps1
idf.py set-target esp32s3
idf.py build
```

刷写仍使用项目现有工具选择本工程 `build/flasher_args.json`。本任务只构建，不自动刷写硬件。

## 不启动 EXE 的原生 CDC 测试

PC 侧原生 USB 枚举出的 CDC COM 独占打开后运行（通常不是 CH340 COM）：

```powershell
python .\tools\test_dual_proxy_serial.py --port COMx --cursor
```

脚本默认先发送 `DeviceProbe`，只有收到签名 `HIDBRDG2` 且 nonce 匹配的 `DeviceHello` 才继续；随后发送 `SessionStart`、小幅 `+4`、等待、等量 `-4`、`Ping`，不发送点击、键盘或 `SendInput`。`--no-probe` 仅用于明确知道目标 COM 时的调试。脚本打开端口前显式将 DTR/RTS 置为 false，避免 CH340 自动复位行为；原生 CDC 本身不因 DTR/RTS 复位。CDC 只传二进制帧，不与设备日志混流；同一 COM 不能同时被主机 EXE 或其他工具打开。

真实鼠标报告、单一 HID 设备、实体移动与软件移动叠加仍必须在实际刷写和受控输入下验收；本工程构建通过不等于硬件通过。
