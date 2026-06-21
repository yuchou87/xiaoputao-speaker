#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback for downlink PCM16 24kHz mono audio bytes from GLM-Realtime.
 *
 * @param pcm16 Pointer to PCM16 sample bytes.
 * @param len   Byte count.
 */
typedef void (*glm_audio_cb_t)(const uint8_t *pcm16, size_t len);

/**
 * @brief Callback for server event type strings (e.g. "session.created").
 *
 * @param type Null-terminated event type string. Valid only during the callback.
 */
typedef void (*glm_event_cb_t)(const char *type);

/**
 * @brief Register a downlink audio callback.
 *        Must be called before glm_rt_start (or at any point; takes effect immediately).
 */
void glm_rt_set_audio_cb(glm_audio_cb_t cb);

/**
 * @brief Register a server event callback for non-audio events.
 */
void glm_rt_set_event_cb(glm_event_cb_t cb);

/**
 * @brief Connect to GLM-Realtime WebSocket, authenticate, and send session.update.
 *
 * Reads NVS_KEY_BACKEND_URL (optional override) and NVS_KEY_GLM_KEY.
 * Returns ESP_OK if the client task was started successfully.
 */
esp_err_t glm_rt_start(void);

/**
 * @brief Disconnect and destroy the WebSocket client.
 */
void glm_rt_stop(void);

/**
 * @brief Returns true if the WebSocket is currently connected.
 */
bool glm_rt_connected(void);

/**
 * @brief Send PCM16 audio samples upstream as an input_audio_buffer.append message.
 *
 * Base64-encodes the samples and sends via the WebSocket text channel.
 *
 * @param pcm16   Pointer to 16-bit signed PCM samples.
 * @param samples Number of samples (not bytes).
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t glm_rt_send_audio(const int16_t *pcm16, size_t samples);

/**
 * @brief Commit the input audio buffer, signalling end-of-utterance.
 *
 * Sends input_audio_buffer.commit so the backend runs the STT->LLM->TTS turn
 * on the audio streamed so far. Used when the device's AFE VAD detects the
 * user has stopped speaking.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t glm_rt_commit(void);

#ifdef __cplusplus
}
#endif
