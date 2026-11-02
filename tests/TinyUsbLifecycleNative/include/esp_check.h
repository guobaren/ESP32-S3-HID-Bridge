#pragma once

#include "esp_err.h"

#define ESP_RETURN_ON_ERROR(expression, tag, format, ...) \
    do { \
        const esp_err_t check_result = (expression); \
        if (check_result != ESP_OK) return check_result; \
    } while (0)

#define ESP_RETURN_ON_FALSE(condition, error, tag, format, ...) \
    do { \
        if (!(condition)) return (error); \
    } while (0)
