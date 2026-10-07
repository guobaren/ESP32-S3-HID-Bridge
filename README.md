# ESP32-S3 HID Bridge

本项目用两块 ESP32-S3 开发板把真实鼠标直通到目标电脑，也可按需运行 Windows Host 进行 Lua/宏控制、网络鼠标输入和设备维护。旧单板方案仍作为独立兼容路径保留，不能与双板操作混用。

命令中的仓库路径默认相对于项目根目录；Markdown 链接相对于当前文档。历史记录中的尖括号外部路径表示可选、不会随仓库分发的证据目录。开发环境与按需安装的依赖见 [依赖说明](docs/dependencies.md)。

## 快速开始

### 只使用双板实体鼠标

1. M、P 两块板都刷入同一版本的 `firmware/dual_proxy` 固件。已经刷好可跳过；尚未刷写时，先按[依赖说明](docs/dependencies.md)准备环境，再按[双板构建与刷写](#构建与刷写双板固件)操作。
2. 板间连接 UART1：M GPIO17 TX→P GPIO18 RX、M GPIO18 RX←P GPIO17 TX，并共地；不要连接两板的 5V 或 3V3。M 需通过 USB-UART/CH340 维护口连接 Windows 电脑或合适的 USB 电源供电，真实鼠标接 M 原生 USB Host 口；P 原生 USB 接目标电脑供电并枚举。
3. 实体鼠标由 M→P 硬件直通；这条路径不需要启动 Host EXE。板卡供电和兼容开发板的更多连接说明见 [M 板与 P 板](#m-板与-p-板)及[双板协议说明](docs/protocol.md)。

### 需要 Lua/宏或网络 UDP 时再运行 Host

1. 将 M 的 USB-UART/CH340 维护口连接到运行 Host 的 Windows 电脑；需要网络 UDP 时，确保发送端可以访问 Host 电脑。
2. 已有 `HidBridge.Host.exe` 时安装 [.NET 8 Desktop Runtime x64](docs/dependencies.md) 后跳过构建。没有 EXE 时，按[Host 构建备注](#host-构建与配置实现备注)从源码生成。
3. 在项目根目录启动 `.\HidBridge.Host.exe`，设置页保持“使用旧版单板通路”未勾选（默认关闭），让 Host 自动发现 M。
4. 只有使用宏或 Lua 时，才在 Host 页面启用、编辑或选择相应自动化配置；默认活动配置是 `Global`。需要网络 UDP 输入时，将发送端指向 Host 电脑的地址和默认 UDP 端口 `24814`。

更多细节：[Host 功能](#host-功能)、[Host 配置与日志](#host-配置与日志)、[宏](#宏)、[Lua](#lua-脚本)、[UDP/kmboxNet](#udp-与-kmboxnet-输入)、[Makcu 接口](#makcu-兼容接口)、[固件刷写](#固件刷写)、[驱动](#windows-ch340ch341-驱动)、[FAQ](#常见问题与安全边界)。

## 当前主线：双板透明鼠标代理

两块 ESP32-S3 开发板协作，让目标电脑看到由 P 板模拟的物理鼠标。M 板连接真实鼠标并读取其 USB 描述符和输入；P 板在目标电脑侧克隆鼠标并转发输入与厂商 HID 通信。

```text
真实鼠标/接收器 ─USB Host─> M 板 ──UART1──> P 板 ─USB Device─> 目标电脑
                                ↑                       ↑
                           UART0/CH340              UART0/开发口
                       软件命令与维护工具             刷写与诊断
```

实体鼠标由 M→P 硬件直通，无需 Host EXE。Host 提供可选的软件鼠标输入、自动化和设备维护功能；实体鼠标路径不依赖这些功能。

### M 板与 P 板

| 板卡 | USB 角色 | 主要连接与职责 |
|---|---|---|
| M（鼠标侧） | USB Host | USB Host 口连接真实鼠标/接收器；采集描述符及输入；通过 UART1 向 P 发送设备信息和数据；UART0/CH340 用于软件命令与维护 |
| P（电脑侧） | USB Device | 原生 USB 连接目标电脑；安装并呈现 M 提供的鼠标克隆；接收 M 转发的输入和厂商 HID 数据 |

两板烧录同一个 `firmware/dual_proxy` 镜像，由运行时协商 M/P 角色。板间 UART1 交叉连接：M GPIO17 TX→P GPIO18 RX，M GPIO18 RX←P GPIO17 TX，并共地。不要把两板的 5V 或 3V3 相连。目标电脑接 P 的原生 USB；真实鼠标接 M 的 USB Host 口。

当前双板协议、统计字段和运行边界见 [docs/protocol.md](docs/protocol.md)。执行状态、已知风险和硬件验收记录见 [docs/交接.md](docs/交接.md) 与 [docs/审计.md](docs/审计.md)。

### 软件输入与平滑

- M/P 转发真实鼠标输入和厂商 HID 通信；无法安全克隆的设备可能保持断开，不能保证所有鼠标型号都兼容。
- Host 的网络 UDP 和 Lua/宏鼠标移动可发送给 M；Host 平滑设置统一作用于这些软件移动。实体鼠标 M→P 硬件直通不经过 Host 平滑或输出灵敏度。
- Makcu/V4 输入与 Host 软件输入使用不同路径；Makcu 自身按 P 端配置平滑，不受 Host 平滑设置影响。不要同时用多个程序打开同一串口。
- 双板构建选项和串口协议边界见[协议说明](docs/protocol.md)。历史串口或目标端检查结果见[审计记录](docs/审计.md)，记录只适用于其中明确注明的固件和测试环境。

### 构建与刷写双板固件

使用 ESP-IDF 6.0.2。双板工程必须从 `firmware/dual_proxy` 构建，不要从旧的 `firmware/` 单板工程生成双板镜像。

```powershell
Set-Location .\firmware\dual_proxy
. ..\..\scripts\Enter-EspIdf.ps1

# 仅全新且尚未设置 target 的构建目录执行
idf.py set-target esp32s3
idf.py build

$port = 'COMx' # 替换成当前板卡串口
idf.py -p $port flash
```

<details>
<summary>备注：双板接口构建选项</summary>

默认 CMake 选项 `DUAL_PROXY_ENABLE_MAKCU_V4_API` 为 `ON`。仅需构建不含 V4 的旧 A5 双板镜像时，可显式执行 `idf.py -D DUAL_PROXY_ENABLE_MAKCU_V4_API=OFF build`；该选项不会改变已经刷入开发板的固件。旧 V3 风格 ASCII 选项与 V4 互斥。

</details>

M/P 的 UART0 速率和可用维护接口取决于两板的固件构建配置。连接前应按[协议说明](docs/protocol.md)核对对应角色和速率；不要让多个程序同时打开同一串口。

只有全新且尚未设置 target 的构建目录，才首次执行 `idf.py set-target esp32s3`。日常构建沿用双板工程已有的 ESP32-S3 target 和 sdkconfig。

生成的镜像与刷写清单位于 `firmware/dual_proxy/build/`。两块板需要使用同一版本，并分别选中各自正确的串口刷写。刷写会复位开发板；操作前确认 M/P 串口对应关系。项目刷写接口说明见 [docs/firmware-update-api.md](docs/firmware-update-api.md)。

## 旧方案：单板固件

`firmware/` 根目录工程是早期单 ESP32-S3 方案，主机软件经 USB-UART/CH340 把软件输入交给单板，单板自行输出固定 HID 设备。它不采集真实鼠标 USB 描述符，也不提供 M/P 双板克隆链路；其串口协议、镜像和连接方法均与 `firmware/dual_proxy` 分开。

| 子系统 | 旧单板方案记录 | 与双板主线的关系 |
|---|---|---|
| USB 输出 | 固定键盘 + 相对触摸板 HID | 不是动态克隆物理鼠标；不用于双板镜像 |
| 主机输入 | Host 经 CH340 串口转发软件键鼠输入 | 使用旧版单板模式，不使用 M/P 双板直通 |
| 自动化与网络 | 旧版 Host 支持宏/Lua、UDP/kmboxNet，仅适配原单板固件 | 当前 Host 支持向 M 注入软件鼠标；自动化键盘使用 Win32；网络 UDP 由 Host 适配，双板固件没有软件 KeyboardReport 注入 |
| BLE / Wi-Fi | 支持 BLE HID；Wi-Fi/SoftAP 未启用 | 双板透明代理不依赖这些路径 |
| 构建与验收 | 使用根目录 `firmware/` 的独立配置和产物 | 与双板工程分开构建、刷写和验收，不要交叉使用清单或镜像 |



#### 旧单板接线、构建与 BLE

旧单板通过 USB-UART/CH340 接运行 Host 的电脑，Host 经该串口发送软件输入；开发板的原生 USB 口连接目标电脑，作为旧版固定 HID 输出。它不需要 M/P 两板之间的 UART1 交叉线。

旧单板固件从项目根目录下的 `firmware/` 构建，产物与双板工程分开：

```powershell
Set-Location .\firmware
. ..\scripts\Enter-EspIdf.ps1
# 仅全新且尚未设置 target 的构建目录执行
idf.py set-target esp32s3
idf.py build

$port = 'COMx' # 替换成当前开发板的 USB-UART 串口
idf.py -p $port flash
```

旧单板固件还保留 BLE HID。设备名默认是 `Keyboard with Touchpad`。USB HID 未连接或不可用且固件启用了 BLE 时，板卡会开始广播；在目标电脑的 Windows“蓝牙和设备”中选择添加设备，找到该设备并完成配对。该固件使用无需输入 PIN 的 Just Works 配对并保存 bond。USB HID 可用时 BLE 后端可能不广播；双板固件不提供此 BLE 输出路径。

<details>
<summary>备注：旧单板源码与本地打包</summary>

单板固件和 Host 的历史代码仍保留在仓库；这些功能的当前可构建性与设备可用性需针对旧工程单独验证，不能据此推断已接入双板主线。

旧单板固件和 Host 可通过 `scripts/Prepare-Release.ps1` 生成本地交付目录。该脚本读取 `firmware/build/` 的旧单板 manifest 和镜像，不是双板发布工具；执行 Host Release 构建前应先退出正在运行的 Host，因为构建会更新项目根目录 EXE。

</details>

## Host 功能

Host 是可选的 Windows 桌面工具，提供 Lua/宏自动化、网络鼠标输入、设备维护和本地固件刷写。双板实体鼠标由 M→P 硬件直通，单纯使用实体鼠标时无需运行 Host。

- Host 会监听运行它的 Windows 电脑上的本地键盘和鼠标事件，Lua `IsPressed` 与本机触发可在两种模式下使用。
- 旧版单板模式只适配原单板固件。实体键鼠仅在 HOME 开启时由 EXE 捕获并串口转发；HOME 关闭时 Lua/宏使用本机 Win32 输出。旧版“始终开启 UDP 输出”仅控制 HOME 关闭时网络 UDP 输入是否继续发送。
- 双板模式下，实体鼠标由 M→P 硬件直通，EXE 不重复转发；HOME 不控制这条通路，也不锁定本机光标。Lua/宏鼠标输入优先发给可用的 M 串口，否则回退到运行 Host 的本机；自动化键盘使用本机输出，因为双板固件当前只支持鼠标软件输入。
- 双板模式的网络 UDP 鼠标输入不受 HOME 或旧单板 UDP 开关限制；M 不可用时不会回退到本机光标。旧单板的“UDP 模拟输入测试”只在旧单板、HOME 和测试开关同时启用时生效，不会发送网络 UDP 数据报。

### HOME / END 快捷键

- `HOME`：切换旧单板模式的实体键鼠同步/转发状态。双板实体鼠标由 M→P 硬件直通，不受 HOME 控制。
- `END`：释放 Host 跟踪的输出状态并结束程序。

Host 软件输入与[Makcu 直接调用](#makcu-兼容接口)使用不同通路；直接调用时必须先退出 Host，避免多个程序同时占用 M 的 UART0。

## Host 配置与日志

开发环境依赖、官方来源和安装步骤见[依赖说明](docs/dependencies.md)。Host 使用 .NET 8 桌面应用运行时；运行已发布的单文件 EXE 需要安装 .NET 8 Desktop Runtime x64。

### 运行 Host 与选择配置

在项目根目录启动 `HidBridge.Host.exe`，在“设置”页选择活动配置，按需启用宏、Lua 或 UDP 输入。示例配置位于 `profiles.example/示例配置/`，复制到程序所在目录的 `profiles/` 后即可选择。

日志默认保存在程序目录下的 `log/`，包括 Host、设备和自动化日志，默认各保留 10 个文件。需要手动修改网络监听、日志或本地配置时，展开下方备注。

<details>
<summary>备注：Host 构建与配置实现</summary>

### Host 构建与配置实现备注

从源码生成 Host 需要 .NET 8 SDK，安装位置与依赖见[依赖说明](docs/dependencies.md)。

### 构建 Host

在项目根目录执行：

```powershell
dotnet build .\host\HidBridge.Host\HidBridge.Host.csproj -c Release
```

Release 构建会更新项目根目录的 `HidBridge.Host.exe`。构建前先正常退出正在运行的 Host。

### 运行 Host 与加载配置

可运行构建好的程序，也可从源码启动：

```powershell
# 在项目根目录运行
.\HidBridge.Host.exe

# 或运行源码项目
dotnet run --project .\host\HidBridge.Host\HidBridge.Host.csproj -c Release
```

Host 从当前程序集/EXE 所在目录读取 `bridge.local.json` 和 `bridge.json`。若同目录存在 `bridge.local.json`，就完整使用它；不会与 `bridge.json` 按字段合并，未写入的字段使用代码默认值。默认模板位于 `host/HidBridge.Host/bridge.json`。

若从项目根目录运行根 EXE，可在不覆盖已有本地配置时复制模板：

```powershell
if (-not (Test-Path -LiteralPath '.\bridge.local.json')) {
    Copy-Item -LiteralPath '.\host\HidBridge.Host\bridge.json' -Destination '.\bridge.local.json'
}
```

`remoteInputEnabled` 开启或关闭 Host UDP 输入服务；`remoteInputBindAddress` 控制绑定网卡地址。日志文件名中的 `{timestamp}` 会替换为当前时间，默认日志位于程序目录下的 `log/`。自动化选项（例如活动配置、统一输出灵敏度和旧版 UDP 开关）保存在 `automation.settings.json`，不属于 `bridge.local.json`。

从源码运行时，配置目录是项目根下 `host/HidBridge.Host/bin/.../` 中实际生成程序集所在目录，不是当前工作目录或项目根。`bridge.local.json` 是本机配置，不应提交。Host 自动化设置保存在程序集目录中的 `automation.settings.json`；自动化配置和日志也以该目录为基准。

示例宏与 Lua 配置在 `profiles.example/示例配置/`。复制到程序所在目录的 `profiles/` 后，在 Host“设置”页选择配置。每个配置用 `profile.json` 保存触发键和文件关联；宏正文在 `macros/*.txt`，Lua 正文默认在 `lua/main.txt`。

常用 `bridge.local.json` 字段如下，未列出的字段使用代码默认值：

```json
{
  "transport": "serial",
  "portName": "auto",
  "baudRate": 921600,
  "remoteInputEnabled": true,
  "remoteInputBindAddress": "0.0.0.0",
  "remoteInputPort": 24814,
  "hostLogPath": "log/host/host-runtime-{timestamp}.log",
  "hostLogRetentionCount": 10,
  "deviceLogPath": "log/device/host-serial-{timestamp}.log",
  "deviceLogRetentionCount": 10,
  "showDeviceLogInUi": false,
  "automationLogPath": "log/automation/automation-runtime-{timestamp}.log",
  "automationLogRetentionCount": 10,
  "firmwareUpdateApiPort": 24815
}
```

三个 `*LogPath` 都相对于 Host 程序目录，文件名中的 `{timestamp}` 用于区分每次日志；对应 `*LogRetentionCount` 默认保留 10 个匹配文件，可设为 1..1000。`showDeviceLogInUi` 默认关闭串口设备日志在界面中的显示。完整默认值和可选字段见 `host/HidBridge.Host/bridge.json`。


</details>

## 宏

宏是按键触发脚本，保存在活动配置目录的 `macros/` 子目录中，每个宏一个 `.txt` 文件。触发键可使用键盘键、鼠标侧键或组合键，例如 `f13`、`ctrl+f1`、`mouse_side1`。

每行写一条 `命令(参数)`，参数用逗号分隔；`#` 及其后的内容作为注释。命令名不区分大小写。

| 命令 | 参数 | 说明 |
|---|---|---|
| `move` | dx, dy | 相对移动鼠标；宏参数为整数 |
| `moveto` | x, y | 请求绝对坐标；远端路径会用本机光标位置计算相对差值，不保证目标端精确到达 |
| `mouse` | button, state | 1 左、2 中、3 右、4 后退、5 前进；state 为 1 按下、0 松开 |
| `keydown` / `keyup` | key | 按下或松开键盘键，支持键名或 HID Usage |
| `keypress` | key[, hold_ms] | 点按键；缺省按住 10 ms |
| `wheel` | amount | 输出垂直滚轮增量 |
| `delay` / `sleep` | ms | 可取消延时 |
| `randsleep` / `randdelay` | base, variance | 在 max(0, base−variance) 到 base+variance 之间随机延时 |

示例：

```text
# 按住触发键时循环移动，松开触发键后停止
move(0, 5)
delay(10)
```

支持四种运行模式：

| 模式 | 行为 |
|---|---|
| `once` | 每次触发执行一遍 |
| `toggle` | 按一次开始循环，再按一次停止 |
| `hold_loop` | 按住循环，松开停止 |
| `staged` | 按下时运行按下段；按住循环运行按住段；松开时运行松开段 |

`staged` 模式使用以下段标签：

```text
[on_press]
mouse(1, 1)

[while_hold]
randsleep(40, 20)
move(0, 5)

[on_release]
mouse(1, 0)
```

## Lua 脚本

每个活动配置可运行一段 Lua 脚本，入口为 `function OnEvent(event, arg)`。`event` 为 `pressed` 或 `released`；键盘 `arg` 是小写友好键名，鼠标按钮参数为 1 左、2 中、3 右、4/5 侧键。

`IsPressed(arg)` 查询运行 Host 的 Windows 电脑当前观测到的物理键盘或鼠标按键状态，不查询远端目标电脑或 M 板连接的真实鼠标。字符串可用 `"a"`、`"f13"`、`"mouse_left"` 等键名；数字 1..5 表示鼠标按钮，其他数字按 Win32 虚拟键码处理。`VK_CODES` 提供常见虚拟键名。

| API | 说明 |
|---|---|
| `move(x, y)` | 相对移动，接受小数并累计余量后输出整数 HID 位移 |
| `moveto(x, y)` | 本机 Win32 路径使用屏幕绝对坐标；远端 M 路径以本机光标为基准换算相对差值 |
| `mouse(button, state)` / `wheel(delta)` | 输出鼠标按钮和垂直滚轮 |
| `keydown(key)` / `keyup(key)` / `keypress(key[, ms])` | 输出键盘按键，支持友好键名或 HID Usage |
| `delay(ms)` / `sleep(ms)` / `Sleep(ms)` | 可取消的毫秒延时 |
| `randdelay(base[, range])` / `randsleep(base[, range])` | 在基准时间附近随机延时 |
| `IsPressed(arg)` | 查询 Host 本机当前物理按键状态 |
| `DebugLog(...)` / `ClearLog()` | 输出或清空 Lua 日志 |
| `VK_CODES` | 常见 Win32 虚拟键码表 |

远端 `moveto(x,y)` 没有目标屏幕坐标反馈，实际是读取本机光标坐标并发送相对增量，不能保证目标端落在给定位置。双板模式自动化鼠标优先注入 M，M 角色串口不可用时回退到本机 Win32；自动化键盘在双板模式下使用本机 Win32，因为当前双板不支持软件键盘输入。

```lua
function OnEvent(event, arg)
    if event == "pressed" and arg == 1 then
        while IsPressed(1) do
            move(0.5, 2)
            sleep(10)
        end
    end
    DebugLog("event=%s arg=%s", tostring(event), tostring(arg))
end
```

Lua 页的“检查”会校验脚本并整理缩进与常见行内空格；运行错误会显示脚本行号。脚本主动调用的 `DebugLog(...)` 内容会显示在 Lua 日志中。

## UDP 与 kmboxNet 输入

Host 默认监听 UDP `0.0.0.0:24814`，同一端口可接收 JSON 鼠标命令或兼容的 kmboxNet 数据报。该接口没有身份认证，仅应在受信网络使用；本机测试可把 `remoteInputBindAddress` 设为 `127.0.0.1`。JSON 只接受相对移动和滚轮，不包含按键：

```json
{"dx":12,"dy":-4,"wheel":0,"pan":0}
```

`dx`/`dy` 范围为 -32768..32767，`wheel`/`pan` 范围为 -128..127；四项全为零的数据报会被拒绝。可选 `smoothing_slots` 接受 0、5、10、15、20。JSON 与 kmboxNet 共用 `remoteInputPort`，默认 24814。

双板模式下，UDP 鼠标移动和 kmboxNet 鼠标按键经 Host 软件鼠标通路发给 M，不会因 M 不可用而回退成本机光标输入。kmboxNet 键盘命令在双板模式下不会输出到目标电脑，也不会回退到本机；旧单板模式仍可使用旧版键盘转发路径。

UDP 和串口是两种不同的调用方式：本节 Makcu 接口直接连接 M 板 UART0/CH340，不经过 UDP，也不通过 Host；开始直连前先退出 Host，避免串口占用。

### Makcu 兼容接口

双板固件默认在 M 板 UART0/CH340 提供 Makcu V4 鼠标接口，默认串口为 **115200、8N1**。直接从 Python 或其他串口程序调用前，先正常退出 Host 并关闭其他占用 M 串口的程序。当前只实现鼠标功能子集，不等同于完整原厂 Makcu 设备，也不提供键盘、手柄、flick 或设备管理接口。旧 V3 风格 ASCII 是另一套可选旧接口，不应与本节命令混用。

ASCII 命令以 CR/LF 结束，必须使用 `km.` 前缀。常用调用如下：

| 调用 | 用途 |
|---|---|
| `km.device()` / `km.version()` | 查询设备类型 `mouse` 与接口标识 `km.MAKCU` |
| `km.move(dx,dy)` / `km.wheel(delta)` | 相对移动或滚轮 |
| `km.left(1)` / `km.left(0)` | 按下/松开左键；也可用 `right`、`middle`、`side1`、`side2` |
| `km.click(button[,count[,hold_ms]])` | button 为 1 左、2 右、3 中、4 后退、5 前进；count 为 1..255，hold_ms 为 1..5000；省略/为 0 时随机按住 35..75 ms |
| `km.interpolate()` / `km.interpolate(value)` | 查询/设置 P 端的软件鼠标平滑；值为 0..100 或 `255`（AUTO） |

查询响应以 `\r\n>>> ` 结束；参数或队列错误返回 `ERR\r\n>>> `。默认关闭 echo 时，成功的设置命令不会返回文本。可用 [pyserial](https://pyserial.readthedocs.io/en/latest/pyserial_api.html) 直接发送 ASCII 命令；以下示例先查询接口，再移动并按下左键，最后在 `finally` 中松开：

```python
import serial
import time

port = "COMx"  # 替换为 M 板的 UART0/CH340 端口
ser = serial.Serial(port=None, baudrate=115200, timeout=1, write_timeout=1)
ser.port = port
ser.dtr = False
ser.rts = False
ser.open()
try:
    ser.reset_input_buffer()
    ser.write(b"km.version()\r\n")
    reply = ser.read_until(b">>> ")
    if not reply.endswith(b">>> "):
        raise TimeoutError("Makcu 查询超时")
    if b"ERR\r\n" in reply:
        raise RuntimeError(reply.decode("ascii", errors="replace"))
    if b"km.MAKCU" not in reply:
        raise RuntimeError("串口未返回预期的 Makcu 接口标识")
    print(reply.decode("ascii", errors="replace"))

    ser.write(b"km.move(20,-5)\r\n")
    ser.write(b"km.left(1)\r\n")
    time.sleep(0.05)
finally:
    try:
        if ser.is_open:
            ser.write(b"km.left(0)\r\n")
    finally:
        ser.close()
```

二进制接口使用 `DE AD LEN:u16le CMD PAYLOAD` 帧，`LEN` 是 payload 字节数且不包含命令字节。例如 `move(20,-5)` 使用 opcode `0x18`，payload 是两个有符号 16 位小端整数，完整帧为 `DE AD 04 00 18 14 00 FB FF`。更多 ASCII/二进制命令与返回格式见[Makcu V4 协议说明](docs/protocol.md#makcu-v4-鼠标-apim-板-uart0)。有效鼠标设置会续 1500 ms 输入租约；查询不会续租，停止发送后固件会释放注入按钮。`interpolate` 的 0/25/50/75/100 分别对应关闭/5/10/15/20 个平滑槽，其他 0..100 值就近选择；它不受 Host 平滑设置影响。建议使用 115200；本机 CH340 的 4 Mbaud 切换测试失败，不能按协议支持推断适配器可用。调用本接口不需要启动 Host。

### kmboxNet 示例

已有兼容版本 `kmNet.pyd` 的用户可将目标地址设为运行 Host 的电脑。以下 `192.0.2.10` 为文档保留地址，请替换为实际 Host 地址；UUID 使用原 API 所需的 8 位十六进制字符串：

```python
import kmNet

kmNet.init("192.0.2.10", "24814", "AF425414")
kmNet.move(10, -5)
kmNet.wheel(1)
```

Host 兼容鼠标移动、按钮、滚轮和键盘命令。`move_auto` 和贝塞尔移动命令会被当作普通相对移动处理，自动步进和曲线轨迹参数不生效。`monitor` 上报的状态以及兼容客户端 `isdown_*` 查询所依据的按键缓存来自 Host 本机物理输入，不是目标电脑状态；`mask`/`unmask` 只影响 Host 捕获并转发的实体输入，不会屏蔽双板 M→P 硬件直通，也不改变软件输入。`trace` 用于切换 Host 软件鼠标移动的平滑开关，不指定固定槽数。设备专用的重启、配置、屏幕等命令没有对应的 Host 功能。双板目标端目前仅支持软件鼠标输入。

### 项目内 Python 示例

`tools/esp32_move.py` 提供 `Esp32MouseSender`；`tools/send-remote-mouse-sample.py` 提供正方形移动示例。发送动作会移动目标鼠标，先检查脚本中的 Host 地址和位移参数，再按需运行：

```powershell
python .\tools\send-remote-mouse-sample.py
```

也可在自己的脚本中调用：

```python
from tools.esp32_move import Esp32MouseSender

sender = Esp32MouseSender("192.0.2.10", 24814)
try:
    sender.move(10, 0)
    sender.move(0, 10)
finally:
    sender.close()
```

将文档保留地址换成运行 Host 电脑在目标网络中的 IP。JSON UDP 只输入鼠标相对移动和滚轮，不会直接移动运行 Host 的本机光标。

## 统一输出灵敏度

主窗口鼠标捕获区域提供 `0.30` 到 `3.00` 的输出灵敏度，默认 `1.00`。`1.00` 保持原始 X/Y 移动量；低于 1 会缩小，高于 1 会放大，程序会保留小数余量。该比例只作用于 Host 发送到板端的 X/Y 相对移动，包括旧单板实体转发、网络 UDP、双板 M 软件注入，以及路由到板端的 Lua/宏移动。

双板 M→P 的实体鼠标硬件直通和 Lua/宏回退到本机 Win32 的动作不经过 Host 输出灵敏度。滚轮、水平滚动和鼠标按钮不受该设置影响。

## 固件刷写

设置页“本地固件刷写”模块允许选择刷写串口和 JSON 清单，检查后确认即可开始；Host 会释放串口并在刷写完成后恢复连接。清单描述 bootloader、partition table 和 app 镜像；清单中的相对镜像路径按 JSON 所在目录解析。Host EXE 不嵌入固件镜像。

<details>
<summary>备注：刷写工具构建与查找</summary>

Host 优先使用构建时嵌入的独立 esptool；若 EXE 没有嵌入 esptool，则尝试项目 ESP-IDF Python 环境中的 esptool。需要自行准备嵌入工具时，可运行 `scripts/build-embedded-esptool.ps1` 后再构建 Host，依赖见[依赖说明](docs/dependencies.md)。

</details>

远程刷写 API 默认关闭。设置页启用后监听局域网 TCP 24815，并使用设置页保存的串口；请求提供的是**运行 Host 的电脑上的** manifest 路径。远程请求电脑上的路径不会自动映射到 Host。manifest 相对路径按 Host 进程当前工作目录解析，镜像相对路径按 JSON 文件目录解析。接口没有账号认证，仅限受信网络；启用前请阅读[固件刷写 API 说明](docs/firmware-update-api.md)。

```powershell
# 先从项目根目录启动 Host；相对 manifestPath 按 Host 进程当前工作目录解析
$manifestPath = 'firmware/dual_proxy/build/flasher_args.json'
$headers = @{ 'X-HidBridge-Action' = 'flash-firmware' }
$body = @{ manifestPath = $manifestPath } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri 'http://192.0.2.10:24815/api/v1/firmware/flash' -Headers $headers -ContentType 'application/json' -Body $body
```

## Windows CH340/CH341 驱动

M 板 UART0/CH340 串口用于 Host 软件命令、维护和日志。若 Windows 没有出现 COM 端口，可从[WCH 官方驱动页面](https://www.wch-ic.com/downloads/CH341SER_ZIP.html)下载驱动，或查看仓库中的[驱动来源和安装说明](drivers/wch-ch341ser/README.md)。

Host 在串口模式启动时会检查驱动状态。若找到随包 INF 且确认缺少匹配驱动，程序会先显示安装确认；安装仍需用户确认并通过 UAC。驱动安装完成不代表串口已可用：双板模式还需完成 M/P 角色握手；旧单板兼容模式跳过新的角色握手。手动安装时可解压 WCH 驱动包，在设备管理器中选择更新驱动并指向 `CH341SER.INF`，或运行驱动包自带安装器。

常见排查方式：

- 在设备管理器“端口 (COM 和 LPT)”查看 CH340/CH341 对应端口。
- 若设备显示 Problem Code 28，通常表示 Windows 没有匹配驱动；按官方驱动说明安装后再检查 COM 端口。
- 同一串口不能同时由 Host、ESP-IDF monitor、刷写器或其他串口工具打开；关闭占用程序后让 Host 重新发现设备。
- 安装驱动后仍无 COM 端口时，检查 USB 数据线、板卡的 USB-UART 接口和设备管理器状态。P 板原生 USB HID 口与 UART/CH340 是不同接口。

## 常见问题与安全边界

- **Host 找不到串口：** 检查 CH340/CH341 驱动、USB 数据线和设备管理器；关闭占用串口的 monitor、刷写器或其他程序。双板自动发现依赖 M/P 角色握手；旧单板兼容模式跳过新的角色握手。
- **COM 号改变：** `portName` 使用 `auto` 时 Host 会自动发现；固定串口配置只适用于明确知道端口的环境。
- **双板实体鼠标没有经过 Host：** 这是预期路径。真实鼠标由 M→P 硬件直通，Host 的 HOME 和灵敏度设置不改变这条路径。
- **双板 Lua/宏键盘输出没有到目标电脑：** 双板模式自动化键盘使用运行 Host 的本机 Win32；当前双板不支持软件键盘输入。kmboxNet 键盘不会回退成本机输入。
- **Host EXE 无法启动：** 安装[依赖说明](docs/dependencies.md)中列出的 .NET 8 Desktop Runtime x64。
- **BLE 配对：** BLE HID 是旧单板固件路径；设备名默认是 `Keyboard with Touchpad`。USB HID 可用时 BLE 可能不广播。若 Windows 配对失败，先在 Windows 蓝牙设置中删除已保存的旧设备，再让旧单板重新广播并配对；不能据此推断双板透明鼠标代理支持 BLE。
- **刷写提示清单或镜像不存在：** 检查 JSON 路径及其引用的镜像；相对镜像路径以 JSON 所在目录解析。
- **网络输入无输出：** 核对 Host 地址、UDP 端口、防火墙及 M 端串口角色握手。UDP 输入只适用于受信网络，不要暴露到公网。
- **避免输入回路：** 不要把目标电脑的鼠标输出经 USB 接回同一台正在捕获并转发输入的电脑。

停止转发、串口断开、刷写切换和程序退出时，Host 会释放自己跟踪的按键与鼠标按钮，避免目标设备卡键。不同鼠标的克隆兼容性及目标端是否消费报告，请以[审计记录](docs/审计.md)中对应版本的验证边界为准。

## 备注

<details>
<summary>备注：源码目录与检查入口</summary>

### 仓库目录

```text
firmware/                 旧单板固件工程
firmware/dual_proxy/      当前双板透明鼠标代理固件
host/HidBridge.Host/      Windows Host 与设备维护工具
shared/                   主机与固件共享协议代码
tests/                    Host 检查及固件逻辑测试
tools/                    串口、统计、注入和探针工具
docs/                     协议、刷写、交接和审计记录
artifacts/                本地测试证据（通常不提交）
log/                      本地运行日志
```

Host 检查程序入口为 `tests/HidBridge.Host.Checks/`，可在项目根目录运行：

```powershell
dotnet run --project .\tests\HidBridge.Host.Checks -c Release
```

完整 Host 检查包含依赖旧单板构建 manifest 的项目；若该构建产物不存在，相关检查会失败。双板纯逻辑检查入口为 `scripts/Test-DualProxyLogic.ps1`，需要 clang 和按[依赖说明](docs/dependencies.md)准备的双板构建环境。源码检查、模拟逻辑检查和真实开发板验收是不同层级，测试目录或命令本身不代表设备验证通过。

BLE 设备名可通过旧单板的 `CONFIG_HID_BRIDGE_BLE_DEVICE_NAME` 修改；双板不提供 BLE。Host 使用 A5 帧编码软件鼠标输入，Makcu 使用独立 V4 ASCII/二进制命令；具体实现及完整协议见 [协议文档](docs/protocol.md)。

</details>
