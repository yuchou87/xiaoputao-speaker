#pragma once
#include <stddef.h>
#include <stdint.h>

/**
 * Play raw PCM bytes arriving from the GLM downlink.
 *
 * @param pcm24_bytes  Pointer to int16 PCM data at 24 kHz mono (little-endian).
 * @param len          Byte length of the buffer (samples = len / 2).
 *
 * The function resamples to 16 kHz and plays via bsp_audio_play().
 * It processes input in blocks to handle arbitrarily large chunks.
 *
 * Signature matches glm_audio_cb_t (const uint8_t*, size_t) so it can be
 * registered directly as the GLM downlink audio callback.
 */
void glm_audio_out_play(const uint8_t *pcm24_bytes, size_t len);
