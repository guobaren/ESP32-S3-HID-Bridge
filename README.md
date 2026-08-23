# ESP32-S3 HID Bridge

把 Windows 电脑的键盘和鼠标事件，经 ESP32-S3-DevKitC-1 转换成独立的键盘与相对触摸板 HID，输出到手机、平板、嵌入式设备或其他项目。当前原生 USB 固定枚举为键盘 + 相对触摸板 HID，不依赖 EXE 是否运行或 UART 握手；BLE HID 保持可用。Wi-Fi 输入、配网和 Target Agent 输出代码暂时保留但不启用。

## 功能特性

- Windows 全局键盘/鼠标捕获：低级钩子 + Raw Input，1000 Hz / 1 ms 鼠标报告聚合上限
- 串口自动发现（COM 改变后自动重连）与随机数设备握手
- 原生 USB：固定枚举「键盘 + 相对触摸板」HID-only，不依赖启动期 UART 帧
- BLE HID 键盘/鼠标输出，NimBLE Just Works 配对 + 绑定密钥持久化
- USB/BLE 双输出活动链路锁定与 100 ms 失活切换，切换前后自动 ReleaseAll
- 局域网 UDP 模拟鼠标输入（默认 0.0.0.0:24814，20 ms 平滑分摊）
- 宏（多段脚本）与 Lua 脚本（OnEvent 事件模型）
- 鼠标移动记录与分析图（左右键同按触发）
- 固件刷写双入口：设置页本地刷写 + 本机 HTTP 接口，均使用内置独立 esptool，无需 Python
- 板载 RGB 状态灯（USB 绿 / BLE 蓝 / 无活动红灯闪烁）

## 数据路径

```text
Windows 键盘/鼠标
        │ 键鼠低级钩子 + 鼠标 Raw Input
        ▼
HidBridge.Host
        │ USB-to-UART/CH340 控制输入，自动发现与二进制帧握手
        ▼
ESP32-S3-DevKitC-1
        ├─ 原生 USB OTG，固定键盘 + 相对触摸板 HID
        ├─ BLE HID ────────────────> 手机/电脑
```

开发板支持两种有线主机输入方式：

- **USB-to-UART**：CH340 COM 口接收主机端生成的 HID 报告。
- **ESP32-S3 USB**：当前始终枚举为 `USB Keyboard with Touchpad`（HID-only）。UART/CH340 是正式的主机控制输入；固件中保留的 CDC 输入代码不再参与当前 USB profile 选择，也不会因晚到 UART 帧重启切换。

USB HID 与 BLE HID 同时可用时，先连接并成为活动输出的链路保持锁定，另一链路不得抢占；活动链路连续 100 ms 不可发送时切换到仍在线的另一链路，切换前后都执行 ReleaseAll。BLE 已活动时 USB 恢复不会抢占，BLE 断开后才按可用性回退到 USB。官方 DevKitC-1 支持两个 USB 端口同时供电；第三方兼容板需先核对原理图，确认两端口间没有 VBUS 回灌路径。

## 架构与生命周期

### 主机端职责

- `HidBridge.Host` 使用 `WH_KEYBOARD_LL`、`WH_MOUSE_LL` 和鼠标 Raw Input 捕获实体输入；捕获线程只做快速入队，独立分发线程负责状态更新、Lua/宏事件和报告发送。
- `MouseReportPump` 以 1000 Hz / 1 ms 为发送上限，连续相对移动在队列中合并，按钮、滚轮和键盘边沿保持顺序；Windows 调度不保证每份报告严格间隔 1 ms。
- `SerialBridge` 通过 `DeviceProbe`/`DeviceHello` 自动发现串口并建立二进制会话；主机负责发送 `ReleaseAll`、维护输入租约和记录诊断日志。
- 固件刷写设置页与 Loopback API 共用校验和刷写服务，但各自提供本机 JSON 清单路径；EXE 不内嵌固件镜像。
- 普通键鼠捕获转发路径不创建虚拟 HID 设备；UDP、Lua/宏等自动化路径属于主动输出路径，不能据此推断为“完全没有本机输入注入”。

### 固件端职责

- UART 接收主机报告和控制帧；原生 USB 固定提供 HID 输出；BLE 提供备用 HID 输出。
- 输出选择器采用单一活动租约：USB 在线时禁止 BLE 抢占，活动链路断开或连续不可用约 100 ms 后才允许切换；切换、断线、复位和退出路径都释放键盘与鼠标状态。
- Wi-Fi/SoftAP/Target Agent 代码保留但由 Kconfig 与 `HID_BRIDGE_WIFI_RUNTIME_ENABLED=0` 双重闸门禁用；当前正式链路不是未经认证的 Wi-Fi 输入。

### HID 报告

| Report ID | 当前用途 | 长度/内容 |
|---|---|---|
| `1` | Boot Keyboard | 8 字节键盘报告，含修饰键、保留字节和最多 6 个按键 |
| `2` | 相对触摸板鼠标 | 7 字节报告，使用有符号 16 位相对 X/Y，另含按钮、滚轮和横滚轮 |

报告路径中的累计位移使用更宽的内部整数，最终按 HID 字段范围分块；USB/BLE 端点完成只证明固件完成发送，不证明目标系统或目标应用已经消费报告。

### 验证边界

构建、策略测试和主机自检分别记录；它们不能替代真实 USB 枚举、BLE 配对/重连、插拔顺序、Raw Input 和被控端光标行为验收。真实链路测试必须保留设备日志、目标端结果和对应版本/镜像 SHA-256。

## 快速开始

### 1. 烧录固件

要求：ESP-IDF 6.0.2（位于 .esp-idf/，不提交 Git）、ESP32-S3-DevKitC-1。

```powershell
Set-Location D:/ESP32-S3-HID-Bridge/firmware
. ../scripts/Enter-EspIdf.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p <实际串口> flash
```

激活脚本只修改当前 PowerShell 会话；重新打开终端后需要再次执行。

### 2. 运行主机端

方式 A：直接运行根目录单文件 exe（Release 构建产物，自包含，仅需 .NET 8 Desktop Runtime）：

```powershell
Set-Location D:/ESP32-S3-HID-Bridge
./HidBridge.Host.exe
```

方式 B：源码运行 / 重新构建：

```powershell
Set-Location D:/ESP32-S3-HID-Bridge/host/HidBridge.Host
dotnet run -c Release
```

默认配置自动发现开发板串口，无需填写 COM 号。需要固定串口或调整参数时，复制本机配置再修改（该文件被 Git 忽略）：

```powershell
Copy-Item bridge.json bridge.local.json
```

### 3. 快捷键

- `HOME`：启用 / 停止向目标设备转发。
- `END`：安全释放所有按键并退出。

同步开启后，除 HOME/END 外的键盘、组合键和鼠标输入都会被主机拦截并转发到对端；同步关闭后恢复本机输入。

## 固件刷写

控制软件可以在不退出进程的情况下释放串口刷写固件，刷写完成后自动恢复连接。两个入口**最终都调用内置的独立版 esptool.exe**（构建时嵌入 exe，目标机无需安装 Python / ESP-IDF 环境；首次刷写时解压到 %LOCALAPPDATA%/HidBridge/embedded 缓存）。

EXE 不内嵌固件。设置页和远程 API 各自指定运行控制软件电脑上的 JSON 清单；两者只共用清单校验、esptool 调用和串口恢复逻辑。

### 方式一：设置页本地刷写（手动，推荐）

1. 运行 HidBridge.Host.exe，确认日志显示已自动发现并连接串口。
2. 打开「设置」页，底部「本地固件刷写」区，点「选择 JSON」。
3. 选择三段完整刷写清单；清单里的相对镜像路径按 JSON 所在目录解析，可按标准子目录（bootloader/、partition_table/）或与清单同目录平铺放置。
4. 点「确定」→ 弹窗二次确认 → 打开小日志窗口实时显示 esptool 进度（百分比、哈希校验、RTS 复位），完成后显示「刷写完成」。

### 方式二：远端刷写接口（远程调用）

接口固定监听本机 127.0.0.1，默认端口 24815，不接受局域网地址直接连接；需要远程刷写时，在运行控制软件的电脑上通过既有远程控制发起本机请求。

**启用**：主界面「设置」页勾选「启用本机固件刷写接口」（保存后立即生效，接口只绑定 Loopback）。

**启动刷写**：

```powershell
$headers = @{ 'X-HidBridge-Action' = 'flash-firmware' }
$body = @{ manifestPath = 'D:\ESP32-S3-HID-Bridge\firmware\build\flasher_args.json' } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:24815/api/v1/firmware/flash' -Headers $headers -ContentType 'application/json' -Body $body
```

**查询状态**：

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:24815/api/v1/firmware/status'
```

状态 state 取值：`idle`（未执行）/ `running`（释放串口、刷写或恢复中）/ `succeeded`（三段哈希校验 + RTS 复位 + 串口恢复通过）/ `failed`（原因见 message，退出码见 exitCode）。重复提交时不会并发执行，返回 HTTP 409 和当前任务状态。

**安全边界**：只绑定 127.0.0.1；POST 必须携带确认头，并在 JSON 正文中指定本机 `manifestPath`；不接受固件上传、串口名或命令行参数；清单必须且只能包含 0x0、0x8000、0x10000 三段且镜像位于 JSON 所在目录内；刷写期间独占串口并暂停同步，程序退出时等待 esptool 安全结束。成功后日志输出「固件刷写最终摘要」（三段 SHA-256、设备校验计数、RTS 复位结果）。

### 内置 esptool 的重新构建

内置 esptool 由 scripts/build-embedded-esptool.ps1 用 PyInstaller 生成（独立单文件，v5.3.1，约 14MB），产物放在 host/HidBridge.Host/EmbeddedAssets/（不提交 Git），构建时作为嵌入资源打进 exe：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-embedded-esptool.ps1
dotnet build host/HidBridge.Host/HidBridge.Host.csproj -c Release
```

固件更新只需替换本地 JSON 及其引用镜像，不需要重新构建 EXE。

## 宏

宏是主机端按键脚本，保存在 profiles/<配置名>/ 下（宏文件为 .txt，profile.json 记录触发键与模式），在设置页选择激活的配置（automation.settings.json 的 ActiveProfile）。触发键示例：`f13`、`ctrl+f1`、`mouse_side1`。仓库提供完整示例配置（含宏与 Lua），见 [profiles.example/](profiles.example/)，复制到 profiles/ 即可使用。

### 语法

每行一条命令，格式 `命令(参数)`，参数用逗号分隔，可用引号包裹，`#` 开头为注释：

| 命令 | 参数 | 说明 |
|---|---|---|
| `move` | dx, dy | 相对移动鼠标 |
| `moveto` | x, y | 移动到绝对坐标 |
| `mouse` | button, state | 鼠标键（1 左 / 2 右 / 3 中 / 4 / 5），state 1 按下 0 松开 |
| `keydown` | key | 按住按键 |
| `keyup` | key | 松开按键 |
| `keypress` | key [, hold_ms] | 点按按键，可带按住时长 |
| `wheel` | amount | 垂直滚轮增量 |
| `delay` / `sleep` | ms | 延时 |
| `randsleep` / `randdelay` | min, max | 随机延时（毫秒） |

示例：

```text
# 按住 F13 连点
keydown(f13)
delay(50)
mouse(1, 1)
randsleep(30, 60)
mouse(1, 0)
keyup(f13)
```

### 运行模式

| 模式 | 行为 |
|---|---|
| `once` | 触发一次执行一遍 |
| `toggle` | 按一次开始循环，再按一次停止 |
| `hold_loop` | 按住循环，松开停止 |
| `staged` | 分段脚本：按下 / 按住 / 松开三段 |

staged 模式用段标签分段：

```text
[on_press]
mouse(1, 1)

[while_hold]
randsleep(40, 80)
move(0, 5)

[on_release]
mouse(1, 0)
```

## Lua 脚本

每个配置可携带一段 Lua 脚本（profile.json 的 lua_script_text），激活配置时自动运行。采用鼠标宏常见的 OnEvent 事件模型：按键/鼠标事件到达时调用 `OnEvent(event, arg)`，event 为 `pressed` / `released`，arg 为按键名（字符串，如 "a"、"f13"、"num0"）或鼠标键数字（1 左 / 2 右 / 3 中）。

### 可用 API

| API | 说明 |
|---|---|
| `move(x, y)` | 相对移动鼠标 |
| `moveto(x, y)` | 绝对坐标移动 |
| `mouse(button, state)` | 鼠标键按下/松开（1 左 / 2 右 / 3 中） |
| `wheel(amount)` | 垂直滚轮 |
| `keydown(key)` / `keyup(key)` / `keypress(key, hold_ms)` | 按键控制 |
| `sleep(ms)` / `Sleep(ms)` | 延时 |
| `randdelay(min, max)` / `randsleep(min, max)` | 随机延时 |
| `IsPressed(key)` | 查询按键当前是否按住（可轮询） |
| `DebugLog(...)` | 输出到 Lua 诊断日志 |
| `ClearLog()` | 清空 Lua 日志 |
| `VK_CODES` | 虚拟键码表 |

示例：

```lua
function OnEvent(event, arg)
    if event == "pressed" and arg == 1 then
        while IsPressed(1) do
            move(0, 2)
            sleep(10)
        end
    end
    DebugLog("event=%s arg=%s", tostring(event), tostring(arg))
end
```

Lua 详细诊断日志写入 `artifacts/automation-runtime-<时间戳>.log`；Lua/宏 UI 只显示初版的普通简略日志，文件中另外保留 `LuaEvent`、`LuaOutput` 及开始/完成序号，便于分析长按、松开和连点问题。

## UDP 模拟鼠标接口

主机 EXE 默认监听 UDP 0.0.0.0:24814，接收 UTF-8 JSON 相对移动命令（仅 HOME 开启同步后转发，不直接移动本机光标）：

```json
{"dx":12,"dy":-4,"wheel":0,"pan":0}
```

- dx/dy 范围 -32768..32767，wheel/pan 范围 -128..127，四项不能全零。
- 命令分摊到固定 10 个 2 ms 槽（20 ms 低延迟平滑），连续输入在槽内合并。
- 无身份认证：仅应在受信任网络使用；只做本机测试可把 remoteInputBindAddress 改为 127.0.0.1。
- 可选发送示例：tools/send-remote-mouse.ps1 -HostAddress 192.168.1.20 -Port 24814 -Dx 25 -Dy -10。
- 主界面「UDP 平滑」开关可临时关闭分摊做 A/B；「模拟 UDP」开关可把实体鼠标按 30/60/100/140/200/500 Hz/无上限 分桶成模拟 UDP 源。

## 鼠标移动记录与分析图

HOME 同步开启期间同时按住鼠标左键+右键开始记录；左右键松开 3 秒后自动停止并弹出分析图（X 有符号值时间序列 + Y 有符号值时间序列），同时保存到 artifacts/mouse-movement-<时间戳>.png。记录点位于 1000 Hz 报告实际提交边界，不做平滑或降采样。

## 主机配置参考（bridge.local.json）

```json
{
  "transport": "serial",
  "portName": "auto",
  "baudRate": 921600,
  "remoteInputEnabled": true,
  "remoteInputBindAddress": "0.0.0.0",
  "remoteInputPort": 24814,
  "hostLogPath": "artifacts/host-runtime-{timestamp}.log",
  "deviceLogPath": "artifacts/host-serial-{timestamp}.log",
  "showDeviceLogInUi": false,
  "firmwareUpdateApiPort": 24815,
  "firmwareFlashBaudRate": 460800,
  "firmwareFlashTimeoutSeconds": 180
}
```

- transport 只能是 serial（wifi 实现保留但当前被功能闸门拒绝）。
- portName 为 auto 时自动扫描 COM 并完成随机数握手；固定串口模式不自动扫描。
- hostLogPath / deviceLogPath 支持 {timestamp} 占位符；deviceLogPath 置空可关闭设备日志。
- showDeviceLogInUi 为启动默认值：false 精简模式 / true 完整日志模式；窗口内可随时切换。
- EXE 只内嵌刷写工具，不内嵌固件；设置页选择本机 JSON 清单，远程 API 在请求正文中单独指定本机 JSON 路径。

### 串口、日志与输入租约

- `portName: "auto"` 会扫描串口并发送 `DeviceProbe`/`DeviceHello`；固定 COM 只适用于明确知道设备端口的环境。默认波特率为 `921600`。
- 同一个 COM 口不能同时由 Host、`idf.py monitor` 或其他串口工具打开；刷写前必须释放串口，刷写结束后再恢复会话。
- `host-runtime-{timestamp}.log` 保存主机运行日志，`host-serial-{timestamp}.log` 保存设备日志，`automation-runtime-{timestamp}.log` 保存 Lua/宏详细事件和输出时间线。
- `showDeviceLogInUi` 默认关闭。完整设备日志只用于短时排障；文件写入、UI 投递和实时输入线程相互隔离，UI 采用批量刷新和有界文本。
- 输入租约默认约 `1500 ms`；停止转发、COM 断开、USB/BLE 切换、HOME/END 和进程退出都必须执行 `ReleaseAll`。

### ESP-IDF 与 Wi-Fi/SoftAP

- 固件构建前进入项目提供的 ESP-IDF 终端，执行 `idf.py set-target esp32s3` 和 `idf.py build`；默认 USB/BLE 拓扑不需要启用 Wi-Fi。
- 设备配置使用仓库中的 `sdkconfig`/`sdkconfig.defaults`；BLE 绑定持久化依赖 `CONFIG_BT_NIMBLE_NVS_PERSIST`，板载 RGB 使用 GPIO48，当前约定为 USB 绿、BLE 蓝、无活动红灯闪烁。
- Wi-Fi 输入、SoftAP 配网和 Target Agent 后端目前只保留代码，不作为可用功能发布。运行时还必须保持 `HID_BRIDGE_WIFI_RUNTIME_ENABLED=0`，不能仅凭编译产物存在就认为 Wi-Fi 已启用。
- 未来恢复 SoftAP 前，需要重新设计认证、PSK/AES-256-GCM、计数器/时间窗防重放、心跳超时、重连和 `ReleaseAll`；不能直接开放当前无认证的 UDP 规则到 Wi-Fi。
- 规划中的配网入口可使用临时 `HID-Bridge-Setup-XXXX` 热点和 `192.168.4.1`，但当前不应把该流程当作已实现或已验收功能。

### UDP 平滑与安全边界

- 默认监听 `0.0.0.0:24814`，JSON 字段为 `dx`、`dy`、`wheel`、`pan`；范围分别为 `-32768..32767`、`-128..127`，四项全零的数据报拒绝并应由发送端在复用 socket 的前提下跳过。
- 平滑器使用固定 20 个 1 ms 槽覆盖最多 20 ms 尾部，整数位移在槽间守恒且不无界积压；关闭“UDP 平滑”只用于 A/B 对比。
- “模拟 UDP”可按 30/60/100/140/200/500 Hz 或无上限生成测试源；模拟结果不能替代真实 USB/BLE 和目标端 Raw Input 验收。
- 当前 UDP 入口没有身份认证、计数器或时间窗防重放，只适用于受信任局域网；本机测试应将 `remoteInputBindAddress` 改为 `127.0.0.1`。

### BLE 维护

- BLE 使用 NimBLE HID、Just Works 配对和绑定密钥持久化；升级固件或更换设备后若反复显示“已配对/已连接”，应在目标系统删除旧配对后重新配对。
- BLE 鼠标路径按约 10 ms 节拍发送合并状态；API 调用率、空口报告率和目标端 Raw Input 频率是三个不同指标，必须分别测量。
- 当前 USB/BLE 活动锁保证后连接链路不抢占；真实冷启动、插回 USB、BLE 断开回退和目标端输入仍需按版本单独验收。

## 常见问题

- **COM 被占用**：主机程序与 idf.py monitor、串口工具不能同时打开同一 COM 口；关闭占用程序后主机会自动重连。
- **手动刷写提示「刷写镜像不存在」**：JSON 引用的 bin 必须位于清单所在目录内（支持 bootloader/、partition_table/ 子目录或平铺）。
- **BLE 配对后反复「已配对/已连接」**：旧固件无绑定持久化，升级后需在目标设备删除/忽略旧配对，重新配对一次。
- **防火墙弹窗**：UDP 24814 与刷写接口 24815 首次监听可能触发 Windows 防火墙提示。
- **exe 无法启动**：需要 .NET 8 Desktop Runtime（x64）。
- **不要输入回路**：不要把开发板原生 USB 口接回同一台电脑测试转发并同时抑制本地输入。

## 安全边界

- UDP 模拟鼠标入口默认开启且无身份认证，默认监听 0.0.0.0:24814，仅限受信任网络。
- 固件刷写接口默认关闭、只绑定 127.0.0.1；不接受固件上传、串口名或命令参数，只接受本机 JSON 清单路径。
- 主机退出、串口断开或切换转发状态时发送 ReleaseAll，防止目标设备卡键。
- 预共享密钥类配置只写入被 Git 忽略的本地文件（bridge.local.json、firmware/sdkconfig 等），不要写入仓库文件。

## 仓库结构

```text
firmware/                 ESP-IDF 固件（build 产物不提交）
host/HidBridge.Host/      主机控制程序（WinForms，.NET 8）
shared/HidBridge.Protocol 主机与固件共享协议
target/                   保留的 Target Agent 输出后端
tests/                    主机/硬件自检程序与固件测试
scripts/                  构建与工具脚本（含内置 esptool 构建）
profiles/                 宏/Lua 配置（运行目录，不提交默认内容）
profiles.example/         示例配置（宏 + Lua，可复制到 profiles/）
docs/                     项目文档（协议、刷写 API、交接与审计记录）
artifacts/                运行日志与产物（不提交）
tools/                    辅助工具（如 UDP 发送示例）
```

主机检查程序（自检，无需连接开发板）：

```powershell
dotnet run --project tests/HidBridge.Host.Checks -c Release
```

协议细节（帧格式、消息类型、握手与 UDP 接口）见 [docs/protocol.md](docs/protocol.md)，固件刷写接口见 [docs/firmware-update-api.md](docs/firmware-update-api.md)；连接、配置和架构说明已整合在本 README。
