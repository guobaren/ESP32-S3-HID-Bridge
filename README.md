# ESP32-S3 HID Bridge

把 Windows 电脑现有的键盘和鼠标事件，经 ESP32-S3-DevKitC-1 转换成独立的复合 HID 键鼠设备，输出到手机、平板、嵌入式设备或其他项目。当前运行配置支持板载 USB-to-UART 或原生 USB CDC 输入，以及 USB HID 与 BLE HID 输出；Wi-Fi 输入、配网和 Target Agent 输出代码暂时保留但不启用。

## 数据路径

```text
Windows 键盘/鼠标
        │ 键鼠低级钩子 + 鼠标 Raw Input
        ▼
HidBridge.Host
        │ USB-to-UART 或原生 USB CDC，自动发现与二进制帧握手
        ▼
ESP32-S3-DevKitC-1
        ├─ 原生 USB OTG，CDC + 复合 HID ──> 主机输入 / USB 目标设备
        ├─ BLE HID ────────────────> 手机/电脑
```

开发板支持两种有线主机输入方式：

- `USB-to-UART` 接电脑，通过 CH340 COM 口接收主机端生成的 HID 报告。
- `ESP32-S3 USB` 接电脑时枚举为 `CDC + HID` 复合设备；CDC COM 口接收同一套二进制协议，HID 接口仍可作为 USB 输出。

因此只使用一根原生 USB 线，也可以让 `HidBridge.Host` 通过自动发现的 CDC COM 口把本机键鼠送入开发板，再由 BLE HID 输出到另一台设备。若原生 USB 枚举后仍没有 COM 口，应先确认已烧录包含 CDC 的本版固件，并检查 Windows 设备枚举状态。

USB HID 与 BLE HID 同时可用时，已完成加密且真正可发送的 BLE HID 优先取得输出；BLE 断开后自动安全回退到 USB HID。切换前后都会执行 `ReleaseAll`，避免目标设备卡键。

官方 ESP32-S3-DevKitC-1 支持两个 USB 端口同时供电。第三方兼容板必须先核对原理图，确认两个端口之间没有不安全的 VBUS 回灌路径。

板载 RGB 状态灯默认使用 GPIO48，并显示“当前真正接收键鼠报告的活动输出”：USB HID 为绿灯，BLE HID 为蓝灯，无活动输出时红灯每 500 ms 闪烁。USB 与 BLE 同时在线且 BLE 已就绪时显示蓝灯；兼容板的 RGB 灯接线不同，可在 `menuconfig` 的 `HID Bridge` 菜单修改 GPIO 或关闭该功能。

## 当前里程碑

- [x] Windows 全局键盘和鼠标捕获骨架
- [x] Raw Input 原生相对鼠标移动
- [x] 500 Hz 鼠标位移累计、16 位 X/Y 报告和端到端统计
- [x] `HOME` 转发开关
- [x] 可配置串口与同步状态下的本机输入拦截
- [x] 带序号、长度与 CRC16 的串口帧
- [x] ESP-IDF UART 与原生 USB CDC 接收，共用流式协议解析
- [x] 原生 USB CDC + HID 复合设备，HID 使用双 Report ID 键盘/鼠标描述符
- [x] 链路心跳、输入租约、断连超时检测与自动 `ReleaseAll`
- [x] 电脑到开发板的认证加密 Wi-Fi 输入代码（当前运行入口暂时禁用）
- [x] SoftAP 网页配网、NVS 凭据保存和 BOOT 长按重新配网代码（当前运行入口暂时禁用）
- [x] 开发板到 Windows 目标 Agent 的认证加密 Wi-Fi 输出代码（当前运行入口暂时禁用）
- [x] 开发板到目标设备的 BLE HID 键盘/鼠标输出
- [x] 在实物 ESP32-S3-DevKitC-1 上完成固件构建、烧录和 USB-UART 通信验证
- [x] 串口自动发现、随机数设备握手与重连逻辑
- [x] 无 JSON 自包含 Windows 捕获 EXE
- [x] 带预共享密钥的局域网 UDP 模拟鼠标输入与主机实时日志落盘
- [x] BLE 优先、USB 回退的输出选择与安全接管逻辑
- [ ] 增加托盘界面与配置页

## 计划中的连接方式

输入传输与目标输出已拆分为可组合的后端；当前主机串口输入默认使用自动发现，固定端口仍可通过本机配置覆盖：

| 方向 | 连接方式 | 目标定位 |
|---|---|---|
| 电脑 → 开发板 | USB-to-UART | 已实现，CH340 COM 输入通道 |
| 电脑 → 开发板 | 原生 USB CDC | 已实现，与 UART 使用同一协议和主机自动发现 |
| 电脑 → 开发板 | Wi-Fi | 实现代码保留，当前运行入口暂时禁用 |
| 开发板 → 目标设备 | USB HID | 当前默认输出通道，可用于无需安装配套程序的目标设备 |
| 开发板 → 目标设备 | Wi-Fi | Target Agent 代码保留，当前运行入口暂时禁用 |
| 开发板 → 目标设备 | BLE HID | 已实现标准 BLE 键盘和相对鼠标报告 |

所有输入通道都必须提供心跳或连接租约。当前活动输入通道断开、超时或切换时，开发板必须释放全部键盘按键和鼠标按钮，之后才能接受新的输入会话。

## 快速开始

### 1. Windows 主机端

默认配置会自动发现开发板，不需要填写 COM 号：

```powershell
Set-Location D:\ESP32-S3-HID-Bridge\host\HidBridge.Host
dotnet run
```

需要固定串口或调整其他参数时，再复制本机配置并修改：

```powershell
Copy-Item bridge.json bridge.local.json
```

快捷键：

- `HOME`：启用或停止向目标设备转发。
- `END`：安全释放所有按键并退出。

同步开启后，除 HOME/END 控制键外的键盘、组合键和鼠标输入都会被主机拦截并转发到对端；同步关闭后恢复本机输入。

### 2. ESP32-S3 固件

要求：

- 项目内已安装 ESP-IDF 6.0.2，位于 `.esp-idf/`（该目录不提交到 Git）
- ESP32-S3-DevKitC-1
- 电脑连接开发板的 `USB-to-UART` 或原生 `ESP32-S3 USB` 端口

```powershell
Set-Location D:\ESP32-S3-HID-Bridge\firmware
. ..\scripts\Enter-EspIdf.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p <实际串口> flash
```

激活脚本只修改当前 PowerShell 会话；重新打开终端后需要再次执行。

当前 BLE、USB CDC、日志及保留的 Wi-Fi 实现说明见 [docs/configuration.md](docs/configuration.md)。当前构建不会启动 Wi-Fi、临时 SoftAP 或 Target Agent 输出；相关配置只作为未来恢复参考。预共享密钥仍只能写入被 Git 忽略的 `firmware/sdkconfig`、`bridge.local.json` 和 `agent.local.json`，不要写入仓库文件。

烧录后可任选输入连接方式：

- **USB-to-UART 输入**：保持 `USB-to-UART` 端口连接输入电脑；如果目标使用 USB HID，再把 `ESP32-S3 USB` 原生口连接目标设备。
- **原生 USB CDC 输入 + BLE 输出**：只把 `ESP32-S3 USB` 原生口连接输入电脑，确认 Windows 同时枚举 CDC COM 与 HID；BLE 配对完成后，输入电脑的数据经 CDC 进入开发板并从 BLE 输出到目标设备。请先等待 BLE 就绪且状态灯变蓝，再按 `HOME` 开始转发；BLE 尚未就绪时会按回退策略使用同一根线上的 USB HID，可能把输入送回输入电脑。

随后启动主机端；默认会自动发现正确的 COM 口，不需要填写端口号。按 `HOME` 开始转发。

主机 Release 构建会把单文件 `HidBridge.Host.exe` 复制到项目根目录。`bridge.json` 不是必需的；删除所有 JSON 后仍使用串口自动发现。若需要固定端口或调整日志参数，再创建 `bridge.local.json`；`transport=wifi` 当前会被功能闸门拒绝。

## 安全边界

- 局域网 UDP 模拟鼠标入口默认关闭；启用时必须设置至少 16 字节的独立预共享密钥，并仅在可信局域网开放对应防火墙端口。
- 模拟入口只接受相对移动和滚轮增量，且仅在 `HOME` 同步开启时进入现有 HID 转发链路。
- 主机程序退出、串口断开或切换转发状态时必须发送 `ReleaseAll`，防止目标设备卡键。
- HOME/END 始终由主机端优先处理；停止转发、串口断开或程序退出时会发送 `ReleaseAll`。
- 不要将 USB 原生口接回同一台电脑测试转发并同时抑制本地输入，以免形成难以操作的输入回路。

详细设计见 [docs/architecture.md](docs/architecture.md)，串口格式见 [docs/protocol.md](docs/protocol.md)。
