/* host-test stub — not the real ESP-IDF header */
#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(x, tag, msg, ...) \
    do { esp_err_t _e = (x); if (_e != ESP_OK) return _e; } while (0)
