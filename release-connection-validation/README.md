# 双 ESP32-S3 连接验证固件发布件

这是独立于正式 HID Bridge 固件的第一阶段连接验证镜像。两块开发板刷入同一个 `dual_s3_connection_validation.bin`，用于验证 USB Device/Host 角色切换、UART1 板间链路和鼠标侧 HID 枚举；本镜像不发送鼠标或键盘输入报告。

## 使用现有 Host 刷写工具

在现有 `HidBridge.Host.exe` 的固件刷写页面中选择本目录下的：

```text
firmware/flasher_args.json
```

清单与三段镜像必须保持相对目录关系。清单使用 ESP32-S3 的标准偏移：`0x0` 为 bootloader、`0x8000` 为 partition table、`0x10000` 为应用；Flash 参数为 `dio / 80m / 8MB`。也可以使用 `firmware/flash_project_args` 作为 ESP-IDF/esptool 的命令行参考。

两块板都刷入同一个清单和同一个应用镜像。刷写前确认目标串口，避免选错生产设备。

## UART1 接线

两块板断电后交叉连接，之后再分别上电：

- A 板 GPIO17（TX）→ B 板 GPIO18（RX）
- A 板 GPIO18（RX）← B 板 GPIO17（TX）
- A 板 GND ↔ B 板 GND

UART1 参数为 `921600 8N1`，GPIO17/18 是 3.3 V TTL 电平。不要连接 RS-232 电平。

不要连接两块板的 `5V` 或 `3V3`，不要把两块板的电源并联；每块板使用各自的 USB/CH340 供电。鼠标侧按实验方案使用外置供电，并确保信号地共地。

## 当前实机结论

- 两块板均已实际刷写成功：COM4 第二板刷写 Pass，bootloader、分区表和应用三段均完成校验；两块板使用同一镜像。
- 双板 UART1 验证 Pass：两端 node ID 不同，双方 `tx/rx` 均增长，`crc=0`，双方均报告 `peer online`。
- USB 角色验证 Pass：连接电脑的一端锁定 `PC_DEVICE`、`usb=1`，电脑枚举出 `COM7`；另一端锁定 `MOUSE_HOST`、`usb=2`。
- 鼠标 Host 枚举 Pass：发现 Logitech `VID=046D`、`PID=C092`；接口 0 为 `protocol=2`，接口 1 为 `protocol=0`。
- 本次没有主动移动鼠标或点击按键，因此 `HID reports=0`、`errors=0` 只能说明观察期间无报告/错误，实际输入报告内容仍未验证。
- 完整鼠标透传、单一逻辑 HID 报告合并、软件移动叠加以及 Logitech 驱动完整功能仍未实现或未验证；当前发布件只用于连接、角色和枚举验证。

Logitech 的 VID/PID 与接口描述已经被 Host 侧识别，但这不等同于 Logitech 驱动加载、特性报告和完整输入行为已验收。本发布件不应覆盖正式 `firmware/` 发布件。

## 文件校验

`SHA256SUMS.txt` 覆盖本目录内的所有发布文件（不包含自身），用于复制或刷写前核对文件完整性。
