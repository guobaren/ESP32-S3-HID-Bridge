# 当前进度

更新时间：2026-08-12

当前状态：稳定。本机固件刷写 API 已在远端以当前镜像完成真实烧录，控制软件无需退出或重启。

## 当前运行范围

```text
Windows ── USB-to-UART 或原生 USB CDC ──> ESP32-S3
ESP32-S3 ── USB 键盘触摸板 HID 或 BLE HID ──> 目标设备
```

- 启动窗口内收到 UART 有效协议帧时，原生 USB 枚举 HID-only；否则枚举 CDC-only。
- 晚到 UART 协议会触发一次安全重启，使下一次启动进入 HID-only。
- USB/BLE 采用先连接锁定策略；活动输出断开后自动释放并回退到在线链路。
- BLE 鼠标按 10 ms 节拍发送合并位移。
- 日常使用精简高性能日志；完整日志仅用于短时诊断。
- 设置页可启用默认关闭的本机刷写接口；接口仅监听 `127.0.0.1:24815`。
- 刷写成功后日志用一行汇总三段 SHA-256、`设备校验=3/3` 和 RTS 复位结果。

## 最新验证

| 层级 | 结果 |
|---|---|
| 主机 Release Build | Pass |
| 主机格式检查 | Pass |
| ESP-IDF 固件 Build | Pass |
| BLE 连接策略测试 | Pass |
| 三段固件烧录与设备端哈希 | Pass |
| 动态 USB 枚举 | Pass |
| USB/BLE 切换与 BLE 重连 | Pass（用户验收） |
| 精简日志模式连续鼠标移动 | Pass（用户验收） |
| 本机 API 路由与安全边界 | Pass（Loopback 集成检查） |
| API 驱动真实 COM 刷写 | Pass |

## 最新产物

- `firmware/build/esp32_s3_hid_bridge.bin`：550736 bytes，SHA-256 `D762212434F83F22CA69D9221B7BB5C73C51158001FFE9E66F2FEF5E9E3229CA`。
- `HidBridge.Host.exe`：820571 bytes，SHA-256 `0464BC40426125F0FDC9528527D4E6E447951CB3A2B9E99D9D8875B8FF1773F2`。

## 下一步

无。等待新需求。
