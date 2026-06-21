#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Audio: shared I2S (MCLK2/BCLK48/WS38/DOUT47/DIN39) + ES8311 DAC (0x18).
// ES7210 capture (0x40) is added in a later task on the same I2S RX.
// Format: 16 kHz, 16-bit, mono.
esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_play(const int16_t *pcm, size_t samples);  // blocking, 16-bit mono
esp_err_t bsp_audio_read_raw(int16_t *buf, size_t n_samples);  // blocking, raw 4ch interleaved (n=frames*4)
void bsp_audio_set_volume(int pct);   // 0..100
