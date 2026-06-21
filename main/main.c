#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "lvgl.h"
#include "bsp_i2c.h"
#include "bsp_exio.h"
#include "bsp_battery.h"
#include "st77916_panel.h"
#include "bsp_lvgl.h"
#include "cst816.h"
#include "bsp_audio.h"
#include "bsp_nvs.h"
#include "bsp_wifi.h"
#include "bsp_prov.h"
#include "voice_app.h"

static const char *TAG = "xpt";

/* ---------- UI status label ---------- */

static lv_obj_t *s_status_label = NULL;

/*
 * ui_set_status() — non-static so it overrides the weak stub in voice_app at
 * link time.  Safe to call from any task after bsp_lvgl_init().
 */
void ui_set_status(const char *s)
{
    if (!s_status_label) return;
    bsp_lvgl_lock();
    lv_label_set_text(s_status_label, s);
    bsp_lvgl_unlock();
}

/* ---------- I2C scan (optional, kept for debug) ---------- */

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C scan:");
    int found = 0;
    for (uint8_t a = 1; a < 0x7f; a++) {
        if (i2c_master_probe(bsp_i2c_bus(), a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  found 0x%02X", a);
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan done, %d device(s)", found);
}

/* ---------- app_main ---------- */

void app_main(void)
{
    ESP_LOGI(TAG, "xiaoputao-speaker boot");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %s, %d core(s), rev %d",
             CONFIG_IDF_TARGET, chip.cores, chip.revision);

    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM free: %u bytes (%.1f MB)",
             (unsigned)psram, psram / 1048576.0);

    /* ---- 1. Hardware init ---- */
    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_scan();

    ESP_ERROR_CHECK(bsp_exio_init());
    ESP_LOGI(TAG, "TCA9554 init ok");

    bsp_battery_init();
    ESP_LOGI(TAG, "battery: %.2f V", bsp_battery_voltage());

    LCD_Init();
    ESP_LOGI(TAG, "LCD init done");

    ESP_ERROR_CHECK(bsp_lvgl_init());
    Touch_Init();
    Touch_LVGL_Init();

    /* ---- 2. Create status label ---- */
    bsp_lvgl_lock();
    s_status_label = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_label, lv_pct(85));
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_status_label, "启动中");
    bsp_lvgl_unlock();

    /* ---- Audio init ---- */
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed (continuing)");
    }

    /* ---- 3. NVS ---- */
    ESP_ERROR_CHECK(bsp_nvs_init());

    /* ---- 4/5. Provisioning vs WiFi connect ---- */
    if (!bsp_nvs_has(NVS_KEY_WIFI_SSID)) {
        /* No credentials — start captive-portal provisioning */
        ui_set_status("配网: 连 XiaoPuTao-Setup\n浏览器开 192.168.4.1");
        ESP_LOGI(TAG, "no WiFi creds, starting SoftAP provisioning");
        bsp_prov_start_softap();   /* blocks until form submitted */
        esp_restart();
    } else {
        /* Have credentials — try to connect */
        ui_set_status("连接 WiFi...");
        ESP_ERROR_CHECK(bsp_wifi_init());

        char ssid[33]  = {0};
        char pass[65]  = {0};
        bsp_nvs_get_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid));
        bsp_nvs_get_str(NVS_KEY_WIFI_PASS, pass, sizeof(pass));

        ESP_LOGI(TAG, "connecting to SSID: %s", ssid);
        esp_err_t r = bsp_wifi_connect_sta(ssid, pass, 20000);

        if (r == ESP_OK) {
            char ip[32] = {0};
            bsp_wifi_get_ip(ip, sizeof(ip));

            char status_buf[64];
            snprintf(status_buf, sizeof(status_buf), "已连接\n%s", ip);
            ui_set_status(status_buf);
            ESP_LOGI(TAG, "WiFi connected, IP: %s", ip);

            esp_err_t va = voice_app_start();
            if (va == ESP_OK) {
                ui_set_status("待命 (说: 你好小智)");
            } else {
                ESP_LOGE(TAG, "voice_app_start failed: %s", esp_err_to_name(va));
                ui_set_status("语音初始化失败");
            }
        } else {
            ESP_LOGE(TAG, "WiFi connect failed: %s", esp_err_to_name(r));
            ui_set_status("WiFi 失败,重启配网");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
    }

    /* FreeRTOS keeps tasks (LVGL tick, voice tasks) running */
}
