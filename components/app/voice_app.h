#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the voice conversation orchestration state machine.
 *
 * Call after WiFi is connected. This:
 *  - wires the GLM-Realtime audio + event callbacks,
 *  - starts the GLM-Realtime WebSocket client,
 *  - creates the uplink queue + task that drains mic PCM to GLM,
 *  - registers the bsp_sr audio callback (non-blocking enqueue),
 *  - starts esp-sr wake-word detection,
 *  - and enters the IDLE state (waiting for the wake word).
 *
 * Assumes bsp_audio_init() and the esp-sr model are already initialized.
 *
 * @return ESP_OK on success, or an error from glm_rt_start / bsp_sr_start /
 *         resource allocation.
 */
esp_err_t voice_app_start(void);

#ifdef __cplusplus
}
#endif
