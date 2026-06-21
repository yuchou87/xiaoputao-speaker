#include "voice_app.h"

#include "bsp_sr.h"
#include "glm_realtime.h"
#include "glm_audio_out.h"
#include "ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "voice_app";

/* ---- Conversation state ---------------------------------------------------- */

typedef enum {
    VOICE_IDLE = 0,   /* waiting for the wake word */
    VOICE_LISTENING,  /* streaming mic; device AFE VAD owns end-of-utterance */
    VOICE_THINKING,   /* committed; waiting for the backend to produce the reply */
    VOICE_SPEAKING,   /* playing GLM audio deltas */
} voice_state_t;

static volatile voice_state_t s_state = VOICE_IDLE;

/* Monotonic timestamp (us) of the last activity that should reset the LISTENING
 * watchdog: wake, speech_started, or first audio delta. */
static volatile int64_t s_last_activity_us = 0;

#define LISTEN_TIMEOUT_US (15LL * 1000 * 1000) /* ~15s with no progress -> IDLE */
#define THINK_TIMEOUT_US  (40LL * 1000 * 1000)  /* backend builds the full TTS reply
                                                  before streaming; long replies need
                                                  generous headroom before giving up */

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

static QueueHandle_t   s_uplink_q = NULL;
static TaskHandle_t    s_uplink_task = NULL;
static StaticQueue_t  *s_uplink_q_struct = NULL;   /* internal RAM */
static uint8_t        *s_uplink_q_storage = NULL;   /* PSRAM */

/* ---- Downlink ringbuffer (WS event task -> downlink task) ------------------ */

/*
 * The GLM audio cb runs on the WS event task and MUST NOT block on playback
 * (bsp_audio_play stalls for the duration of the audio). It copies the delta
 * bytes into a byte ringbuffer (non-blocking) and returns; a dedicated downlink
 * task drains it and plays. On a full ringbuffer we drop the newest bytes
 * rather than stall WS RX.
 *
 * 24 kHz mono int16 -> 48000 bytes/s. ~96 KB ~= 2s of headroom, placed in PSRAM.
 */
#define DOWNLINK_RB_BYTES   (192 * 1024)   /* ~6s @16k; headroom for jitter */
#define DOWNLINK_DRAIN_MAX  4096   /* max bytes handed to play per iteration */

static RingbufHandle_t s_downlink_rb = NULL;
static StaticRingbuffer_t *s_downlink_rb_struct = NULL;
static TaskHandle_t        s_downlink_task = NULL;

/* ---- State transitions ----------------------------------------------------- */

static void set_state(voice_state_t next)
{
    if (s_state != next) {
        s_state = next;
        ESP_LOGI(TAG, "state -> %d", next);
    }
    switch (next) {
        case VOICE_LISTENING: ui_set_state(UI_LISTENING); break;
        case VOICE_THINKING:  ui_set_state(UI_THINKING);  break;
        case VOICE_SPEAKING:  ui_set_state(UI_SPEAKING);  break;
        case VOICE_IDLE:
        default:              ui_set_state(UI_IDLE);       break;
    }
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
    set_state(VOICE_LISTENING);
}

/* Runs on the esp-sr detect task when the AFE VAD reports end-of-utterance.
 * Stop streaming and commit so the backend produces the reply. */
static void on_eou(void)
{
    if (s_state != VOICE_LISTENING) {
        return;   /* only meaningful while actively listening */
    }
    ESP_LOGI(TAG, "end-of-utterance -> commit");
    bsp_sr_set_streaming(false);
    glm_rt_commit();
    s_last_activity_us = esp_timer_get_time();
    set_state(VOICE_THINKING);
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

/* ~96ms of audio per WS frame. Coalescing many 32ms slots into one larger frame
 * cuts the WS/TCP/WiFi frame rate ~3x, which keeps sustained uplink from
 * saturating the WiFi TX path (the streaming-time poll_write timeouts). */
#define UPLINK_BATCH_SAMPLES (UPLINK_SLOT_SAMPLES * 3)

/* Dedicated uplink task: coalesce queued slots into larger frames and send. */
static void uplink_task(void *arg)
{
    (void)arg;
    static int16_t batch[UPLINK_BATCH_SAMPLES];
    size_t fill = 0;
    uplink_chunk_t chunk;
    for (;;) {
        /* Short timeout so a trailing partial batch still flushes promptly. */
        if (xQueueReceive(s_uplink_q, &chunk, pdMS_TO_TICKS(40)) == pdTRUE) {
            size_t n = chunk.samples > UPLINK_SLOT_SAMPLES ? UPLINK_SLOT_SAMPLES
                                                           : chunk.samples;
            if (fill + n > UPLINK_BATCH_SAMPLES) {
                if (glm_rt_connected()) glm_rt_send_audio(batch, fill);
                fill = 0;
            }
            memcpy(batch + fill, chunk.pcm, n * sizeof(int16_t));
            fill += n;
            if (fill >= UPLINK_BATCH_SAMPLES) {
                if (glm_rt_connected()) glm_rt_send_audio(batch, fill);
                fill = 0;
            }
        } else if (fill > 0) {
            if (glm_rt_connected()) glm_rt_send_audio(batch, fill);
            fill = 0;
        }
    }
}

/*
 * GLM downlink audio cb. Runs on the WS event task: copy the delta bytes into
 * the ringbuffer (non-blocking) and return immediately. NEVER call playback
 * here — that would stall WS RX for seconds. On a full ringbuffer, drop the
 * oldest bytes to make room for the newest delta.
 */
static void audio_out_wrapper(const uint8_t *pcm16, size_t len)
{
    if (!s_downlink_rb || !pcm16 || len == 0) {
        return;
    }

    if (s_state != VOICE_SPEAKING) {
        set_state(VOICE_SPEAKING);
    }
    s_last_activity_us = esp_timer_get_time();

    /* Non-blocking send. If full, drain oldest bytes and retry once. */
    if (xRingbufferSend(s_downlink_rb, pcm16, len, 0) != pdTRUE) {
        size_t drained = 0;
        void *old = xRingbufferReceiveUpTo(s_downlink_rb, &drained, 0, len);
        if (old) {
            vRingbufferReturnItem(s_downlink_rb, old);
        }
        if (xRingbufferSend(s_downlink_rb, pcm16, len, 0) != pdTRUE) {
            ESP_LOGW(TAG, "downlink ringbuffer full, dropping %zu bytes", len);
        }
    }
}

/* Dedicated downlink task: drain the ringbuffer and play (may block on codec). */
/* Jitter buffer: accumulate this many bytes (24kHz PCM16) before starting to
 * play a reply, so WiFi delivery jitter doesn't underrun the codec (stutter). */
#define DOWNLINK_PREBUF_BYTES  (24000 * 2 * 4 / 5)   /* ~0.8s @24k */

static void downlink_task(void *arg)
{
    (void)arg;
    bool playing = false;
    int  prebuf_ms = 0;     /* time spent waiting on the current prebuffer */
    for (;;) {
        if (!playing) {
            /* Wait until enough audio is buffered to ride out jitter -- but don't
             * stall forever: a reply shorter than the prebuffer target would never
             * reach it. Once any audio is present, start after at most ~1.5s. */
            size_t fill = DOWNLINK_RB_BYTES - xRingbufferGetCurFreeSize(s_downlink_rb);
            if (fill == 0) {
                prebuf_ms = 0;             /* nothing yet; keep waiting fresh */
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }
            if (fill < DOWNLINK_PREBUF_BYTES && prebuf_ms < 1500) {
                prebuf_ms += 15;
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }
            playing = true;
            prebuf_ms = 0;
        }
        size_t n = 0;
        uint8_t *bytes = (uint8_t *)xRingbufferReceiveUpTo(
            s_downlink_rb, &n, pdMS_TO_TICKS(150), DOWNLINK_DRAIN_MAX);
        if (bytes) {
            if (n > 0) {
                s_last_activity_us = esp_timer_get_time();
                glm_audio_out_play(bytes, n);
            }
            vRingbufferReturnItem(s_downlink_rb, bytes);
        } else {
            /* Drained dry (reply finished or a gap) -> re-prebuffer next time. */
            playing = false;
        }
    }
}

/* GLM server event callback (non-audio events). */
static void on_glm_event(const char *type)
{
    if (!type) {
        return;
    }

    if (strcmp(type, "_ws_connected") == 0) {
        ui_set_state(UI_IDLE);
        ui_set_status("backend online");
    } else if (strcmp(type, "_ws_disconnected") == 0) {
        ui_set_state(UI_CONNECTING);
        ui_set_status("reconnecting...");
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
        s_last_activity_us = esp_timer_get_time();
        set_state(VOICE_LISTENING);
    } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
        ui_set_state(UI_THINKING);   /* server processing, reply pending */
    } else if (strcmp(type, "response.audio.done") == 0 ||
               strcmp(type, "response.done") == 0) {
        /* One question -> one answer per wake. Either terminal event ends the
         * turn: stop streaming, return to IDLE. */
        bsp_sr_set_streaming(false);
        set_state(VOICE_IDLE);
    } else if (strcmp(type, "error") == 0) {
        ESP_LOGE(TAG, "GLM error event");
        ui_set_state(UI_ERROR);
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
        /* Time out both LISTENING and SPEAKING if no progress, so the state
         * machine can't wedge in "聆听" or "说话". */
        voice_state_t st = s_state;
        if (st == VOICE_LISTENING || st == VOICE_THINKING || st == VOICE_SPEAKING) {
            int64_t budget = (st == VOICE_THINKING) ? THINK_TIMEOUT_US : LISTEN_TIMEOUT_US;
            int64_t now = esp_timer_get_time();
            if (now - s_last_activity_us > budget) {
                ESP_LOGW(TAG, "turn timeout (state=%d), returning to IDLE", st);
                bsp_sr_set_streaming(false);
                set_state(VOICE_IDLE);
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

    /* Uplink queue + task. Storage (~66 KB) in PSRAM, control struct in internal
     * RAM — a plain xQueueCreate would need that much contiguous internal DMA RAM,
     * which isn't available after WiFi/AFE/LVGL init. Queue is task-only (no ISR). */
    s_uplink_q_storage = heap_caps_malloc((size_t)UPLINK_QUEUE_DEPTH * sizeof(uplink_chunk_t),
                                          MALLOC_CAP_SPIRAM);
    s_uplink_q_struct  = heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL);
    if (!s_uplink_q_storage || !s_uplink_q_struct) {
        ESP_LOGE(TAG, "failed to alloc uplink queue");
        return ESP_ERR_NO_MEM;
    }
    s_uplink_q = xQueueCreateStatic(UPLINK_QUEUE_DEPTH, sizeof(uplink_chunk_t),
                                    s_uplink_q_storage, s_uplink_q_struct);
    if (!s_uplink_q) {
        ESP_LOGE(TAG, "failed to create uplink queue");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uplink_task, "voice_uplink", 4096, NULL, 5, &s_uplink_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create uplink task");
        return ESP_ERR_NO_MEM;
    }

    /* Downlink ringbuffer + task. Storage in PSRAM (large), control struct in
     * internal RAM (required by the static ringbuffer API). */
    uint8_t *rb_storage = heap_caps_malloc(DOWNLINK_RB_BYTES, MALLOC_CAP_SPIRAM);
    s_downlink_rb_struct = heap_caps_malloc(sizeof(StaticRingbuffer_t),
                                            MALLOC_CAP_INTERNAL);
    if (!rb_storage || !s_downlink_rb_struct) {
        ESP_LOGE(TAG, "failed to alloc downlink ringbuffer");
        return ESP_ERR_NO_MEM;
    }
    s_downlink_rb = xRingbufferCreateStatic(DOWNLINK_RB_BYTES,
                                            RINGBUF_TYPE_BYTEBUF,
                                            rb_storage, s_downlink_rb_struct);
    if (!s_downlink_rb) {
        ESP_LOGE(TAG, "failed to create downlink ringbuffer");
        return ESP_ERR_NO_MEM;
    }
    /* 8KB stack: glm_audio_out_play -> bsp_audio_play uses a 4KB stack scratch
     * buffer; 4096 overflowed and crashed mid-playback.
     * Pinned to core 1 at priority 6 (above the AFE detect task at 5): playback
     * is real-time critical and must preempt wake detection, otherwise the AFE
     * starves it of CPU and the codec underruns (stutter). WS receive stays on
     * core 0, unaffected. */
    if (xTaskCreatePinnedToCore(downlink_task, "voice_downlink", 8192, NULL, 6,
                                &s_downlink_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create downlink task");
        return ESP_ERR_NO_MEM;
    }

    /* Idle/turn watchdog. */
    if (xTaskCreate(watchdog_task, "voice_wdt", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create watchdog task");
        return ESP_ERR_NO_MEM;
    }

    /* Register mic uplink + end-of-utterance cbs before enabling wake/streaming. */
    bsp_sr_set_audio_cb(uplink_enqueue);
    bsp_sr_set_eou_cb(on_eou);

    err = bsp_sr_start(on_wake);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sr_start failed: %s", esp_err_to_name(err));
        return err;
    }

    set_state(VOICE_IDLE);
    ESP_LOGI(TAG, "voice app ready (IDLE)");
    return ESP_OK;
}
