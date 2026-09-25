# dual_proxy：双板单鼠标第一阶段固件

这是独立于 `firmware/main` 的双 ESP32-S3 透明鼠标代理工程。两块板刷同一个镜像，启动时先通过 UART1 广播 `UNRESOLVED`，再在 USB Device 与 USB Host 间轮换探测，每个探测窗口为 3 秒：

- USB Device 检测到电脑 attach 或完成配置（`tud_mounted()`）：本机锁定为 `PC_DEVICE`；收到完整物理 Profile 后严格克隆真实鼠标 Device/Configuration/String/Report descriptor，并代理 raw Input/Output/Feature；
- USB Host 成功启动至少一个物理 HID 接口：本机锁定为 `MOUSE_HOST`，以 USB Host 接真实鼠标/接收器，同时从 UART0/CH340 接收电脑 A 的软件输入。此角色确认不要求先解析出标准鼠标布局，避免复合设备枚举期间按周期切断重试；实体移动只由已识别的鼠标 Report layout 转换和转发；
- 任一板锁定后通过周期性 `LINK_HELLO` 告知对端，未定身份的一侧锁定为相反角色。锁定后 USB 或 UART 暂时断开不会重新选角色；角色仅保存在 RAM，单板复位由仍在线的对端告知互补角色，两板都断电后双方重新从 `UNRESOLVED` 开始。

板载 ESP32-S3-DevKitC-1 WS2812B 使用 GPIO48：身份未定时熄灭；锁定身份但板间对端未在线时红色常亮；对端在线时 `MOUSE_HOST` 为绿色、`PC_DEVICE` 为蓝色。本地 Profile 采集、诊断版 5 秒接收确认或 USB 克隆失败时红色闪烁。电脑侧 USB 已挂载且软件移动报告提交成功时蓝灯短暂熄灭。LED 刷新由独立任务执行，1 kHz 输入路径只更新状态。

## 运行连接

正式拓扑中，电脑 B 只连接 PC 侧板原生 USB-C；电脑 A 通过鼠标侧板 UART0/CH340 USB-C 给板卡供电并发送软件命令，鼠标侧板原生 USB Host 口连接并给真实鼠标供电。PC 侧板 UART0 只在刷写/开发时连接。两板通过 UART1 连接：

```text
PC板 GPIO17 (TX)  ->  鼠标板 GPIO18 (RX)
PC板 GPIO18 (RX)  <-  鼠标板 GPIO17 (TX)
两板 GND 共地
```

不要连接两板的 5V 或 3V3。鼠标侧板由电脑 A 的 UART USB-C 供电，板载 5V 路径再给 Host 口鼠标供电；当前硬件已实际让测试鼠标正常上电。需要区分：固件轮换的是 ESP32-S3 USB 控制器软件栈，不会切换板上的 CC 电阻或 VBUS 电源路径。现有硬件记录显示 USB-C CC1/CC2 固定下拉且没有可控的标准 Host VBUS/CC 角色电路；反复切换时的 VBUS/反灌表现仍须实测，固件构建通过不能替代该验证。

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
| `0x2D` | DEVICE_GONE：物理 USB 鼠标/接收器已拔出（`sender_generation`、`target_generation`、非零 `event_id`、`reason`，13 bytes） |
| `0x2E` | PROFILE_ACK：P 已完成最终 USB 挂载或明确拒绝 Profile（`transfer_id`、`crc32`、`status`、`recipient_generation`、`sender_generation`，17 bytes） |
| `0x2F` | ROLE_ACK：对端角色声明确认 |
| `0x30` | PROFILE_REQUEST：P 发起一次当前 USB 信息重新采集申请 |
| `0x31` | PROFILE_OFFER：M 发起新 Profile 克隆提议 |
| `0x32` | FLOW_ACK：REQUEST、OFFER、COMMIT 或 DEVICE_GONE 的接收确认 |

UART1 会拒绝相同 node ID 或相同角色的对端；3 个 250 ms 周期无有效帧、鼠标侧掉电、物理鼠标拔出或 Profile 超时均按设备拔出处理：先释放所有输入，再让 PC 侧卸载 USB Device。新 Profile 完整校验前不会重新连接电脑 B。

每次启动生成新的非零 32 位 UART1 generation。对端 generation 变化会清除旧序号窗口与本会话申请状态；P 再发一次 `PROFILE_REQUEST`，M 重新获取当前 USB 信息并发 OFFER。旧会话缓存的未确认 Profile 不会在新会话自行提议，以免与 P 请求形成双克隆。该字段位于 12-byte `LINK_HELLO` 和 4-byte `LINK_PING` 中，因此两板必须刷入同一版本。

## 连接恢复事务（有界重试）

`link_recovery_logic.h` 是 M/P 共用的纯逻辑状态模型；`uart1_link.c` 和 `pc_hid_output.c` 只按它的判定执行，逻辑测试直接喂同一组函数做故障注入。规则如下：

- 同一时刻只允许一个有效的清理屏障和一个 Profile 传输。事务身份为 `(M generation, P generation, mouse connection id, event/flow/transfer id, epoch)`，重试只重放同一事务，不新建 ID。
- 鼠标拔出：M 生成非零 `event_id` 后按 `LINK_GONE_RETRY_INTERVAL_US`（0.7 s）、最多 `LINK_GONE_MAX_ATTEMPTS`（8）次重发同一事件；P 报告清理失败时保留同一事务继续重试，而不是永久作废。等待窗口超过 `LINK_GONE_STAGE_TIMEOUT_US`（5 s）或次数用尽才判失败并闪红灯，但**双方 generation 与 event ID 都匹配的迟到确认仍然有效**，可以让同一事件重新开放克隆门。
- 快速插回：新鼠标可以先枚举并采集 Profile，但 OFFER 与分片必须等旧 `DEVICE_GONE` 屏障完成；旧事件不会清除新连接状态。
- 电脑侧清理失败：保持克隆门关闭、保留事务身份，本地最多重试 `LINK_CLEANUP_MAX_ATTEMPTS`（3）次；清理成功后立刻登记结果，若只有确认帧入队失败，则只补发 ACK（最多 `LINK_ACK_ENQUEUE_MAX_ATTEMPTS` 次），不重复卸载、不清输入、不递增 epoch。
- Profile 流程：重复 `PROFILE_REQUEST`/`PROFILE_OFFER` 按 generation + flow ID 去重；物理鼠标尚未枚举时 M 受理并等待设备到达，不判永久失败。CHUNK 对重复到达的前缀分片幂等（内容一致才忽略，冲突即失败）；同一 `transfer_id` 的重复 BEGIN/COMMIT 直接返回既有结果，不重新发布、不第二次重枚举。最终 `PROFILE_ACK` 丢失时 M 只重放同一 transfer 的 COMMIT，P 只补发两个确认。
- M 只有在收到与本机 generation、对端 generation 和当前活动 transfer 全部匹配的成功 `PROFILE_ACK` 后才把本端 `usb_state` 更新为 `HID_CONNECTED`；失败或旧会话确认不更新状态。
- 输入错误与设备断开分离：`on_mouse_release(false)`（报告队列满、传输错误等）只释放按钮并记录输入错误，不触发 `DEVICE_GONE`、Profile 清理或 USB 重枚举；只有 `on_mouse_release(true)`（真实物理设备消失）才进入断开事务。
- 不可重试原因（不支持的描述符、安装失败后的终态、状态非法）与可重试原因（暂未枚举、队列忙、卸载/安装暂时失败、确认超时）在日志里用 `link_failure_cause_name()` 区分，不把不可克隆误报为超时。
- 恢复总预算 `LINK_RECOVERY_BUDGET_US` = 10 秒，各阶段共用剩余预算，不串联多个完整等待窗口。

重试间隔/次数是**待实测定标的保守初值**，全部集中在 `link_recovery_logic.h`，现场按日志调整即可。事务日志只在状态转移时打印：`恢复事务[stage] role=… gen=… peer_gen=… conn=… event=… flow=… transfer=… budget_left_ms=…`，不记录高频鼠标帧。

## 多型号动态 Profile（运行时观察阶段）

`hid_device_profile` 提供与具体鼠标无关的有界二进制模型，保存原始 Device/Configuration 描述符、UTF-8 字符串以及按物理 interface number 索引的 HID Report 描述符。Profile 最大 4096 字节；接口数、字符串和单份描述符都有独立上限。UART1 分片接收器只接受严格连续 offset，并且只有在 BEGIN/COMMIT 字段一致、总 CRC32 正确、反序列化恰好消费全部数据后才发布新 Profile。错误或中断的替换传输不会清除上一份已发布 Profile。

鼠标侧在每个 HID 接口成功打开后收集动态 interface number、subclass、protocol、Report descriptor、VID/PID/字符串及原始 Device/Configuration descriptor；最后一个接口后约 200 ms 防抖，Profile 序列化后通过 UART1 的 `0x24..0x26` 有界分片发送。电脑侧每收到一份完整 Profile 都强制替换当前克隆：立即释放输入并使旧厂商 HID 会话代号失效，清空排队/已出队待发的旧 vendor 报告，卸载旧 TinyUSB 设备；旧设备断开后清除活动 Profile 与鼠标报告模板，等待 300 ms，再构建新描述符并安装，最后重新枚举电脑 USB。无效 Profile 也保持断开，不回退使用旧克隆。描述符缓冲区禁止在旧 TinyUSB 设备仍安装时改写，避免旧会话枚举过程观察到混合描述符。

工程通过独立只读 USB Host client 取得 raw Device/Configuration descriptor；公开 HID Host API 继续提供逐接口 Report descriptor 和字符串。任一关键描述符只能 synthetic/partial 时，严格克隆拒绝启用。Profile v2 的每个报告项保存原始 `interface_number/subclass/protocol`。

Profile 串流只有在 M 的 `PROFILE_OFFER` 获 P 接收确认后才发送；实体鼠标安全释放和 raw Input 优先，普通鼠标报告每累计 8 个最多让出一个 Profile 帧。P 锁定角色且对端在线后对该会话只发送一次 `PROFILE_REQUEST`，要求 M 重新读取当前 USB Device/Configuration 信息并发布 Profile。M 自行完成新鼠标采集时也能发起 OFFER；P 接受 OFFER 时先释放输入并排队撤下旧克隆。REQUEST、OFFER、完整 COMMIT 和 DEVICE_GONE 都有 `FLOW_ACK` 接收确认，按上面的“连接恢复事务”做有界重试与去重，不再一次超时就永久终止；最终 USB 安装和挂载完成后 P 再回带双方 generation 的 `PROFILE_ACK`。无效 Profile 保持 USB 断开。无法安全解析相对 X/Y、wheel、pan 或 buttons 的型号仍可原样透传厂商通信，但禁用软件叠加。C092 的 G HUB 和 C539 的动态枚举/实体移动已有实机样本，不能外推为所有型号通过。

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

## 纯逻辑回归

不需要硬件，直接编译并运行事务状态模型、去重规则和故障注入：

```powershell
pwsh -File .\scripts\Test-DualProxyLogic.ps1
```

通过时输出 `dual_proxy_logic_test: PASS`。要求 `clang` 在 PATH 中，并且本工程已经执行过一次 `idf.py build`（脚本使用 `build/config/sdkconfig.h`）。这一项只证明模型级判定，不代表 PnP、G HUB、输入释放或 1000 Hz 性能通过。

## 不启动 EXE 的鼠标侧 UART0 测试

鼠标侧 CH340 COM 独占打开后运行：

```powershell
python .\tools\test_dual_proxy_serial.py --port COMx --cursor
```

脚本默认先发送 `DeviceProbe`，只有收到签名 `HIDBRDG2`、nonce 匹配且角色为 `MOUSE_HOST` 的 `DeviceHello` 才继续；随后发送 `SessionStart`、小幅 `+4`、等待、等量 `-4`、`Ping`，不发送点击、键盘或 `SendInput`。UART0 会与 ESP_LOG 混流，解析器会跳过非协议字节；同一 COM 不能同时被主机 EXE 或其他工具打开。

真实鼠标报告、单一 HID 设备、实体移动与软件移动叠加仍必须在实际刷写和受控输入下验收；本工程构建通过不等于硬件通过。
