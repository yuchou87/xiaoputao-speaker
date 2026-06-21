#include "bsp_prov.h"
#include "bsp_nvs.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "bsp_prov";

#define PROV_DONE_BIT BIT0

static EventGroupHandle_t s_prov_eg = NULL;

/* ------------------------------------------------------------------ */
/*  HTML form                                                           */
/* ------------------------------------------------------------------ */

static const char *PROV_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>小葡萄配网</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 16px}"
    "h1{font-size:1.3em}label{display:block;margin-top:12px;font-size:.9em}"
    "input{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;font-size:1em}"
    "button{margin-top:20px;width:100%;padding:10px;font-size:1em;background:#4c8;border:none;border-radius:4px;cursor:pointer}"
    "</style></head><body>"
    "<h1>小葡萄配网</h1>"
    "<form method='POST' action='/save'>"
    "<label>Wi-Fi 名称 (SSID)<input name='wifi_ssid' required></label>"
    "<label>Wi-Fi 密码<input name='wifi_pass' type='password'></label>"
    "<label>GLM API Key<input name='glm_key'></label>"
    "<label>后端地址 (可选)<input name='backend_url' placeholder='https://...'></label>"
    "<button type='submit'>保存并重启</button>"
    "</form></body></html>";

static const char *PROV_SAVED_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>已保存</title></head><body>"
    "<h2>已保存，正在重启…</h2>"
    "</body></html>";

/* ------------------------------------------------------------------ */
/*  URL decode                                                          */
/* ------------------------------------------------------------------ */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode in-place; returns new length. */
static int urldecode(char *dst, const char *src, int src_len)
{
    int j = 0;
    for (int i = 0; i < src_len; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[j++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[j++] = src[i];
            }
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

/* ------------------------------------------------------------------ */
/*  HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t get_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, PROV_HTML);
    return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    char *raw = malloc(total + 1);
    if (!raw) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, raw + received, total - received);
        if (r <= 0) {
            free(raw);
            return ESP_FAIL;
        }
        received += r;
    }
    raw[total] = '\0';
    ESP_LOGI(TAG, "POST /save body: %s", raw);

    /* Parse each field */
    struct {
        const char *field;
        const char *nvs_key;
    } fields[] = {
        { "wifi_ssid",   NVS_KEY_WIFI_SSID   },
        { "wifi_pass",   NVS_KEY_WIFI_PASS    },
        { "glm_key",     NVS_KEY_GLM_KEY      },
        { "backend_url", NVS_KEY_BACKEND_URL  },
    };

    char encoded[256];
    char decoded[256];

    for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
        esp_err_t qret = httpd_query_key_value(raw, fields[i].field,
                                               encoded, sizeof(encoded));
        if (qret == ESP_OK && encoded[0] != '\0') {
            urldecode(decoded, encoded, (int)strlen(encoded));
            ESP_LOGI(TAG, "  %s = [%s]", fields[i].field, decoded);
            esp_err_t sret = bsp_nvs_set_str(fields[i].nvs_key, decoded);
            if (sret != ESP_OK) {
                ESP_LOGW(TAG, "  nvs_set_str(%s) failed: %s",
                         fields[i].nvs_key, esp_err_to_name(sret));
            }
        }
    }
    free(raw);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, PROV_SAVED_HTML);

    /* Signal completion */
    if (s_prov_eg) {
        xEventGroupSetBits(s_prov_eg, PROV_DONE_BIT);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t bsp_prov_start_softap(void)
{
    esp_err_t ret;

    /* --- netif init (safe if already done) --- */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- default event loop (safe if already done) --- */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- create default AP netif --- */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
        return ESP_FAIL;
    }

    /* --- wifi init (safe if already done) --- */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- configure AP --- */
    wifi_config_t ap_config = {
        .ap = {
            .ssid          = "XiaoPuTao-Setup",
            .ssid_len      = 0,   /* auto from strlen */
            .channel       = 1,
            .authmode      = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP started: SSID=XiaoPuTao-Setup");

    /* --- HTTP server --- */
    s_prov_eg = xEventGroupCreate();
    if (!s_prov_eg) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return ESP_ERR_NO_MEM;
    }

    httpd_handle_t server = NULL;
    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    server_cfg.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&server, &server_cfg));

    httpd_uri_t uri_get = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = get_root_handler,
    };
    httpd_uri_t uri_post = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = post_save_handler,
    };
    httpd_register_uri_handler(server, &uri_get);
    httpd_register_uri_handler(server, &uri_post);

    ESP_LOGI(TAG, "HTTP server started — waiting for provisioning form submission");

    /* Block until the user submits the form */
    xEventGroupWaitBits(s_prov_eg, PROV_DONE_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "Provisioning complete — caller should reboot");

    /* Minimal cleanup */
    httpd_stop(server);
    vEventGroupDelete(s_prov_eg);
    s_prov_eg = NULL;

    return ESP_OK;
}
