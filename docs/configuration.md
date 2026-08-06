# 连接与安全配置

## 输出组合

固件支持以下输出后端：

- USB HID：始终启用，连接后可作为 USB 复合键鼠。
- BLE HID：默认编译启用，设备名可配置。
- Wi-Fi Target Agent：默认关闭，需要目标 Windows 设备运行配套程序。

USB HID 与 BLE HID 同时在线时，完成加密且真正可发送的 BLE HID 优先接收键鼠报告；BLE 断开后自动回退到 USB HID。切换旧目标和新目标时都会执行 `ReleaseAll`。Wi-Fi Target Agent 按自身安全连接状态工作。

电脑到开发板只能有一个活动输入租约。串口或 Wi-Fi 会话断开、超过租约时间或被另一会话替换时，开发板会向全部输出后端发送 `ReleaseAll`。

## 配置开发板

进入 ESP-IDF 环境后打开项目配置：

```powershell
Set-Location D:\ESP32-S3-HID-Bridge\firmware
. ..\scripts\Enter-EspIdf.ps1
idf.py menuconfig
```

在 `HID Bridge` 菜单配置：

- `Wi-Fi SSID` 和 `Wi-Fi 密码`：仅作为可选的首次启动回退值，通常留空并使用网页配网。
- `启用 SoftAP 网页配网`：默认启用。
- 配网热点密码：默认 `hidbridge`，正式部署前建议修改。
- 自动进入配网的连接失败时间：默认 30 秒。
- 配网按钮 GPIO 和长按时间：ESP32-S3-DevKitC-1 默认使用 GPIO0/BOOT，长按 5 秒。
- `Wi-Fi 输入预共享密钥`：电脑通过 Wi-Fi 连接开发板时使用，至少 16 字节。
- `通过 Wi-Fi 输出到目标 Agent`：需要网络目标端时启用。
- `目标 Agent IPv4 地址`、端口和独立的预共享密钥。
- `BLE HID 设备名称`。
- `启用 RGB 状态指示灯` 和其 GPIO：默认启用并使用 ESP32-S3-DevKitC-1 板载 RGB 灯的 GPIO48。灯色按当前活动输出显示，而不是按是否插入 USB 线显示：USB HID 为绿灯，BLE HID 为蓝灯，无活动输出时红灯每 500 ms 闪烁。USB 与 BLE 同时在线且 BLE 已就绪时显示蓝灯。
- 输入租约超时时间，默认 1500 ms。

编译选项保存在被 Git 忽略的 `firmware/sdkconfig`，网页提交的 SSID 和密码由 Wi-Fi 驱动保存到开发板 NVS。项目使用适配 2 MiB Flash 的单应用分区表；启用 USB、Wi-Fi、BLE 和网页配网后仍保留约一半应用空间。

## SoftAP 网页配网

不需要在每次更换 Wi-Fi 时重新构建或烧录。以下任一条件会进入配网模式：

- NVS 中没有保存过 Wi-Fi。
- 已保存的网络连续 30 秒无法连接。
- 固件运行期间长按开发板 `BOOT` 键 5 秒。

进入配网前固件会结束当前输入租约并发送 `ReleaseAll`。操作步骤：

1. 在手机或电脑上连接 `HID-Bridge-Setup-XXXX` 热点。
2. 输入配网热点密码，默认是 `hidbridge`。
3. 等待系统自动弹出配网页面；没有弹出时访问 `http://192.168.4.1`。
4. 从扫描结果选择一个 2.4 GHz 网络，填写密码并保存。
5. 页面会显示开发板在新网络中获得的 IP；临时热点在连接成功约 10 秒后关闭。
6. 把显示的 IP 填入主机 `bridge.local.json` 的 `wiFiHost`。

新凭据保存到 NVS，后续重启会自动连接。更换网络只需再次长按 `BOOT` 并重复上述步骤。网页只修改路由器 SSID 和密码，不会改变 Wi-Fi 输入通道的 `networkPresharedKey`。

默认配网热点密码用于开发和首次测试。部署到不受信任环境前，应在 `menuconfig` 中修改它；也可以留空建立开放配网热点，但不建议这样做。

## 电脑通过串口连接开发板

默认配置使用 `"portName": "auto"`。主机枚举当前 COM 口并完成随机数握手，只有返回有效 `DeviceHello` 的设备才会被连接；USB 重新插入后即使 COM 编号改变，也会重新发现。

需要固定串口时再复制本机配置：

```powershell
Copy-Item .\host\HidBridge.Host\bridge.json .\host\HidBridge.Host\bridge.local.json
```

保持 `transport` 为 `serial`，把 `portName` 改为例如 `COM3`。固定串口模式不执行自动扫描。主机会在连接后发送 `SessionStart`，并每 500 ms 发送一次心跳。
主机与 `idf.py monitor` 不能同时打开同一个 COM 口；主机程序会复用自己的串口读取设备日志，并按 `deviceLogPath` 写入日志文件。默认配置为 `artifacts/host-serial-{timestamp}.log`，设备文件采用约 500 ms 的缓冲刷新，避免每条 BLE notify 诊断都触发同步磁盘刷新。窗口日志标题栏提供“精简日志（高性能）/完整日志（排障）”下拉框：精简模式只保留连接参数、周期统计、警告和错误，不把普通 ESP-IDF Info/Debug/Verbose 日志写入设备日志或镜像到窗口；完整模式从切换时开始保存全部设备串口行，并把 ESP-IDF 分级日志批量镜像到窗口。`showDeviceLogInUi` 用作启动默认值：`false` 默认进入精简模式，`true` 默认进入完整模式。运行时切换不改写 JSON；将 `deviceLogPath` 设为空字符串可完全关闭设备日志读取。

为降低完整日志模式对输入线程的干扰，后台日志采用约 50 ms 的 UI 批量刷新，不再为每一行单独执行一次 `BeginInvoke`；窗口可见日志约在 50 万字符后滚动裁剪，避免长时间运行导致 WinForms 文本框无限增长。完整模式仍会增加串口解析、磁盘写入和窗口绘制负载，只应在采集诊断数据时临时启用。

## 主机 EXE 的局域网模拟鼠标接口

主机 EXE 可选开启 UDP 模拟输入监听。该入口只把相对移动和滚轮增量注入现有的 500 Hz 鼠标报告链路；它不会直接移动运行主机上的 Windows 光标。只有按 `HOME` 开启同步后命令才会被转发，同步关闭、窗口关闭或程序退出仍会执行 `ReleaseAll` 并解除鼠标锁定。

在被 Git 忽略的 `bridge.local.json` 中增加：

```json
{
  "remoteInputEnabled": true,
  "remoteInputBindAddress": "0.0.0.0",
  "remoteInputPort": 24814,
  "remoteInputPresharedKey": "替换为至少16字节的随机密钥",
  "hostLogPath": "artifacts/host-runtime-{timestamp}.log"
}
```

- `remoteInputBindAddress` 为 `0.0.0.0` 时监听全部 IPv4 网卡；只做本机测试可填 `127.0.0.1`。
- `remoteInputPresharedKey` 至少 16 个 UTF-8 字节；每个数据报都必须携带相同的 `token`。
- Windows 防火墙需要允许主机 EXE 的 UDP 入站端口。不要把该端口暴露到公网。
- 单个 UTF-8 JSON 数据报格式：`{"token":"...","dx":12,"dy":-4,"wheel":0,"pan":0}`。`dx/dy` 范围为 `-32768..32767`，`wheel/pan` 范围为 `-128..127`，四个增量不能全为零。

项目提供发送示例：

```powershell
.\tools\send-remote-mouse.ps1 `
  -HostAddress 192.168.1.20 `
  -Port 24814 `
  -PresharedKey '替换为相同随机密钥' `
  -Dx 25 -Dy -10
```

主机窗口日志与 `Console.Out/Console.Error` 会按行实时追加到 `hostLogPath`；默认文件名为 `artifacts/host-runtime-{timestamp}.log`。设备日志由窗口下拉框控制：默认“精简日志（高性能）”，需要采集原始串口细节时临时切换到“完整日志（排障）”。

## 电脑通过 Wi-Fi 连接开发板

在 `bridge.local.json` 中设置：

```json
{
  "transport": "wifi",
  "wiFiHost": "192.168.1.50",
  "wiFiPort": 24813,
  "networkPresharedKey": "替换为与固件 Wi-Fi 输入配置一致的随机密钥",
  "reconnectDelayMilliseconds": 1000,
  "heartbeatIntervalMilliseconds": 500
}
```

同步开启时主机端会强制拦截本机键鼠输入，HOME/END 保留为控制键；同步关闭后恢复本机输入。网络通道使用双向挑战认证、AES-256-GCM、独立会话随机数和严格递增计数器。认证失败、数据被篡改或检测到重放时会立即断开。

## 开发板通过 Wi-Fi 连接 Windows 目标设备

在目标 Windows 设备上复制配置：

```powershell
Copy-Item .\target\HidBridge.TargetAgent\agent.json .\target\HidBridge.TargetAgent\agent.local.json
dotnet run --project .\target\HidBridge.TargetAgent\HidBridge.TargetAgent.csproj -c Release
```

`agent.local.json` 中的 `presharedKey` 必须和固件的目标 Agent 密钥一致。目标 Agent 只接受一个活动连接，把键盘报告和相对鼠标报告转换为 Windows `SendInput`。开发板断开或心跳超时后，Agent 会释放全部按键和鼠标按钮。

目标 Agent 是 Windows 用户会话程序，不是系统服务。它不能控制安全桌面、UAC 安全提示或登录前界面。

## BLE HID

BLE 使用 NimBLE、Just Works 配对和绑定机制，对外提供同一报告映射中的键盘 Report ID 1 与相对鼠标 Report ID 2。设备没有屏幕和输入键盘，因此不会显示或要求输入配对码；烧录后在目标设备的蓝牙设置中搜索配置的设备名并确认配对即可。

固件已启用 NimBLE 绑定密钥 NVS 持久化，完成一次成功配对后，开发板正常重启会继续使用同一组 LTK/IRK 自动重连。升级自未持久化绑定密钥的旧固件时，目标设备仍可能保存开发板已丢失的旧 LTK；此时系统界面会在“已配对”和“已连接”之间反复切换。需要在目标设备删除/忽略 `HidBridge Keyboard Mouse`，关闭再打开蓝牙后重新配对一次。

清除开发板 NVS 会同时删除 BLE 绑定密钥、网页保存的 Wi-Fi 和 Wi-Fi 驱动状态；下次启动会自动回到配网模式，并且所有目标设备都需要删除旧配对后重新配对。


## 原生 USB CDC 输入

当前固件把 `ESP32-S3 USB` 枚举为 `CDC + HID` 复合设备。CDC COM 口与板载 CH340 的 USB-to-UART 使用完全相同的二进制协议、`DeviceProbe` / `DeviceHello` 握手和输入租约。主机配置保持 `portName: "auto"` 时会枚举所有 COM 口并选择能够正确响应握手的端口，因此不需要为原生 USB CDC 增加新的 EXE 配置。

当原生 USB HID 与 BLE 同时连接时，CDC 只负责输入传输，不决定输出目标；BLE 加密就绪后仍按 BLE 优先策略接管，BLE 断开后回退 USB HID。使用“原生 USB CDC 输入 + BLE 输出”时，应先等 BLE 加密就绪并确认蓝灯，再开启主机转发；否则回退中的 USB HID 会把报告送回连接 CDC 的输入电脑。
