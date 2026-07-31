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

协议层与输出后端解耦后，可增加：

- USB HID 与 BLE HID 同时输出。
- 多配置文件与目标设备切换。
- 媒体键和 Consumer Control。
- 键盘 NKRO 报告。
- 双向握手、固件版本查询和链路看门狗。
