#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
