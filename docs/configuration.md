# 连接与安全配置

## 输出组合

固件支持以下输出后端：

- USB HID：启动后 1.5 秒内检测到 UART 有效协议帧时启用，枚举为键盘与相对触摸板 HID-only。
- BLE HID：默认编译启用，设备名可配置。
- Wi-Fi Target Agent：实现代码保留，当前统一运行时开关关闭，不创建网络连接或输出任务。

USB HID 与 BLE HID 同时在线时，先连接并成为活动输出的链路保持锁定，另一链路随后连接不得抢占；USB HID 端点连续 100 ms 不可发送时视为失活并切换到仍在线的 BLE，不依赖可能滞留为真的 TinyUSB `mounted` 状态。切换旧目标和新目标时都会执行 `ReleaseAll`。当前不会向 Wi-Fi Target Agent 镜像报告。

电脑到开发板只能有一个活动输入租约。原生 USB CDC 在启动期未检测到 UART 协议时先出现；检测到 UART 时原生 USB 为 HID-only。若有效 UART 协议帧在 1.5 秒窗口后才到达，固件会安全释放并自动重启一次，使持续探测的 UART 在下一启动窗口内触发 HID-only；已经是 HID-only 时不会循环重启。活动会话断开或超过租约时间时，开发板会向当前 USB/BLE 输出后端发送 `ReleaseAll`。Wi-Fi 输入代码保留但当前不启动。

## 配置开发板

进入 ESP-IDF 环境后打开项目配置：

```powershell
Set-Location D:\ESP32-S3-HID-Bridge\firmware
. ..\scripts\Enter-EspIdf.ps1
idf.py menuconfig
```

在 `HID Bridge` 菜单配置。当前交付配置中的 Wi-Fi 统一运行入口被 `firmware/main/runtime_features.h` 关闭，因此下列 Wi-Fi 项仅作为保留实现说明：

- `Wi-Fi SSID` 和 `Wi-Fi 密码`：仅作为可选的首次启动回退值，通常留空并使用网页配网。
- `启用 SoftAP 网页配网`：默认启用。
- 配网热点密码：默认 `hidbridge`，正式部署前建议修改。
- 自动进入配网的连接失败时间：默认 30 秒。
- 配网按钮 GPIO 和长按时间：ESP32-S3-DevKitC-1 默认使用 GPIO0/BOOT，长按 5 秒。
- `Wi-Fi 输入预共享密钥`：电脑通过 Wi-Fi 连接开发板时使用，至少 16 字节。
- `通过 Wi-Fi 输出到目标 Agent`：需要网络目标端时启用。
- `目标 Agent IPv4 地址`、端口和独立的预共享密钥。
- `BLE HID 设备名称`。
- `启用 RGB 状态指示灯` 和其 GPIO：默认启用并使用 ESP32-S3-DevKitC-1 板载 RGB 灯的 GPIO48。灯色按当前活动输出显示，而不是按是否插入 USB 线显示：USB HID 为绿灯，BLE HID 为蓝灯，无活动输出时红灯每 500 ms 闪烁。USB 与 BLE 同时在线时仍显示先成为活动输出的链路对应颜色。
- 输入租约超时时间，默认 1500 ms。

编译选项保存在被 Git 忽略的 `firmware/sdkconfig`。当前镜像不启动 Wi-Fi、SoftAP 配网或 Target Agent；项目仍保留相关实现和配置项，后续恢复时必须同步打开统一运行时开关并重新进行安全与真实链路验收。

## SoftAP 网页配网（保留实现，当前禁用）

> 当前构建不会启动 SoftAP，也不会响应 BOOT 长按进入配网。以下内容仅说明未来重新启用 Wi-Fi 运行入口后的既有流程。

重新启用后，不需要在每次更换 Wi-Fi 时重新构建或烧录。以下任一条件会进入配网模式：

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

主机 EXE 默认开启 UDP 模拟输入监听，默认地址为 `0.0.0.0:24814`。该入口只把相对移动和滚轮增量注入现有的 500 Hz 鼠标报告链路；它不会直接移动运行主机上的 Windows 光标。只有按 `HOME` 开启同步后命令才会被转发，同步关闭、窗口关闭或程序退出仍会执行 `ReleaseAll` 并解除鼠标锁定。

如需覆盖默认监听地址或端口，在被 Git 忽略的 `bridge.local.json` 中设置：

```json
{
  "remoteInputBindAddress": "127.0.0.1",
  "remoteInputPort": 24814,
  "hostLogPath": "artifacts/host-runtime-{timestamp}.log"
}
```

- `remoteInputBindAddress` 为 `0.0.0.0` 时监听全部 IPv4 网卡；只做本机测试可填 `127.0.0.1`。
- 不再校验预共享密钥，也不要求 `token` 字段；旧调用方即使继续携带 `token`，该未知字段也会被忽略。
- 默认监听全部 IPv4 网卡且无身份认证。Windows 防火墙应限制可信来源；不要把该端口暴露到公网。
- 单个 UTF-8 JSON 数据报格式：`{"dx":12,"dy":-4,"wheel":0,"pan":0}`。`dx/dy` 范围为 `-32768..32767`，`wheel/pan` 范围为 `-128..127`，四个增量不能全为零。
- 每条非零 UDP 命令从到达时起立即按整数余数分摊到固定 10 个未来发送槽，每槽 2 ms。首条快速移动也会从第一槽开始渐进输出，不再存在“首个 100 ms 窗口整包直出”。
- 后续命令直接叠加到同一组环形未来槽，而不是串行追加命令队列。连续输入时计划尾部始终最多 10 槽（20 ms），停止输入后最多再输出 9 槽，不会随持续时间增长到旧实现的约 200 ms 积压。
- X/Y/垂直滚轮/横向滚轮分别进行整数分摊，每条命令在 10 槽中的代数和严格等于原值。新的空闲计划从首槽响应；当多条小步命令在同一计划内重叠时，整数余数会轮转到不同槽，避免全部集中到首槽。
- 突发输入会在固定 10 槽中按槽合并，内存和计划延迟不会随数据报数量增长。若单槽累计值超过 HID 报告字段范围，500 Hz 报告泵仍会按字段上限分批发送，这是协议数值上限而不是平滑队列延迟。
- 该功能只处理 UDP 公共输入；未开启“模拟 UDP”的实体鼠标仍直接进入原有 500 Hz 聚合路径。关闭 `HOME`、断线或退出会清空尚未发送的 10 槽状态并执行 `ReleaseAll`。
- BLE 输出仍受固件 10 ms 发送节拍限制：10 个主机 2 ms 槽通常会被合并为约 2–3 个 BLE 报告（取决于相位和连接调度），USB HID 则可保留主机 2 ms 节拍。关闭主机平滑不会关闭 BLE 自身的 10 ms 合并。
- BLE 广播名为 `Keyboard with Touchpad`，Appearance 为 Keyboard；Windows 对既有配对可能缓存旧名称，验证新名称时应删除旧配对后重新扫描。

### UDP 平滑开关

主机窗口顶部提供“UDP 平滑”复选框，默认开启且只保存到当前运行会话。该开关用于对照固定 20 ms 低延迟分摊：

- 开启时，真实网络 UDP 和模拟 UDP 都进入 `UdpMouseSmoother`，每条命令分摊到固定 10 个 2 ms 槽。
- 关闭时，两类 UDP 移动都跳过 `UdpMouseSmoother`，直接进入 500 Hz 报告聚合；模拟 UDP 的有限频率分桶或无上限逐事件语义仍然保留。
- 同步开启时该开关禁用，必须先按 `HOME` 关闭同步再切换，然后重新开启同步。关闭同步会按既有会话边界清理输入状态并发送 `ReleaseAll`，因此不会混合旧平滑队列与新直通数据，也不会为了切换而静默丢弃活动会话中的位移。
- 该开关不改变实体鼠标直接输入、鼠标按钮、UDP 协议格式、串口、USB HID 或 BLE 固件 10 ms 节拍。关闭平滑也不代表 BLE 输出可达到 500 Hz。
- 重启主机后开关恢复为开启。A/B 时应使用“关闭 HOME → 切换 → 开启 HOME”的独立会话，并重复同一轨迹。

### 模拟 UDP 输入开关

主机窗口顶部提供“模拟 UDP”开关和频率列表，列表固定为 `30/60/100/140/200/500 Hz/无上限`，默认选择 `100 Hz`，开关默认关闭。

- 频率只影响实体鼠标转换为模拟 UDP 命令之前的分桶频率。例如选择 `100 Hz` 时，主机把连续 10 ms 内的实体鼠标 X/Y、垂直滚轮和横向滚轮分别求和，生成最多一条模拟 UDP 命令。
- 选择 `500 Hz` 时分桶周期为 2 ms；选择“无上限”时不等待定时分桶，每一个原始鼠标移动/滚轮事件都立即形成一条内部模拟 UDP 命令。无上限仅表示模拟源不限频，不绕过 UDP 平滑开关、500 Hz 主机报告上限或 BLE 10 ms 节拍。
- 模拟命令生成后进入与真实网络 UDP 完全相同的公共后续链路；“UDP 平滑”开启时两者都经过 `UdpMouseSmoother`，关闭时两者都直接进入 500 Hz `MouseReportPump`，后续不再区分来源。
- 频率列表不会限速真实网络 UDP，也不会修改 UDP 平滑器固定的 10 槽/20 ms 窗、主机 500 Hz 报告频率或 BLE 固件 10 ms 节拍。
- 鼠标按钮不在当前 UDP JSON 协议字段中，因此按钮转换保持即时发送；移动和滚轮才按所选频率整合。这样左右键记录触发和 `ReleaseAll` 不会增加一个分桶周期的延迟。
- 切换频率或关闭模拟开关时，当前已整合的非零移动会先进入公共 UDP 平滑链路，避免丢失；关闭 `HOME`、断线或退出时则按安全策略丢弃尚未发送的模拟缓冲并执行 `ReleaseAll`。
- 没有实体鼠标增量的时间桶不会生成全零模拟命令。

D:\ESP32-S3-HID-Bridge\tools\send-remote-mouse.ps1 只是可选的命令行示例客户端，不是接口运行依赖。主机 EXE 只接收 UDP 数据报；任何支持 UDP 的软件都可以直接向主机地址和端口发送相同的 UTF-8 JSON。连续高频发送时应复用同一个 UDP socket，避免每条移动命令都重新创建连接对象。

项目提供发送示例：

```powershell
.\tools\send-remote-mouse.ps1 `
  -HostAddress 192.168.1.20 `
  -Port 24814 `
  -Dx 25 -Dy -10
```

## 鼠标移动记录与分析图

在 `HOME` 同步开启期间，同时按住鼠标左键和右键即可开始记录。记录点位于主机 500 Hz `MouseReport` 的实际提交边界，因此触发后由实体鼠标和 UDP 接口产生、并实际提交给传输层的移动报告都会进入同一份记录。

- 只记录 `x` 或 `y` 非零的移动报告；按钮变化但 `x/y` 全零的报告不计为样本。
- `x`、`y` 保留实际发送报告中的正负号，因此分析图可以区分左右和上下方向。
- 左右键全部松开后开始 3 秒倒计时；倒计时内重新按下任一左右键会继续同一份记录。
- 松开超过 3 秒后自动停止，在新窗口中显示分析图：左侧为 X 有符号值，时间顺序轴竖直并向上增加，负值在零轴左侧、正值在右侧；右侧为 Y 有符号值，时间顺序从左向右增加，负值在零轴下方、正值在上方。
- 图表不做平均、平滑或分桶降采样，按照发送顺序连接每一个原始 X/Y 样本；样本数明显多于图表像素时，多个原始点会显示在相同或相邻像素中。
- 分析图同时保存到主机 EXE 目录下的 `artifacts/mouse-movement-{timestamp}.png`。

主机窗口日志与 `Console.Out/Console.Error` 会按行实时追加到 `hostLogPath`；默认文件名为 `artifacts/host-runtime-{timestamp}.log`。设备日志由窗口下拉框控制：默认“精简日志（高性能）”，需要采集原始串口细节时临时切换到“完整日志（排障）”。

## 电脑通过 Wi-Fi 连接开发板（保留实现，当前禁用）

> 当前主机端会拒绝 `transport=wifi`。以下配置仅供未来恢复该功能时参考。

恢复运行入口后，在 `bridge.local.json` 中设置：

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

## 开发板通过 Wi-Fi 连接 Windows 目标设备（保留实现，当前禁用）

> 当前固件不会初始化或连接 Target Agent。以下步骤仅供未来恢复该输出后端时参考。

恢复运行入口后，在目标 Windows 设备上复制配置：

```powershell
Copy-Item .\target\HidBridge.TargetAgent\agent.json .\target\HidBridge.TargetAgent\agent.local.json
dotnet run --project .\target\HidBridge.TargetAgent\HidBridge.TargetAgent.csproj -c Release
```

`agent.local.json` 中的 `presharedKey` 必须和固件的目标 Agent 密钥一致。目标 Agent 只接受一个活动连接，把键盘报告和相对鼠标报告转换为 Windows `SendInput`。开发板断开或心跳超时后，Agent 会释放全部按键和鼠标按钮。

目标 Agent 是 Windows 用户会话程序，不是系统服务。它不能控制安全桌面、UAC 安全提示或登录前界面。

## BLE HID

BLE 使用 NimBLE、Just Works 配对和绑定机制，对外提供同一报告映射中的键盘 Report ID 1 与相对鼠标 Report ID 2。设备没有屏幕和输入键盘，因此不会显示或要求输入配对码；烧录后在目标设备的蓝牙设置中搜索配置的设备名并确认配对即可。

固件已启用 NimBLE 绑定密钥 NVS 持久化，完成一次成功配对后，开发板正常重启会继续使用同一组 LTK/IRK 自动重连。升级自未持久化绑定密钥的旧固件时，目标设备仍可能保存开发板已丢失的旧 LTK；此时系统界面会在“已配对”和“已连接”之间反复切换。需要在目标设备删除/忽略 `HidBridge Keyboard Mouse`，关闭再打开蓝牙后重新配对一次。

清除开发板 NVS 会同时删除 BLE 绑定密钥及历史 Wi-Fi 数据。当前 Wi-Fi/SoftAP 入口关闭，因此重启后不会进入配网模式；BLE 目标设备仍需要删除旧配对后重新配对。


## 原生 USB CDC 输入

固件启动后先监听 UART 1.5 秒。没有收到有效桥接协议帧时，`ESP32-S3 USB` 以 `VID:PID=303A:4001`、产品名 `HID Bridge CDC` 枚举为 CDC-only。CDC COM 口与板载 CH340 使用相同的二进制协议、`DeviceProbe` / `DeviceHello` 握手和输入租约；`portName: "auto"` 可自动发现。

收到有效 UART 协议帧时，原生 USB 改为 `VID:PID=303A:4004`、产品名 `USB Keyboard with Touchpad` 的 HID-only 设备，CDC 不会出现。HID Report ID 1 为键盘，Report ID 2 为五键、16 位相对 X/Y、Wheel/Pan 指针；该“触摸板”属于相对指针兼容模式，不是 Precision Touchpad。

选择结果保持到下次复位。仅插入没有协议通信的 UART 线无法被固件识别为连接；切换拓扑后必须复位，并确保控制端在 1.5 秒窗口内发送或不发送 UART 协议。
