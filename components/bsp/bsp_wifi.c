#include "bsp_wifi.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "bsp_wifi";

#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1
#define MAX_RETRY     5

static bool              s_initialized   = false;
static bool              s_connected     = false;
static int               s_retry_count   = 0;
static EventGroupHandle_t s_wifi_eg      = NULL;
static char              s_ip[16]        = "0.0.0.0";

/* -------------------------------------------------------------------------- */
/* Event handlers                                                              */
/* -------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting...");
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        snprintf(s_ip, sizeof(s_ip), "0.0.0.0");

        if (s_retry_count < MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "Disconnected, retry %d/%d", s_retry_count, MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Max retries reached, giving up");
            if (s_wifi_eg) {
                xEventGroupSetBits(s_wifi_eg, FAIL_BIT);
            }
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected   = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
        if (s_wifi_eg) {
            xEventGroupSetBits(s_wifi_eg, CONNECTED_BIT);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_wifi_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret;

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

esp_err_t bsp_wifi_connect_sta(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (!s_initialized) {
        esp_err_t ret = bsp_wifi_init();
        if (ret != ESP_OK) return ret;
    }

    if (s_wifi_eg == NULL) {
        s_wifi_eg = xEventGroupCreate();
        if (s_wifi_eg == NULL) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    } else {
        xEventGroupClearBits(s_wifi_eg, CONNECTED_BIT | FAIL_BIT);
    }

    s_retry_count = 0;
    s_connected   = false;

    /* Register event handlers */
    esp_event_handler_instance_t inst_wifi;
    esp_event_handler_instance_t inst_ip;

    esp_err_t ret;
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, &inst_wifi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &ip_event_handler, NULL, &inst_ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(ret));
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_wifi);
        return ret;
    }

    /* Configure and connect */
    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Wait for CONNECTED_BIT or FAIL_BIT */
    TickType_t ticks = (timeout_ms == portMAX_DELAY)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                                           CONNECTED_BIT | FAIL_BIT,
                                           pdFALSE,   /* don't clear on exit */
                                           pdFALSE,   /* wait for any bit */
                                           ticks);

    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected, IP: %s", s_ip);
        ret = ESP_OK;
    } else if (bits & FAIL_BIT) {
        ESP_LOGE(TAG, "Connection failed after %d retries", MAX_RETRY);
        ret = ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "Connection timed out after %"PRIu32" ms", timeout_ms);
        ret = ESP_ERR_TIMEOUT;
    }

cleanup:
    esp_event_handler_instance_unregister(IP_EVENT,   IP_EVENT_STA_GOT_IP, inst_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,    inst_wifi);
    return ret;
}

bool bsp_wifi_is_connected(void)
{
    return s_connected;
}

void bsp_wifi_get_ip(char *out, size_t len)
{
    if (out == NULL || len == 0) return;
    snprintf(out, len, "%s", s_ip);
}
