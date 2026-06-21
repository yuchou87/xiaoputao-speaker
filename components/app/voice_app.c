#include "voice_app.h"

#include "bsp_sr.h"
#include "glm_realtime.h"
#include "glm_audio_out.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "voice_app";

/*
 * Weak UI hook. T4 provides the real ui_set_status(); until then this links as
 * a no-op so transitions can call it unconditionally.
 */
__attribute__((weak)) void ui_set_status(const char *s) { (void)s; }

/* ---- Conversation state ---------------------------------------------------- */

typedef enum {
    VOICE_IDLE = 0,   /* waiting for the wake word */
    VOICE_LISTENING,  /* streaming mic to GLM, server VAD owns the turn */
    VOICE_SPEAKING,   /* playing GLM audio deltas */
} voice_state_t;

static volatile voice_state_t s_state = VOICE_IDLE;

/* Monotonic timestamp (us) of the last activity that should reset the LISTENING
 * watchdog: wake, speech_started, or first audio delta. */
static volatile int64_t s_last_activity_us = 0;

#define LISTEN_TIMEOUT_US (15LL * 1000 * 1000) /* ~15s with no progress -> IDLE */

/* ---- Uplink queue (detect task -> uplink task) ----------------------------- */

/*
 * The bsp_sr audio cb runs on the detect/feed task and MUST NOT block on the
 * websocket send. Each AFE chunk is copied into a fixed-size slot and pushed to
 * a FreeRTOS queue with zero wait; a dedicated uplink task drains and sends it.
 * Full queue -> drop the chunk (never block the detect task).
 *
 * AFE feeds ~16kHz mono. Slots are sized for typical AFE chunk (~10-30ms). A
 * queue depth of 64 slots @ 320 samples ~= 20480 samples ~= 1.28s of headroom.
 */
#define UPLINK_SLOT_SAMPLES 512
#define UPLINK_QUEUE_DEPTH  64

typedef struct {
    int16_t pcm[UPLINK_SLOT_SAMPLES];
    size_t  samples;
} uplink_chunk_t;

static QueueHandle_t s_uplink_q = NULL;
static TaskHandle_t  s_uplink_task = NULL;

/* ---- State transitions ----------------------------------------------------- */

static void set_state(voice_state_t next, const char *ui)
{
    if (s_state != next) {
        s_state = next;
        ESP_LOGI(TAG, "state -> %s", ui);
    }
    ui_set_status(ui);
}

/* ---- Callbacks ------------------------------------------------------------- */

/* Runs on the esp-sr detect task. Toggles streaming and enters LISTENING. */
static void on_wake(void)
{
    if (!glm_rt_connected()) {
        ESP_LOGW(TAG, "wake ignored: GLM not connected");
        return;
    }
    ESP_LOGI(TAG, "wake detected");
    s_last_activity_us = esp_timer_get_time();
    bsp_sr_set_streaming(true);
    set_state(VOICE_LISTENING, "聆听");
}

/* Runs on the bsp_sr feed task. Copy + enqueue, never block. */
static void uplink_enqueue(const int16_t *pcm16, size_t samples)
{
    if (!s_uplink_q || !pcm16 || samples == 0) {
        return;
    }

    /* Forward in slot-sized fragments so large chunks still fit. */
    size_t offset = 0;
    while (offset < samples) {
        size_t n = samples - offset;
        if (n > UPLINK_SLOT_SAMPLES) {
            n = UPLINK_SLOT_SAMPLES;
        }

        uplink_chunk_t chunk;
        memcpy(chunk.pcm, pcm16 + offset, n * sizeof(int16_t));
        chunk.samples = n;

        /* Zero wait: drop on full queue rather than stall the detect task. */
        if (xQueueSend(s_uplink_q, &chunk, 0) != pdTRUE) {
            ESP_LOGW(TAG, "uplink queue full, dropping %zu samples", n);
            return; /* drop the rest of this chunk too */
        }

        offset += n;
    }
}

/* Dedicated uplink task: drain the queue and send to GLM (may block on WS). */
static void uplink_task(void *arg)
{
    (void)arg;
    uplink_chunk_t chunk;
    for (;;) {
        if (xQueueReceive(s_uplink_q, &chunk, portMAX_DELAY) == pdTRUE) {
            if (glm_rt_connected()) {
                esp_err_t err = glm_rt_send_audio(chunk.pcm, chunk.samples);
                if (err != ESP_OK) {
                    ESP_LOGD(TAG, "send_audio failed: %s", esp_err_to_name(err));
                }
            }
        }
    }
}

/* Wraps glm_audio_out_play: on the first delta of a response, flip to SPEAKING. */
static void audio_out_wrapper(const uint8_t *pcm16, size_t len)
{
    if (s_state != VOICE_SPEAKING) {
        set_state(VOICE_SPEAKING, "说话");
    }
    s_last_activity_us = esp_timer_get_time();
    glm_audio_out_play(pcm16, len);
}

/* GLM server event callback (non-audio events). */
static void on_glm_event(const char *type)
{
    if (!type) {
        return;
    }

    if (strcmp(type, "_ws_connected") == 0) {
        ui_set_status("已连接云端");
    } else if (strcmp(type, "_ws_disconnected") == 0) {
        ui_set_status("重连中...");
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
        s_last_activity_us = esp_timer_get_time();
        set_state(VOICE_LISTENING, "聆听");
    } else if (strcmp(type, "response.audio.done") == 0) {
        /* One question -> one answer per wake. Stop streaming, return to IDLE. */
        bsp_sr_set_streaming(false);
        set_state(VOICE_IDLE, "待命");
    } else if (strcmp(type, "error") == 0) {
        ESP_LOGE(TAG, "GLM error event");
        ui_set_status("云端错误");
    } else {
        ESP_LOGD(TAG, "GLM event: %s", type);
    }
}

/* ---- Idle/turn watchdog ---------------------------------------------------- */

static void watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_state == VOICE_LISTENING) {
            int64_t now = esp_timer_get_time();
            if (now - s_last_activity_us > LISTEN_TIMEOUT_US) {
                ESP_LOGW(TAG, "listen timeout, returning to IDLE");
                bsp_sr_set_streaming(false);
                set_state(VOICE_IDLE, "待命");
            }
        }
    }
}

/* ---- Public API ------------------------------------------------------------ */

esp_err_t voice_app_start(void)
{
    ESP_LOGI(TAG, "starting voice app");

    /* Wire GLM callbacks before starting the client. */
    glm_rt_set_audio_cb(audio_out_wrapper);
    glm_rt_set_event_cb(on_glm_event);

    esp_err_t err = glm_rt_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "glm_rt_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Uplink queue + task. */
    s_uplink_q = xQueueCreate(UPLINK_QUEUE_DEPTH, sizeof(uplink_chunk_t));
    if (!s_uplink_q) {
        ESP_LOGE(TAG, "failed to create uplink queue");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uplink_task, "voice_uplink", 4096, NULL, 5, &s_uplink_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create uplink task");
        return ESP_ERR_NO_MEM;
    }

    /* Idle/turn watchdog. */
    if (xTaskCreate(watchdog_task, "voice_wdt", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create watchdog task");
        return ESP_ERR_NO_MEM;
    }

    /* Register mic uplink cb before enabling wake/streaming. */
    bsp_sr_set_audio_cb(uplink_enqueue);

    err = bsp_sr_start(on_wake);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sr_start failed: %s", esp_err_to_name(err));
        return err;
    }

    set_state(VOICE_IDLE, "待命");
    ESP_LOGI(TAG, "voice app ready (IDLE)");
    return ESP_OK;
}
