# ESP32-S3 HID Bridge

把 Windows 电脑的键盘和鼠标事件，经 ESP32-S3-DevKitC-1 转换成独立的键盘与相对触摸板 HID，输出到手机、平板、嵌入式设备或其他项目。原生 USB 在启动时根据 UART 协议握手二选一枚举为 CDC-only 或 HID-only；BLE HID 保持可用。Wi-Fi 输入、配网和 Target Agent 输出代码暂时保留但不启用。

## 功能特性

- Windows 全局键盘/鼠标捕获：低级钩子 + Raw Input，500 Hz 鼠标报告聚合
- 串口自动发现（COM 改变后自动重连）与随机数设备握手
- 原生 USB：启动期 UART 握手选择 CDC-only 或「键盘 + 相对触摸板」HID-only（不同 PID）
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
        │ USB-to-UART 或原生 USB CDC，自动发现与二进制帧握手
        ▼
ESP32-S3-DevKitC-1
        ├─ 原生 USB OTG，启动时选择 CDC-only 或键盘触摸板 HID-only
        ├─ BLE HID ────────────────> 手机/电脑
```

开发板支持两种有线主机输入方式：

- **USB-to-UART**：CH340 COM 口接收主机端生成的 HID 报告。
- **ESP32-S3 USB**：固件启动后 1.5 秒内收到 UART 有效协议帧 → 枚举为 `USB Keyboard with Touchpad`（HID-only）；未收到 → 枚举为 `HID Bridge CDC`（CDC-only，可作输入并经 BLE 输出）。

USB HID 与 BLE HID 同时可用时，先连接并成为活动输出的链路保持锁定，另一链路不得抢占；活动链路连续 100 ms 不可发送时切换到仍在线的另一链路，切换前后都执行 ReleaseAll。官方 DevKitC-1 支持两个 USB 端口同时供电；第三方兼容板需先核对原理图，确认两端口间没有 VBUS 回灌路径。

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

固件来源优先级：本地 firmware/build（最新构建）> 构建时嵌入 exe 的默认固件 > 用户在设置页手动选择的文件。

### 方式一：设置页本地刷写（手动，推荐）

1. 运行 HidBridge.Host.exe，确认日志显示已自动发现并连接串口。
2. 打开「设置」页，底部「本地固件刷写」区，点「选择固件文件」。
3. 固件文件两种选择：
   - `flasher_args.json`：三段完整刷写（bootloader 0x0、partition table 0x8000、应用 0x10000）。清单里的镜像可按标准子目录（bootloader/、partition_table/）或与清单同目录平铺放置。
   - 单个 `.bin`：仅按 0x10000 刷写应用分区。
4. 点「确定」→ 弹窗二次确认 → 打开小日志窗口实时显示 esptool 进度（百分比、哈希校验、RTS 复位），完成后显示「刷写完成」。

### 方式二：远端刷写接口（远程调用）

接口固定监听本机 127.0.0.1，默认端口 24815，不接受局域网地址直接连接；需要远程刷写时，在运行控制软件的电脑上通过既有远程控制发起本机请求。

**启用**：主界面「设置」页勾选「启用本机固件刷写接口」（保存后立即生效，接口只绑定 Loopback）。

**启动刷写**：

```powershell
$headers = @{ 'X-HidBridge-Action' = 'flash-firmware' }
Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:24815/api/v1/firmware/flash' -Headers $headers
```

**查询状态**：

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:24815/api/v1/firmware/status'
```

状态 state 取值：`idle`（未执行）/ `running`（释放串口、刷写或恢复中）/ `succeeded`（三段哈希校验 + RTS 复位 + 串口恢复通过）/ `failed`（原因见 message，退出码见 exitCode）。重复提交时不会并发执行，返回 HTTP 409 和当前任务状态。

**安全边界**：只绑定 127.0.0.1；POST 必须携带确认头且不接受请求体；不接受远程上传、镜像路径、串口名或命令行参数；清单必须且只能包含 0x0、0x8000、0x10000 三段且文件位于 build 目录内；刷写期间独占串口并暂停同步，程序退出时等待 esptool 安全结束。成功后日志输出「固件刷写最终摘要」（三段 SHA-256、设备校验计数、RTS 复位结果）。

### 内置 esptool 的重新构建

内置 esptool 由 scripts/build-embedded-esptool.ps1 用 PyInstaller 生成（独立单文件，v5.3.1，约 14MB），产物放在 host/HidBridge.Host/EmbeddedAssets/（不提交 Git），构建时作为嵌入资源打进 exe：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-embedded-esptool.ps1
dotnet build host/HidBridge.Host/HidBridge.Host.csproj -c Release
```

注意：替换内置默认固件需要重新构建 exe（构建时自动嵌入当前 firmware/build）。

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

Lua 诊断日志写入 artifacts/automation-runtime-<时间戳>.log，主界面 Lua 页可实时查看。

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

HOME 同步开启期间同时按住鼠标左键+右键开始记录；左右键松开 3 秒后自动停止并弹出分析图（X 有符号值时间序列 + Y 有符号值时间序列），同时保存到 artifacts/mouse-movement-<时间戳>.png。记录点位于 500 Hz 报告实际提交边界，不做平滑或降采样。

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
  "firmwareProjectRoot": "",
  "firmwareFlashBaudRate": 460800,
  "firmwareFlashTimeoutSeconds": 180
}
```

- transport 只能是 serial（wifi 实现保留但当前被功能闸门拒绝）。
- portName 为 auto 时自动扫描 COM 并完成随机数握手；固定串口模式不自动扫描。
- hostLogPath / deviceLogPath 支持 {timestamp} 占位符；deviceLogPath 置空可关闭设备日志。
- showDeviceLogInUi 为启动默认值：false 精简模式 / true 完整日志模式；窗口内可随时切换。
- firmwareProjectRoot 留空时从 exe 目录和当前目录向上查找 firmware/build。

## 常见问题

- **COM 被占用**：主机程序与 idf.py monitor、串口工具不能同时打开同一 COM 口；关闭占用程序后主机会自动重连。
- **手动刷写提示「刷写镜像不存在」**：flasher_args.json 引用的 bin 需与清单同级（支持 bootloader/、partition_table/ 子目录或平铺），或直接选单个应用 .bin。
- **BLE 配对后反复「已配对/已连接」**：旧固件无绑定持久化，升级后需在目标设备删除/忽略旧配对，重新配对一次。
- **防火墙弹窗**：UDP 24814 与刷写接口 24815 首次监听可能触发 Windows 防火墙提示。
- **exe 无法启动**：需要 .NET 8 Desktop Runtime（x64）。
- **不要输入回路**：不要把开发板原生 USB 口接回同一台电脑测试转发并同时抑制本地输入。

## 安全边界

- UDP 模拟鼠标入口默认开启且无身份认证，默认监听 0.0.0.0:24814，仅限受信任网络。
- 固件刷写接口默认关闭、只绑定 127.0.0.1，不接受上传、文件路径或命令参数。
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
docs/                     设计文档（协议、配置、架构、交接与审计记录）
artifacts/                运行日志与产物（不提交）
tools/                    辅助工具（如 UDP 发送示例）
```

主机检查程序（自检，无需连接开发板）：

```powershell
dotnet run --project tests/HidBridge.Host.Checks -c Release
```

协议细节（帧格式、消息类型、握手与 UDP 接口）见 [docs/protocol.md](docs/protocol.md)，连接与配置细节见 [docs/configuration.md](docs/configuration.md)。
