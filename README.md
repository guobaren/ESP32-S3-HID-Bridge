# ESP32-S3 HID Bridge

两块 ESP32-S3 开发板把真实鼠标**透明直通**到目标电脑：M 板（鼠标侧）接真实鼠标，P 板（电脑侧）在目标电脑上克隆出同一只鼠标。需要自动化时，可选运行 Windows Host，用 Lua/宏、网络输入或盒子兼容接口驱动目标电脑鼠标。单板固件是另一条分支（`single-board`），优先级低于双板主线，但仍是持续维护的功能。

## 目录

- [快速开始](#快速开始)
- [硬件与固件要求](#硬件与固件要求)
- [当前主线：双板透明鼠标代理](#当前主线双板透明鼠标代理)
- [构建与刷写双板固件](#构建与刷写双板固件)
- [Host 功能与配置](#host-功能与配置)
- [自动化：宏与 Lua](#自动化宏与-lua)
- [外部输入接口](#外部输入接口)
  - [本项目 UDP 接口](#本项目-udp-接口)
  - [KmboxNet 兼容接口](#kmboxnet-兼容接口)
  - [Makcu 盒子兼容接口](#makcu-盒子兼容接口)
- [验证与自检](#验证与自检)
- [固件刷写](#固件刷写)
- [统一输出灵敏度](#统一输出灵敏度)
- [Windows CH340/CH341 驱动](#windows-ch340ch341-驱动)
- [常见问题与安全边界](#常见问题与安全边界)
- [单板分支](#单板分支)
- [发布流程](#发布流程)
- [文档索引](#文档索引)
- [仓库目录](#仓库目录)
- [许可证](#许可证)

## 快速开始

### 只用实体鼠标（不需要 Host）

1. 给 M、P 两块板刷入**同一版本**的 `firmware/` 双板固件。已经刷好可跳过；尚未刷写时先看[硬件与固件要求](#硬件与固件要求)，再按[构建与刷写双板固件](#构建与刷写双板固件)操作。
2. 板间 UART1 交叉接线：M GPIO17 TX→P GPIO18 RX，M GPIO18 RX←P GPIO17 TX，两板共地；**不要连接两板的 5V 或 3V3**。
3. M 板用 USB-UART/CH340 维护口接 Windows 电脑或合适的 USB 电源供电，真实鼠标/接收器接 M 板原生 USB Host 口；P 板原生 USB 接目标电脑。
4. 实体鼠标由 M→P 硬件直通，这条路径**不需要启动 Host**；目标电脑把它识别为一只普通鼠标（见[目标电脑侧免驱](#目标电脑侧免驱)）。

### 需要 Lua/宏或网络输入时再运行 Host

1. 把 M 板 USB-UART/CH340 维护口接到运行 Host 的 Windows 电脑；需要网络输入时，确保发送端可以访问 Host 电脑。
2. 已有 `HidBridge.Host.exe` 时，安装 [.NET 8 Desktop Runtime x64](docs/dependencies.md) 后跳过构建；没有 EXE 时按 [Host 功能与配置](#host-功能与配置)从源码生成。
3. 在项目根目录启动 `.\HidBridge.Host.exe`，设置页保持“使用旧版单板通路”**未勾选**（默认关闭），让 Host 自动发现 M。
4. 只有使用宏或 Lua 时才需要在 Host 页面启用、编辑或选择自动化配置，默认活动配置是 `Global`；网络输入默认使用 UDP 端口 `24814`。

## 硬件与固件要求

| 项目 | 要求 |
|---|---|
| 主控 | 两块 **ESP32-S3** 开发板，原生 USB-OTG 可用（一块做 M，一块做 P） |
| Flash | **≥ 4 MB**。分区表占用到 `0x110000`（约 1.06 MB：nvs 24 KB + phy_init 4 KB + factory 应用 1 MB），刷写参数为 `--flash-size 4MB`；8 MB 板卡同样可用（更大的 Flash 不会被使用） |
| PSRAM | **不需要**。固件未启用 `CONFIG_SPIRAM`，无需购买带 PSRAM 的型号 |
| 状态灯 | 固件按板载 WS2812B 驱动状态灯（ESP32-S3-DevKitC-1 为 GPIO48）。板子没有该灯不影响鼠标转发，只是看不到状态指示 |
| 维护/供电口 | M 板 UART0/CH340（Host 软件输入、Makcu 兼容接口、统计与日志，并给 M 供电） |
| 板间连线 | UART1 交叉接线 + 共地；不要互连两板 5V/3V3 |
| 目标电脑 | 不需要安装本项目提供的软件或驱动 |

应用镜像约 471 KB，占 1 MB 应用分区（余量约 55%）。分区表不再包含 storage 分区，所以 **4 MB Flash 板卡即可使用**，8 MB 板卡同样可用；此前刷过含 4 MB `storage` 旧分区表的板卡需要整片重刷一次（bootloader + partition table + app）。

### 目标电脑侧免驱

P 板克隆真实鼠标的 USB 描述符（VID/PID、字符串、接口、端点与 HID collections），目标电脑按**标准 HID 鼠标**直接识别，**不需要**安装本项目提供的任何软件或驱动。带厂商功能的鼠标（例如 Logitech HID++ / G HUB）其厂商功能仍取决于目标电脑上是否装有对应厂商驱动；无法满足 ESP32-S3 Full-Speed、端点/FIFO 或描述符安全预算的设备会保持断开，固件不会回退成通用鼠标冒充。

## 当前主线：双板透明鼠标代理

两块开发板烧录**同一镜像**，运行时按 USB 状态协商 M（鼠标侧）与 P（电脑侧）角色；M 板读取真实鼠标的描述符与输入，P 板在目标电脑侧克隆该鼠标并转发输入与厂商 HID 通信。

```text
真实鼠标/接收器 ─USB Host─> M 板 ──UART1──> P 板 ─USB Device─> 目标电脑
                                ↑                       ↑
                           UART0/CH340              UART0/开发口
                       软件命令与维护工具             刷写与诊断
```

| 板卡 | USB 角色 | 主要连接与职责 |
|---|---|---|
| M（鼠标侧） | USB Host | USB Host 口接真实鼠标/接收器；采集描述符与输入；经 UART1 向 P 发送设备信息与数据；UART0/CH340 用于软件输入、Makcu 接口与维护 |
| P（电脑侧） | USB Device | 原生 USB 接目标电脑；安装并呈现 M 提供的鼠标克隆；接收并输出 M 转发的输入与厂商 HID 数据 |

- 角色锁定后，USB 或 UART 临时断开不会重新选角色；两板都断电后重新从 `UNRESOLVED` 开始。
- 板载状态灯：未定身份熄灭；已锁定但对端离线红色常亮；对端在线时 M 绿色、P 蓝色；采集、确认超时或克隆失败红色闪烁。
- 目标电脑接 P 的原生 USB；真实鼠标接 M 的原生 USB Host 口；不要同时把两板的 5V/3V3 连在一起。

### 软件输入与平滑

- 实体鼠标走 M→P 硬件直通，**不经过 Host**，也不经过 Host 的平滑或输出灵敏度设置。
- Host 的网络输入与 Lua/宏鼠标移动可发送给 M；Host 的平滑与[统一输出灵敏度](#统一输出灵敏度)统一作用于这些软件移动。
- Makcu 兼容接口是另一条通路，按 P 端配置平滑，不受 Host 平滑设置影响；同一串口同时只允许一个程序占用。
- 具体参数、连接恢复事务、动态 USB Profile 与冻结检测见[固件说明](docs/固件.md)与[协议说明](docs/protocol.md)。

## 构建与刷写双板固件

使用 **ESP-IDF 6.0.2**。本分支的 `firmware/` 就是双板主固件工程（工程名 `dual_s3_hid_proxy`），不要再从其他目录寻找双板工程。

```powershell
Set-Location .\firmware
. ..\scripts\Enter-EspIdf.ps1

# 仅全新且尚未设置 target 的构建目录执行
idf.py set-target esp32s3
idf.py build

$port = 'COMx' # 替换成当前板卡串口
idf.py -p $port flash
```

- 只有全新且尚未设置 target 的构建目录才首次执行 `idf.py set-target esp32s3`；日常构建沿用已有 target 与 `sdkconfig`。
- **两块板必须刷同一版本固件**：`PROFILE_ACK` 长度本身就是新旧判别条件，混刷会因长度不匹配判失败。
- 构建产物在 `firmware/build/`：`flasher_args.json` 与 bootloader、partition table、应用镜像 `dual_s3_hid_proxy.bin`。
- 双板接口编译选项见下方备注；协议与统计字段见[协议说明](docs/protocol.md)，工程与连接细节见[固件说明](docs/固件.md)。

<details>
<summary>备注：双板接口构建选项</summary>

默认 CMake 选项 `DUAL_PROXY_ENABLE_MAKCU_V4_API` 为 `ON`，在 M 板 UART0/CH340 提供 Makcu V4 鼠标接口。旧 V3 风格 ASCII 子集由 `DUAL_PROXY_ENABLE_MAKCU_ASCII_API` 控制，默认 `OFF`，且与 V4 在同一 UART0 上互斥。例如只构建不含 V4 的镜像：

```powershell
idf.py -D DUAL_PROXY_ENABLE_MAKCU_V4_API=OFF build
```

编译选项不会改变已经刷入开发板的固件。

</details>

## Host 功能与配置

Host 是可选的 Windows 桌面工具，提供 Lua/宏自动化、网络鼠标输入、设备维护和本地固件刷写。**只使用实体鼠标时无需运行 Host。**

- Host 会监听运行它的 Windows 电脑上的本地键盘与鼠标事件；Lua 的 `IsPressed` 与本机触发在两种模式下都可用。
- 单板兼容模式只适配 `single-board` 分支的固件：实体键鼠仅在 `HOME` 开启时由 EXE 捕获并经串口转发；`HOME` 关闭时 Lua/宏使用本机 Win32 输出。
- 双板模式下，实体鼠标由 M→P 硬件直通，EXE 不重复转发，`HOME` 不控制这条通路，也不锁定本机光标。Lua/宏鼠标输入优先发给可用的 M 串口，否则回退到运行 Host 的本机；自动化键盘使用本机 Win32 输出，因为双板固件当前只支持鼠标软件输入。
- 双板模式的网络输入不受 `HOME` 或旧单板 UDP 开关限制；M 不可用时不会静默回退成本机光标输入。

### HOME / END 快捷键

- `HOME`：切换单板兼容模式的实体键鼠同步/转发状态；双板实体鼠标不受它控制。
- `END`：释放 Host 跟踪的输出状态并结束程序。

### 配置与日志

在项目根目录启动 `HidBridge.Host.exe`，在“设置”页选择活动配置，按需启用宏、Lua 或网络输入。Host 从**程序集/EXE 所在目录**读取 `bridge.local.json` 与 `bridge.json`：若同目录存在 `bridge.local.json` 就完整使用它，**不会**与 `bridge.json` 按字段合并，未写入的字段使用代码默认值。默认模板位于 `host/HidBridge.Host/bridge.json`。

若从项目根目录运行根 EXE，可在不覆盖已有本地配置的前提下复制模板：

```powershell
if (-not (Test-Path -LiteralPath '.\bridge.local.json')) {
    Copy-Item -LiteralPath '.\host\HidBridge.Host\bridge.json' -Destination '.\bridge.local.json'
}
```

常用字段如下（未列出的字段使用代码默认值，完整清单见模板文件）：

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

- 三个 `*LogPath` 都相对于 Host 程序目录，文件名中的 `{timestamp}` 用于区分每次日志；对应 `*LogRetentionCount` 默认保留 10 个匹配文件，可设为 1..1000。
- `remoteInputEnabled` 开关 Host 的网络输入服务，`remoteInputBindAddress` 控制绑定网卡地址；日志默认保存在程序目录下的 `log/`（Host、设备、自动化各保留 10 个文件）。
- 自动化选项（活动配置、统一输出灵敏度、旧版 UDP 开关等）保存在程序集目录的 `automation.settings.json`，不属于 `bridge.local.json`。
- 示例配置位于 `profiles.example/示例配置/`，复制到程序所在目录的 `profiles/` 后即可在“设置”页选择。每个配置用 `profile.json` 保存触发键与文件关联，宏正文在 `macros/*.txt`，Lua 正文默认在 `lua/main.txt`。
- `bridge.local.json` 是本机配置，不应提交。

<details>
<summary>备注：Host 构建与源码运行</summary>

从源码生成 Host 需要 .NET 8 SDK（安装位置与依赖见[依赖说明](docs/dependencies.md)）。Release 构建会更新项目根目录的 `HidBridge.Host.exe`，**构建前先正常退出正在运行的 Host**。

```powershell
# 构建（项目根目录执行）
dotnet build .\host\HidBridge.Host\HidBridge.Host.csproj -c Release

# 运行已构建的 EXE
.\HidBridge.Host.exe

# 或从源码运行
dotnet run --project .\host\HidBridge.Host\HidBridge.Host.csproj -c Release
```

从源码运行时，配置目录是 `host/HidBridge.Host/bin/.../` 中实际生成程序集所在目录，不是当前工作目录或项目根；自动化配置与日志也以该目录为基准。

</details>

## 自动化：宏与 Lua

### 宏

宏是按键触发的脚本，保存在活动配置目录的 `macros/` 子目录中，每个宏一个 `.txt` 文件。触发键可使用键盘键、鼠标侧键或组合键，例如 `f13`、`ctrl+f1`、`mouse_side1`。每行写一条 `命令(参数)`，参数用逗号分隔，`#` 及其后的内容作为注释，命令名不区分大小写。

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

支持四种运行模式：

| 模式 | 行为 |
|---|---|
| `once` | 每次触发执行一遍 |
| `toggle` | 按一次开始循环，再按一次停止 |
| `hold_loop` | 按住循环，松开停止 |
| `staged` | 按下时运行按下段；按住循环运行按住段；松开时运行松开段 |

`staged` 模式使用段标签：

```text
[on_press]
mouse(1, 1)

[while_hold]
randsleep(40, 20)
move(0, 5)

[on_release]
mouse(1, 0)
```

### Lua 脚本

每个活动配置可运行一段 Lua 脚本，入口为 `function OnEvent(event, arg)`。`event` 为 `pressed` 或 `released`；键盘 `arg` 是小写友好键名，鼠标按钮参数为 1 左、2 中、3 右、4/5 侧键。

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

`IsPressed(arg)` 查询的是**运行 Host 的 Windows 电脑**当前观测到的物理键盘或鼠标按键，不查询目标电脑或 M 板连接的真实鼠标。字符串可用 `"a"`、`"f13"`、`"mouse_left"` 等键名；数字 1..5 表示鼠标按钮，其他数字按 Win32 虚拟键码处理。

远端 `moveto(x,y)` 没有目标屏幕坐标反馈，实际是读取本机光标坐标并发送相对增量，不保证目标端落在给定位置。双板模式下自动化鼠标优先注入 M，M 串口不可用时回退到本机 Win32；自动化键盘在双板模式下使用本机 Win32。

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

## 外部输入接口

下面三条通路**目标功能相同**——让外部程序驱动目标电脑鼠标——区别只在通路：本项目 UDP 接口与 KmboxNet 兼容接口都通过 Host 的网络输入进入，Makcu 盒子兼容接口直接连 M 板串口、不经过 Host。

| 通路 | 连接对象 | 需要 Host | 典型用途 |
|---|---|---|---|
| [本项目 UDP 接口](#本项目-udp-接口) | Host 电脑的 UDP 端口（默认 24814） | 是 | 自己写的脚本或程序发送 JSON |
| [KmboxNet 兼容接口](#kmboxnet-兼容接口) | 同一个 UDP 端口 | 是 | 已有的 `kmNet.pyd` 或 C++ 客户端 |
| [Makcu 盒子兼容接口](#makcu-盒子兼容接口) | M 板 UART0/CH340（115200、8N1） | 否 | 直接发送 Makcu 风格串口命令 |

命令覆盖范围、帧格式、返回格式与完整示例见[协议说明](docs/protocol.md)；本文件只说明面向使用者的能力与注意事项。

### 本项目 UDP 接口

Host 默认监听 UDP `0.0.0.0:24814`，接受 JSON 鼠标命令：

```json
{"dx":12,"dy":-4,"wheel":0,"pan":0}
```

- 只接受相对移动和滚轮，**不包含按键**；`dx`/`dy` 范围 -32768..32767，`wheel`/`pan` 范围 -128..127，四项全为零的数据报会被拒绝。
- 可选 `smoothing_slots` 取 0、5、10、15、20。
- 该接口没有身份认证，只应在受信网络使用；本机测试可把 `remoteInputBindAddress` 设为 `127.0.0.1`。
- 双板模式下，JSON 鼠标移动经 Host 软件鼠标通路发给 M，不会因 M 不可用而回退成本机光标。

项目内提供了 Python 发送示例：

```powershell
python .\tools\send-remote-mouse-sample.py
```

也可在自己的脚本中调用 `tools/esp32_move.py` 的 `Esp32MouseSender`，把文档保留地址换成运行 Host 电脑在目标网络中的实际 IP。

### KmboxNet 兼容接口

同一 UDP 端口可直接接收原版 kmboxNet 数据报，已有 `kmNet.pyd` 的用户只需把目标地址指向运行 Host 的电脑：

```python
import kmNet

kmNet.init("192.0.2.10", "24814", "AF425414")
kmNet.move(10, -5)
kmNet.wheel(1)
```

使用注意：

- 鼠标移动、按钮、滚轮可用；`move_auto` 和贝塞尔移动命令按普通相对移动处理，自动步进与曲线参数不生效。
- `monitor` 上报的状态与兼容客户端 `isdown_*` 查询依据的是 **Host 本机物理输入**，不是目标电脑状态。
- `mask`/`unmask` 只影响 Host 捕获并转发的实体输入，不屏蔽双板 M→P 硬件直通，也不改变软件输入；`trace` 只切换 Host 软件鼠标移动的平滑开关。
- 设备专用的重启、配置、屏幕等命令没有对应实现；双板模式下键盘命令不会输出到目标电脑，也不会回退成本机输入。
- 该接口与 JSON 输入共用端口，同样没有身份认证，只应在受信网络使用。

### Makcu 盒子兼容接口

双板固件默认在 **M 板 UART0/CH340** 提供 Makcu V4 鼠标接口，串口参数默认 **115200、8N1**。它直接连接 M 板，**不经过 UDP，也不需要启动 Host**。

使用注意：

- 调用前先正常退出 Host 并关闭其他占用 M 串口的程序；同一串口同时只允许一个程序打开。
- 当前只实现鼠标功能子集，**不等同于原厂 Makcu 盒子**：不提供键盘、手柄、flick 或设备管理接口，未实现命令会明确拒绝，不会伪造成功。
- 有效的鼠标设置会续 1500 ms 输入租约；查询不会续租，停止发送后固件会释放注入的按钮。
- 旧 V3 风格 ASCII 是另一套可选旧接口（默认关闭），与 V4 互斥，不应与本节命令混用。
- 协议允许更高波特率，但本机 CH340 在 4 Mbaud 配置失败，建议使用 115200。
- 只读检查工具（不发送复位命令或鼠标输入）：

```powershell
python .\tools\test_makcu_v4.py --port COMx
```

ASCII/二进制命令、返回格式与完整示例见[协议说明](docs/protocol.md)。

## 验证与自检

验证分四层：源码/静态检查 → 纯逻辑回归（模拟）→ Host 检查 → 真实开发板验收。**低层级通过不能替代真机验收**，完整清单、前置条件与结论边界见[验证与自检文档](docs/验证.md)。

```powershell
# 双板纯逻辑回归（需 clang，且 firmware/ 已构建过一次）
pwsh -File .\scripts\Test-DualProxyLogic.ps1

# 不启动 Host 的 M 侧串口测试
python .\tools\test_dual_proxy_serial.py --port COMx --cursor

# Host 检查（需要 firmware/build/flasher_args.json）
dotnet run --project .\tests\HidBridge.Host.Checks -c Release
```

- Windows PnP 节点存在不等于厂商驱动识别成功；固件记录到 USB report submit/complete 也不等于接收电脑的应用已消费。
- 打开 CH340 调试口采集日志会经 DTR/RTS 触发板卡复位，采集结果必须标注这一扰动。
- 早期的鼠标随机断连问题**已解决，此后未再复现**；未验证项与历史边界见[审计记录](docs/审计.md)。

## 固件刷写

设置页“本地固件刷写”模块允许选择刷写串口和 JSON 清单，检查后确认即可开始；Host 会释放串口并在刷写完成后恢复连接。清单描述 bootloader、partition table 和应用镜像；清单中的相对镜像路径按 JSON 所在目录解析。Host EXE 不嵌入固件镜像，双板清单为 `firmware/build/flasher_args.json`。

远程刷写 API 默认关闭。设置页启用后监听局域网 TCP `24815`，并使用设置页保存的串口；请求提供的是**运行 Host 的电脑上的** manifest 路径，远程请求电脑上的路径不会自动映射到 Host。`manifestPath` 相对路径按 Host 进程当前工作目录解析，镜像相对路径按 JSON 文件目录解析。该接口没有账号认证，仅限受信网络；启用前请阅读[固件刷写 API 说明](docs/firmware-update-api.md)。

```powershell
# 先从项目根目录启动 Host；相对 manifestPath 按 Host 进程当前工作目录解析
$manifestPath = 'firmware/build/flasher_args.json'
$headers = @{ 'X-HidBridge-Action' = 'flash-firmware' }
$body = @{ manifestPath = $manifestPath } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri 'http://192.0.2.10:24815/api/v1/firmware/flash' -Headers $headers -ContentType 'application/json' -Body $body
```

## 统一输出灵敏度

主窗口鼠标捕获区域提供 `0.30` 到 `3.00` 的输出灵敏度，默认 `1.00`。`1.00` 保持原始 X/Y 移动量，低于 1 缩小、高于 1 放大，程序保留小数余量。该比例只作用于 Host 发送到板端的 X/Y 相对移动，包括单板实体转发、网络输入、双板软件注入以及路由到板端的 Lua/宏移动。

双板 M→P 的实体鼠标硬件直通、以及 Lua/宏回退到本机 Win32 的动作**不经过**该设置。滚轮、水平滚动和鼠标按钮不受影响。

## Windows CH340/CH341 驱动

M 板 UART0/CH340 串口用于 Host 软件输入、Makcu 兼容接口、维护和日志。若 Windows 没有出现 COM 端口，可从 [WCH 官方驱动页面](https://www.wch-ic.com/downloads/CH341SER_ZIP.html)下载，或查看仓库中的[驱动来源和安装说明](drivers/wch-ch341ser/README.md)。

Host 在串口模式启动时会检查驱动状态：若找到随包 INF 且确认缺少匹配驱动，程序会先显示安装确认，安装仍需用户确认并通过 UAC。驱动安装完成不代表串口已可用——双板模式还需完成 M/P 角色握手；单板兼容模式跳过新的角色握手。

常见排查：

- 在设备管理器“端口 (COM 和 LPT)”查看 CH340/CH341 对应端口。
- 设备显示 Problem Code 28 通常表示 Windows 没有匹配驱动；按官方说明安装后再检查 COM 端口。
- 同一串口不能同时由 Host、ESP-IDF monitor、刷写器或其他串口工具打开；关闭占用程序后让 Host 重新发现设备。
- 安装驱动后仍无 COM 端口时，检查 USB 数据线、板卡的 USB-UART 接口和设备管理器状态。P 板原生 USB HID 口与 UART/CH340 是不同接口。

## 常见问题与安全边界

- **Host 找不到串口：** 检查 CH340/CH341 驱动、USB 数据线和设备管理器；关闭占用串口的 monitor、刷写器或其他程序。双板自动发现依赖 M/P 角色握手，单板兼容模式跳过新的角色握手。
- **COM 号改变：** `portName` 使用 `auto` 时 Host 会自动发现；固定串口配置只适用于明确知道端口的环境。
- **双板实体鼠标没有经过 Host：** 这是预期路径。真实鼠标由 M→P 硬件直通，Host 的 `HOME` 与灵敏度设置不改变这条路径。
- **双板 Lua/宏键盘输出没有到目标电脑：** 双板模式自动化键盘使用运行 Host 的本机 Win32，当前双板固件不支持软件键盘输入；KmboxNet 键盘命令也不会回退成本机输入。
- **目标电脑没有出现鼠标：** 确认 P 板接的是目标电脑、M 板已接真实鼠标、两板固件同版本且 UART1 交叉接线 + 共地正确；有些鼠标无法安全克隆，会保持断开。
- **Host EXE 无法启动：** 安装[依赖说明](docs/dependencies.md)中列出的 .NET 8 Desktop Runtime x64。
- **刷写提示清单或镜像不存在：** 检查 JSON 路径及其引用的镜像；相对镜像路径以 JSON 所在目录解析。
- **网络输入无输出：** 核对 Host 地址、UDP 端口、防火墙及 M 端串口角色握手。网络输入只适用于受信网络，不要暴露到公网。
- **避免输入回路：** 不要把目标电脑的鼠标输出经 USB 接回同一台正在捕获并转发输入的电脑。

停止转发、串口断开、刷写切换和程序退出时，Host 会释放自己跟踪的按键与鼠标按钮，避免目标设备卡键。不同鼠标的克隆兼容性及目标端是否消费报告，以[审计记录](docs/审计.md)中对应版本验证边界为准。

## 单板分支

单板固件是**另一条分支** `single-board`，与双板主线并行维护：优先级低于双板，但仍是持续支持的功能。本分支（`main`）的 `firmware/` 是双板工程，单板固件只能从 `single-board` 分支构建与刷写，两者不要交叉使用清单或镜像。

| 子系统 | 单板分支 | 与双板主线的关系 |
|---|---|---|
| USB 输出 | 固定键盘 + 相对触摸板 HID | 不是动态克隆物理鼠标 |
| 主机输入 | Host 经 CH340 串口转发软件键鼠输入 | 使用旧版单板模式，不使用 M/P 直通 |
| 自动化与网络 | Host 宏/Lua、UDP/kmboxNet 适配原单板固件 | 网络输入由 Host 适配；双板固件没有软件 KeyboardReport 注入 |
| BLE / Wi-Fi | 支持 BLE HID；Wi-Fi/SoftAP 未启用 | 双板透明代理不依赖这些路径 |
| 构建与验收 | 使用该分支自己的配置与产物 | 与双板工程分开构建、刷写和验收 |

单板固件保留 BLE HID：设备名默认是 `Keyboard with Touchpad`，可用该分支的 `CONFIG_HID_BRIDGE_BLE_DEVICE_NAME` 修改。USB HID 未连接或不可用且固件启用 BLE 时板卡开始广播，在目标电脑的 Windows“蓝牙和设备”中添加设备并完成 Just Works 配对；USB HID 可用时 BLE 后端可能不广播。若 Windows 配对失败，先在蓝牙设置中删除已保存的旧设备再重新配对。

## 发布流程

发布件（GitHub Release 的 ZIP 或本地 `release/`）内容为：`HidBridge.Host.exe`、`bridge.json`、`profiles/`、`drivers/wch-ch341ser/`、`LICENSE`、`README.md`、**双板固件**（`firmware/flasher_args.json` 与同目录三段镜像，应用镜像为 `dual_s3_hid_proxy.bin`）以及 `SHA256SUMS.txt`。

- 本地一键发布：根目录 `一键发布.cmd`（可加 `/SkipHostBuild`、`/NoFirmware`、`/Github` 等参数），它调用 `scripts/Publish-GitHubRelease.ps1` 与 `scripts/Prepare-Release.ps1`，产出 `release/` 与 `dist/` 下的 ZIP 及校验文件。
- 自动发布：推送 `v*` 标签（例如 `v0.1.2`）会触发 GitHub Actions 构建双板固件、打包并创建/更新对应的 GitHub Release。发布标签统一使用 `v*` 形式。
- `scripts/Prepare-Release.ps1` 只在 `firmware/sdkconfig` 不存在时才执行 `idf.py set-target esp32s3`，避免重置既有构建配置。

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/固件.md](docs/固件.md) | 双板主固件工程：角色协商、连接恢复事务、动态 USB Profile、冻结检测、日志与统计、构建刷写 |
| [docs/验证.md](docs/验证.md) | 验证入口、前置条件、分层结论与结论边界 |
| [docs/protocol.md](docs/protocol.md) | UART/USB 协议、消息类型、统计快照、UDP/kmboxNet 与 Makcu V4 接口细节 |
| [docs/连接流程.md](docs/连接流程.md) | 从上电初始化到克隆输入的完整时序、状态灯与控制通道加固 |
| [docs/dependencies.md](docs/dependencies.md) | 开发环境、官方来源与安装步骤 |
| [docs/firmware-update-api.md](docs/firmware-update-api.md) | 远程固件刷写 API 的请求格式与边界 |
| [docs/交接.md](docs/交接.md) | 当前状态快照、执行路线图与历史索引 |
| [docs/审计.md](docs/审计.md) | 质量报告、Known Issues、技术债与未验证清单 |
| [tools/README.md](tools/README.md) | 命令行工具（蓝牙验证、统计读取、延迟测量、注入与探针） |
| [drivers/wch-ch341ser/README.md](drivers/wch-ch341ser/README.md) | WCH CH340/CH341 驱动来源与安装说明 |
| [profiles.example/README.md](profiles.example/README.md) | 示例自动化配置说明 |

## 仓库目录

```text
firmware/                 双板主固件工程（ESP-IDF，工程名 dual_s3_hid_proxy）
firmware/connection_validation/  双板连接验证工程（只验证链路，不输出键鼠）
firmware/usb_bandwidth_probe/    USB 吞吐探针（本地实验工程，未纳入版本控制）
host/HidBridge.Host/      Windows Host：自动化、网络输入、维护与本地刷写
shared/                   主机与固件共享协议代码
tests/                    Host 检查与统计快照夹具
tools/                    串口、统计、注入、探针与测试工具（索引见 tools/README.md）
scripts/                  环境进入、逻辑回归、发布与打包脚本
docs/                     固件、验证、协议、连接流程、依赖、刷写 API 与状态记录
drivers/                  WCH CH340/CH341 驱动
profiles.example/         示例自动化配置（宏与 Lua）
release/                  本地发布件产物（不提交）
artifacts/                本地测试证据（通常不提交）
log/                      本地运行日志
一键发布.cmd              本地一键打包发布入口
```

## 许可证

本项目使用 MIT 许可证，详见 [LICENSE](LICENSE)。
