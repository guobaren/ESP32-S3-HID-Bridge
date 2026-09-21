# 主机输入协议（UART / USB CDC）

## 帧格式

所有多字节整数采用小端序。

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 2 | Magic：`A5 5A` |
| 2 | 1 | 协议版本：`02` |
| 3 | 1 | 消息类型 |
| 4 | 2 | 序号 |
| 6 | 1 | Payload 长度 |
| 7 | N | Payload |
| 7+N | 2 | CRC16-CCITT |

CRC 覆盖从 `版本` 到 `Payload` 的全部字节，初值 `0xFFFF`，多项式 `0x1021`。

## 消息类型

| 值 | 名称 | Payload |
|---:|---|---|
| `0x01` | KeyboardReport | 标准 8 字节 Boot Keyboard Report |
| `0x02` | MouseReport | Host 当前发送 8 字节：前 7 字节为 `buttons`、小端有符号 16 位 `x/y`、有符号 8 位 `wheel/pan`，第 8 字节为固件平滑槽数 `0` 或 `5`；固件继续兼容旧 7 字节直通报告 |
| `0x03` | ReleaseAll | 空 |
| `0x04` | Ping | 空；维持当前输入租约 |
| `0x05` | SessionStart | 空；申请新的输入租约，并先释放旧会话的全部输入 |
| `0x06` | DeviceProbe | 8 字节随机数；仅用于 COM 输入通道自动发现，不申请输入租约 |
| `0x07` | DeviceHello | `HIDBRDG2` ASCII 标识加原样返回的 8 字节随机数；序号与 DeviceProbe 相同 |

主机在每次 UART、原生 USB CDC 或 Wi-Fi 连接建立后先发送 `SessionStart`，之后至少每 500 ms 发送一次 `Ping`。固件默认在 1500 ms 内未收到当前会话的有效帧时执行 `ReleaseAll`，避免断线卡键。

`portName` 为 `auto` 时，主机依次打开当前可用 COM 口并发送 `DeviceProbe`。只有收到 CRC、序号、固定标识和随机数均匹配的 `DeviceHello` 后，才把该 COM 口认定为 HID Bridge。启动日志等非协议字节会被跳过。

### 双板内部动态 HID Profile（运行时观察阶段；尚未重枚举）

双板 `dual_proxy` 的 UART1 复用同一帧封装，并预留以下内部消息；它们不属于主机 CDC 控制 API：

| Type | 名称 | Payload |
|---:|---|---|
| `0x24` | PROFILE_BEGIN | `transfer_id:u32`、`total_length:u32`、`crc32:u32` |
| `0x25` | PROFILE_CHUNK | `transfer_id:u32`、`offset:u32`、`data:1..56 bytes` |
| `0x26` | PROFILE_COMMIT | `transfer_id:u32`、`total_length:u32`、`crc32:u32` |

Profile blob v2 的固定 20 字节头依次为 `magic:u32`（`HIDP`）、`version:u16`、`header_length:u16`、Device descriptor 长度、Configuration descriptor 长度、manufacturer/product/serial UTF-8 长度、报告项数量和 `flags:u8`；随后按长度排列各段数据。每个报告项为 `interface_number:u8`、`subclass:u8`、`protocol:u8`、保留字节、`report_length:u16` 和原始 HID Report descriptor。所有整数均为小端，完整 blob 上限 4096 字节，最多 8 个接口、单份报告描述符 512 字节、每个字符串 128 字节。

接收端要求 BEGIN 合法、CHUNK 的 transfer ID 正确且 offset 严格连续，COMMIT 的总长度/CRC32 与 BEGIN 一致，并在 CRC32 和完整反序列化成功后才发布；乱序、重复、越界、错误 CRC 或新 BEGIN 会丢弃当前未完成传输，但保留上一份有效 Profile。运行时鼠标侧只通过公开 Host API 提供 VID/PID、字符串和每接口报告描述符；raw Device/Configuration descriptor 不可得时，`flags` 明确标记 partial/synthetic，不能把规范化占位称为原始描述符。

当前阶段已经接入 Host 采集、UART1 有界公平串流和 PC 侧观察接收，但尚未把 Profile 连接到 USB Device 动态重枚举、VID/PID 克隆或 Logitech 驱动识别；这些结果不能宣称驱动识别已经完成。
主机同步程序打开 UART 或原生 USB CDC 对应的 COM 口后，会由同一个 `SerialPort` 实例读取设备日志并缓冲写入 `deviceLogPath` 指定的文件（默认是 EXE 同目录 `log/device/host-serial-{timestamp}.log`），并按 `deviceLogRetentionCount` 清理最旧文件，因此不需要、也不能再同时运行 `idf.py monitor` 独占同一个 COM 口。为避免高频 BLE notify 日志重复触发主机日志落盘和 WinForms 重绘，`showDeviceLogInUi` 默认关闭；该选项只影响窗口镜像，不影响独立设备日志文件。

UART 与原生 USB CDC 都直接承载上述帧；两者使用相同的字节流解析、设备发现和输入租约。Wi-Fi TCP 通道先用预共享密钥进行双向挑战认证，再使用 AES-256-GCM、单调包计数器和会话随机数保护每个完整帧；计数器不连续或认证标签错误时立即断开连接。

固件启动后的 1500 ms 为原生 USB profile 检测窗口：收到任何 CRC 正确的 UART 协议帧时选择键盘与相对触摸板 HID-only；未收到时先选择 CDC-only。若 CDC-only 启动后 UART 才收到首个有效协议帧，固件会执行 `ReleaseAll` 并自动重启一次；控制端持续探测会在新的启动窗口内命中，使原生 USB 重新枚举为 HID-only。已经处于 HID-only 时不会重复重启。选择只影响原生 USB 描述符，不改变本协议帧格式。

解析器遇到错误 Magic、过长 Payload 或 CRC 错误时丢弃当前候选帧，并继续寻找下一个 `A5 5A`。

协议 v2 不兼容旧的 5 字节鼠标报告。主机、固件与 Target Agent 必须使用同一版本。


## 主机局域网模拟鼠标 UDP 接口

该接口是 Windows 主机 EXE 的输入适配层，不改变电脑到 ESP32 的串口/Wi-Fi 帧格式。默认监听 `0.0.0.0:24814`；可用 `bridge.local.json` 的 `remoteInputBindAddress` 和 `remoteInputPort` 覆盖。主界面左上角显示当前可用的局域网监听 IP 和端口。该 UDP 接口不做身份认证。

每个 UDP 数据报必须是一个 UTF-8 JSON 对象：

```json
{"dx":12,"dy":-4,"wheel":0,"pan":0}
```

- `dx`、`dy`：有符号 16 位相对位移。
- `wheel`、`pan`：有符号 8 位滚轮增量。
- 四个增量不能全为零；未知字段会被忽略，超范围或无效 JSON 的数据报直接拒绝。
- 命令在主机 `HOME` 同步开启，或“始终开启 UDP 输出”开关开启时进入 `MouseReportPump`。Host 以最高 500 Hz / 2 ms 聚合完整位移，不再在 Windows 用户态展开平滑槽。
- 默认开启“UDP 平滑”时，桥接报告第 8 字节为 `5`。原生 USB 固件将 X/Y/Wheel/Pan 分别按整数商和余数分摊到滚动的 5 个 1 ms 槽；新命令叠加到现有未来槽，不串行追加，因此停止输入后的计划尾部不超过 5 ms且每条命令代数和严格守恒。
- 关闭“UDP 平滑”后，第 8 字节为 `0`；固件先把已有 5 槽余量合并到当前槽，再加入新位移，下一次 USB 1 ms 周期直接输出。主界面开关只能在 `HOME` 同步关闭时切换；kmboxNet `trace` 可在线切换。同步关闭路径仍发送 `ReleaseAll` 并清空固件槽。
- BLE 固件接受 7/8 字节桥接报告但忽略第 8 字节，仍以 10 ms 节拍合并位移；只有原生 USB 路径执行 5 槽、1000 Hz 消费。
- 监听停止、程序退出或关闭“始终开启 UDP 输出”时不保留远端待发送状态，并继续走现有 `ReleaseAll` 清理路径；仅按 HOME 关闭同步不会阻断该开关允许的后续 UDP 输入。

### Python 调用示范

项目内 `tools/esp32_move.py` 提供 `Esp32MouseSender`：

```python
from tools.esp32_move import Esp32MouseSender

sender = Esp32MouseSender("192.168.1.20", 24814)
try:
    sender.move(25, -10, wheel=0, pan=0)
finally:
    sender.close()
```

仓库中的 `tools/send-remote-mouse-sample.py` 使用项目内调用库，目标 IP、端口和正方形参数直接
写在脚本顶部，不读取命令行或其他外部输入。默认按顺时针分四条边发送边长 `100`、步长
`100` 的正方形；实际发送模式、本机 Host 接收、ESP32 接收和目标 HID 行为需要在具备目标
设备时单独验证。

### kmboxNet 二进制兼容

同一 UDP 端口也接受原版 kmboxNet pyd/C++ 库的数据包。调用侧继续使用 `kmNet.init()`、`move()`、`left()`、`keydown()` 等原接口，仅把目标 IP/端口改为 Host 左上角显示的地址。

- 明文头固定为 16 字节小端序 `mac/rand/index/cmd`；普通鼠标包为 72 字节，键盘包为 28 字节。
- `init` 必须先发送明文连接包。Host 记录来源端点和 MAC，之后返回包含相同 `mac/rand/index/cmd` 的 16 字节 ACK。
- `enc_*` 使用原库的 128 字节 XXTEA 风格加密包；解密密钥由先前明文 `init` 的 MAC 派生。
- `move`、`move_auto`、贝塞尔移动及加密版本都忽略盒子轨迹参数，统一取包内最终 X/Y/Wheel 增量进入现有 move 链路。
- 鼠标包和键盘包携带完整状态；Host 对按下/松开状态进行替换，而不是把重复包当作新的边沿。
- `trace` 的低 24 位 `value > 0` 时开启当前固件固定 5 槽 UDP 平滑，否则关闭；不复刻原盒子的四种曲线算法。
- `monitor(port)` 只登记回传端口，不改变 Host 捕获开关。实体输入变化时 Host 按原结构回传 9 字节鼠标报告和 12 字节键盘报告，供 pyd 的 `isdown_*` 本地查询。
- `mask_*`、`unmask_keyboard` 和 `unmask_all` 按原位图过滤实体鼠标按钮、X/Y、滚轮和指定 HID 键；monitor 回传仍保留过滤前状态。
- `reboot`、`setconfig`、`setvidpid` 和 LCD 命令不应答，也不改变 Host 或 ESP32 配置。

主机设置页的“模拟 UDP 输入（测试）”模式不会改变上述网络数据报格式，也不会向本机 UDP socket 发送回环数据报，默认关闭。它先按所选 `30/60/100/140/200/500 Hz` 最大频率整合实体鼠标移动和滚轮；选择“无上限”时，每个原始事件直接形成一条内部模拟 UDP 命令。结果提交到与网络命令相同的公共 UDP 后续入口，随后由 Host 500 Hz 聚合并由固件按 0/5 槽处理。按钮转换仍保持顺序。
