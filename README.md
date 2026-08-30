# ESP32-S3 HID Bridge

把 Windows 电脑的键盘和鼠标事件，经 ESP32-S3-DevKitC-1 转换成独立的键盘与相对触摸板 HID，输出到手机、平板、嵌入式设备或其他项目。当前原生 USB 固定枚举为键盘 + 相对触摸板 HID，不依赖 EXE 是否运行或 UART 握手；BLE HID 保持可用。Wi-Fi 输入、配网和 Target Agent 输出代码暂时保留但不启用。

## 功能特性

- Windows 全局键盘/鼠标捕获：低级钩子 + Raw Input，500 Hz / 2 ms 桥接报告聚合上限
- 串口自动发现（COM 改变后自动重连）与随机数设备握手
- 原生 USB：固定枚举「键盘 + 相对触摸板」HID-only，不依赖启动期 UART 帧
- BLE HID 键盘/鼠标输出，NimBLE Just Works 配对 + 绑定密钥持久化
- USB/BLE 双输出活动链路锁定与 100 ms 失活切换，切换前后自动 ReleaseAll
- 局域网 UDP 模拟输入（默认 0.0.0.0:24814，兼容 JSON 与 kmboxNet，固件 5 个 1 ms 槽平滑）
- 宏（多段脚本）与 Lua 脚本（OnEvent 事件模型）
- 鼠标移动记录与分析图（左右键同按触发）
- 固件刷写双入口：设置页本地刷写 + 局域网 HTTP 接口，均使用内置独立 esptool，无需 Python
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

## Windows CH340/CH341 驱动

当前硬件枚举为 `USB\VID_1A86&PID_7523`。历史诊断中该设备曾为
`CM_PROB_FAILED_INSTALL`、Problem Code `28`，表示 Windows 缺少可用的匹配驱动；
本机历史上曾完成安装并枚举为 `USB-SERIAL CH340 (COM6)`、Status `OK`；当前驱动包已入库，
但没有 CH340/CH341 设备节点或 COM 口。这不是 Code 43，也不是 ESP32-S3 原生 USB HID
能够替代的串口链路。驱动离线包已保存于
[`drivers/wch-ch341ser/CH341SER_v4.0_2026-06-26.zip`](drivers/wch-ch341ser/CH341SER_v4.0_2026-06-26.zip)，
详细校验记录见 [`drivers/wch-ch341ser/README.md`](drivers/wch-ch341ser/README.md)。

来源是 [WCH 官方页面](https://www.wch-ic.com/downloads/CH341SER_ZIP.html) 和
[官方直链](https://www.wch-ic.com/download/file?id=5)，页面元数据为 v4.0、
2026-06-26、696KB。离线安装步骤：

1. 解压上述 ZIP；以管理员身份运行 `CH341SER\SETUP.EXE`，或在设备管理器中对
   `USB\VID_1A86&PID_7523` 选择“更新驱动程序”，浏览到解压后的 `CH341SER` 目录/`CH341SER.INF`。
2. 安装完成后在“端口 (COM 和 LPT)”确认出现 `USB-SERIAL CH340 (COMx)` 或同类 COM 设备。
3. 用下面的 PowerShell 命令核对设备和 COM 名称，再让 Host 自动发现：

   ```powershell
   Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_1A86&PID_7523*'
   [System.IO.Ports.SerialPort]::GetPortNames()
   ```

发布件中的 Host 会在启动时检查该设备：检测到 Problem Code `28` 且找到随包 INF 时显示确认窗口；
只有用户确认并通过 UAC 后才执行安装，完成后会重新检查 PnP 状态和 COM。其他 Problem Code
只提示手动处理，不会自动覆盖现有驱动。

仅在 `serial` 模式启动时执行这项检查。若设备节点暂时不可见，Host 还会只读执行
`pnputil /enum-drivers`：Driver Store 缺少 `CH341SER.INF` 且随包 INF 存在时，即使卸载驱动后设备节点暂时消失，也会显示安装确认；
Driver Store 已有驱动、设备已正常工作或 Driver Store 探测失败时不弹安装窗口。未插入设备但 Driver Store 确实缺少驱动时仍会提示，以便预先安装随包驱动。
用户确认后仍必须通过 UAC。当前 Host 使用随包、签名有效的 WCH 官方 `SETUP.EXE /S` 安装，
与手动点击官方安装器的 INSTALL 走同一安装器路径；后续可运行同一 `SETUP.EXE` 点击 UNINSTALL，
或使用其内置参数 `/U` 卸载、`/D` 卸载并删除驱动。安装器成功仍不代表设备已经枚举或 COM 口可用；
如果安装后的瞬时复检仍没有 CH340/CH341 设备或 COM 口，Host 不显示可能过时的阻塞弹窗，
而是继续启动并由串口握手自动发现；只有握手成功才记录“已连接 COMx”。这既不会把“驱动包入库”误报成“串口可用”，
也不会在 Windows 稍后完成 COM 初始化时错误要求重插 USB。WCH 自带卸载器在没有已绑定设备时可能提示“无驱动可卸载”，
是否已入库应以 `pnputil /enum-drivers` 中的 `CH341SER.INF` 为准。

## 架构与生命周期

### 主机端职责

- `HidBridge.Host` 使用 `WH_KEYBOARD_LL`、`WH_MOUSE_LL` 和鼠标 Raw Input 捕获实体输入；捕获线程只做快速入队，独立分发线程负责状态更新、Lua/宏事件和报告发送。
- `MouseReportPump` 以 500 Hz / 2 ms 为桥接发送上限，连续相对移动在队列中合并，按钮、滚轮和键盘边沿保持顺序；原生 USB 固件每 1 ms 消费一个 5 槽平滑样本。
- `SerialBridge` 通过 `DeviceProbe`/`DeviceHello` 自动发现串口并建立二进制会话；主机负责发送 `ReleaseAll`、维护输入租约和记录诊断日志。
- 固件刷写设置页与局域网 API 共用校验和刷写服务；API 提供对端本机 JSON 清单路径，并自动使用设置页已保存的串口；EXE 不内嵌固件镜像。
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

### 3. 生成可直接交付的发布件

发布脚本会先构建最新 Host，并把根目录最新 `HidBridge.Host.exe`、默认 `profiles/`、CH340/CH341
驱动和三段可刷写固件复制到 `release/`；根目录仍保留同一份最新 EXE。固件默认使用最近一次
ESP-IDF 构建结果，要求重新构建固件时加上 `-BuildFirmware`：

```powershell
.\scripts\Prepare-Release.ps1
# 重新构建 ESP32-S3 后再生成发布件
.\scripts\Prepare-Release.ps1 -BuildFirmware
```

发布目录结构：

```text
release/
├─ HidBridge.Host.exe
├─ bridge.json
├─ profiles/                       默认 Global 与示例配置
├─ drivers/wch-ch341ser/           WCH 官方驱动与 INF
├─ firmware/                       flasher_args.json + 三段镜像
├─ SHA256SUMS.txt
└─ README.md
```

### 4. 创建 GitHub Release

`Publish-GitHubRelease.ps1` 会先调用 `Prepare-Release.ps1` 重建 `release/`，校验根目录与
`release/HidBridge.Host.exe` 大小和 SHA-256 一致，再把 `release/` 内容直接打入 ZIP 根目录，
输出 `dist/ESP32-S3-HID-Bridge-<Tag>.zip` 及外部 `.zip.sha256`。打包后会用 .NET ZipArchive
核对 ZIP 条目、`release/SHA256SUMS.txt` 和强制驱动 INF；默认发布命令最后才调用
`gh release create <Tag> --target <SHA>` 上传，不会覆盖已有 tag/Release。

正常发布前置条件：当前目录必须是 Git 仓库；tracked 工作树必须干净（确认风险后才使用
`-AllowDirty`）；已安装并登录 GitHub CLI `gh`；当前本地/远端 tag 和 GitHub Release 不得存在；
`Target` 默认为当前 HEAD SHA，也可显式传入分支或提交。默认会重建 Host，固件需要重建时加
`-BuildFirmware`；只有明确确认已有根目录 EXE 时才使用 `-SkipHostBuild`。

只打包、不联网、不登录 gh、也不要求工作树干净：

```powershell
.\scripts\Publish-GitHubRelease.ps1 -Tag v0.0.0-localtest -PackageOnly
```

创建新 Release（示例不会在本轮执行）：

```powershell
.\scripts\Publish-GitHubRelease.ps1 -Tag v1.2.3 -Title 'ESP32-S3 HID Bridge v1.2.3'
.\scripts\Publish-GitHubRelease.ps1 -Tag v1.2.3 -Repo owner/repo -Target <commit-or-branch> `
    -NotesFile .\docs\release-notes-v1.2.3.md -Prerelease
```

可用参数：`Tag`（必填，`vX.Y` 或 `vX.Y.Z`/预发布后缀）、`Title`、`NotesFile`（缺省使用
`--generate-notes`）、`Repo`、`Target`、`Draft`、`Prerelease`、`BuildFirmware`、
`SkipHostBuild`、`AllowDirty` 和 `PackageOnly`。脚本只会删除 `dist/` 下当前 Tag 对应的
ZIP/哈希文件；不会递归清理其他发布产物，也不会自动创建或覆盖已有 GitHub Release。

### 5. 快捷键

- `HOME`：启用 / 停止向目标设备转发。
- `END`：安全释放所有按键并退出。

同步开启后，除 HOME/END 外的键盘、组合键和鼠标输入都会被主机拦截并转发到对端；同步关闭后恢复本机输入。

## 固件刷写

控制软件可以在不退出进程的情况下释放串口刷写固件，刷写完成后自动恢复连接。两个入口**最终都调用内置的独立版 esptool.exe**（构建时嵌入 exe，目标机无需安装 Python / ESP-IDF 环境；首次刷写时解压到 %LOCALAPPDATA%/HidBridge/embedded 缓存）。

EXE 不内嵌固件。设置页和远程 API 各自指定运行控制软件电脑上的 JSON 清单与串口；两者只共用清单校验、esptool 调用和串口恢复逻辑。刷写串口不要求应用固件先完成 HID Bridge 握手，便于应用固件异常时恢复。

### 方式一：设置页本地刷写（手动，推荐）

1. 运行 HidBridge.Host.exe，打开「设置」页底部的「本地固件刷写」模块。
2. 在模块内选择或输入刷写串口（例如 `COM3`）；点「刷新端口」可重新枚举。默认优先当前已连接串口，其次使用上次选择或端口列表首项。
3. 点「选择 JSON」并选择三段完整刷写清单；清单里的相对镜像路径按 JSON 所在目录解析，可按标准子目录（bootloader/、partition_table/）或与清单同目录平铺放置。
4. 点「确定」→ 弹窗确认 → 打开小日志窗口实时显示 esptool 进度。检测到多个串口时，确认窗口会列出全部串口和本次所选端口，确认后只刷写该端口。

### 方式二：远端刷写接口（远程调用）

接口监听所有 IPv4 网卡，默认端口 24815，可从受信任局域网直接请求，例如 `192.168.3.50:24815`。接口没有账号或令牌认证，不应暴露到公网或不可信网络。

**启用**：主界面「设置」页先选择并保存刷写串口，再勾选「启用局域网固件刷写接口」（保存后立即生效）。远程请求自动使用该串口。

**启动刷写**：

```powershell
$headers = @{ 'X-HidBridge-Action' = 'flash-firmware' }
$body = @{
    manifestPath = 'D:\ESP32-S3-HID-Bridge\firmware\build\flasher_args.json'
} | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri 'http://192.168.3.50:24815/api/v1/firmware/flash' -Headers $headers -ContentType 'application/json' -Body $body
```

**查询状态**：

```powershell
Invoke-RestMethod -Uri 'http://192.168.3.50:24815/api/v1/firmware/status'
```

状态 state 取值：`idle`（未执行）/ `running`（释放串口、刷写或恢复中）/ `succeeded`（三段哈希校验 + RTS 复位 + 串口恢复通过）/ `failed`（原因见 message，退出码见 exitCode）。重复提交时不会并发执行，返回 HTTP 409 和当前任务状态。

**安全边界**：监听局域网且没有强认证，只能用于受信任网络；POST 必须携带确认头，并在 JSON 正文中指定对端本机 `manifestPath`；不接受固件上传或命令行参数。串口自动取设置页已保存且当前存在的 `COM`；清单必须且只能包含 0x0、0x8000、0x10000 三段且镜像位于 JSON 所在目录内；刷写期间独占所选串口并暂停同步，程序退出时等待 esptool 安全结束。成功后日志输出「固件刷写最终摘要」（三段 SHA-256、设备校验计数、RTS 复位结果）。

### 内置 esptool 的重新构建

内置 esptool 由 scripts/build-embedded-esptool.ps1 用 PyInstaller 生成（独立单文件，v5.3.1，约 14MB），产物放在 host/HidBridge.Host/EmbeddedAssets/（不提交 Git），构建时作为嵌入资源打进 exe：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-embedded-esptool.ps1
dotnet build host/HidBridge.Host/HidBridge.Host.csproj -c Release
```

固件更新只需替换本地 JSON 及其引用镜像，不需要重新构建 EXE。

## 宏

宏是主机端按键脚本，保存在 profiles/<配置名>/macros/ 下，每个宏一个 .txt；配置目录中的 profile.json 只记录触发键、模式、启用状态和文件关联。在设置页选择激活的配置（automation.settings.json 的 ActiveProfile）。触发键示例：`f13`、`ctrl+f1`、`mouse_side1`。仓库提供完整示例配置（含宏与 Lua），见 [profiles.example/](profiles.example/)，复制到 profiles/ 即可使用。

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
| `delay` / `sleep` | ms | 可取消延时，底层共用同一实现 |
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

每个配置可携带一段 Lua 脚本，正文保存在 profiles/<配置名>/lua/ 下的 txt，profile.json 的 lua_script_file 记录关联文件，激活配置时自动运行。采用鼠标宏常见的 OnEvent 事件模型：按键/鼠标事件到达时调用 `OnEvent(event, arg)`，event 为 `pressed` / `released`，arg 为按键名（字符串，如 "a"、"f13"、"num0"）或鼠标键数字（1 左 / 2 右 / 3 中）。

Lua 页的“检查”会先校验脚本，再自动调整已有行的 4 空格缩进，并规范运算符、逗号等行内空格；不会插入或重排换行，也不会修改字符串和注释。旧版本把 Lua 正文写在 profile.json 的 lua_script_text 时，打开配置会先读取旧字段并自动转换为 lua/、macros/ 目录下的 txt，再保存新版 JSON。

Lua 语法检查、启动失败和运行时失败都会在状态或日志中显示脚本错误行号。

Lua 输入栏左侧显示随滚动同步的行号。Host 不再为每次按键额外生成 `press arg=...` / `release arg=...` 日志；脚本主动调用 `DebugLog(...)` 的内容仍会照常显示。

### 可用 API

| API | 说明 |
|---|---|
| `move(x, y)` | 相对移动鼠标，支持小数并按累计结果输出整数 HID 位移 |
| `moveto(x, y)` | 绝对坐标移动 |
| `mouse(button, state)` | 鼠标键按下/松开（1 左 / 2 右 / 3 中） |
| `wheel(amount)` | 垂直滚轮 |
| `keydown(key)` / `keyup(key)` / `keypress(key, hold_ms)` | 按键控制 |
| `delay(ms)` / `sleep(ms)` / `Sleep(ms)` | 可取消延时，底层共用同一实现，单位为毫秒 |
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

Lua 详细诊断日志写入 EXE 同目录的 `log/automation/automation-runtime-<时间戳>.log`；文件中保留 `LuaEvent`、`LuaOutput` 及开始/完成序号，便于分析长按、松开和连点问题。宏/Lua 页面只显示实际发布的状态日志和脚本 `DebugLog`，不会重复显示 Host 自动生成的按键摘要。

## UDP 模拟鼠标接口

主机 EXE 默认监听 UDP 0.0.0.0:24814，同一端口自动识别 UTF-8 JSON 和 kmboxNet 二进制协议。鼠标捕获页左上角会显示其他电脑实际可填写的局域网 IP 和端口。默认开启“始终开启 UDP 输出”时，即使 HOME 关闭也会发送到 ESP32；UDP 输入不会直接移动运行 EXE 电脑的光标。

```json
{"dx":12,"dy":-4,"wheel":0,"pan":0}
```

- dx/dy 范围 -32768..32767，wheel/pan 范围 -128..127，四项不能全零。
- EXE 以最高 500 Hz 聚合发送完整位移；开启平滑时，原生 USB 固件把每条报告叠加到滚动的 5 个 1 ms 槽并以 1000 Hz 消费，停止输入后的计划尾部不超过 5 ms。
- 无身份认证：仅应在受信任网络使用；只做本机测试可把 remoteInputBindAddress 改为 127.0.0.1。
- 可选发送示例：tools/send-remote-mouse.ps1 -HostAddress 192.168.1.20 -Port 24814 -Dx 25 -Dy -10。
- 主界面「UDP 平滑」开关可临时关闭分摊做 A/B；「始终开启 UDP 输出」默认开启，关闭 HOME 时仍允许网络 UDP 输入输出到 ESP32，关闭后网络 UDP 仅在 HOME 同步开启时输出。
- 设置页的「模拟 UDP 输入（测试）」默认关闭；启用后可把实体鼠标按 30/60/100/140/200/500 Hz/无上限分桶成模拟 UDP 源，仅用于测试输入聚合、平滑和输出链路，不代表真实网络性能。

### kmboxNet 兼容调用

可以继续使用 `kmboxnet-main/python_pyd` 中与 Python 版本匹配的 `kmNet.cp*.pyd`，调用名称不变，只需把 IP 和端口指向主界面左上角显示的监听地址。UUID 仍按原来的 8 位十六进制字符串传入：

```python
import kmNet

kmNet.init("192.168.1.20", "24814", "AF425414")
kmNet.move(10, -5)
kmNet.wheel(1)
```

兼容范围包括明文/加密鼠标与键盘输入、按钮、滚轮、`mouse_all`、`move_auto`、贝塞尔移动、`monitor/isdown_*`、`mask/unmask` 和 `trace`。其中：

- 所有 move 类调用忽略原盒子的轨迹参数，统一提交到现有 UDP move 链路。
- `trace(type, value)` 只映射为当前固件 5 槽 UDP 平滑开关：`value > 0` 开启，否则关闭。
- `monitor(port)` 不改变 EXE 的捕获状态，只登记原版 pyd 接收 21 字节实体键鼠状态的回传端口；`isdown_*` 继续读取 pyd 的本地状态缓存。
- `mask_*` 过滤实体键鼠到 ESP32 的转发，但 monitor 仍能看到被屏蔽的物理状态。
- `reboot`、`setconfig`、`setvidpid` 和 LCD 接口没有主机端等价设备，本版不处理。

### Python 调用示范

项目在 `tools/esp32_move.py` 内提供自己的 `Esp32MouseSender` 调用库，
`tools/send-remote-mouse-sample.py` 使用该库发送命令，不依赖外部项目路径。目标 IP、端口和
正方形参数都直接写在 `send-remote-mouse-sample.py` 顶部，不读取命令行参数或其他外部输入。

先编辑脚本顶部的 `TARGET_HOST`、`TARGET_PORT`、`SQUARE_SIDE_PIXELS`、
`MOVE_STEP_PIXELS` 和 `MOVE_INTERVAL_SECONDS`，然后直接运行：

```powershell
python .\tools\send-remote-mouse-sample.py
```

当前默认示范会顺时针分四条边发送一个边长 `100`、步长 `100` 的正方形，每条边发送
一次相对移动命令；每条边前暂停 `10 ms`，命令间隔为 `1 ms`。运行后可能导致目标鼠标移动。

调用库的最小用法为：

```python
from tools.esp32_move import Esp32MouseSender

sender = Esp32MouseSender("192.168.1.20", 24814)
try:
    sender.move(10, 0)
    sender.move(0, 10)
finally:
    sender.close()
```

本轮只执行了源码检查和不连接网络的正方形纯逻辑检查，没有在本机执行正方形发送演示。

## 统一输出灵敏度

鼠标捕获页底部的「输出灵敏度」是发送到固件前的最后一道 X/Y 相对移动处理。左侧滑块可在 `0.3` 到 `3.0` 之间拖动，右侧输入框也可直接输入数值；`1` 表示保持原始移动量，小于 `1` 会降低输出，大于 `1` 会放大输出。输入超出范围时会自动限制到边界，输入无效时恢复上一次有效值。

该比例在主机统一的 500 Hz 鼠标报告泵中生效：实体鼠标、UDP 输入、Lua `move()` 和宏 `move()` 都会经过同一处理，再编码为发送给 ESP32 固件的报告。因此无论输入来源如何，最终 X/Y 输出都使用同一个灵敏度；小数比例会保留未满一个整数报告的余量，连续移动不会因为逐条取整而丢失。滚轮、水平滚动和鼠标按键不受该设置影响；未开启 HOME 同步时走本机 Win32 输出的 Lua/宏动作也不会被该固件输出比例改写。

## 鼠标移动记录与分析图

该功能默认关闭，可在设置页开启。HOME 同步开启期间同时按住鼠标左键+右键开始记录；左右键松开 3 秒后自动停止并弹出分析图（X 有符号值时间序列 + Y 有符号值时间序列），同时保存到 EXE 同目录的 `log/mouse-movement-<时间戳>.png`。记录点位于 500 Hz 桥接报告实际提交边界；固件内的 1 ms 平滑样本不回传到该分析图。

## 主机配置参考（bridge.local.json）

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
  "automationLogPath": "log/automation/automation-runtime-{timestamp}.log",
  "automationLogRetentionCount": 10,
  "showDeviceLogInUi": false,
  "firmwareUpdateApiPort": 24815,
  "firmwareFlashBaudRate": 460800,
  "firmwareFlashTimeoutSeconds": 180
}
```

- transport 只能是 serial（wifi 实现保留但当前被功能闸门拒绝）。
- portName 为 auto 时自动扫描 COM 并完成随机数握手；固定串口模式不自动扫描。
- hostLogPath / deviceLogPath / automationLogPath 支持 `{timestamp}` 占位符；路径为相对路径时相对于 EXE 所在目录；deviceLogPath 置空可关闭设备日志。
- hostLogRetentionCount / deviceLogRetentionCount / automationLogRetentionCount 分别限制三类日志目录中的文件数量，默认每类保留 10 个，范围为 1..1000；创建新日志时优先删除最旧文件。
- showDeviceLogInUi 为启动默认值：false 精简模式 / true 完整日志模式；窗口内可随时切换。
- EXE 只内嵌刷写工具，不内嵌固件；远程 API 在请求正文中指定对端本机 JSON 路径，并自动使用设置页已保存的刷写串口。

### 串口、日志与输入租约

- `portName: "auto"` 会扫描串口并发送 `DeviceProbe`/`DeviceHello`；固定 COM 只适用于明确知道设备端口的环境。默认波特率为 `921600`。
- 同一个 COM 口不能同时由 Host、`idf.py monitor` 或其他串口工具打开；刷写前必须释放串口，刷写结束后再恢复会话。
- EXE 同目录的 `log/host` 保存主机运行日志，`log/device` 保存设备串口日志，`log/automation` 保存 Lua/宏详细事件和输出时间线；三类目录分别执行文件数量限制。
- `showDeviceLogInUi` 默认关闭。完整设备日志只用于短时排障；文件写入、UI 投递和实时输入线程相互隔离，UI 采用批量刷新和有界文本。
- 输入租约默认约 `1500 ms`；停止转发、COM 断开、USB/BLE 切换、HOME/END 和进程退出都必须执行 `ReleaseAll`。

### ESP-IDF 与 Wi-Fi/SoftAP

- 固件构建前进入项目提供的 ESP-IDF 终端，执行 `idf.py set-target esp32s3` 和 `idf.py build`；默认 USB/BLE 拓扑不需要启用 Wi-Fi。
- 设备配置使用仓库中的 `sdkconfig`/`sdkconfig.defaults`；BLE 绑定持久化依赖 `CONFIG_BT_NIMBLE_NVS_PERSIST`，板载 RGB 使用 GPIO48，当前约定为 USB 绿、BLE 蓝、无活动红灯闪烁。
- Wi-Fi 输入、SoftAP 配网和 Target Agent 后端目前只保留代码，不作为可用功能发布。运行时还必须保持 `HID_BRIDGE_WIFI_RUNTIME_ENABLED=0`，不能仅凭编译产物存在就认为 Wi-Fi 已启用。
- 未来恢复 SoftAP 前，需要重新设计认证、PSK/AES-256-GCM、计数器/时间窗防重放、心跳超时、重连和 `ReleaseAll`；不能直接开放当前无认证的 UDP 规则到 Wi-Fi。
- 规划中的配网入口可使用临时 `HID-Bridge-Setup-XXXX` 热点和 `192.168.4.1`，但当前不应把该流程当作已实现或已验收功能。

### UDP 平滑与安全边界

- 默认监听 `0.0.0.0:24814`，同端口接受 JSON 和 kmboxNet 二进制包；JSON 字段为 `dx`、`dy`、`wheel`、`pan`，范围分别为 `-32768..32767`、`-128..127`，四项全零的数据报拒绝并应由发送端在复用 socket 的前提下跳过。
- Host 最高 500 Hz 发送；原生 USB 固件使用固定 5 个 1 ms 滚动槽，整数位移在槽间守恒，连续命令叠加而不串行积压；关闭“UDP 平滑”时固件排空既有尾部并在下一 USB 周期直接输出。
- 真实延迟/平滑验收：`python tools/measure_udp_smoothing.py` 会向指定远端发送单次 20 px 和每 10 ms 连续三次 20 px，在本机按 1 ms 时间桶采样鼠标坐标；统计窗口从首条命令发送前 5 ms 开始，持续到最后位移结束后 5 ms，输出每次命令的开始延迟、50% 位移耗时、每步距离、总移动耗时，以及两张 SVG 图、CSV 和 JSON。测试结束会发送反向位移恢复鼠标起点；仅在明确授权时运行。
- “模拟 UDP 输入（测试）”位于设置页且默认关闭，可按 30/60/100/140/200/500 Hz 或无上限生成测试源；模拟结果不能替代真实 USB/BLE 和目标端 Raw Input 验收。
- 当前 UDP 入口没有身份认证、计数器或时间窗防重放，只适用于受信任局域网；本机测试应将 `remoteInputBindAddress` 改为 `127.0.0.1`。

### BLE 维护

- BLE 使用 NimBLE HID、Just Works 配对和绑定密钥持久化；升级固件或更换设备后若反复显示“已配对/已连接”，应在目标系统删除旧配对后重新配对。
- BLE 鼠标路径按约 10 ms 节拍发送合并状态；API 调用率、空口报告率和目标端 Raw Input 频率是三个不同指标，必须分别测量。
- 当前 USB/BLE 活动锁保证后连接链路不抢占；真实冷启动、插回 USB、BLE 断开回退和目标端输入仍需按版本单独验收。

## 常见问题

- **COM 被占用**：主机程序与 idf.py monitor、串口工具不能同时打开同一 COM 口；关闭占用程序后主机会自动重连。
- **检测到 CH340/CH341 Code 28**：发布件中的 Host 会显示驱动路径和管理员权限提示；只有确认“安装驱动”并通过 UAC 后才执行随包 WCH 驱动，其他 Problem Code 请在设备管理器中处理。
- **生成发布件**：在项目根目录执行 `scripts/Prepare-Release.ps1`；如果要把最新 ESP-IDF 构建也纳入发布件，使用 `-BuildFirmware`。
- **手动刷写提示「刷写镜像不存在」**：JSON 引用的 bin 必须位于清单所在目录内（支持 bootloader/、partition_table/ 子目录或平铺）。
- **BLE 配对后反复「已配对/已连接」**：旧固件无绑定持久化，升级后需在目标设备删除/忽略旧配对，重新配对一次。
- **防火墙弹窗**：UDP 24814 与刷写接口 24815 首次监听可能触发 Windows 防火墙提示。
- **exe 无法启动**：需要 .NET 8 Desktop Runtime（x64）。
- **不要输入回路**：不要把开发板原生 USB 口接回同一台电脑测试转发并同时抑制本地输入。

## 安全边界

- UDP 模拟鼠标入口默认开启且无身份认证，默认监听 0.0.0.0:24814，仅限受信任网络。
- 固件刷写接口默认关闭；启用后监听局域网 `0.0.0.0:24815`，不接受固件上传或串口参数，只接受对端本机 JSON 清单路径，并自动使用设置页已选择的串口。仅限受信任局域网。
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
drivers/wch-ch341ser/    WCH CH340/CH341 官方驱动归档与可安装 INF
release/                  可直接交付的 Host、默认 profiles、驱动和三段固件发布件
profiles/                 宏/Lua 配置（每个配置含 profile.json、macros/*.txt、lua/*.txt）
profiles.example/         示例配置（宏 + Lua，可复制到 profiles/）
docs/                     项目文档（协议、刷写 API、交接与审计记录）
log/                      EXE 运行日志、自动化日志、设备日志和可选分析图
artifacts/                检查/调试临时产物（不提交）
tools/                    辅助工具（如 UDP 发送示例）
```

主机检查程序（自检，无需连接开发板）：

```powershell
dotnet run --project tests/HidBridge.Host.Checks -c Release
```

协议细节（帧格式、消息类型、握手与 UDP 接口）见 [docs/protocol.md](docs/protocol.md)，固件刷写接口见 [docs/firmware-update-api.md](docs/firmware-update-api.md)；连接、配置和架构说明已整合在本 README。
