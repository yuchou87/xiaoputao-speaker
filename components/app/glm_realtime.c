#include "glm_realtime.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "bsp_nvs.h"
#include "bsp_wifi.h"

static const char *TAG = "glm_rt";

#define GLM_DEFAULT_URL     "wss://open.bigmodel.cn/api/paas/v4/realtime"
#define GLM_KEY_MAXLEN      256
#define GLM_URL_MAXLEN      256
#define GLM_REASSEMBLY_CAP  (64 * 1024)   /* max reassembly buffer (64 KB) */

/* ------------------------------------------------------------------ */
/*  Module state                                                        */
/* ------------------------------------------------------------------ */

static esp_websocket_client_handle_t s_client = NULL;
static volatile bool                 s_connected = false;

static glm_audio_cb_t  s_audio_cb = NULL;
static glm_event_cb_t  s_event_cb = NULL;

/* Text-frame reassembly buffer */
static uint8_t *s_rx_buf    = NULL;
static size_t   s_rx_len    = 0;
static size_t   s_rx_cap    = 0;
/* Whether the frame currently being reassembled is a text frame we should keep.
 * The op_code is only meaningful on the first event (payload_offset == 0);
 * continuation events report op_code 0, so we latch the decision here. */
static bool     s_rx_is_text = false;

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t rx_buf_ensure(size_t extra)
{
    size_t needed = s_rx_len + extra + 1; /* +1 for NUL */
    if (needed <= s_rx_cap) return ESP_OK;
    size_t new_cap = needed < 4096 ? 4096 : needed;
    if (new_cap > GLM_REASSEMBLY_CAP) {
        ESP_LOGE(TAG, "reassembly buffer would exceed cap (%u)", GLM_REASSEMBLY_CAP);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *p = realloc(s_rx_buf, new_cap);
    if (!p) return ESP_ERR_NO_MEM;
    s_rx_buf = p;
    s_rx_cap = new_cap;
    return ESP_OK;
}

static void rx_buf_reset(void)
{
    s_rx_len = 0;
    if (s_rx_buf) s_rx_buf[0] = '\0';
}

/* ------------------------------------------------------------------ */
/*  JSON message dispatch                                               */
/* ------------------------------------------------------------------ */

static void dispatch_message(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "failed to parse JSON message");
        return;
    }

    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_j) || !type_j->valuestring) {
        ESP_LOGW(TAG, "message missing 'type' field");
        cJSON_Delete(root);
        return;
    }

    const char *type = type_j->valuestring;
    ESP_LOGD(TAG, "server event: %s", type);

    if (strcmp(type, "response.audio.delta") == 0) {
        /* Try both "audio" and "delta" field names */
        cJSON *audio_j = cJSON_GetObjectItemCaseSensitive(root, "audio");
        if (!cJSON_IsString(audio_j) || !audio_j->valuestring) {
            audio_j = cJSON_GetObjectItemCaseSensitive(root, "delta");
        }

        if (cJSON_IsString(audio_j) && audio_j->valuestring) {
            const char *b64 = audio_j->valuestring;
            size_t b64_len  = strlen(b64);
            size_t out_len  = 0;

            /* First call to get required output length */
            mbedtls_base64_decode(NULL, 0, &out_len,
                                  (const unsigned char *)b64, b64_len);

            uint8_t *pcm_buf = malloc(out_len);
            if (!pcm_buf) {
                ESP_LOGE(TAG, "OOM decoding audio delta");
            } else {
                int ret = mbedtls_base64_decode(pcm_buf, out_len, &out_len,
                                                (const unsigned char *)b64, b64_len);
                if (ret == 0) {
                    if (s_audio_cb) {
                        s_audio_cb(pcm_buf, out_len);
                    }
                } else {
                    ESP_LOGE(TAG, "base64 decode error: %d", ret);
                }
                free(pcm_buf);
            }
        }
    } else {
        if (s_event_cb) {
            s_event_cb(type);
        }
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/*  session.update builder                                              */
/* ------------------------------------------------------------------ */

static void send_session_update(esp_websocket_client_handle_t client)
{
    cJSON *root    = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON *modalities = cJSON_CreateArray();
    cJSON *vad = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "type", "session.update");

    cJSON_AddStringToObject(session, "model",               "glm-realtime");
    cJSON_AddStringToObject(session, "input_audio_format",  "pcm16");
    cJSON_AddStringToObject(session, "output_audio_format", "pcm");
    cJSON_AddStringToObject(session, "voice",               "tongtong");  // required per GLM doc

    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(session, "modalities", modalities);

    cJSON_AddStringToObject(vad, "type",              "server_vad");
    cJSON_AddBoolToObject(vad, "create_response",     true);
    cJSON_AddBoolToObject(vad, "interrupt_response",  true);
    cJSON_AddNumberToObject(vad, "silence_duration_ms", 500);
    cJSON_AddItemToObject(session, "turn_detection", vad);

    // beta_fields is required per GLM doc; chat_mode "audio" = voice-only call.
    cJSON *beta = cJSON_CreateObject();
    cJSON_AddStringToObject(beta, "chat_mode", "audio");
    cJSON_AddItemToObject(session, "beta_fields", beta);

    cJSON_AddItemToObject(root, "session", session);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "failed to serialise session.update");
        return;
    }

    int sent = esp_websocket_client_send_text(client, json_str, strlen(json_str),
                                              pdMS_TO_TICKS(3000));
    if (sent < 0) {
        ESP_LOGE(TAG, "send session.update failed");
    } else {
        ESP_LOGI(TAG, "session.update sent (%d bytes)", sent);
    }
    free(json_str);
}

/* ------------------------------------------------------------------ */
/*  WebSocket event handler                                             */
/* ------------------------------------------------------------------ */

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_connected = true;
        rx_buf_reset();
        send_session_update(s_client);
        if (s_event_cb) s_event_cb("_ws_connected");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_connected = false;
        rx_buf_reset();
        if (s_event_cb) s_event_cb("_ws_disconnected");
        break;

    case WEBSOCKET_EVENT_DATA:
        /* Reassemble fragmented text messages. A single large text frame is
         * delivered across multiple events: only the first (payload_offset == 0)
         * carries the real op_code; continuation events report op_code 0. So we
         * must NOT gate on op_code per-event — latch the decision on the first
         * event and accumulate the rest regardless of their op_code. */
        if (data->payload_offset == 0) {
            rx_buf_reset();                      /* new message starts */
            /* Skip clearly-non-text control frames (close/ping/pong). Text is
             * op_code 1; a fragmented frame's first event also reports 1. */
            switch (data->op_code) {
            case 0x8:   /* close */
            case 0x9:   /* ping  */
            case 0xA:   /* pong  */
                s_rx_is_text = false;
                break;
            default:
                s_rx_is_text = true;
                break;
            }
        }

        if (!s_rx_is_text) break;                /* ignore non-text frames */

        if (data->data_len > 0 && data->data_ptr) {
            if (rx_buf_ensure(data->data_len) != ESP_OK) {
                rx_buf_reset();
                s_rx_is_text = false;
                break;
            }
            memcpy(s_rx_buf + s_rx_len, data->data_ptr, data->data_len);
            s_rx_len += data->data_len;
            s_rx_buf[s_rx_len] = '\0';
        }

        /* When we have the full payload, dispatch */
        if (s_rx_len == (size_t)data->payload_len) {
            dispatch_message((char *)s_rx_buf);
            rx_buf_reset();
            s_rx_is_text = false;
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        s_connected = false;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void glm_rt_set_audio_cb(glm_audio_cb_t cb)
{
    s_audio_cb = cb;
}

void glm_rt_set_event_cb(glm_event_cb_t cb)
{
    s_event_cb = cb;
}

bool glm_rt_connected(void)
{
    return s_connected;
}

esp_err_t glm_rt_start(void)
{
    if (s_client) {
        ESP_LOGW(TAG, "already started");
        return ESP_OK;
    }

    /* --- Read GLM API key from NVS --- */
    char glm_key[GLM_KEY_MAXLEN] = {0};
    esp_err_t err = bsp_nvs_get_str(NVS_KEY_GLM_KEY, glm_key, sizeof(glm_key));
    if (err != ESP_OK || glm_key[0] == '\0') {
        ESP_LOGE(TAG, "NVS key '%s' missing or empty — cannot connect", NVS_KEY_GLM_KEY);
        return ESP_ERR_INVALID_STATE;
    }

    /* --- Read optional backend URL override --- */
    char url[GLM_URL_MAXLEN] = {0};
    if (bsp_nvs_has(NVS_KEY_BACKEND_URL)) {
        bsp_nvs_get_str(NVS_KEY_BACKEND_URL, url, sizeof(url));
    }
    if (url[0] == '\0') {
        strlcpy(url, GLM_DEFAULT_URL, sizeof(url));
    }
    bool is_tls = (strncmp(url, "wss://", 6) == 0);
    ESP_LOGI(TAG, "connecting to %s (%s)", url, is_tls ? "TLS" : "plain");

    /* --- Configure WebSocket client --- */
    /* Only attach the cert bundle for wss:// (cloud GLM). The local Mac backend
       is plain ws:// — no TLS, so crt_bundle must be NULL there. */
    esp_websocket_client_config_t ws_cfg = {
        .uri              = url,
        .crt_bundle_attach = is_tls ? esp_crt_bundle_attach : NULL,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .buffer_size          = 8192,   /* larger RX buffer for big text frames */
    };

    s_client = esp_websocket_client_init(&ws_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "failed to init WebSocket client");
        return ESP_FAIL;
    }

    /* --- Append auth header --- */
    err = esp_websocket_client_append_header(s_client, "Authorization", glm_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to append Authorization header: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    /* --- Register event handler --- */
    err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                        ws_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register events: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    /* --- Start client task --- */
    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start WebSocket client: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WebSocket client started");
    return ESP_OK;
}

void glm_rt_stop(void)
{
    if (!s_client) return;

    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client    = NULL;
    s_connected = false;
    rx_buf_reset();

    /* Free reassembly buffer */
    free(s_rx_buf);
    s_rx_buf = NULL;
    s_rx_len = 0;
    s_rx_cap = 0;

    ESP_LOGI(TAG, "stopped");
}

esp_err_t glm_rt_send_audio(const int16_t *pcm16, size_t samples)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t byte_len = samples * sizeof(int16_t);

    /* Calculate base64 output size: ceil(byte_len / 3) * 4 + 1 */
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len,
                          (const unsigned char *)pcm16, byte_len);

    unsigned char *b64_buf = malloc(b64_len + 1);
    if (!b64_buf) {
        ESP_LOGE(TAG, "OOM encoding audio");
        return ESP_ERR_NO_MEM;
    }

    int ret = mbedtls_base64_encode(b64_buf, b64_len + 1, &b64_len,
                                    (const unsigned char *)pcm16, byte_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "base64 encode error: %d", ret);
        free(b64_buf);
        return ESP_FAIL;
    }
    b64_buf[b64_len] = '\0';

    /* Build JSON message */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type",  "input_audio_buffer.append");
    cJSON_AddStringToObject(root, "audio", (char *)b64_buf);
    free(b64_buf);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "OOM serialising audio append");
        return ESP_ERR_NO_MEM;
    }

    /* Uplink audio is disposable under backpressure: use a short timeout so a
     * stalled socket can't block the uplink task for seconds. */
    int sent = esp_websocket_client_send_text(s_client, json_str, strlen(json_str),
                                              pdMS_TO_TICKS(150));
    free(json_str);

    if (sent < 0) {
        ESP_LOGE(TAG, "send_audio failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}
