#pragma once

#include "esp_err.h"

/**
 * @brief Start SoftAP + HTTP captive-portal for Wi-Fi/config provisioning.
 *
 * Starts an open AP ("XiaoPuTao-Setup") and an HTTP server with a web form.
 * Blocks until the user submits the form (credentials saved to NVS), then
 * returns ESP_OK so the caller can reboot the device.
 *
 * esp_netif_init(), esp_event_loop_create_default(), and esp_wifi_init() are
 * called internally (harmless if already done by bsp_wifi_init).
 */
esp_err_t bsp_prov_start_softap(void);
