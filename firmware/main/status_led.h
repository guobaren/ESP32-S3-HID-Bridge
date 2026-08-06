#pragma once

#include "esp_err.h"
#include "output_mode_selector.h"

/** 初始化板载可寻址 RGB 状态指示灯。 */
esp_err_t status_led_init(void);

/** 根据当前真正接收键鼠报告的活动输出更新灯色。 */
void status_led_set_active_mode(output_mode_t mode);
