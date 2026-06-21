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
#include <string.h>

#define ECHO_SAMPLES (16000 * 2)   // 2s mono @ 16kHz

static const char *TAG = "bsp_sr";
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static bsp_sr_wake_cb_t s_cb;

static volatile bool s_echo_req;
static bool s_echo_active;
static int16_t *s_echo_buf;
static size_t s_echo_len;

static bsp_sr_audio_cb_t s_audio_cb;
static volatile bool s_streaming;

/* Digital gain applied to AFE output before uplink (see detect_task). */
#define BSP_SR_UPLINK_GAIN 12

void bsp_sr_echo(void) { s_echo_req = true; }

void bsp_sr_set_audio_cb(bsp_sr_audio_cb_t cb) { s_audio_cb = cb; }

void bsp_sr_set_streaming(bool on) { s_streaming = on; }

static void feed_task(void *arg)
{
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int nch = s_afe->get_feed_channel_num(s_afe_data);   // 4 for "RMNM"
    int16_t *buf = heap_caps_malloc(chunk * nch * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "feed: chunk=%d nch=%d", chunk, nch);
    while (buf) {
        bsp_audio_read_raw(buf, chunk * nch);
        s_afe->feed(s_afe_data, buf);

        // Echo feature: capture the next 2s of one mic channel, then play back.
        // Raw layout is "RMNM" -> mic is channel index 1.
        if (s_echo_req && !s_echo_active) {
            if (!s_echo_buf) s_echo_buf = heap_caps_malloc(ECHO_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (s_echo_buf) { s_echo_active = true; s_echo_len = 0; ESP_LOGI(TAG, "echo: capturing 2s..."); }
            s_echo_req = false;
        }
        if (s_echo_active) {
            for (int f = 0; f < chunk && s_echo_len < ECHO_SAMPLES; f++) {
                s_echo_buf[s_echo_len++] = buf[f * nch + 1];   // mic channel
            }
            if (s_echo_len >= ECHO_SAMPLES) {
                s_echo_active = false;
                int32_t peak = 0;
                for (size_t i = 0; i < ECHO_SAMPLES; i++) { int32_t v = s_echo_buf[i]; if (v < 0) v = -v; if (v > peak) peak = v; }
                ESP_LOGI(TAG, "echo: captured peak=%d, playing back 2s", (int) peak);
                bsp_audio_play(s_echo_buf, ECHO_SAMPLES);   // blocks feed ~2s (ok)
                ESP_LOGI(TAG, "echo: done");
            }
        }
    }
    vTaskDelete(NULL);
}

static void detect_task(void *arg)
{
    while (1) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;
        // Forward AFE-processed audio to streaming callback if enabled.
        // The AFE output sits ~20-30 dB below a usable speech level (backend saw
        // RMS ~80), so apply a fixed digital gain with saturation before sending.
        if (s_streaming && s_audio_cb && res->data && res->data_size > 0) {
            static int16_t gbuf[512];   // detect_task is single-threaded -> static ok
            const int16_t *src = (const int16_t *)res->data;
            size_t total = res->data_size / sizeof(int16_t);
            size_t off = 0;
            while (off < total) {
                size_t n = total - off;
                if (n > 512) n = 512;
                for (size_t i = 0; i < n; i++) {
                    int32_t v = (int32_t)src[off + i] * BSP_SR_UPLINK_GAIN;
                    if (v > 32767) v = 32767;
                    else if (v < -32768) v = -32768;
                    gbuf[i] = (int16_t)v;
                }
                s_audio_cb(gbuf, n);
                off += n;
            }
        }
        // Single-channel AFE signals WAKENET_DETECTED; multi-channel ("RMNM")
        // signals WAKENET_CHANNEL_VERIFIED after picking the best mic.
        if (res->wakeup_state == WAKENET_DETECTED || res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            ESP_LOGI(TAG, "WAKE (state=%d, ch=%d)", res->wakeup_state, res->trigger_channel_id);
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

    // "RMNM": ch0=reference(AEC), ch1=mic, ch2=null, ch3=mic — matches the board's
    // ES7210 4-channel raw layout (bsp_get_input_format). Enables mic array + AEC.
    afe_config_t *cfg = afe_config_init("RMNM", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) { ESP_LOGE(TAG, "afe create failed"); return ESP_FAIL; }

    xTaskCreatePinnedToCore(detect_task, "sr_detect", 8 * 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(feed_task, "sr_feed", 8 * 1024, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "esp-sr started");
    return ESP_OK;
}
