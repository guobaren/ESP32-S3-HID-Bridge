# ESP32-S3 HID Bridge

把 Windows 电脑现有的键盘和鼠标事件，经 ESP32-S3-DevKitC-1 转换成独立的复合 HID 键鼠设备，输出到手机、平板、嵌入式设备或其他项目。当前支持 USB HID、BLE HID 和 Wi-Fi Target Agent 三种输出后端；输入默认使用板载 USB-to-UART 串口。

## 数据路径

```text
Windows 键盘/鼠标
        │ 键鼠低级钩子 + 鼠标 Raw Input
        ▼
HidBridge.Host
        │ USB-to-UART，自动发现与二进制帧握手
        ▼
ESP32-S3-DevKitC-1
        ├─ 原生 USB OTG，复合 HID ──> USB 目标设备
        ├─ BLE HID ────────────────> 手机/电脑
        └─ Wi-Fi Agent ────────────> Windows 目标端
```

开发板同时使用两个接口：

- `USB-to-UART` 接电脑，接收主机端生成的 HID 报告。
- `ESP32-S3 USB` 接目标设备，对外枚举为复合 USB HID 键盘和鼠标。

USB HID 与 BLE HID 同时连接时，首个真正建立连接的后端取得输出锁；另一个后端仍可连接但不会收到报告。活动后端断开后，已在线的另一后端才会安全接管，并在接管前发送 `ReleaseAll`。

官方 ESP32-S3-DevKitC-1 支持两个 USB 端口同时供电。第三方兼容板必须先核对原理图，确认两个端口之间没有不安全的 VBUS 回灌路径。

板载 RGB 状态灯默认使用 GPIO48：USB HID 已被目标设备枚举时亮绿灯，BLE HID 已连接时亮蓝灯，两者均未连接时红灯每 500 ms 闪烁。USB 和 BLE 同时连接时优先显示 USB 的绿灯；兼容板的 RGB 灯接线不同，可在 `menuconfig` 的 `HID Bridge` 菜单修改 GPIO 或关闭该功能。

## 当前里程碑

- [x] Windows 全局键盘和鼠标捕获骨架
- [x] Raw Input 原生相对鼠标移动
- [x] 500 Hz 鼠标位移累计、16 位 X/Y 报告和端到端统计
- [x] `HOME` 转发开关
- [x] 可配置串口与同步状态下的本机输入拦截
- [x] 带序号、长度与 CRC16 的串口帧
- [x] ESP-IDF UART 接收与流式协议解析
- [x] 单 HID 接口、双 Report ID 的 USB 键盘/鼠标描述符
- [x] 链路心跳、输入租约、断连超时检测与自动 `ReleaseAll`
- [x] 电脑到开发板的认证加密 Wi-Fi 输入通道
- [x] SoftAP 网页配网、NVS 凭据保存和 BOOT 长按重新配网
- [x] 开发板到 Windows 目标 Agent 的认证加密 Wi-Fi 输出通道
- [x] 开发板到目标设备的 BLE HID 键盘/鼠标输出
- [x] 在实物 ESP32-S3-DevKitC-1 上完成固件构建、烧录和 USB-UART 通信验证
- [x] 串口自动发现、随机数设备握手与重连逻辑
- [x] 无 JSON 自包含 Windows 捕获 EXE
- [x] BLE/USB 输出后端首连接锁定与安全接管逻辑
- [ ] 增加托盘界面与配置页

## 计划中的连接方式

输入传输与目标输出已拆分为可组合的后端；当前主机串口输入默认使用自动发现，固定端口仍可通过本机配置覆盖：

| 方向 | 连接方式 | 目标定位 |
|---|---|---|
| 电脑 → 开发板 | USB-to-UART | 当前默认输入通道 |
| 电脑 → 开发板 | Wi-Fi | 已实现预共享密钥认证、AES-256-GCM 加密和防重放计数器 |
| 开发板 → 目标设备 | USB HID | 当前默认输出通道，可用于无需安装配套程序的目标设备 |
| 开发板 → 目标设备 | Wi-Fi | 已实现 Windows Target Agent；不作为通用 HID |
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
- 电脑连接开发板的 `USB-to-UART` 端口

```powershell
Set-Location D:\ESP32-S3-HID-Bridge\firmware
. ..\scripts\Enter-EspIdf.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p <实际串口> flash
```

激活脚本只修改当前 PowerShell 会话；重新打开终端后需要再次执行。

Wi-Fi、BLE 和目标 Agent 的配置步骤见 [docs/configuration.md](docs/configuration.md)。首次烧录后可通过临时 SoftAP 网页设置 Wi-Fi；SSID 和密码保存在开发板 NVS，不需要为更换网络重新编译。预共享密钥仍只写入被 Git 忽略的 `firmware/sdkconfig`、`bridge.local.json` 和 `agent.local.json`，不要写入仓库文件。

烧录后：

1. 保持 `USB-to-UART` 端口连接电脑。
2. 用数据线把 `ESP32-S3 USB` 原生 USB 端口连接目标设备。
3. 启动主机端；默认会自动发现开发板，不需要填写 COM 号。
4. 按 `HOME` 开始转发。

主机端也可以发布为自包含单 EXE。发布目录中的 `bridge.json` 不是必需的；删除所有 JSON 后，程序仍会使用自动发现模式。若需要固定端口或 Wi-Fi 参数，再创建 `bridge.local.json`。

## 安全边界

- 本项目只转发本机实际产生的键鼠事件，不提供网络远程控制入口。
- 主机程序退出、串口断开或切换转发状态时必须发送 `ReleaseAll`，防止目标设备卡键。
- HOME/END 始终由主机端优先处理；停止转发、串口断开或程序退出时会发送 `ReleaseAll`。
- 不要将 USB 原生口接回同一台电脑测试转发并同时抑制本地输入，以免形成难以操作的输入回路。

详细设计见 [docs/architecture.md](docs/architecture.md)，串口格式见 [docs/protocol.md](docs/protocol.md)。
