# 项目内 ESP-IDF `esp_hid` 覆盖组件

本目录基于 ESP-IDF v6.0.2 的 `components/esp_hid`，用于在项目内追踪和修复 NimBLE HID 设备连接状态问题，避免直接修改 `.esp-idf` 环境。

当前差异集中在 `src/nimble_hidd.c`：

- 检查 `ble_gap_event_listener_register()` 返回值；
- 记录 ESP-HID GAP listener 的连接/断开状态变化；
- 在 HID 输入被拒绝时记录内部连接句柄及 `ble_gap_conn_find()` 结果。

升级 ESP-IDF 时必须重新与上游同版本组件比较，不能直接沿用本覆盖目录。
