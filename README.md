# ESP32-S3 HID Bridge

把 Windows 电脑现有的键盘和鼠标事件，经 ESP32-S3-DevKitC-1 转换成一个独立的复合 HID 键鼠设备，供手机、平板、嵌入式设备或其他项目使用。

## 数据路径

```text
Windows 键盘/鼠标
        │ 键鼠低级钩子 + 鼠标 Raw Input
        ▼
HidBridge.Host
        │ USB-to-UART，二进制帧
        ▼
ESP32-S3-DevKitC-1
        │ 原生 USB OTG，复合 HID
        ▼
目标设备（键盘 + 鼠标）
```

开发板同时使用两个接口：

- `USB-to-UART` 接电脑，接收主机端生成的 HID 报告。
- `ESP32-S3 USB` 接目标设备，对外枚举为复合 USB HID 键盘和鼠标。

官方 ESP32-S3-DevKitC-1 支持两个 USB 端口同时供电。第三方兼容板必须先核对原理图，确认两个端口之间没有不安全的 VBUS 回灌路径。

板载 RGB 状态灯默认使用 GPIO48：USB HID 已被目标设备枚举时亮绿灯，BLE HID 已连接时亮蓝灯，两者均未连接时红灯每 500 ms 闪烁。USB 和 BLE 同时连接时优先显示 USB 的绿灯；兼容板的 RGB 灯接线不同，可在 `menuconfig` 的 `HID Bridge` 菜单修改 GPIO 或关闭该功能。

## 当前里程碑

- [x] Windows 全局键盘和鼠标捕获骨架
- [x] Raw Input 原生相对鼠标移动
- [x] 500 Hz 鼠标位移累计、16 位 X/Y 报告和端到端统计
- [x] `Ctrl+Alt+F12` 转发开关
- [x] 可配置串口和本地输入抑制
- [x] 带序号、长度与 CRC16 的串口帧
- [x] ESP-IDF UART 接收与流式协议解析
- [x] 单 HID 接口、双 Report ID 的 USB 键盘/鼠标描述符
- [x] 链路心跳、输入租约、断连超时检测与自动 `ReleaseAll`
- [x] 电脑到开发板的认证加密 Wi-Fi 输入通道
- [x] SoftAP 网页配网、NVS 凭据保存和 BOOT 长按重新配网
- [x] 开发板到 Windows 目标 Agent 的认证加密 Wi-Fi 输出通道
- [x] 开发板到目标设备的 BLE HID 键盘/鼠标输出
- [ ] 在实物 ESP32-S3-DevKitC-1 上完成首次烧录验证
- [ ] 增加串口自动发现和设备握手
- [ ] 增加托盘界面与配置页

## 计划中的连接方式

后续将把输入传输与目标输出拆分为可组合的后端：

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

复制默认配置：

```powershell
Set-Location E:\ESP32-S3-HID-Bridge\host\HidBridge.Host
Copy-Item bridge.json bridge.local.json
```

修改 `bridge.local.json` 中的串口号，然后运行：

```powershell
dotnet run
```

快捷键：

- `Ctrl+Alt+F12`：启用或停止向目标设备转发。
- `Ctrl+Alt+F11`：安全释放所有按键并退出。

默认不会阻止输入继续传给 Windows。确认桥接工作正常后，才建议把 `suppressLocalInput` 改为 `true`。

### 2. ESP32-S3 固件

要求：

- 项目内已安装 ESP-IDF 6.0.2，位于 `.esp-idf/`（该目录不提交到 Git）
- ESP32-S3-DevKitC-1
- 电脑连接开发板的 `USB-to-UART` 端口

```powershell
Set-Location E:\ESP32-S3-HID-Bridge\firmware
. ..\scripts\Enter-EspIdf.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash
```

激活脚本只修改当前 PowerShell 会话；重新打开终端后需要再次执行。

Wi-Fi、BLE 和目标 Agent 的配置步骤见 [docs/configuration.md](docs/configuration.md)。首次烧录后可通过临时 SoftAP 网页设置 Wi-Fi；SSID 和密码保存在开发板 NVS，不需要为更换网络重新编译。预共享密钥仍只写入被 Git 忽略的 `firmware/sdkconfig`、`bridge.local.json` 和 `agent.local.json`，不要写入仓库文件。

烧录后：

1. 保持 `USB-to-UART` 端口连接电脑。
2. 用数据线把 `ESP32-S3 USB` 原生 USB 端口连接目标设备。
3. 修改主机配置中的串口号。
4. 启动主机端并按 `Ctrl+Alt+F12`。

## 安全边界

- 本项目只转发本机实际产生的键鼠事件，不提供网络远程控制入口。
- 主机程序退出、串口断开或切换转发状态时必须发送 `ReleaseAll`，防止目标设备卡键。
- 启用 `suppressLocalInput` 后，紧急退出热键仍由主机端优先处理。
- 不要将 USB 原生口接回同一台电脑测试转发并同时抑制本地输入，以免形成难以操作的输入回路。

详细设计见 [docs/architecture.md](docs/architecture.md)，串口格式见 [docs/protocol.md](docs/protocol.md)。
