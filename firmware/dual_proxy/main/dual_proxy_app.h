#pragma once

#include <stdbool.h>

/* 手动 Profile 可覆盖在线会话；关闭时清理克隆并重新申请真实 Profile。 */
void dual_proxy_set_manual_profile(bool enabled);
bool dual_proxy_manual_profile_enabled(void);
