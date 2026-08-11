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
| `0x02` | MouseReport | 7 字节：`buttons`、小端有符号 16 位 `x/y`、有符号 8 位 `wheel/pan`；`x/y` 表示相对位移 |
| `0x03` | ReleaseAll | 空 |
| `0x04` | Ping | 空；维持当前输入租约 |
| `0x05` | SessionStart | 空；申请新的输入租约，并先释放旧会话的全部输入 |
| `0x06` | DeviceProbe | 8 字节随机数；仅用于 COM 输入通道自动发现，不申请输入租约 |
| `0x07` | DeviceHello | `HIDBRDG2` ASCII 标识加原样返回的 8 字节随机数；序号与 DeviceProbe 相同 |

主机在每次 UART、原生 USB CDC 或 Wi-Fi 连接建立后先发送 `SessionStart`，之后至少每 500 ms 发送一次 `Ping`。固件默认在 1500 ms 内未收到当前会话的有效帧时执行 `ReleaseAll`，避免断线卡键。

`portName` 为 `auto` 时，主机依次打开当前可用 COM 口并发送 `DeviceProbe`。只有收到 CRC、序号、固定标识和随机数均匹配的 `DeviceHello` 后，才把该 COM 口认定为 HID Bridge。启动日志等非协议字节会被跳过。
主机同步程序打开 UART 或原生 USB CDC 对应的 COM 口后，会由同一个 `SerialPort` 实例读取设备日志并缓冲写入 `deviceLogPath` 指定的文件（默认 `artifacts/host-serial-{timestamp}.log`），因此不需要、也不能再同时运行 `idf.py monitor` 独占同一个 COM 口。为避免高频 BLE notify 日志重复触发主机日志落盘和 WinForms 重绘，`showDeviceLogInUi` 默认关闭；该选项只影响窗口镜像，不影响独立设备日志文件。

UART 与原生 USB CDC 都直接承载上述帧；两者使用相同的字节流解析、设备发现和输入租约。Wi-Fi TCP 通道先用预共享密钥进行双向挑战认证，再使用 AES-256-GCM、单调包计数器和会话随机数保护每个完整帧；计数器不连续或认证标签错误时立即断开连接。

固件启动后的 1500 ms 为原生 USB profile 检测窗口：收到任何 CRC 正确的 UART 协议帧时选择键盘与相对触摸板 HID-only；未收到时先选择 CDC-only。若 CDC-only 启动后 UART 才收到首个有效协议帧，固件会执行 `ReleaseAll` 并自动重启一次；控制端持续探测会在新的启动窗口内命中，使原生 USB 重新枚举为 HID-only。已经处于 HID-only 时不会重复重启。选择只影响原生 USB 描述符，不改变本协议帧格式。

解析器遇到错误 Magic、过长 Payload 或 CRC 错误时丢弃当前候选帧，并继续寻找下一个 `A5 5A`。

协议 v2 不兼容旧的 5 字节鼠标报告。主机、固件与 Target Agent 必须使用同一版本。


## 主机局域网模拟鼠标 UDP 接口

该接口是 Windows 主机 EXE 的输入适配层，不改变电脑到 ESP32 的串口/Wi-Fi 帧格式。默认监听 `0.0.0.0:24814`；可用 `bridge.local.json` 的 `remoteInputBindAddress` 和 `remoteInputPort` 覆盖。该 UDP 接口不做身份认证。

每个 UDP 数据报必须是一个 UTF-8 JSON 对象：

```json
{"dx":12,"dy":-4,"wheel":0,"pan":0}
```

- `dx`、`dy`：有符号 16 位相对位移。
- `wheel`、`pan`：有符号 8 位滚轮增量。
- 四个增量不能全为零；未知字段会被忽略，超范围或无效 JSON 的数据报直接拒绝。
- 命令仅在主机 `HOME` 同步开启时进入 `MouseReportPump`。默认开启“UDP 平滑”时，每条命令从到达时起立即分摊到固定 10 个 2 ms 环形发送槽；首条命令与后续命令使用相同规则。
- 新命令按槽叠加到已有计划，不串行追加等待队列。连续或突发输入的计划尾部始终不超过 20 ms；停止输入后最多再输出 9 个主机槽。
- X/Y/Wheel/Pan 独立采用整数商和余数分摊，每条命令的 10 槽代数和与原命令严格相等。空闲后首条命令从当前槽响应；重叠命令的余数起点按命令轮转，使同一泵周期内连续 10 条 `(1,-1)` 均匀分布为每槽 `(1,-1)`。
- 关闭“UDP 平滑”后，真实和模拟 UDP 都跳过固定槽分摊，直接进入 500 Hz 报告聚合。开关只能在 `HOME` 同步关闭时切换；同步关闭路径先清理当前会话并发送 `ReleaseAll`，重新开启后从全新会话开始，网络 JSON 格式不变。
- BLE HID 输出由固件以 10 ms 节拍合并主机报告，因此 20 ms 主机平滑窗通常映射为约 2–3 个 BLE 报告；USB HID 不受 BLE 节拍限制。该差异不改变协议字段和位移总量。
- 监听停止、同步关闭或程序退出不保留远端状态，并继续走现有 `ReleaseAll` 清理路径。

主机界面的“模拟 UDP”模式不会改变上述网络数据报格式，也不会向本机 UDP socket 发送回环数据报。它先按所选 `30/60/100/140/200/500 Hz` 最大频率整合实体鼠标移动和滚轮；选择“无上限”时，每个原始事件直接形成一条内部模拟 UDP 命令。结果提交到与网络命令相同的公共 UDP 后续入口。频率只属于模拟源；“UDP 平滑”开启时两类输入都执行固定 10 槽分摊，关闭时都跳过该步骤，之后的 500 Hz 提交和 BLE 10 ms 合并仍一致。按钮转换因不属于 UDP JSON 字段而继续即时进入鼠标报告队列。
