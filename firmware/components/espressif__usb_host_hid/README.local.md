# 本地 USB Host HID 组件

此目录从 Espressif `usb_host_hid` 1.2.1 复制，包含原版断开清理修复及许可证。项目通过 ESP-IDF 的本地组件优先级使用它，避免修改不受版本控制的 `managed_components`。

本地改动仅针对中断 IN 接收：用户回调返回后的重新提交失败会产生传输错误事件；`hid_host_device_rearm_input()` 仅对未成功重新提交的 transfer 做有界重试，依靠 USB Host API 拒绝仍在途的重复提交。升级上游组件时须重新审查这两处改动及断开清理路径。
