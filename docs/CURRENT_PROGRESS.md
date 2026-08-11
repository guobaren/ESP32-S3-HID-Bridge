# 当前进度

更新时间：2026-08-06

详细执行路线见 [`交接.md`](交接.md)，质量与风险见 [`审计.md`](审计.md)。

## 当前运行范围

```text
Windows ── USB-to-UART 或原生 USB CDC ──> ESP32-S3
ESP32-S3 ── USB HID 或 BLE HID ──> 目标设备
```

- USB-to-UART 与原生 USB CDC 使用同一套自动发现、握手、会话租约和输入协议。
- USB HID 与 BLE HID 由先连接并成为活动输出的链路锁定；后连接链路不得抢占。USB HID 连续 100 ms 不可发送时视为失活并切换到仍在线 BLE，解决外部供电时拔线后 `mounted` 状态滞留的问题。USB 活动输出显示绿灯，BLE 活动输出显示蓝灯。
- BLE 鼠标移动按固定 10 ms 节拍发送最新合并位移。
- Wi-Fi 输入、SoftAP 配网和 Target Agent Wi-Fi 输出代码仍在仓库中，但当前主机和固件运行入口均关闭。
- 主机 Release 构建会把 `HidBridge.Host.exe` 复制到项目根目录。

## 最新验证

| 层级 | 结果 | 边界 |
|---|---|---|
| 主机 Release Build | Pass | 0 警告、0 错误 |
| Checks Build/Run | Pass | 自动检查，不替代真实设备 |
| 主机/Checks Format | Pass | 格式错误数 0 |
| ESP-IDF 固件 Build | Pass | 镜像已生成，尚未烧录 |
| 当前固件真实设备 | Not Run | USB/CDC/BLE/Wi-Fi 关闭状态均待本版验收 |

## 最新产物

- `HidBridge.Host.exe`：334020 bytes，SHA-256 `2ABD8E0F4DF17DC87CC7198B0557E4A22FC10996A7D58BC424171639D1D1E5F9`。
- `firmware/build/esp32_s3_hid_bridge.bin`：SHA-256 `BB8F3119F201475E08F196F9BDE447414E141A22AA22C94A14F98436E4169DE1`。

## 下一步

1. 烧录当前固件并保存完整启动日志，确认没有 Wi-Fi/SoftAP/Target Agent 任务。
2. 分别验收 USB-to-UART 与原生 USB CDC 输入到 USB/BLE 输出的真实数据路径和 `ReleaseAll`。
3. 采集对端鼠标轨迹与 BLE 连接/发送统计，量化 7.5–10 ms 连接参数下的实际抖动。
