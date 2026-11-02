#pragma once

#include <stdint.h>

/* 仅返回本板当前 USB 证据确认的身份，不返回 UART 推导结果。 */
uint8_t dual_proxy_usb_role_claim(void);

#include <stdbool.h>

/* 手动 Profile 可覆盖在线会话；关闭时清理克隆并重新申请真实 Profile。 */
void dual_proxy_set_manual_profile(bool enabled);
bool dual_proxy_manual_profile_enabled(void);
