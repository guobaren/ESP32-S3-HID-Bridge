# 串口协议

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
| `0x06` | DeviceProbe | 8 字节随机数；仅用于串口自动发现，不申请输入租约 |
| `0x07` | DeviceHello | `HIDBRDG2` ASCII 标识加原样返回的 8 字节随机数；序号与 DeviceProbe 相同 |

主机在每次串口或 Wi-Fi 连接建立后先发送 `SessionStart`，之后至少每 500 ms 发送一次 `Ping`。固件默认在 1500 ms 内未收到当前会话的有效帧时执行 `ReleaseAll`，避免断线卡键。

`portName` 为 `auto` 时，主机依次打开当前可用串口并发送 `DeviceProbe`。只有收到 CRC、序号、固定标识和随机数均匹配的 `DeviceHello` 后，才把该串口认定为 HID Bridge。启动日志等非协议字节会被跳过。
主机同步程序打开串口后会由同一个 `SerialPort` 实例读取设备日志并缓冲写入 `deviceLogPath` 指定的文件（默认 `artifacts/host-serial-{timestamp}.log`），因此不需要、也不能再同时运行 `idf.py monitor` 独占同一个 COM 口。为避免高频 BLE notify 日志重复触发主机日志落盘和 WinForms 重绘，`showDeviceLogInUi` 默认关闭；该选项只影响窗口镜像，不影响独立设备日志文件。

串口直接承载上述帧。Wi-Fi TCP 通道先用预共享密钥进行双向挑战认证，再使用 AES-256-GCM、单调包计数器和会话随机数保护每个完整帧；计数器不连续或认证标签错误时立即断开连接。

解析器遇到错误 Magic、过长 Payload 或 CRC 错误时丢弃当前候选帧，并继续寻找下一个 `A5 5A`。

协议 v2 不兼容旧的 5 字节鼠标报告。主机、固件与 Target Agent 必须使用同一版本。


## 主机局域网模拟鼠标 UDP 接口

该接口是 Windows 主机 EXE 的输入适配层，不改变电脑到 ESP32 的串口/Wi-Fi 帧格式。监听地址、端口和预共享密钥由 `bridge.local.json` 的 `remoteInput*` 字段配置。

每个 UDP 数据报必须是一个 UTF-8 JSON 对象：

```json
{"token":"至少16字节的预共享密钥","dx":12,"dy":-4,"wheel":0,"pan":0}
```

- `dx`、`dy`：有符号 16 位相对位移。
- `wheel`、`pan`：有符号 8 位滚轮增量。
- 四个增量不能全为零；未知、超范围或密钥不匹配的数据报直接拒绝。
- 命令仅在主机 `HOME` 同步开启时进入 `MouseReportPump`，随后仍按 500 Hz 聚合并编码为现有 `MouseReport`。
- 监听停止、同步关闭或程序退出不保留远端状态，并继续走现有 `ReleaseAll` 清理路径。
