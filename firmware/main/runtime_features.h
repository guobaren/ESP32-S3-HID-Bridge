#pragma once

/*
 * 当前交付配置接受 USB-to-UART 或原生 USB CDC 输入，并只启用 USB HID / BLE HID 输出。
 * Wi-Fi 输入、SoftAP 配网和 Target Agent 输出实现继续保留；完成真实链路验收后，
 * 可将此开关改为 1，并同步恢复配置文档与安全测试。
 */
#define HID_BRIDGE_WIFI_RUNTIME_ENABLED 0
