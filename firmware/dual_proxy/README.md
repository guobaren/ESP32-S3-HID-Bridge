# dual_proxy：双板单鼠标第一阶段固件

这是独立于 `firmware/main` 的双 ESP32-S3 透明鼠标代理工程。两块板刷同一个镜像，固件在启动时先把原生 USB 当作 Device 探测：

- 被电脑枚举：锁定为 `PC_DEVICE`；收到完整物理 Profile 后严格克隆真实鼠标 Device/Configuration/String/Report descriptor，并代理 raw Input/Output/Feature；
- 没有电脑在探测窗口内枚举：卸载 Device，切换为 `MOUSE_HOST`，以 USB Host 接真实鼠标/接收器，同时从 UART0/CH340 接收电脑 A 的软件输入。

板载 ESP32-S3-DevKitC-1 WS2812B 使用 GPIO48：启动/角色探测、断开、`MOUSE_HOST` 尚未成功启动受支持的 Boot Mouse、或 `PC_DEVICE` 尚未 `tud_mounted()` 时常亮红色；连接后鼠标侧常亮绿色、电脑侧常亮蓝色。软件移动报告在 HID 真正提交成功后短暂熄灭，再恢复蓝色；连续高频输入通过短暂闪烁冷却合并，避免 LED 刷新干扰实时路径。LED 刷新由独立低频任务执行，USB/1 kHz 输入路径只更新状态，不直接做 RMT/LED I/O。

## 运行连接

正式拓扑中，电脑 B 只连接 PC 侧板原生 USB-C；电脑 A 通过鼠标侧板 UART0/CH340 USB-C 给板卡供电并发送软件命令，鼠标侧板原生 USB Host 口连接并给真实鼠标供电。PC 侧板 UART0 只在刷写/开发时连接。两板通过 UART1 连接：

```text
PC板 GPIO17 (TX)  ->  鼠标板 GPIO18 (RX)
PC板 GPIO18 (RX)  <-  鼠标板 GPIO17 (TX)
两板 GND 共地
```

不要连接两板的 5V 或 3V3。鼠标侧板由电脑 A 的 UART USB-C 供电，板载 5V 路径再给 Host 口鼠标供电；必须以真实枚举确认通信，不能只凭鼠标灯亮判断。当前硬件已实际让测试鼠标正常上电。

## 协议和功能边界

鼠标侧 UART0/CH340 使用现有 A5 5A、版本 2 协议：`SessionStart`、`MouseReport`、`Ping`、`ReleaseAll`、`DeviceProbe/DeviceHello`。DeviceHello 追加运行角色，主机自动探测只接受 `MOUSE_HOST`。SessionStart 后软件租约为 1500 ms；关闭串口、租约超时和 ReleaseAll 经板间高优先级消息只释放软件输入，不清除实体输入。Host 以 500 Hz 合并软件位移，PC 侧板将每条命令叠加分摊到 5 个滚动 1 ms 槽，以 1000 Hz USB HID 节拍消费；新命令不会排在旧命令尾部，位移代数和保持不变。PC 侧按动态 Report layout 将软件按钮与实体按钮合并，将移动、滚轮和水平滚动相加；实体报告仍直接转发，不进入软件平滑器。

PC 侧正式运行时不追加 CDC、自定义 HID 或隐藏控制 Report；它只呈现物理鼠标原有的 VID/PID、字符串、接口、端点和 HID collections。无法满足 ESP32-S3 Full-Speed、端点/FIFO 或描述符安全预算的设备保持断开，不回退为 `303A:4005` 通用鼠标。

板间 UART1 也使用相同的 CRC16 帧封装，使用内部类型：

| 类型 | 含义 |
|---:|---|
| `0x20` | LINK_HELLO：角色、node ID、USB 状态、generation |
| `0x21` | PHYSICAL_MOUSE：接口、Report ID、按钮、X/Y、wheel/pan |
| `0x22` | PHYSICAL_RELEASE：实体源释放 |
| `0x23` | LINK_PING：链路保活 |
| `0x24` | PROFILE_BEGIN：动态设备 Profile 的 transfer ID、总长度与 CRC32 |
| `0x25` | PROFILE_CHUNK：按严格连续 offset 传输最多 56 字节 Profile 数据 |
| `0x26` | PROFILE_COMMIT：再次核对 transfer ID、总长度与 CRC32 后发布 Profile |
| `0x27..0x2A` | raw HID Input 与 SET/GET_REPORT 双向事务 |
| `0x2B` | SOFTWARE_MOUSE：鼠标侧 UART0 收到的软件鼠标报告 |
| `0x2C` | SOFTWARE_RELEASE：只释放软件输入 |
| `0x2D` | DEVICE_GONE：物理 USB 鼠标/接收器已拔出 |

UART1 会拒绝相同 node ID 或相同角色的对端；3 个 250 ms 周期无有效帧、鼠标侧掉电、物理鼠标拔出或 Profile 超时均按设备拔出处理：先释放所有输入，再让 PC 侧卸载 USB Device。新 Profile 完整校验前不会重新连接电脑 B。

## 多型号动态 Profile（运行时观察阶段）

`hid_device_profile` 提供与具体鼠标无关的有界二进制模型，保存原始 Device/Configuration 描述符、UTF-8 字符串以及按物理 interface number 索引的 HID Report 描述符。Profile 最大 4096 字节；接口数、字符串和单份描述符都有独立上限。UART1 分片接收器只接受严格连续 offset，并且只有在 BEGIN/COMMIT 字段一致、总 CRC32 正确、反序列化恰好消费全部数据后才发布新 Profile。错误或中断的替换传输不会清除上一份已发布 Profile。

鼠标侧在每个 HID 接口成功打开后收集动态 interface number、subclass、protocol、Report descriptor、VID/PID/字符串及原始 Device/Configuration descriptor；最后一个接口后约 200 ms 防抖，Profile 序列化后通过 UART1 的 `0x24..0x26` 有界分片发送。电脑侧只在完整 Profile 通过 CRC、反序列化、端点预算和动态鼠标布局检查后安装严格克隆描述符并重新枚举。

工程通过独立只读 USB Host client 取得 raw Device/Configuration descriptor；公开 HID Host API 继续提供逐接口 Report descriptor 和字符串。任一关键描述符只能 synthetic/partial 时，严格克隆拒绝启用。Profile v2 的每个报告项保存原始 `interface_number/subclass/protocol`。

Profile 串流在 UART1 链路在线时发送；实体鼠标安全释放和 raw Input 优先，普通鼠标报告每累计 8 个最多让出一个 Profile 帧，离线不发送，peer generation 改变从 BEGIN 重新开始。无法安全解析相对 X/Y、wheel、pan 或 buttons 的型号仍可原样透传厂商通信，但禁用软件叠加。C092 的 G HUB 和 C539 的动态枚举/实体移动已有实机样本，不能外推为所有型号通过。

C092 的 VID/PID、字符串、67/151 字节报告描述符仅是首个测试向量，不是默认运行配置。后续动态克隆仍必须检查接口和端点预算。

软件键盘注入只在物理 Profile 本来就包含可安全解析的键盘 collection 时允许；为保持厂商驱动兼容，不给纯鼠标 Profile 追加键盘接口。

## 构建和刷写

在 ESP-IDF 6.0.2 环境中：

```powershell
Set-Location .\firmware\dual_proxy
. ..\..\scripts\Enter-EspIdf.ps1
idf.py set-target esp32s3
idf.py build
```

刷写仍使用项目现有工具选择本工程 `build/flasher_args.json`。本任务只构建，不自动刷写硬件。

## 不启动 EXE 的鼠标侧 UART0 测试

鼠标侧 CH340 COM 独占打开后运行：

```powershell
python .\tools\test_dual_proxy_serial.py --port COMx --cursor
```

脚本默认先发送 `DeviceProbe`，只有收到签名 `HIDBRDG2`、nonce 匹配且角色为 `MOUSE_HOST` 的 `DeviceHello` 才继续；随后发送 `SessionStart`、小幅 `+4`、等待、等量 `-4`、`Ping`，不发送点击、键盘或 `SendInput`。UART0 会与 ESP_LOG 混流，解析器会跳过非协议字节；同一 COM 不能同时被主机 EXE 或其他工具打开。

真实鼠标报告、单一 HID 设备、实体移动与软件移动叠加仍必须在实际刷写和受控输入下验收；本工程构建通过不等于硬件通过。
