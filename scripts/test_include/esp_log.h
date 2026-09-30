/* host-test stub — not the real ESP-IDF header */
#pragma once
#include <stdio.h>
#define ESP_LOGI(t, f, ...) do { (void)(t); if (0) printf(f "\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGW(t, f, ...) do { (void)(t); if (0) printf(f "\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGE(t, f, ...) do { (void)(t); if (0) printf(f "\n", ##__VA_ARGS__); } while (0)
