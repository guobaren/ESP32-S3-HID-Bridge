# 主机输入协议（UART / USB CDC）

> **适用范围**：本文的「帧格式 / 消息类型 / 双板透明代理内部协议 / 有界重试与去重规则 / 板载日志下载命令」适用于当前双板主线 `firmware/dual_proxy`（板载日志容量见下）。
> 「主机局域网模拟鼠标 UDP 接口」整章属于**主机 EXE + 早期单板固件**形态（UDP/kmboxNet 远程输入），双板主线不涉及，保留供该产品线参考。
> 标注为"早期单板"的段落（原生 USB CDC 探测窗口、BLE 节拍、Wi-Fi TCP）只适用于 `firmware/` 单板工程；双板克隆不开 CDC、无 Wi-Fi/BLE。

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
| `0x07` | DeviceHello | `HIDBRDG2` ASCII 标识、原样返回的 8 字节随机数，以及可选的角色字节（`1=PC_DEVICE`、`2=MOUSE_HOST`）；序号与 DeviceProbe 相同 |

主机在每次 UART、原生 USB CDC 或 Wi-Fi 连接建立后先发送 `SessionStart`，之后至少每 500 ms 发送一次 `Ping`。固件默认在 1500 ms 内未收到当前会话的有效帧时执行 `ReleaseAll`，避免断线卡键。

`portName` 为 `auto` 时，主机依次打开当前可用 COM 口并发送 `DeviceProbe`。`dual_proxy` 正式拓扑只接受角色字节为 `MOUSE_HOST` 的响应，避免误连电脑侧板的开发串口；旧固件没有角色字节时只用于兼容诊断脚本。启动日志等非协议字节会被跳过。

### 双板透明代理内部协议

双板 `dual_proxy` 的 UART1 复用同一帧封装，并使用以下内部消息；它们不属于主机 CDC 控制 API：

| Type | 名称 | Payload |
|---:|---|---|
| `0x20` | LINK_HELLO | `role:u8`、`usb_state:u8`、`node_id:6 bytes`、`generation:u32` |
| `0x21` | PHYSICAL_MOUSE | `interface:u8`、`report_id:u8`、`buttons:u8`、`x/y:i16`、`wheel/pan:i8` |
| `0x22` | PHYSICAL_RELEASE | `reason:u8` |
| `0x23` | LINK_PING | `generation:u32` |
| `0x24` | PROFILE_BEGIN | `transfer_id:u32`、`total_length:u32`、`crc32:u32` |
| `0x25` | PROFILE_CHUNK | `transfer_id:u32`、`offset:u32`、`data:1..56 bytes` |
| `0x26` | PROFILE_COMMIT | `transfer_id:u32`、`total_length:u32`、`crc32:u32` |
| `0x27` | RAW_HID_INPUT | 原始 interface、Report ID 和 Input Report 正文 |
| `0x28` | HID_SET_REPORT | 事务 ID、interface、Report ID/type 和原始正文 |
| `0x29` | HID_GET_REPORT_REQUEST | 事务 ID、interface、Report ID/type 和请求长度 |
| `0x2A` | HID_GET_REPORT_RESPONSE | 事务 ID、状态、interface、Report ID 和响应正文 |
| `0x2B` | SOFTWARE_MOUSE | 来自电脑 A 的 7/8 字节标准化软件鼠标报告 |
| `0x2C` | SOFTWARE_RELEASE | 软件输入租约结束、断线或队列故障时释放软件按键 |
| `0x2D` | DEVICE_GONE | `sender_generation:u32`、`target_generation:u32`、非零 `event_id:u32`、`reason:u8`；长度 13 字节 |
| `0x2E` | PROFILE_ACK | `transfer_id:u32`、`crc32:u32`、`status:u8`、`recipient_generation:u32`、`sender_generation:u32`；长度 17 字节，电脑侧在新 Profile 成功配置并 `tud_mounted()` 后回 ACK，`status=1` 表示不可克隆 |
| `0x2F` | ROLE_ACK | `role:u8`、`generation:u32`；确认收到并接受对应身份声明 |
| `0x30` | PROFILE_REQUEST | `generation:u32`、`flow_id:u32`；P 发起一次重新采集申请 |
| `0x31` | PROFILE_OFFER | `flow_id:u32`、`transfer_id:u32`、`crc32:u32`；M 已采集到新 Profile，提议 P 清空旧会话并重新克隆 |
| `0x32` | FLOW_ACK | `acknowledged_type:u8`、`flow_id:u32`、`status:u8`、`recipient_generation:u32`、`sender_generation:u32`；长度 14 字节，确认帧绑定双方会话 |

一次性申请和提议都要求对端先通过 `LINK_HELLO` 声明互补角色；仅有 UART 字节活动或 `UNRESOLVED` 声明不足以触发申请。

`LINK_HELLO.role` 使用 `0=UNRESOLVED`、`1=PC_DEVICE`、`2=MOUSE_HOST`；`usb_state` 使用 `0=WAITING`、`1=MOUNTED`、`2=HID_CONNECTED`、`3=DISCONNECTED`、`4=ERROR`。双板启动后先通过 UART1 广播未定身份，并各自在 USB Device/Host 间轮换探测，每个角色探测窗口为 3 秒。Device 检测到 USB 主机 attach 或配置完成后确认 `PC_DEVICE`；Host 成功启动至少一个物理 HID 接口后确认 `MOUSE_HOST`。未确认身份时 LED 熄灭；身份确认但板间对端未在线时红色常亮；对端在线时 M 绿色、P 蓝色；本地 Profile 采集或克隆流程失败时红色闪烁。已确认身份通过周期性 `LINK_HELLO` 告知对端，以 `ROLE_ACK` 回确认；未定身份的一侧锁定互补角色。角色只保存在 RAM，每次复位或重新上电均重新从 `UNRESOLVED` 开始，单板复位可从仍在线的对端重新确定。`generation` 是每次启动生成的非零 32 位会话 ID；对端观察到变化时清空旧序号窗口和单次申请状态，不自动重放已确认的旧 Profile。

P 锁定身份后先完成本地 USB Device 卸载、清空活动 Profile/描述符/报告模板及厂商 HID 会话，成功后才开放并发送本会话唯一一次 `PROFILE_REQUEST`。M generation 变化等同 M 重新上电：P 关闭申请门，重新执行同一套本地清理，完成后再发送新的单次申请。M 收到 REQUEST 后取消尚未完成的旧提议，安排重新读取当前 USB Device/Configuration 描述符并重新发布 HID Profile，同时以 `FLOW_ACK(REQUEST, flow_id, status)` 确认是否接受。

M 每次采集到 Profile 时发起 `PROFILE_OFFER`。该 OFFER 自身也是清理请求：P 先释放输入、卸载 TinyUSB、清空旧 Profile/描述符/报告模板并使旧厂商 HID 会话失效，全部成功后才发送 `FLOW_ACK(OFFER, flow_id, accepted)`；排入异步任务不算成功。M 只有收到双方 generation 与 flow ID 都匹配的 accepted ACK 后，才发送 BEGIN/CHUNK/COMMIT。P 对完整且校验成功的 COMMIT 回 `FLOW_ACK(COMMIT, transfer_id, status)`，随后执行 USB 克隆；`tud_mounted()` 后再发最终 `PROFILE_ACK(transfer_id, crc32, status)`。

鼠标物理拔出时，M 生成非零 event ID 并发送 `DEVICE_GONE(sender_generation, target_generation, event_id, reason)`。P 只接受两个 generation 与当前会话匹配的事件，取消任何旧 pending Profile，并用操作 epoch 使已经开始但尚未完成的重配置快照失效。P 必须实际完成 TinyUSB 卸载与旧会话清空后，才回 `FLOW_ACK(DEVICE_GONE, event_id, accepted)`。M 在收到匹配确认前禁止发送 PROFILE_OFFER 和任何 Profile 分片；即使新鼠标已采集到 Profile，也只缓存等待。鼠标重新插入后，顺序为：GONE 清理 ACK → 新 OFFER 清理 ACK（即便清理是幂等的也要执行并确认）→ BEGIN/CHUNK/COMMIT → P 挂载后的最终 PROFILE_ACK。重复/迟到的 GONE 按 peer generation 与 event ID 去重，不得撤销更新会话中的新克隆。清理失败或确认超时按“有界重试与去重规则”处理：先在同一 generation 内重试同一事务，预算耗尽才闪红灯终止本轮；新 peer generation/板复位开启新的恢复流程。M 侧报告的输入异常（`on_mouse_release(false)`，如报告队列满或传输错误）不属于物理拔出，只释放按钮并记录输入错误，不发送 `DEVICE_GONE`。

FLOW_ACK 的字段偏移为：`acknowledged_type@0`、`flow_id@1`、`status@5`、`recipient_generation@6`、`sender_generation@10`。PROFILE_ACK 的字段偏移为：`transfer_id@0`、`crc32@4`、`status@8`、`recipient_generation@9`、`sender_generation@13`。接收方必须同时验证本地 generation、对端 generation 和 flow/event/transfer ID，旧会话或错配 ACK 不得解锁流程；M 只有收到三者全部匹配的成功 `PROFILE_ACK` 才把自己的 `usb_state` 更新为 `HID_CONNECTED`。`LINK_HELLO` 为 12 bytes，`LINK_PING` 为 4 bytes；PROFILE_ACK 由 9 bytes 变为 17 bytes，两板必须同时刷入本协议版本（帧头版本字节与 Host 协议共用，仍为 `02`，因此长度本身就是新旧固件的判别条件）。上述均为 UART1 双板内部消息，不改变 Host C# 所用 UART/USB CDC 输入协议及其帧格式。

### 有界重试与去重规则

本轮把“一次超时即永久失败”改为有界事务恢复，参数集中在 `firmware/dual_proxy/main/link_recovery_logic.h`（保守初值，待实机标定）：

- 事务身份为 `(双方 generation, 鼠标连接代号, event/flow/transfer ID, 本地 epoch)`；重试只重放同一事务，不创建新 ID；同一时刻只允许一个清理屏障和一个 Profile 传输。
- `DEVICE_GONE` 按 0.7 秒间隔、最多 8 次重发同一 `event_id`；P 若报告清理失败，M 保留同一事务继续重试。等待窗口 5 秒或次数用尽才判失败，但双方 generation 与 event ID 都匹配的迟到确认仍然有效。快速插回时新 Profile 只缓存，必须等旧屏障完成才能 OFFER。
- P 清理失败保持克隆门关闭并本地重试同一事务最多 3 次；清理成功后立即登记结果，只有确认帧入队失败时最多补发 3 次，不重复卸载 USB、不清输入、不递增 epoch。
- 重复 `PROFILE_REQUEST`/`PROFILE_OFFER` 按 generation + flow ID 去重；物理鼠标尚未枚举时 M 受理并等待，设备到达后继续当前流程。
- 接收端对重复到达的前缀分片幂等（内容一致才忽略，冲突则失败）；同一 `transfer_id` 的重复 BEGIN/CHUNK/COMMIT 返回既有结果，不重新发布，因此不会第二次重枚举 USB。最终 `PROFILE_ACK` 丢失时 M 只重放同一 transfer 的 COMMIT，P 只补发确认。
- 恢复总预算 10 秒，各阶段共用剩余预算，不串联多个完整等待窗口；预算耗尽后明确终止本轮，新物理事件或新会话可重新发起。
- 当前 `FLOW_ACK` 的**阶段超时为 5 秒**（`LINK_PROFILE_STAGE_TIMEOUT_US`，即 `LINK_FLOW_ACK_TIMEOUT_US`），占 10 秒共享预算的一半；原计划恢复为 1 秒尚未执行。控制传输（EP0）另有一套独立参数：单次等待 800 ms、每请求最多 2 次尝试、连续 3 次失败升级恢复，枚举后 10 秒预热期内只重试不升级（见 `docs/连接流程.md` §12）。

Profile blob v2 的固定 20 字节头依次为 `magic:u32`（`HIDP`）、`version:u16`、`header_length:u16`、Device descriptor 长度、Configuration descriptor 长度、manufacturer/product/serial UTF-8 长度、报告项数量和 `flags:u8`；随后按长度排列各段数据。每个报告项为 `interface_number:u8`、`subclass:u8`、`protocol:u8`、保留字节、`report_length:u16` 和原始 HID Report descriptor。所有整数均为小端，完整 blob 上限 4096 字节，最多 8 个接口、单份报告描述符 512 字节、每个字符串 128 字节。

接收端要求 BEGIN 合法、CHUNK 的 transfer ID 正确且 offset 严格连续，COMMIT 的总长度/CRC32 与 BEGIN 一致，并在 CRC32 和完整反序列化成功后才发布。电脑侧每收到完整 Profile 都必须覆盖旧 Profile 并重新插拔电脑 USB：先使旧 vendor HID 会话代号失效、清空输入/控制队列和待处理 GET_REPORT，卸载旧 TinyUSB 设备并清空活动 Profile/报告模板；USB 断开后才构建并安装新描述符，避免枚举期间新旧描述符混用。无法取得完整原始描述符、CRC/顺序错误或安全克隆预算不满足时保持 USB 断开，不允许回退呈现旧 Profile 或通用设备冒充物理鼠标。

实体 raw Input 优先于软件输入；软件 move/release 使用独立有界队列；HID 厂商控制使用独立队列；Profile 只在安全/输入队列允许时分片发送。鼠标拔出、UART1 超时、鼠标侧掉电或 Profile 超时都先释放输入，再由电脑侧卸载 USB Device；重新插入后只有完整新 Profile 校验通过才重新枚举。
**（主机 EXE / 早期单板）** 主机同步程序打开 UART 或原生 USB CDC 对应的 COM 口后，会由同一个 `SerialPort` 实例读取设备日志并缓冲写入 `deviceLogPath` 指定的文件（默认是 EXE 同目录 `log/device/host-serial-{timestamp}.log`），并按 `deviceLogRetentionCount` 清理最旧文件，因此不需要、也不能再同时运行 `idf.py monitor` 独占同一个 COM 口。为避免高频 BLE notify 日志重复触发主机日志落盘和 WinForms 重绘，`showDeviceLogInUi` 默认关闭；该选项只影响窗口镜像，不影响独立设备日志文件。

**（早期单板）** UART 与原生 USB CDC 都直接承载上述帧；两者使用相同的字节流解析、设备发现和输入租约。Wi-Fi TCP 通道先用预共享密钥进行双向挑战认证，再使用 AES-256-GCM、单调包计数器和会话随机数保护每个完整帧；计数器不连续或认证标签错误时立即断开连接。双板 `dual_proxy` 不使用 Wi-Fi/BLE，也不开原生 USB CDC 控制口。

**（早期单板）** 固件启动后的 1500 ms 为原生 USB profile 检测窗口：收到任何 CRC 正确的 UART 协议帧时选择键盘与相对触摸板 HID-only；未收到时先选择 CDC-only。若 CDC-only 启动后 UART 才收到首个有效协议帧，固件会执行 `ReleaseAll` 并自动重启一次；控制端持续探测会在新的启动窗口内命中，使原生 USB 重新枚举为 HID-only。已经处于 HID-only 时不会重复重启。选择只影响原生 USB 描述符，不改变本协议帧格式。

解析器遇到错误 Magic、过长 Payload 或 CRC 错误时丢弃当前候选帧，并继续寻找下一个 `A5 5A`。

**（主机 / 早期单板）** 协议 v2 不兼容旧的 5 字节鼠标报告。主机、固件与 Target Agent 必须使用同一版本。


## 板载日志下载命令

每块板把控制台日志写入自己的 SPIFFS 分区（**4×512 KB 轮转 = 上限 2 MB**，`partitions.csv` 的 `storage` 为 4 MB；跨复位续写）。这些命令走
**板子自己的 UART0**（鼠标侧板＝主机控制口；电脑侧板＝调试口，只跑日志服务），
不占用输入租约，没建会话也能用：

| Type | 名称 | Payload |
|---:|---|---|
| `0x08` | LOG_READ_REQUEST | `offset:u32`、`max_bytes:u8`（1..56）；设备回一条 `LOG_READ_RESPONSE` |
| `0x09` | LOG_READ_RESPONSE | `offset:u32`、`total_bytes:u32`、`data:0..56`；`data` 为空表示流结束 |
| `0x0A` | LOG_CLEAR_REQUEST | 空；清空全部日志文件后回一条 `total_bytes=0` 的 `LOG_READ_RESPONSE` |
| `0x0B` | LOG_DUMP_REQUEST | `offset:u32`、`max_bytes:u32`；设备**连续**回多条 `LOG_READ_RESPONSE` 后以空 `data` 帧收尾 |

逻辑字节流按「最旧 → 最新」排列，`offset` 从 0 开始；`total_bytes` 是板端当前可读的
总字节数（**上限 2 MB**）。客户端的推荐做法是发一条 `LOG_DUMP_REQUEST`，然后流式解析
响应帧直到遇到空 `data` 帧。

解析注意：日志文本与协议帧共用同一个串口，客户端必须**在混杂文本中扫描帧**（找
`A5 5A`、校验长度与 CRC16，失败则后移一字节继续）。命令本身不受会话约束，也不会
更新帧序号窗口。

主机 EXE 侧的入口是鼠标捕获页日志栏右上角的**「转存板载日志」**按钮：它取当前已连接的
串口，发一条 `LOG_DUMP_REQUEST{offset=0, max_bytes=1 MB}`，把收到的分片按 UTF-8 追加写入
`log/device/onboard-log-yyyyMMdd-HHmmss.log`（默认目录，SaveFileDialog 可改）。转存期间
串口被独占借出、键鼠同步先释放全部按键并暂停，结束后自动恢复控制连接；`host/HidBridge.Host/Transport/OnboardLogDownloader.cs`
是该实现，打开串口时显式保持 DTR/RTS 为低，不会复位被测板。命令行/脚本路径见 `tools/fetch_onboard_log.py`。

## 主机局域网模拟鼠标 UDP 接口（主机 EXE / 早期单板，非双板主线）

该接口是 Windows 主机 EXE 的输入适配层，不改变电脑到 ESP32 的串口/Wi-Fi 帧格式。默认监听 `0.0.0.0:24814`；可用 `bridge.local.json` 的 `remoteInputBindAddress` 和 `remoteInputPort` 覆盖。主界面左上角显示当前可用的局域网监听 IP 和端口。该 UDP 接口不做身份认证。

每个 UDP 数据报必须是一个 UTF-8 JSON 对象：

```json
{"dx":12,"dy":-4,"wheel":0,"pan":0}
```

- `dx`、`dy`：有符号 16 位相对位移。
- `wheel`、`pan`：有符号 8 位滚轮增量。
- 四个增量不能全为零；未知字段会被忽略，超范围或无效 JSON 的数据报直接拒绝。
- 命令在主机 `HOME` 同步开启，或“始终开启 UDP 输出”开关开启时进入 `MouseReportPump`。Host 以最高 500 Hz / 2 ms 聚合完整位移，不再在 Windows 用户态展开平滑槽。
- 默认开启“UDP 平滑”时，桥接报告第 8 字节为 `5`。Host 以最高 500 Hz 合并后，该字节会经鼠标侧 UART0 和板间 `SOFTWARE_MOUSE` 原样传到 PC 侧板。PC 侧板将 X/Y/Wheel/Pan 分别按整数商和余数分摊到滚动的 5 个 1 ms 槽，由 1000 Hz USB HID 发送任务消费；新命令叠加到现有未来槽，不串行追加，因此停止输入后的计划尾部不超过 5 ms，且每条命令代数和严格守恒。实体鼠标 report 不进入此平滑器。
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
