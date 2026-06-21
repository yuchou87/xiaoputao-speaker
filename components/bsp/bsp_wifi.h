#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi subsystem (idempotent).
 *
 * Calls esp_netif_init, esp_event_loop_create_default,
 * esp_netif_create_default_wifi_sta, esp_wifi_init, esp_wifi_start.
 * Safe to call multiple times.
 */
esp_err_t bsp_wifi_init(void);

/**
 * @brief Connect to a WiFi AP in STA mode, blocking until connected or timeout.
 *
 * @param ssid       AP SSID (null-terminated)
 * @param pass       AP password (null-terminated)
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on successful IP acquisition, ESP_ERR_TIMEOUT on timeout,
 *         other esp_err_t on failure.
 */
esp_err_t bsp_wifi_connect_sta(const char *ssid, const char *pass, uint32_t timeout_ms);

/**
 * @brief Check whether the station has an IP address (is connected).
 */
bool bsp_wifi_is_connected(void);

/**
 * @brief Get the current IPv4 address as a dotted string.
 *
 * @param out Buffer to receive the string (e.g. "192.168.1.100")
 * @param len Buffer length (at least 16 bytes recommended)
 *
 * Writes "0.0.0.0" when not connected.
 */
void bsp_wifi_get_ip(char *out, size_t len);

#ifdef __cplusplus
}
#endif
