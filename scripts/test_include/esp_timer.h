/* host-test stub — virtual monotonic clock driven by tests */
#pragma once
#include <stdint.h>

extern int64_t host_now_us;
static inline int64_t esp_timer_get_time(void) { return host_now_us; }
