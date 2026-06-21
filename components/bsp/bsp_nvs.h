#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define NVS_KEY_WIFI_SSID   "wifi_ssid"
#define NVS_KEY_WIFI_PASS   "wifi_pass"
#define NVS_KEY_GLM_KEY     "glm_key"
#define NVS_KEY_BACKEND_URL "backend_url"

esp_err_t bsp_nvs_init(void);
esp_err_t bsp_nvs_get_str(const char *key, char *out, size_t out_len);
esp_err_t bsp_nvs_set_str(const char *key, const char *val);
bool      bsp_nvs_has(const char *key);
