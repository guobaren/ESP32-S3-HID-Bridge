# 双 ESP32-S3 连接验证固件

这是独立于正式 HID Bridge 的第一阶段验证工程，不会发送任何键盘或鼠标输出。
两块相同开发板烧录同一个镜像：连接电脑的板会在 USB Device 枚举成功后锁定为
`PC_DEVICE`；未被电脑枚举的板会在 5 秒后卸载 Device 栈并切换到 USB Host，等待并读取
鼠标 HID 输入报告。

## 接线

两板必须断电接线，之后再上电：

- A GPIO17 (TX) -> B GPIO18 (RX)
- A GPIO18 (RX) <- B GPIO17 (TX)
- A GND <-> B GND

UART1 使用 921600 baud、8N1。GPIO17/18 只能接 3.3 V TTL，禁止连接 RS-232 电平。
两块板的 USB-to-UART/CH340 口可以分别接电脑，用 115200 baud 查看日志；它们不参与板间数据传输。

鼠标必须按用户方案外置供电，并与开发板共地。不要把两个 5 V 电源直接并联。

## 构建与刷写

```powershell
Set-Location .\firmware\connection_validation
. ..\..\scripts\Enter-EspIdf.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

两块板使用同一镜像。日志判定：

- `USB角色锁定: PC_DEVICE`：电脑成功枚举原生 USB Device。
- `USB角色锁定: MOUSE_HOST` 且随后出现鼠标 VID/PID、HID interface 和 `HID报告`：鼠标通信成功。
- `UART1对端上线`：GPIO17/18/GND 的双向链路成功，日志还会持续给出收发、CRC错误和超时计数。

## 已知硬件边界

当前板原理图的 USB-C CC1/CC2 固定为 5.1 kΩ 下拉，且没有可控的标准 Host VBUS/CC
角色电路。因此“同一 USB-C 口盲插电脑或鼠标”在这里是实验验证目标，不是固件能够保证的
USB-C 合规能力。如果 Host 阶段无法枚举鼠标，应停止后续完整固件开发并重新评估硬件方案。

## 2026-09-15 实机结果

- ESP-IDF 6.0.2 构建通过，应用镜像为 305,424 bytes。
- 通过 CH340 `COM3` 刷写成功，bootloader、分区表和应用三段均通过写入哈希校验。
- 设备探测 5 秒后正确卸载 USB Device 并锁定为 `MOUSE_HOST`。
- 在用户已连接开发板和外置供电鼠标的条件下连续观察约 40 秒，没有发现 HID 设备；
  `HID reports=0`、驱动错误计数为 0。
- 结论：当前 USB-C/供电/CC 接线条件下鼠标枚举验证 **Failed**。按约定停止，不进入完整鼠标固件。
- 只有一块板参与本轮实测，所以 GPIO17/18 板间 UART 接收仍未验证；`tx` 持续增长且 `rx=0`
  是预期的单板状态，不能据此判断双板 UART 失败。
