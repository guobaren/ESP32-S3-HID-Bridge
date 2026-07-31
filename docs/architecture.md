# 架构说明

## 组件

### HidBridge.Host

Windows 主机程序负责：

1. 通过 `WH_KEYBOARD_LL` 接收全局键盘事件，通过 `WH_MOUSE_LL` 实现可选的本地鼠标输入抑制。
2. 通过 Windows Raw Input 接收鼠标设备的原生相对位移、按钮和滚轮事件。
3. 维护当前键盘修饰键、普通键和鼠标按钮状态。
4. 将状态转换成标准 USB HID Boot Keyboard/Mouse 报告。
5. 通过 USB-to-UART 串口发送给 ESP32-S3。
6. 在停止转发和退出时发送 `ReleaseAll`。

主机端不创建 Windows 虚拟设备，也不注入输入，因此不会与目标设备的 HID 枚举混在一起。

### ESP32-S3 固件

固件同时承担两个互相独立的传输角色：

- UART 接收端：从板载 USB-UART 桥读取主机帧。
- USB Device 端：通过 ESP32-S3 内置 USB PHY 对外暴露 HID。

USB 侧使用一个 HID Interface 和两个 Report ID：

| Report ID | 类型 | 数据长度 |
|---:|---|---:|
| 1 | Boot Keyboard | 8 字节 |
| 2 | 相对坐标 Mouse | 5 字节 |

## 第一阶段边界

- 目标输出先实现 USB HID。
- BLE HID 留作可选输出后端，不与第一阶段耦合。
- 键盘采用 6-key rollover；超过 6 个普通键时只上报最早的 6 个。
- 鼠标采用相对位移，单帧范围为 `-127..127`，主机端自动拆分大位移。
- 鼠标移动优先使用 Raw Input 的设备原生相对量，不依赖屏幕指针坐标，因此不受屏幕边缘和 Windows 指针加速影响；绝对坐标类 Raw Input 设备不作为移动来源。
- 主机与开发板之间暂不做身份认证，因为链路为用户明确连接的本地 USB 串口。

## 后续演进

后续将把系统拆分成独立的输入传输层、状态协调层和目标输出层：

```text
电脑输入
  ├─ USB-to-UART ─┐
  └─ Wi-Fi ───────┤
                   ▼
             状态协调与断连保护
                   │
        ┌──────────┼──────────┐
        ▼          ▼          ▼
     USB HID    BLE HID    Wi-Fi Agent
        │          │          │
        └──────────┴──────────┘
                   ▼
                目标设备
```

### 断连检测与状态安全

- 每个主机连接先发送 `SessionStart`，之后用 `Ping` 维持有限期限的输入租约。
- 固件在活动输入通道超时、主动断开、重连或切换时执行 `ReleaseAll`。
- 重连先重置会话，不能沿用断开前的按键或按钮状态。
- 多个输入通道同时可用时，只允许一个通道持有活动输入租约，避免状态交错；当前通道释放后，其他通道可通过心跳恢复租约。
- Wi-Fi Target Agent 同样维护输出租约，开发板断开后在目标 Windows 会话中释放输入。

### 电脑到开发板

- 保留 USB-to-UART 作为低延迟、无需网络配置的默认通道。
- Wi-Fi 输入使用项目原生安全通道，包含预共享密钥双向认证、AES-256-GCM 和重放保护。
- Wi-Fi 服务拒绝未认证连接，不开放匿名键鼠控制接口。
- Wi-Fi SSID 和密码由 SoftAP captive portal 写入 NVS；无凭据、连接失败或长按 BOOT 时进入配网模式，连接成功后关闭临时热点。
- 进入配网模式前清空输入租约并向所有输出后端发送 `ReleaseAll`。

### 开发板到目标设备

- 保留 USB HID 作为兼容性最强的默认输出。
- BLE HID 使用 NimBLE，使开发板可以作为标准蓝牙键盘和相对鼠标配对。
- Wi-Fi Agent 输出通过同一安全通道连接 Windows 目标端，并由 `SendInput` 注入当前用户会话。
- Wi-Fi Agent 不属于标准 HID，无法覆盖 BIOS、系统登录前界面或不能安装配套程序的设备。

在上述传输与输出后端稳定后，再增加：

- USB HID、BLE HID 与 Wi-Fi Agent 的可配置输出策略。
- 多配置文件与目标设备切换。
- 媒体键和 Consumer Control。
- 键盘 NKRO 报告。
- 固件版本查询、运行状态与链路诊断。
