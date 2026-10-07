# 主机输入协议（UART / USB CDC）

> **适用范围**：本文的「帧格式 / 消息类型 / 双板透明代理内部协议 / 有界重试与去重规则 / 双板内存统计快照」适用于当前双板主线 `firmware/dual_proxy`。
> 主机局域网 UDP/kmboxNet 接口由 Windows Host 接收；Host 可运行于旧版单板或双板模式，实际输出条件和路由见本章对应说明。
> 标注为"早期单板"的段落（原生 USB CDC 探测窗口、BLE 节拍、Wi-Fi TCP）只适用于 `firmware/` 单板工程；双板克隆不开 CDC、无 Wi-Fi/BLE。

## 主机 loopback 串口命令 API

主机 EXE 在现有 `FirmwareUpdateApiServer` HTTP 端口上提供四个仅限 loopback 的串口接口（当前配置端口为 `24815`）。串口 API 随主串口模式启动，即使设置页关闭“局域网固件刷写 API”仍可使用；该开关只关闭固件状态/刷写路由。旧构造方式或无串口桥接的模拟模式不提供串口路由。

| 方法与路径 | 行为 |
|---|---|
| `GET /api/v1/serial/ports` | 返回 `{"ports":["COM12","COM3"]}`；只列出 EXE 当前持有的主控制串口和对端串口镜像 |
| `POST /api/v1/serial/write` | 将 JSON 中的十六进制字节同步写到指定的已打开端口 |
| `POST /api/v1/serial/refresh` | 不带请求体；重新枚举可用串口并立即尝试接管未监听的对端镜像口，返回 `availablePorts` 与 `openPorts` |
| `GET /api/v1/serial/stats` | 不带请求体；向所有当前已打开串口各发一次统计快照请求，收齐这些端口的完整分页后返回 `readAt`、`boards` |

镜像启动后先等待主串口连接，再每 2 秒重试发现晚到的对端口；读线程发现串口断开后会释放镜像句柄并继续重试。每轮最多探测 3 个候选：在 DTR/RTS 低电平下发送既有 `DeviceProbe` 随机挑战，并验证回复签名、挑战和 PC 角色，不依赖设备周期日志。发现/刷新只操作镜像串口，不会关闭或重开健康的主串口。固件刷写等独占租约期间自动暂停发现并让出镜像口，租约释放后先让主串口恢复，再继续发现。此 API 的 `refresh` 会尝试启动镜像监听；设置页“刷新端口”按钮只刷新刷写下拉列表，两者用途不同。

写入请求示例：`{"portName":"COM12","hex":"<完整帧的十六进制字节>"}`。`hex` 必须是非空、偶数长度的 ASCII 十六进制字符串，单次最多 4096 字节；不接受空格、分隔符或奇数位。命令应包含完整 UART0 帧：帧头、版本、类型、序号、Payload 长度、Payload 和 CRC16，不能只发送 Payload。帧字段见下方「帧格式」。

成功响应包含 `portName`、`bytesWritten` 和 `queued:false`，只在同步串口写入返回后发送。参数错误返回 400，数据超限返回 413，EXE 未持有该端口返回 409，写入失败返回 500 或 504。两个串口接口只接受 `127.0.0.1`、`::1` 或等价 loopback 地址；同一 HTTP 端口上的既有固件接口保持原访问策略。

API 复用 EXE 已打开的串口，不打开、关闭或重连端口，也不切换 DTR/RTS，因此不会通过串口打开动作触发板子复位。主串口的注入写入与 `WriterLoop` 共用 `_writeSync`，完整写操作不会与鼠标帧字节交错。正在写出的完整批次会先释放写锁；已经从队列取出但尚未取得写锁的鼠标批次与注入命令按锁的取得顺序发送，所以先后次序可能不同于鼠标帧入队顺序。响应只确认主机串口 `Write` 已返回，不代表固件已接受或处理该 UART0 帧。写入受串口驱动的写超时限制，HTTP 请求超时前会再次检查取消状态，不会把待写命令留在队列中。

统计读取复用 EXE 已打开的串口，不重开端口、不切换 DTR/RTS、不暂停输入。统计请求序号独立于输入会话序号；一次查询不会更改输入帧去重状态。它对每个当前已打开端口发送查询，并要求每个端口的计数器页和队列页均完整且分页一致；缺页、重复页或格式不符会使整次查询失败。`boards` 只包含本次已打开并成功响应的端口：只有主串口打开时返回一块板，主串口和镜像串口都打开且均响应时才会包含 M/P 两板。`readAt` 是主机采集时间，板端 `uptimeMilliseconds` 表示该快照所在启动周期；计数器是自本次板端启动以来累计，不跨重启保存。

主控制串口的设备日志继续写入现有 `DeviceLogPath` 文件。对端日志镜像另外创建同目录的 `host-serial-COMx-年月日-时分秒-毫秒.log` 文件（例如 `host-serial-COM3-20260929-043012-125.log`），每行写完立即刷新；只记录当前镜像日志策略保留的 ESP-IDF 行。固件刷写让出镜像串口时关闭当前镜像日志文件，恢复监听后为对应 COM 口新建文件。日志文件创建或写入失败只影响该路日志保存，不停止镜像串口读取或串口命令写入。

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
| `0x02` | MouseReport | Host 当前发送 8 字节：前 7 字节为 `buttons`、小端有符号 16 位 `x/y`、有符号 8 位 `wheel/pan`，第 8 字节为 M 端 UDP 平滑槽数 `0/5/10/15/20`；兼容旧 7 字节直通报告 |
| `0x03` | ReleaseAll | 空 |
| `0x04` | Ping | 空；维持当前输入租约 |
| `0x05` | SessionStart | 空；申请新的输入租约，并先释放旧会话的全部输入 |
| `0x06` | DeviceProbe | 8 字节随机数；仅用于 COM 输入通道自动发现，不申请输入租约 |
| `0x07` | DeviceHello | `HIDBRDG2` ASCII 标识、原样返回的 8 字节随机数，以及可选的角色字节（`1=PC_DEVICE`、`2=MOUSE_HOST`）；序号与 DeviceProbe 相同 |

主机在每次 UART、原生 USB CDC 或 Wi-Fi 连接建立后先发送 `SessionStart`，之后至少每 500 ms 发送一次 `Ping`。固件默认在 1500 ms 内未收到当前会话的有效帧时执行 `ReleaseAll`，避免断线卡键。

`portName` 为 `auto` 时，主机依次打开当前可用 COM 口并发送 `DeviceProbe`。`dual_proxy` 正式拓扑只接受角色字节为 `MOUSE_HOST` 的响应，避免误连电脑侧板的开发串口；旧固件没有角色字节时只用于兼容诊断脚本。启动日志等非协议字节会被跳过。

### 双板透明代理内部协议

双板 `dual_proxy` 的 UART1 复用同一帧封装，并使用以下内部消息；它们不属于主机 CDC 控制 API：

UART0 诊断另有 `0x1C DIAG_PROFILE_REFRESH_REQUEST`（可选 1 字节载荷），只由 M 角色受理：重新采集物理鼠标 Profile 并提议给 P，回现有 `DIAG_INJECT_RESULT`。这用于按需复现 Profile 流程，不会直接复位或重枚举物理鼠标。

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
| `0x2E` | PROFILE_ACK | `transfer_id:u32`、`crc32:u32`、`status:u8`、`recipient_generation:u32`、`sender_generation:u32`；长度 17 字节，电脑侧仅在同一 transfer/CRC 已安装且 `tud_mounted()` 后回 `status=0`；最终安装或挂载失败回 `status=1` |
| `0x2F` | ROLE_ACK | `role:u8`、`generation:u32`；确认收到并接受对应身份声明 |
| `0x30` | PROFILE_REQUEST | `generation:u32`、`flow_id:u32`；P 发起一次重新采集申请 |
| `0x31` | PROFILE_OFFER | `flow_id:u32`、`transfer_id:u32`、`crc32:u32`；M 已采集到新 Profile，提议 P 清空旧会话并重新克隆 |
| `0x32` | FLOW_ACK | `acknowledged_type:u8`、`flow_id:u32`、`status:u8`、`recipient_generation:u32`、`sender_generation:u32`；长度 14 字节，确认帧绑定双方会话 |

一次性申请和提议都要求对端先通过 `LINK_HELLO` 声明互补角色；仅有 UART 字节活动或 `UNRESOLVED` 声明不足以触发申请。

`LINK_HELLO.role` 使用 `0=UNRESOLVED`、`1=PC_DEVICE`、`2=MOUSE_HOST`；`usb_state` 使用 `0=WAITING`、`1=MOUNTED`、`2=HID_CONNECTED`、`3=DISCONNECTED`、`4=ERROR`。双板启动后先通过 UART1 广播未定身份，并各自在 USB Device/Host 间轮换探测，每个角色探测窗口为 3 秒。Device 检测到 USB 主机 attach 或配置完成后确认 `PC_DEVICE`；Host 成功启动至少一个物理 HID 接口后确认 `MOUSE_HOST`。未确认身份时 LED 熄灭；身份确认但板间对端未在线时红色常亮；对端在线时 M 绿色、P 蓝色；本地 Profile 采集或克隆流程失败时红色闪烁。已确认身份通过周期性 `LINK_HELLO` 告知对端，以 `ROLE_ACK` 回确认；未定身份的一侧锁定互补角色。角色只保存在 RAM，每次复位或重新上电均重新从 `UNRESOLVED` 开始，单板复位可从仍在线的对端重新确定。`generation` 是每次启动生成的非零 32 位会话 ID；对端观察到变化时清空旧序号窗口和单次申请状态，不自动重放已确认的旧 Profile。

P 锁定身份后先完成本地 USB Device 卸载、清空活动 Profile/描述符/报告模板及厂商 HID 会话，成功后才开放并发送本会话唯一一次 `PROFILE_REQUEST`。M generation 变化等同 M 重新上电：P 关闭申请门，重新执行同一套本地清理，完成后再发送新的单次申请。M 收到 REQUEST 后取消尚未完成的旧提议，安排重新读取当前 USB Device/Configuration 描述符并重新发布 HID Profile，同时以 `FLOW_ACK(REQUEST, flow_id, status)` 确认是否接受。

M 每次采集到 Profile 时发起 `PROFILE_OFFER`。该 OFFER 自身也是清理请求：P 先释放输入、卸载 TinyUSB、清空旧 Profile/描述符/报告模板并使旧厂商 HID 会话失效，全部成功后才发送 `FLOW_ACK(OFFER, flow_id, accepted)`；排入异步任务不算成功。M 只有收到双方 generation 与 flow ID 都匹配的 accepted ACK 后，才发送 BEGIN/CHUNK/COMMIT。P 对完整且校验成功的 COMMIT 回 `FLOW_ACK(COMMIT, transfer_id, accepted)` 表示数据已接收并进入安装流程；这不是 USB 挂载成功。P 对挂载超时最多进行两次、每次最多等待 3 秒的安装尝试，中间等待 200 ms；其它安装错误直接终止并回 `PROFILE_ACK(status=1)`。只有相同 transfer/CRC 实际安装并 `tud_mounted()` 后，P 才回最终 `PROFILE_ACK(status=0)`。

鼠标物理拔出时，M 生成非零 event ID 并发送 `DEVICE_GONE(sender_generation, target_generation, event_id, reason)`。P 只接受两个 generation 与当前会话匹配的事件，取消任何旧 pending Profile，并用操作 epoch 使已经开始但尚未完成的重配置快照失效。P 必须实际完成 TinyUSB 卸载与旧会话清空后，才回 `FLOW_ACK(DEVICE_GONE, event_id, accepted)`。M 在收到匹配确认前禁止发送 PROFILE_OFFER 和任何 Profile 分片；即使新鼠标已采集到 Profile，也只缓存等待。鼠标重新插入后，顺序为：GONE 清理 ACK → 新 OFFER 清理 ACK（即便清理是幂等的也要执行并确认）→ BEGIN/CHUNK/COMMIT → P 挂载后的最终 PROFILE_ACK。重复/迟到的 GONE 按 peer generation 与 event ID 去重，不得撤销更新会话中的新克隆。清理失败或确认超时按“有界重试与去重规则”处理：先在同一 generation 内重试同一事务，预算耗尽才闪红灯终止本轮；新 peer generation/板复位开启新的恢复流程。M 侧报告的输入异常（`on_mouse_release(false)`，如报告队列满或传输错误）不属于物理拔出，只释放按钮并记录输入错误，不发送 `DEVICE_GONE`。

FLOW_ACK 的字段偏移为：`acknowledged_type@0`、`flow_id@1`、`status@5`、`recipient_generation@6`、`sender_generation@10`。PROFILE_ACK 的字段偏移为：`transfer_id@0`、`crc32@4`、`status@8`、`recipient_generation@9`、`sender_generation@13`。接收方必须同时验证本地 generation、对端 generation 和 flow/event/transfer ID，旧会话或错配 ACK 不得解锁流程；M 只有收到三者全部匹配的成功 `PROFILE_ACK` 才把自己的 `usb_state` 更新为 `HID_CONNECTED`。`LINK_HELLO` 为 12 bytes，`LINK_PING` 为 4 bytes；PROFILE_ACK 由 9 bytes 变为 17 bytes，两板必须同时刷入本协议版本（帧头版本字节与 Host 协议共用，仍为 `02`，因此长度本身就是新旧固件的判别条件）。上述均为 UART1 双板内部消息，不改变 Host C# 所用 UART/USB CDC 输入协议及其帧格式。

### 有界重试与去重规则

本轮把“一次超时即永久失败”改为有界事务恢复，参数集中在 `firmware/dual_proxy/main/link_recovery_logic.h`（保守初值，待实机标定）：

- 事务身份为 `(双方 generation, 鼠标连接代号, event/flow/transfer ID, 本地 epoch)`；重试只重放同一事务，不创建新 ID；同一时刻只允许一个清理屏障和一个 Profile 传输。
- `DEVICE_GONE` 按 0.7 秒间隔、最多 8 次重发同一 `event_id`；P 若报告清理失败，M 保留同一事务继续重试。等待窗口 5 秒或次数用尽才判失败，但双方 generation 与 event ID 都匹配的迟到确认仍然有效。快速插回时新 Profile 只缓存，必须等旧屏障完成才能 OFFER。
- P 清理失败保持克隆门关闭并本地重试同一事务最多 3 次；清理成功后立即登记结果，只有确认帧入队失败时最多补发 3 次，不重复卸载 USB、不清输入、不递增 epoch。
- 重复 `PROFILE_REQUEST`/`PROFILE_OFFER` 按 generation + flow ID 去重；物理鼠标尚未枚举时 M 受理并等待，设备到达后继续当前流程。
- 接收端对重复到达的前缀分片幂等（内容一致才忽略，冲突则失败）。同一 transfer 的重复 COMMIT 在安装中只回 `FLOW_ACK` 收件回执，不启动第二个安装任务；若相同 transfer/CRC 已实际挂载则补发成功 `PROFILE_ACK`，若该 transfer 已最终失败则补发 NACK。P 端挂载超时最多尝试 2 次，每次等待最多 3 秒，间隔 200 ms；其他 USB 安装错误直接 NACK。操作 epoch/generation 失效后停止旧尝试，不为旧事务补发结果。
- M 每排入一份新 Profile（新 transfer）时刷新恢复预算；同 transfer 的 COMMIT 重放不刷新预算。恢复总预算为 14 秒，各阶段共用剩余预算；COMMIT 等待窗口为 8 秒，最多 10 次、每秒重放一次，以覆盖 P 的两次挂载等待与退避，同时保持有界。预算耗尽后明确终止本轮，新物理事件、新 Profile 或新会话可重新发起。
- 普通 `FLOW_ACK` 阶段超时为 5 秒（`LINK_PROFILE_STAGE_TIMEOUT_US`，即 `LINK_FLOW_ACK_TIMEOUT_US`）；`PROFILE_COMMIT` 单独使用 8 秒窗口（`LINK_COMMIT_STAGE_TIMEOUT_US`）。控制传输（EP0）另有一套独立参数：单次等待 800 ms、每请求最多 2 次尝试；无 reset 版记录连续失败，但默认不升级为接口重挂或根端口断电，枚举后 10 秒预热期内只重试（见 `docs/鼠标随机断连分析.md` §8）。

Profile blob v2 的固定 20 字节头依次为 `magic:u32`（`HIDP`）、`version:u16`、`header_length:u16`、Device descriptor 长度、Configuration descriptor 长度、manufacturer/product/serial UTF-8 长度、报告项数量和 `flags:u8`；随后按长度排列各段数据。每个报告项为 `interface_number:u8`、`subclass:u8`、`protocol:u8`、保留字节、`report_length:u16` 和原始 HID Report descriptor。所有整数均为小端，完整 blob 上限 4096 字节，最多 8 个接口、单份报告描述符 512 字节、每个字符串 128 字节。

接收端要求 BEGIN 合法、CHUNK 的 transfer ID 正确且 offset 严格连续，COMMIT 的总长度/CRC32 与 BEGIN 一致，并在 CRC32 和完整反序列化成功后才发布。若新 Profile CRC 与当前已安装且已挂载的克隆一致，且无在途清理或安装，P 可复用当前克隆而不重新枚举；否则先使旧 vendor HID 会话代号失效、清空输入/控制队列和待处理 GET_REPORT，卸载旧 TinyUSB 设备并清空活动 Profile/报告模板，USB 断开后才构建并安装新描述符。无法取得完整原始描述符、CRC/顺序错误或安全克隆预算不满足时保持 USB 断开，不允许回退呈现旧 Profile 或通用设备冒充物理鼠标。

实体 raw Input 优先于软件输入；软件 move/release 使用独立有界队列；HID 厂商控制使用独立队列；Profile 只在安全/输入队列允许时分片发送。鼠标拔出、UART1 超时、鼠标侧掉电或 Profile 超时都先释放输入，再由电脑侧卸载 USB Device；重新插入后只有完整新 Profile 校验通过才重新枚举。
**（主机 EXE / 早期单板）** 主机同步程序打开 UART 或原生 USB CDC 对应的 COM 口后，会由同一个 `SerialPort` 实例读取设备日志并缓冲写入 `deviceLogPath` 指定的文件（默认是 EXE 同目录 `log/device/host-serial-{timestamp}.log`），并按 `deviceLogRetentionCount` 清理最旧文件，因此不需要、也不能再同时运行 `idf.py monitor` 独占同一个 COM 口。为避免高频 BLE notify 日志重复触发主机日志落盘和 WinForms 重绘，`showDeviceLogInUi` 默认关闭；该选项只影响窗口镜像，不影响独立设备日志文件。

**（早期单板）** UART 与原生 USB CDC 都直接承载上述帧；两者使用相同的字节流解析、设备发现和输入租约。Wi-Fi TCP 通道先用预共享密钥进行双向挑战认证，再使用 AES-256-GCM、单调包计数器和会话随机数保护每个完整帧；计数器不连续或认证标签错误时立即断开连接。双板 `dual_proxy` 不使用 Wi-Fi/BLE，也不开原生 USB CDC 控制口。

**（早期单板）** 固件启动后的 1500 ms 为原生 USB profile 检测窗口：收到任何 CRC 正确的 UART 协议帧时选择键盘与相对触摸板 HID-only；未收到时先选择 CDC-only。若 CDC-only 启动后 UART 才收到首个有效协议帧，固件会执行 `ReleaseAll` 并自动重启一次；控制端持续探测会在新的启动窗口内命中，使原生 USB 重新枚举为 HID-only。已经处于 HID-only 时不会重复重启。选择只影响原生 USB 描述符，不改变本协议帧格式。

解析器遇到错误 Magic、过长 Payload 或 CRC 错误时丢弃当前候选帧，并继续寻找下一个 `A5 5A`。

**（主机 / 早期单板）** 协议 v2 不兼容旧的 5 字节鼠标报告。主机、固件与 Target Agent 必须使用同一版本。


## 双板内存统计快照（UART0）

双板固件以 `0x1D` 主动请求一次性快照，并以 `0x1E` 返回分页数据。它只读取 RAM 中的累计计数和队列状态，不清零计数、不改变队列、不暂停输入，也不依赖周期统计开关。UART0 同时承载 ESP-IDF 实时文本日志和二进制帧；客户端应在混合字节流中扫描帧头并校验 CRC16。

| Type | 名称 | Payload |
|---:|---|---|
| `0x1D` | STATS_SNAPSHOT_REQUEST | 空；在板端当前启动周期内采集一次快照 |
| `0x1E` | STATS_SNAPSHOT_RESPONSE | 版本化计数器页或队列页；帧序号回显请求序号 |

响应 Payload 最长 64 字节，前 12 字节为公共头：`schema:u8, role:u8, kind:u8, page_index:u8, page_count:u8, status:u8, item_count:u8, reserved:u8, uptime_ms:u32`。当前 `schema=1`、`reserved=0`；`role=1` 是 P/PC_DEVICE，`role=2` 是 M/MOUSE_HOST。`kind=1` 表示计数器，`kind=2` 表示队列；页序号从 0 开始，两个 kind 各自编号、分别收齐。所有页来自同一个预先采集的快照，因而同一 role 的 `uptime_ms` 应一致。查询范围是进程当前已打开的所有端口；只返回实际响应且页完整的板卡，不保证一定同时有 P 与 M。非零 `status` 表示板端无法提供完整快照，客户端应判失败。

计数器记录固定 10 字节：`id:u8, value_type:u8, value:u64`。`value_type=1` 时按无符号小端 `u64` 解码；`value_type=2` 时按有符号二补码小端 `i64` 解码（M 的 `motion_rx_dx/dy` 是有符号累计位移）。每页最多 5 条。当前 M 快照包含计数器 ID 1..36、通用 UART1 ID 49..66 和会话状态 ID 68，共 11 页；P 包含 ID 37..48、通用 UART1 ID 49..66 和 USB 状态 ID 67，共 7 页。

| ID | 计数器 |
|---:|---|
| 1..18 | M：reports、vendor_reports、input_fail、control、control_fail、ctrl_retry、urb_sub、urb_ok、urb_to、urb_retry、recover、port_cycle、wheel、ctrl_lat_max_us、slow10、slow100、vmin_gap_us、errors |
| 19..36 | M：motion_rx_dx、motion_rx_dy、motion_rx_ok、motion_rx_badlen、motion_rx_badparse、hb_ok、hb_fail、cycle_req、cycle_attempt、cycle_ok、cycle_fail、cycle_off_fail、cycle_on_fail、cycle_suppressed、input_restored、input_missing、late500、detect_max_us |
| 37..48 | P：not_mounted、not_ready、attempt、submitted、failed、complete、transfer_fail、physical_rx、vendor_rx、vendor_submitted、vendor_dropped、get_timeouts |
| 49..66 | 两板 UART1：uart1_tx、uart1_rx、uart1_rx_bytes、uart1_frame_err、uart1_rx_overflow、uart1_rx_pending_peak_bytes、uart1_heartbeat_gap_peak_ms、uart1_raw_tx_latency_peak_us、uart1_tx_write_fail、uart1_vendor_dropped、uart1_motion_dropped、uart1_profile_fail、uart1_gone_retry、uart1_gone_fail、uart1_budget_exhausted、uart_fifo_ovf_events、uart_buffer_full_events、uart_event_reset_dropped |
| 67 | P：`p_usb_state`。低位 bit0..7 依次表示 attached、installed、clone_active、mounted、reconfigure_in_progress、waiting_host、final_ack_failed、disconnect_pending；bit8..15 是最近结果码（0 unknown、1 wait_host、2 final_ack_pending、3 mounted_acked、4 install_failed、5 final_ack_failed、6 canceled）；bit16 表示等待最终 ACK；高32位是 operation_epoch。 |
| 68 | M：`m_vendor_session_state`。bit0..3 依次表示厂商会话有效、已收到首个厂商请求、peer generation 当前、Profile 等待主机；高32位是 vendor session epoch。 |

以上 ID 与 `firmware/dual_proxy/main/stats_snapshot.h` 的枚举、`host/HidBridge.Host/Transport/DeviceStatistics.cs` 的 `CounterName`、`tools/read_device_stats.py` 的 `COUNTER_NAMES` 一致。Host API 与 Python 同时提供按位解码的 `state` 对象；字段定义与角色分组以三处映射及各自采集代码为准。

队列记录固定 20 字节：`id:u8, unit:u8, capacity:u16, depth:u16, peak:u16, received:u32, rejected:u32, dropped:u32`。`unit=1` 表示队列项，`unit=2` 表示字节；每页最多 2 条。`0xFFFF` 表示未知的 u16 字段，`0xFFFFFFFF` 表示未知的 u32 字段。未知值在主机界面显示为“未采集”，API/Python JSON 输出为 `null`，不得当作零。UART1 RX ring 的队列 ID 13 以字节为单位；UART 硬件 FIFO 事件单独记录在计数器 64/65，不等于软件事件队列容量。

| 队列 ID | 队列 | 所在板 |
|---:|---|---|
| 1..3 | host_hid_event、host_hid_report、host_hid_control | M |
| 4..9 | UART1.tx、motion_tx、safety_tx、software_tx、vendor_tx、event | M 与 P |
| 10..12 | vendor_input、motion_input、vendor_control | P |
| 13 | UART1.rx_ring（字节） | M 与 P |

当前 M 包含队列 1..9、13 共 10 条，队列页 5 页；P 包含队列 4..9、10..13 共 10 条，队列页 5 页。UART1 event 队列的 `received/rejected` 若底层无法准确提供则为 UNKNOWN；`dropped` 只表示 reset 时丢弃的事件通知条数。FIFO overflow 与 buffer-full 通知分别由计数器 64 和 65 汇报，单位是事件次数。

主机 EXE 的鼠标捕获页提供“读取设备统计”入口；loopback API 可用 `GET /api/v1/serial/stats` 读取同一组两板快照。Python 独立串口读取工具为 `python tools/read_device_stats.py --port COM3`，示例端口为 P 板；只能在该端口未被 EXE 占用时使用，它在打开前将 DTR/RTS 设为低电平。当前默认 V4 构建下，P、M 均支持 A5 统计请求。P 维护口为 921600，M 默认为 115200；在 M 上发送有效 A5 请求会切换到 A5 输入所有者，清空待处理软件输入并释放按钮。查询前先停止 Makcu 客户端，不要交替争用同一串口。统计页字段及回归样例见 `tests/fixtures/stats_snapshot_c_frames.txt`。

### MAKCU V4 鼠标 API（M 板 UART0）

当前 `firmware/dual_proxy` 默认构建启用 `DUAL_PROXY_ENABLE_MAKCU_V4_API=ON`，并与旧 V3 风格 ASCII 模式互斥。2026-10-06 已将同版双板固件刷入 M/P：M（本次 COM12）锁定 `MOUSE_HOST` 后，UART0/CH340 使用 V4，默认 115200、8N1；P（本次 COM3）保留 A5/5A v2、921600 baud。串口号可能随拔插变化，使用前应重新确认角色。波特率设置仅保存在运行时，重启后恢复默认。

V4 协议允许 `baud(4000000)`，但本机 Windows CH340 在切换主机串口到 4,000,000 baud 时返回 Win32 `PermissionError(31)`。测试工具已向 M 发送 A5 设置帧，但没有取得 4 Mbaud 下的 A4 查询结果；随后在 115200 的 A4 查询超时。一次 `esptool run` 硬复位（未写入或擦除 Flash）恢复默认速率，之后 115200 下 46 项只读检查为 46 PASS、0 FAIL，含 A4=115200。本机 4M 测试 `Failed`，4M 接口响应未验证；当前建议使用 115200，不应从协议字段推断本机 USB-UART 适配器支持 4 Mbaud。

固定版本的官方 MAKCU SDK（revision `2616b1c3905bd0af65aa6ed338ead2b1c256bb6e`）使用 V4 二进制 API 完成 `Mouse.move(+20,0)` 和 `Mouse.move(-20,0)`。P 侧诊断收到各一条匹配的 `P_USB_REPORT_SUBMIT` 与 `P_USB_REPORT_COMPLETE`，两次 submit→complete 分别为 645 μs 和 484 μs。该观测证明了本轮报告到达 P 的 USB 提交/完成回调，不证明接收电脑的 Windows 光标或应用已经消费；G HUB、真实物理鼠标和长时运行仍待验收。该次历史检查期间 Host EXE 保持关闭。当前固件在 M UART0 共存解析 A5 与 V4，按有效命令切换输入所有者；切换会清空待处理输入并释放按钮，V4 所有者抑制 A5 诊断流。始终只让一个客户端操作该串口。

新版固件在 M UART0 默认 115200 下隔离解析 A5 与 V4 完整帧，按有效命令切换输入所有者，并清空软件位移、释放按钮。默认发送端同时只使用一路；两个进程不能同时占用串口。V4 所有者抑制 A5 日志与诊断，A5 所有者保留 Host 控制；P 维护口仍为 921600。2026-10-06 已部署双板，V4 查询 46/0、五档及阈值 13 项真串口检查通过；真实移动未验收。ASCII `version()` 返回 `km.MAKCU` 作为握手标识；仅兼容下表鼠标子集，不承诺原厂时序。

构建默认开启 V4；可显式关闭以生成保留 A5 的双板镜像。旧 ASCII 模式与 V4 互斥：

```powershell
idf.py build
idf.py -D DUAL_PROXY_ENABLE_MAKCU_V4_API=OFF build
```

ASCII 命令以 CR 或 LF 结束，命令必须带 `km.` 前缀；接收器支持分片和连续帧。查询结果以 `结果\r\n>>> ` 结束；echo 默认关闭，开启后查询先回显原命令（`version()` 不回显）。成功 SET 在 echo 开启时回显命令并输出 `>>> `，echo 关闭时无响应；参数/角色/队列错误返回 `ERR\r\n>>> `。不支持的命令明确拒绝。`device()` 返回 `mouse`。按钮查询值按位表示：0 未按、1 物理、2 软件注入、3 两者同时按下。ASCII `snapshot()` 不支持。

| ASCII 命令 | 二进制 opcode | 支持行为与限制 |
|---|---:|---|
| `device()` / `version()` | `0x02` / `0x04` | 查询设备类型与握手标识。二进制 `0x04` 返回 4 字节小端桥接 API 级别 `4`，不表示原厂固件版本。 |
| `move(x,y)`、`move_now(x,y)`、`wheel(delta)` | `0x18`、`0x67`、`0x19` | 相对移动和滚轮；数值按官方载荷宽度校验，超出 USB 报告范围时分成多份报告，累计相对位移/滚轮量保持不变。`move_now` 仍通过固件调度任务提交。 |
| `left()` … `side2()`、`left(0|1)` … `side2(0|1)` | `0x11..0x15` | 查询物理与注入状态，或通过现有软件鼠标报告注入按钮。`ms1`、`ms2` 是 `side1`、`side2` 别名。 |
| `buttons(mode[,period])`、`stream(mouse,0|1)` | `0x10`、`0x52`、事件 `0x53` | 物理按钮变化订阅。二进制 `0x10` 与 `INPUT_STREAM(mouse)` 共用一个布尔订阅状态，默认关闭；订阅时先从 released 基线发送当前已按物理键，再按序发送边沿。二进制事件 payload 为 `kind=1,button,value`。ASCII mode 1/2 输出 `km.` 前缀、一个原始掩码字节和 CRLF；mode 3 输出一个裸掩码字节。ASCII `period` 接受 0..1000 ms，但当前实现忽略该值。队列溢出会清队列、关闭订阅；二进制发 `0x53`、payload `01 FF FF`，需客户端重新订阅。 |
| `phys_buttons()` | `0x55` | 查询原始物理按钮掩码，低五位对应五个鼠标按钮。 |
| `snapshot()` / `inject_snapshot()` | `0x56` | 仅二进制查询 40 字节桥接注入状态：byte 0 是软件按钮；1..2 为 lock mask；3 为零；4..35 键盘位图全零；36 插值值；37..39 为零。 |
| `left_mask(0|1)`…`side2_mask(0|1)`、`move_mask(l,r,d,u)`、`wheel_mask(down,up)` | `0x16`、`0x17`、`0x1A..0x1E` | 屏蔽物理鼠标输入对应字段；只过滤物理报告，不影响软件注入。参数必须是 0/1。 |
| `interpolate()` / `interpolate(0..100|255)` | `0x1F` | 设置 P 端平滑：0/25/50/75/100% 对应 0/5/10/15/20 个 1 ms 槽。其他 0..100 整数就近选择，13/38/63/88 为向上阈值；255 保留 AUTO，其他越界值拒绝。SET、GET 与 snapshot 返回规范值。 |
| `lock_ml`、`lock_mr`、`lock_mm`、`lock_ms1`、`lock_ms2`、`lock_mx`、`lock_mx+`、`lock_mx-`、`lock_my`、`lock_my+`、`lock_my-`、`lock_mw`、`lock_mw+`、`lock_mw-` | `0x60` | 查询或设置与按钮、轴向、滚轮掩码对应的 14 个物理屏蔽目标；lock 与 mask 是同一屏蔽状态，双方可查询和解除。 |
| `click(button[,count[,hold_ms]])` | `0x61` | button 为 1..5，count 为 1..255；hold 省略/为 0 时随机 35..75 ms，否则 1..5000 ms。点击按队列顺序由 1 ms 调度任务执行。 |
| `moveto(x,y)` / `getpos()` / `screen()` / `screen(w,h)` | `0x62` / `0x63` / `0x64` | 追踪的软件屏幕坐标；屏幕宽高为 1..32767，目标坐标超出已设置屏幕时截到边界。`moveto` 通过相对移动近似定位，不是 USB 绝对指针接口。 |
| `silent(x,y)`、`moving()` | `0x65`、`0x66` | `silent` 以屏幕绝对目标坐标排队执行“左键按下、移动到目标、松开并返回”；`moving` 查询是否有待处理移动。坐标会夹到屏幕范围。 |
| `baud()` / `baud(0|115200|4000000)` | `0xA4` / `0xA5` | 查询/设置 M UART0 波特率；binary baud 使用 4 字节小端 u32，0 恢复默认 115200。协议接受 115200 和 4 Mbaud；本机 4M 测试 `Failed`，4M 接口响应未验证，建议使用 115200。 |

二进制帧为 `DE AD LEN:u16le CMD PAYLOAD[LEN]`，`LEN` 只计 payload、不含 CMD；读取请求的响应沿用同 opcode，成功 SET 默认不返回，`INTERPOLATE(0x1F)` 返回所选比例。错误帧为 `DE AD 01 00 CMD FF`。解析器有界缓存并在声明长度内隔离 payload，避免将二进制内容误当 ASCII 命令；超长/截断输入按帧边界丢弃并超时恢复。

移动、点击和 silent 动作由控制任务上的 1 ms 软件调度器串行执行，最多 8 个排队动作；插值是软件分片，不保证曲线或完成时间与原厂一致。有效输入 setter 续 1500 ms 租约，查询不续租；租约到期、设备断开、停止转发或角色切换会取消排队动作并释放注入按钮。物理按钮流在短输入回调中记录边沿，队列容量 64。键盘、手柄、flick 和设备管理 API 均不在支持范围，不能用构建通过推断真机兼容。

### 旧 V3 风格 ASCII 子集（可选编译，默认关闭）

`DUAL_PROXY_ENABLE_MAKCU_ASCII_API` 保留为旧实验接口，默认 OFF，并与 V4 互斥。它不是 MAKCU V4，也不应与上面的 V4 命令表混用。启用时它占用 M UART0 的 ASCII 命令口、屏蔽同端口 A5 和 ESP_LOG；现有部署没有使用此模式。旧协议实现细节可查 `firmware/dual_proxy/main/makcu_ascii_logic.c`，其硬件验收状态未验证。

### UART0 实时诊断与手动注入（双板固件）

下表沿用本节 `A5 5A` 帧头、版本 2、序号回显和 CRC16。当前默认 V4 构建下，P 和 M 均接受 A5 请求；M 上的有效 A5 请求会切换到 A5 输入所有者。V4 所有者抑制 A5 诊断流，切换所有者会释放软件按钮并清空待发移动；不要交替争用同一串口。命令的结果只表示板端已接收或已排队，USB 提交、完成与电脑软件消费需要分别观察。

| Type | 名称 | Payload / 回复 |
|---:|---|---|
| `0x0D` | DIAG_PROFILE_READ | M 上请求 `offset:u32`；`0x0E` 回 `offset:u32,total:u32,data:0..56`。逐块读取原始 Profile v2，`total=0` 表示暂无快照。 |
| `0x0F` | DIAG_PROFILE_BEGIN | P 上请求 `length:u32,crc32:u32`，长度 1..4096；`0x12` 回单字节状态。 |
| `0x10` | DIAG_PROFILE_CHUNK | P 上请求 `offset:u32,data:1..60`，必须连续；每块回 `0x12`。 |
| `0x11` | DIAG_PROFILE_COMMIT | P 上空载荷提交；校验 CRC32 与完整反序列化，成功后进入手动 Profile 模式并排队重建 USB 克隆；回 `0x12`。在线 M 会话可以被手动 Profile 覆盖。 |
| `0x12` | DIAG_PROFILE_RESULT | `status:u8`；0=受理/排队，1=格式或顺序错误，2=角色不符，3=没有有效在途写入或超时，4=CRC/解码错误，5=重配置无法排队。0 **不是** USB 挂载成功。 |
| `0x13` | DIAG_PROFILE_MODE | P 上请求 `0` 退出手动模式；先清理现有克隆，再重新申请 M 的真实 Profile；回 `0x12`。 |
| `0x14` | DIAG_STREAM_CONTROL | `0` 退订 / `1` 订阅并查询；回 `0x16`，序号与命令相同。 |
| `0x15` | DIAG_STREAM_EVENT | 板端主动发送；`event_id:u32,timestamp_us_low32:u32,source:u8,kind:u8,total_length:u8,offset:u8,data:0..52`。同一事件按 `event_id`、`offset` 重组；时间戳约 71 分钟回绕。 |
| `0x16` | DIAG_STREAM_STATUS | `captured:u32,dropped:u32`。`dropped>0` 或事件重组缺口表示采集不完整。 |
| `0x17` | DIAG_INJECT_REQUEST | `route:u8,type:u8,inner_payload:1..62`；route 1 仅 P 可用，接受 `RAW_HID_INPUT(0x27)` / `HID_GET_REPORT_RESPONSE(0x2A)`；route 2 仅 M 可用，接受 `HID_SET_REPORT(0x28)` / `HID_GET_REPORT_REQUEST(0x29)`。内层字段按同名 UART1 帧解码，非法载荷拒绝。 |
| `0x18` | DIAG_INJECT_RESULT | `route:u8,status:u8`；0=已交给目标处理函数，1=格式错误，2=路由/角色/类型不支持。目标队列和 USB 后续失败须看事件与各队列统计。 |

#### 鼠标报告注入（`0x1B`）

`DUAL_MESSAGE_DIAG_REPORT_INJECT_REQUEST`（`0x1B`）只能发送到 M（`MOUSE_HOST`）的 UART0。Payload 是一条鼠标 Input Report：

- 无 Report ID 的正文：`buttons:u8, reserved:u8, dx:i16le, dy:i16le, wheel:u8, pan:u8`，共 8 字节；
- 已包含 Report ID 的完整报告：当前报告 ID + 上述正文，共 9 字节（C539 当前为 `02` + 8 字节）；
- M 会读取当前枚举到的鼠标布局。8 字节正文会自动补当前 Report ID，9 字节完整报告原样使用；不应把按钮字节误放在 Report ID 位置。

M 收到命令后把报告送入与真实 USB Host 输入相同的 RX 回调，因此会经过 M 解析、UART1、P 分类、HID 提交和完成回调。命令受理不代表 P 已提交；应查询 `0x1D` 统计快照，对照两端输入/提交/完成/失败计数与队列 `depth/peak/rejected/dropped`，并结合 `0x14`/`0x15` 诊断事件检查路径。统计计数自各板启动后累计，需同时记录板端 uptime。

#### 移动命令与 HID 报告注入的区别

当前有两条不同路径，均不是厂商 HID++ 的“移动命令”：

- `--inject-move X Y` 发送项目自定义 UART0 `MouseReport (0x02)`。Payload 是标准化的 `buttons + x/y + wheel/pan + smoothing_slots` 软件输入格式；M 收到后通过 UART1 转成 `SOFTWARE_MOUSE (0x2B)`，P 再按当前克隆鼠标布局生成 HID 报告。这是**软件鼠标输入通道**，不是 USB 原始报告，也不是厂商请求。
- `0x1B` `DIAG_REPORT_INJECT_REQUEST` 发送原始 HID Input Report，直接进入 M 的物理 USB Host RX 回调，使用当前鼠标的接口、Report ID 和报告布局。这是**诊断用原始 HID 报告注入**，不是厂商 HID++ 请求。
- 厂商请求使用 `0x17`/`0x27`/`0x28`/`0x29` 或设备级 Vendor Control 路径，服务 G HUB 等控制事务；它们不用于生成普通鼠标位移。

#### 软件移动命令

1. 通过诊断工具向 M 注入固定位移（工具直连 UART0，不能同时被 EXE 持有）：

```powershell
python tools/inject_mouse_motion.py --port COM12 --count 200 --dx 10 --dy 0 --interval-ms 2
```

2. 通过双 UART 工具注入一次**软件鼠标移动**并实时订阅两板事件：

```powershell
python tools/dual_uart_inspect.py --ports COM3,COM12 --inject-move 20 0
```

3. EXE 已占用串口时，使用 loopback API 转发完整 UART0 `0x1B` 原始 HID 注入帧。API 不重新打开串口，不切换 DTR/RTS：

```powershell
$frameHex = "<A5 5A 帧头、0x1B 类型、序号、8/9 字节报告和 CRC16 的连续十六进制>" -replace ' ', ''
Invoke-RestMethod http://127.0.0.1:24815/api/v1/serial/write `
  -Method Post -ContentType 'application/json' `
  -Body (@{ portName = 'COM12'; hex = $frameHex } | ConvertTo-Json)
```

`tools/uart_protocol.py::build_frame(0x1B, sequence, report)` 可生成完整 UART0 帧；`report` 使用 `struct.pack('<BBhhBB', buttons, 0, dx, dy, wheel, pan)`。API 返回成功只表示 Host 的串口 `Write` 完成。

4. 圆形移动应先按圆周采样点计算相邻整数增量，再逐帧发送；允许 `dx=dy=0`，不要为了“保持频率”强行补 `dx=1`。示意代码：

```python
for i in range(sample_count):
    angle = 2 * math.pi * turns * i / (sample_count - 1)
    point = (round(radius * math.cos(angle)), round(radius * math.sin(angle)))
    dx, dy = point[0] - previous[0], point[1] - previous[1]
    send_uart0_inject(dx, dy)  # 通过 EXE /api/v1/serial/write 或直连串口
    previous = point
```

EXE 逐帧 HTTP 请求的实际间隔通常大于 1 ms；“目标间隔 1 ms”只能作为采样目标，必须在日志中记录实际发送帧数和耗时，不能据此宣称严格 1 kHz。

`source`：1=P 的 USB 应用回调，2=M 的 USB Host HID 报告，3=UART1 接收，4=UART1 已写出，5=UART0 已解析输入帧。UART0/1 的 `kind` 为原消息 type；USB `kind`：`0x80` SET_REPORT、`0x81` GET_REPORT 请求、`0x82` GET_REPORT 结果、`0x83` 设备级 vendor SETUP、`0x84` M 原始 HID 输入、`0x85` P USB 提交尝试、`0x86` USB 完成、`0x87` USB 失败。P 的 `0x83` 目前记录 SETUP 后仍返回 STALL，并不伪造厂商响应。各 USB 事件的 payload 前缀见 `diag_event_details()`；声明长度大于保存长度时客户端标记截断。

队列 `capacity/depth/peak/received/rejected/dropped` 经 `0x1D` 快照读取，单位随记录中的 `unit` 字段为项或字节，不再依赖周期统计行。每个队列使用其实际容量和当前深度；UART1 RX ring 以字节为单位，不能与 UART 硬件 FIFO 容量混用。UART 驱动事件队列的 `received/rejected` 在不可观测时用 UNKNOWN，`dropped` 只计 reset 丢弃的通知条数；UART FIFO overflow 与 buffer-full 通知次数分别在计数器 64/65 中。

`tools/dual_uart_inspect.py` 会实时显示并以 JSONL 保存事件，同时保存各端 UART0 原始 RX/TX 字节。示例：`python tools/dual_uart_inspect.py --ports COM3,COM13 --inject-move 20 0` 可经 M 的既有软件鼠标帧注入相对位移，观察 M UART0→M UART1 TX→P UART1 RX→P USB 提交/完成。`0x1B` 注入正文可不含 Report ID；M 会按当前已枚举鼠标布局自动补齐 Report ID，也接受已经带 ID 的完整报告，避免把按钮字节误当成 ID。P USB 提交成功仅表示 TinyUSB 接受报告，完成回调表示 USB 端传输完成，均不能单独证明目标应用已消费；两板事件目前按载荷与时间窗关联，尚无跨板统一 trace ID。串口订阅只覆盖固件已接入的 USB/HID 与 UART1 观察点，不包括控制器内部 ACK/NAK、总线重试和电脑端 USB 包。

## 主机局域网模拟鼠标 UDP 接口（Host 输入适配层）

该接口是 Windows 主机 EXE 的输入适配层，不改变电脑到 ESP32 的串口/Wi-Fi 帧格式。默认监听 `0.0.0.0:24814`；可用 `bridge.local.json` 的 `remoteInputBindAddress` 和 `remoteInputPort` 覆盖。主界面左上角显示当前可用的局域网监听 IP 和端口。该 UDP 接口不做身份认证。

每个 UDP 数据报必须是一个 UTF-8 JSON 对象：

```json
{"dx":12,"dy":-4,"wheel":0,"pan":0}
```

- `dx`、`dy`：有符号 16 位相对位移。
- `wheel`、`pan`：有符号 8 位滚轮增量。
- 四个增量不能全为零；未知字段会被忽略，超范围或无效 JSON 的数据报直接拒绝。
- 旧版单板模式下，命令仅在 HOME 同步开启或“始终开启 UDP 输出”打开时进入 `MouseReportPump`；双板模式下网络 UDP 输出不受 HOME 和隐藏的旧版开关限制。Host 以最高 500 Hz / 2 ms 聚合完整位移，不在 Windows 用户态展开固件平滑槽。
- 双板模式的 Host UDP 软件报告只在 M 平滑，档位为 0/5/10/15/20 个 1 ms 滚动槽；新命令叠加到未来槽。M 发给 P 的报告第 8 字节恒为 0，P 不做二次平滑。实体鼠标由 M→P 硬件直通，不进入 Host 软件平滑器。旧版单板模式使用原 7 字节鼠标报告，不使用 M/P 平滑。
- EXE 复选框后可选 5/10/15/20 槽，取消勾选为 0。HOME 同步关闭后可修改本地档位；UDP JSON 可携带 `smoothing_slots`（仅 0/5/10/15/20，省略时保留 Host 设置）。此配置不修改 Makcu `interpolate`。ReleaseAll、会话重建及链路故障清空 M 待发位移并释放按钮。
- BLE 固件接受 7/8 字节桥接报告但忽略第 8 字节，仍以 10 ms 节拍合并位移；只有原生 USB 路径执行 5 槽、1000 Hz 消费。
- 监听停止或程序退出时不保留远端待发送状态，并继续走现有 `ReleaseAll` 清理路径。旧版单板模式关闭“始终开启 UDP 输出”且 HOME 关闭后会阻断网络输入；双板模式的该旧版开关隐藏，网络 UDP 不依赖 HOME 状态。

### Python 调用示范

项目内 `tools/esp32_move.py` 提供 `Esp32MouseSender`：

```python
from tools.esp32_move import Esp32MouseSender

sender = Esp32MouseSender("192.0.2.10", 24814)
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

设置页的“UDP 模拟输入测试”默认关闭，仅旧版单板通路下显示，并且只在旧版单板模式、HOME 开启且测试开关打开时生效。它不会改变网络数据报格式，也不会向 UDP socket 发送回环数据报；测试输入按所选 `30/60/100/140/200/500 Hz` 聚合实体鼠标移动，再写入旧版单板 7 字节鼠标报告。选择“无上限”时每个原始事件直接进入内部报告路径。该测试源不适用于双板 M 软件输入，按钮边沿仍按顺序发送。
