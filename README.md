# ESP32-S3 HID Bridge

把 Windows 电脑现有的键盘和鼠标事件，经 ESP32-S3-DevKitC-1 转换成独立的键盘与相对触摸板 HID，输出到手机、平板、嵌入式设备或其他项目。原生 USB 在启动时根据 UART 协议握手二选一枚举为 CDC-only 或 HID-only；BLE HID 保持可用。Wi-Fi 输入、配网和 Target Agent 输出代码暂时保留但不启用。

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

- `USB-to-UART` 接电脑，通过 CH340 COM 口接收主机端生成的 HID 报告。
- `ESP32-S3 USB` 在固件启动后的 1.5 秒内等待 UART 有效协议帧：检测到 UART 时枚举为 `USB Keyboard with Touchpad` HID-only；未检测到时枚举为 `HID Bridge CDC` CDC-only。

两种模式使用不同 PID，避免 Windows 缓存错误接口。选择结果保持到下次复位；“UART 已连接”指收到 CRC 正确的桥接协议帧，不代表仅插入一根没有通信的 USB-UART 线。CDC 模式可把原生 USB 作为输入并通过 BLE 输出；HID 模式应通过 CH340/UART 输入，并把原生 USB 接到被控端。

USB HID 与 BLE HID 同时可用时，先连接并成为活动输出的链路会保持锁定；另一链路随后连接不得抢占。USB HID 连续 100 ms 不可发送时会被判定为失活，即使 TinyUSB 的 `mounted` 状态因开发板仍由另一端口供电而没有清除，也会切换到仍在线的 BLE；反向切换同样只在当前活动链路失活后发生。切换前后都会执行 `ReleaseAll`，避免目标设备卡键。

官方 ESP32-S3-DevKitC-1 支持两个 USB 端口同时供电。第三方兼容板必须先核对原理图，确认两个端口之间没有不安全的 VBUS 回灌路径。

板载 RGB 状态灯默认使用 GPIO48，并显示“当前真正接收键鼠报告的活动输出”：USB HID 为绿灯，BLE HID 为蓝灯，无活动输出时红灯每 500 ms 闪烁。USB 与 BLE 同时在线时仍显示先成为活动输出的链路对应颜色；兼容板的 RGB 灯接线不同，可在 `menuconfig` 的 `HID Bridge` 菜单修改 GPIO 或关闭该功能。

## 当前里程碑

- [x] Windows 全局键盘和鼠标捕获骨架
- [x] Raw Input 原生相对鼠标移动
- [x] 500 Hz 鼠标位移累计、16 位 X/Y 报告和端到端统计
- [x] `HOME` 转发开关
- [x] 可配置串口与同步状态下的本机输入拦截
- [x] 带序号、长度与 CRC16 的串口帧
- [x] ESP-IDF UART 与原生 USB CDC 接收，共用流式协议解析
- [x] 原生 USB 根据启动期 UART 握手选择 CDC-only 或键盘+相对触摸板 HID-only，使用不同 PID
- [x] 链路心跳、输入租约、断连超时检测与自动 `ReleaseAll`
- [x] 电脑到开发板的认证加密 Wi-Fi 输入代码（当前运行入口暂时禁用）
- [x] SoftAP 网页配网、NVS 凭据保存和 BOOT 长按重新配网代码（当前运行入口暂时禁用）
- [x] 开发板到 Windows 目标 Agent 的认证加密 Wi-Fi 输出代码（当前运行入口暂时禁用）
- [x] 开发板到目标设备的 BLE HID 键盘/鼠标输出
- [x] 在实物 ESP32-S3-DevKitC-1 上完成固件构建、烧录和 USB-UART 通信验证
- [x] 串口自动发现、随机数设备握手与重连逻辑
- [x] 无 JSON 自包含 Windows 捕获 EXE
- [x] 默认启用、无身份认证的局域网 UDP 模拟鼠标输入与主机实时日志落盘
- [x] UDP 输入从首条命令开始分摊到固定 10 个 2 ms 槽，连续输入重叠合并且最大计划尾部固定为 20 ms，不再按 100 ms 前窗排队
- [x] 主界面提供默认开启的“UDP 平滑”开关，可让真实和模拟 UDP 临时跳过低延迟分摊以进行 A/B
- [x] 可将实体鼠标模拟为 30/60/100/140/200/500 Hz 或无上限 UDP 来源；所选频率只控制源事件分桶，之后与真实 UDP 共用完整处理链路
- [x] 左右键同时按下触发实际发送位移记录，松开 3 秒后显示并保存带正负方向的 X/Y 原始样本折线图
- [x] 先连接链路锁定、当前链路断开后切换另一在线链路的 USB/BLE 输出选择与安全释放逻辑
- [x] 托盘界面、配置页，以及默认关闭且仅监听本机的固件刷写接口

## 计划中的连接方式

输入传输与目标输出已拆分为可组合的后端；当前主机串口输入默认使用自动发现，固定端口仍可通过本机配置覆盖：

| 方向 | 连接方式 | 目标定位 |
|---|---|---|
| 电脑 → 开发板 | USB-to-UART | 已实现，CH340 COM 输入通道 |
| 电脑 → 开发板 | 原生 USB CDC | 启动期未检测到 UART 协议时启用，与 UART 使用同一协议和主机自动发现 |
| 电脑 → 开发板 | Wi-Fi | 实现代码保留，当前运行入口暂时禁用 |
| 开发板 → 目标设备 | USB HID | 启动期检测到 UART 协议时启用，枚举为键盘与相对触摸板 |
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

当前 BLE、USB CDC、日志及保留的 Wi-Fi 实现说明见 [docs/configuration.md](docs/configuration.md)。当前构建不会启动 Wi-Fi、临时 SoftAP 或 Target Agent 输出；相关配置只作为未来恢复参考。Wi-Fi 输入与 Target Agent 的预共享密钥仍只能写入被 Git 忽略的 `firmware/sdkconfig`、`bridge.local.json` 和 `agent.local.json`，不要写入仓库文件。

控制软件也可以在不退出进程的情况下释放串口并刷写固件，共有两个入口，最终都调用**内置的独立版 esptool.exe**（构建时嵌入，目标机无需安装 Python / ESP-IDF 环境）：

- **远端接口**：设置页勾选“启用本机固件刷写接口”（默认关闭），固定监听 127.0.0.1；调用方式见 [docs/firmware-update-api.md](docs/firmware-update-api.md)。固件来源为本地 firmware/build，找不到时回退到构建时嵌入的默认固件。
- **设置页本地刷写**：设置页底部“本地固件刷写”区，选择固件文件（flasher_args.json 三段刷写，或单个 .bin 按 0x10000 应用分区刷写），点“确定”后弹窗二次确认，并打开带实时日志的小窗口显示刷写进度。

内置 esptool 由 [scripts/build-embedded-esptool.ps1](scripts/build-embedded-esptool.ps1) 生成并放入 host/HidBridge.Host/EmbeddedAssets/（不提交 git），构建时作为嵌入资源打进 exe；首次刷写时解压到 %LOCALAPPDATA%/HidBridge/embedded 缓存。

烧录后可任选输入连接方式：

- **USB-to-UART 输入 + 原生 USB HID 输出**：先让主机程序通过 CH340/UART 发送握手，再复位开发板；启动后 1.5 秒内检测到有效 UART 帧时，原生 USB 枚举为 `USB Keyboard with Touchpad`。
- **原生 USB CDC 输入 + BLE 输出**：启动和复位期间不要让 UART 发送桥接协议；超时后原生 USB 枚举为 `HID Bridge CDC`，随后主机程序可自动发现该 COM 口并通过 BLE 输出。

当前选择只在启动时执行，切换连接方式后需要复位开发板。HID 中的“触摸板”是与现有相对鼠标报告兼容的相对指针，不是 Windows Precision Touchpad，也不提供多点触控手势。

随后启动主机端；默认会自动发现正确的 COM 口，不需要填写端口号。按 `HOME` 开始转发。

主机 Release 构建会把单文件 `HidBridge.Host.exe` 复制到项目根目录。`bridge.json` 不是必需的；删除所有 JSON 后仍使用串口自动发现。若需要固定端口或调整日志参数，再创建 `bridge.local.json`；`transport=wifi` 当前会被功能闸门拒绝。

## 安全边界

- 局域网 UDP 模拟鼠标入口默认开启，且不做身份认证；默认监听 `0.0.0.0:24814`。仅应在受信任网络使用；如需缩小暴露面，请改为 `127.0.0.1` 或通过防火墙限制来源。
- 模拟入口只接受相对移动和滚轮增量，且仅在 `HOME` 同步开启时进入现有 HID 转发链路。
- 固件刷写 API 默认关闭且只绑定 `127.0.0.1`，不接受上传、文件路径或命令参数；远程操作应在目标机上发起本机请求。
- 主机程序退出、串口断开或切换转发状态时必须发送 `ReleaseAll`，防止目标设备卡键。
- HOME/END 始终由主机端优先处理；停止转发、串口断开或程序退出时会发送 `ReleaseAll`。
- 不要将 USB 原生口接回同一台电脑测试转发并同时抑制本地输入，以免形成难以操作的输入回路。

详细设计见 [docs/architecture.md](docs/architecture.md)，串口格式见 [docs/protocol.md](docs/protocol.md)。
