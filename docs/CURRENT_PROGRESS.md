# 当前进度

更新时间：2026-08-12

详细执行路线见 [`交接.md`](交接.md)，质量与风险见 [`审计.md`](审计.md)。

## 当前运行范围

```text
Windows ── USB-to-UART 或原生 USB CDC ──> ESP32-S3
ESP32-S3 ── 启动期选择的 USB 键盘触摸板 HID 或 BLE HID ──> 目标设备
```

- 启动后 1.5 秒内检测到 UART 有效协议帧时原生 USB 为 HID-only；否则为 CDC-only。两种 profile 使用不同 PID，选择保持到下次复位。
- USB HID 与 BLE HID 由先连接并成为活动输出的链路锁定；后连接链路不得抢占。USB HID 连续 100 ms 不可发送时视为失活并切换到仍在线 BLE，解决外部供电时拔线后 `mounted` 状态滞留的问题。USB 活动输出显示绿灯，BLE 活动输出显示蓝灯。初始 CDC 模式若在 1.5 秒窗口后才收到 UART 有效帧，会安全释放并单次重启，让持续探测在下一启动窗口切换为 HID-only。
- BLE 鼠标移动按固定 10 ms 节拍发送最新合并位移。
- Wi-Fi 输入、SoftAP 配网和 Target Agent Wi-Fi 输出代码仍在仓库中，但当前主机和固件运行入口均关闭。
- 主机 Release 构建会把 `HidBridge.Host.exe` 复制到项目根目录。

## 最新验证

| 层级 | 结果 | 边界 |
|---|---|---|
| 主机 Release Build | Pass | 0 警告、0 错误 |
| Checks Build/Run | Pass | 自动检查，不替代真实设备 |
| 主机/Checks Format | Pass | 格式错误数 0 |
| ESP-IDF 固件 Build | Pass | HCI 状态纠正及定向重连镜像 549968 bytes 已生成并烧录，SHA-256 `369A1F0E3C511FCF4E29C4B56992F397C3CFBC8885C784A974D2195AA848787A` |
| 当前固件真实设备 | Partial | 第一次蓝牙重启首次连接成功；第二次重启连续两次 HCI 监督超时后第三次完成加密，随后拔 USB 可立即切回 BLE；日志已证伪低延迟参数请求假设 |

## 最新产物

- `HidBridge.Host.exe`：334020 bytes，SHA-256 `2ABD8E0F4DF17DC87CC7198B0557E4A22FC10996A7D58BC424171639D1D1E5F9`。
- `firmware/build/esp32_s3_hid_bridge.bin`：549968 bytes，SHA-256 `369A1F0E3C511FCF4E29C4B56992F397C3CFBC8885C784A974D2195AA848787A`。

## 下一步

1. 核对 ESP-IDF v6.0.2 在远端版本/特性交换阶段 HCI 0x08 超时的可控恢复接口。
2. 若外设侧无法缩短中心端重新上电后的监督超时，完善时间线日志并保留立即广播恢复。
3. 分别验收 USB-to-UART 与原生 USB CDC 输入到 USB/BLE 输出的真实数据路径和 `ReleaseAll`。
