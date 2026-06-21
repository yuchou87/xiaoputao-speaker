#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "bsp_i2c.h"
#include "bsp_exio.h"
#include "st77916_panel.h"
#include "bsp_lvgl.h"
#include "cst816.h"
#include "bsp_audio.h"
#include "bsp_sr.h"
#include "bsp_battery.h"
#include "lvgl.h"
#include <math.h>

static int16_t s_tone[16000];   // 1s @ 16kHz, 16-bit mono

static const char *TAG = "xpt";

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

static void tap_event_cb(lv_event_t *e)
{
    static int count = 0;
    lv_obj_t *lbl = (lv_obj_t *) lv_event_get_user_data(e);
    count++;
    lv_label_set_text_fmt(lbl, "TAP: %d", count);
    ESP_LOGI(TAG, "touch tap #%d", count);
}

static void on_wake(void)
{
    ESP_LOGI(TAG, "=== WAKE: 你好小葡萄 ===");
    bsp_audio_play(s_tone, 3200);   // ~0.2s beep ack (s_tone holds 1kHz sine)
}

void app_main(void)
{
    ESP_LOGI(TAG, "xiaoputao-speaker P0 bring-up boot");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %s, %d core(s), rev %d", CONFIG_IDF_TARGET, chip.cores, chip.revision);

    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM free: %u bytes (%.1f MB)", (unsigned) psram, psram / 1048576.0);

    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_scan();

    ESP_ERROR_CHECK(bsp_exio_init());
    ESP_LOGI(TAG, "TCA9554 init ok");

    bsp_battery_init();
    ESP_LOGI(TAG, "battery: %.2f V", bsp_battery_voltage());

    // Display: ST77916 QSPI + backlight. LCD_Init() draws a color-bar test
    // pattern (test_draw_bitmap) so we can confirm the panel lights up.
    LCD_Init();
    ESP_LOGI(TAG, "LCD init done (color bars should be visible)");

    // LVGL on top of the panel: draw a centered label.
    ESP_ERROR_CHECK(bsp_lvgl_init());
    // Touch: CST816 -> LVGL pointer indev.
    Touch_Init();
    Touch_LVGL_Init();

    bsp_lvgl_lock();
    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "XiaoPuTao P0");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -50);

    // Tap test: a button with a counter, proves touch -> LVGL works.
    lv_obj_t *btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 160, 70);
    lv_obj_center(btn);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "TAP: 0");
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn, tap_event_cb, LV_EVENT_CLICKED, btn_lbl);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "UI ready (tap the button)");

    // Audio out: ES8311 + I2S. Play a 1 kHz test tone at boot. Non-fatal.
    if (bsp_audio_init() == ESP_OK) {
        for (int i = 0; i < 16000; i++) {
            s_tone[i] = (int16_t) (8000.0f * sinf(2.0f * (float) M_PI * 1000.0f * i / 16000.0f));
        }
        ESP_LOGI(TAG, "playing 1kHz test tone x2");
        for (int k = 0; k < 2; k++) bsp_audio_play(s_tone, 16000);
        ESP_LOGI(TAG, "tone done");

        // Loopback: record 3s from ES7210 mic, then play it back on ES8311.
        size_t n = 16000 * 3;
        int16_t *rec = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (rec) {
            ESP_LOGI(TAG, "recording 3s from mic... (speak now)");
            bsp_audio_read(rec, n);
            ESP_LOGI(TAG, "playing recording back");
            bsp_audio_play(rec, n);
            ESP_LOGI(TAG, "loopback done");
            heap_caps_free(rec);
        }

        // Wake word: esp-sr AFE + WakeNet (builtin 你好小智 for now).
        bsp_sr_start(on_wake);
    } else {
        ESP_LOGE(TAG, "audio init failed (continuing)");
    }
}
