#include "bsp_sr.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"

static const char *TAG = "bsp_sr";
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static bsp_sr_wake_cb_t s_cb;

static void feed_task(void *arg)
{
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int nch = s_afe->get_feed_channel_num(s_afe_data);
    int16_t *buf = heap_caps_malloc(chunk * nch * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "feed: chunk=%d nch=%d", chunk, nch);
    while (buf) {
        bsp_audio_read(buf, chunk * nch);
        s_afe->feed(s_afe_data, buf);
    }
    vTaskDelete(NULL);
}

static void detect_task(void *arg)
{
    while (1) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "WAKE DETECTED");
            if (s_cb) s_cb();
        }
    }
}

esp_err_t bsp_sr_start(bsp_sr_wake_cb_t cb)
{
    s_cb = cb;
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || models->num == 0) {
        ESP_LOGE(TAG, "no SR models in 'model' partition");
        return ESP_FAIL;
    }
    for (int i = 0; i < models->num; i++) ESP_LOGI(TAG, "model[%d]=%s", i, models->model_name[i]);

    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) { ESP_LOGE(TAG, "afe create failed"); return ESP_FAIL; }

    xTaskCreatePinnedToCore(detect_task, "sr_detect", 8 * 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(feed_task, "sr_feed", 8 * 1024, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "esp-sr started");
    return ESP_OK;
}
