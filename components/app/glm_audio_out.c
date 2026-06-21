#include "glm_audio_out.h"
#include "resample.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "glm_audio_out";

/*
 * Block size for streaming resampling.
 * 4096 input samples @ 24 kHz  => ~170 ms per block.
 * Output block is at most ceil(4096 * 2/3) + 2 = 2731 samples.
 */
#define BLOCK_IN_SAMPLES   4096
#define BLOCK_OUT_SAMPLES  (BLOCK_IN_SAMPLES * 2 / 3 + 4)

static int16_t s_in_buf[BLOCK_IN_SAMPLES];
static int16_t s_out_buf[BLOCK_OUT_SAMPLES];

void glm_audio_out_play(const uint8_t *pcm24_bytes, size_t len)
{
    if (!pcm24_bytes || len < 2) {
        return;
    }

    const int16_t *src = (const int16_t *)(const void *)pcm24_bytes;
    size_t total_in = len / 2;  /* byte len -> sample count */

    ESP_LOGD(TAG, "play: %zu input samples (24 kHz)", total_in);

    size_t offset = 0;
    while (offset < total_in) {
        size_t block = total_in - offset;
        if (block > BLOCK_IN_SAMPLES) {
            block = BLOCK_IN_SAMPLES;
        }

        /* Copy to aligned scratch buffer (src may be unaligned) */
        memcpy(s_in_buf, src + offset, block * sizeof(int16_t));

        size_t out_samples = resample_24k_to_16k(s_in_buf, block, s_out_buf);

        if (out_samples > 0) {
            esp_err_t err = bsp_audio_play(s_out_buf, out_samples);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "bsp_audio_play failed: %s", esp_err_to_name(err));
            }
        }

        offset += block;
    }
}
