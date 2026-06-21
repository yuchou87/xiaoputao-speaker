#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "es7210_adc.h"
#include "esp_log.h"

#define I2S_MCLK 2
#define I2S_BCLK 48
#define I2S_WS   38
#define I2S_DOUT 47
#define I2S_DIN  39
#define ES8311_ADDR 0x30   // esp_codec_dev wants 8-bit addr (0x18<<1); it >>1 internally
#define ES7210_ADDR 0x80   // 0x40<<1
#define SAMPLE_RATE 16000

static const char *TAG = "bsp_audio";
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_out, s_in;
static const audio_codec_data_if_t *s_data;

// Exposed so the ES7210 capture task (later) can reuse the same RX channel.
i2s_chan_handle_t bsp_audio_i2s_rx(void) { return s_rx; }

esp_err_t bsp_audio_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, &s_rx));

    // This board's ES8311/ES7210 are clocked for 32-bit STEREO I2S slots
    // (matches the working demo's I2S_CONFIG_DEFAULT). esp_codec_dev bridges
    // the per-codec open format (16/mono) to this 32/stereo slot.
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK, .bclk = I2S_BCLK, .ws = I2S_WS,
            .dout = I2S_DOUT, .din = I2S_DIN,
            .invert_flags = { 0 },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    audio_codec_i2c_cfg_t ic = { .port = 0, .addr = ES8311_ADDR, .bus_handle = bsp_i2c_bus() };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ic);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    audio_codec_i2s_cfg_t isc = { .port = 0, .tx_handle = s_tx, .rx_handle = s_rx };
    s_data = audio_codec_new_i2s_data(&isc);

    // --- Output: ES8311 DAC ---
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = 15,            // NS4150B amp enable = GPIO15 HIGH (from Waveshare Arduino demo: digitalWrite(15,HIGH))
        .use_mclk = true,        // MCLK (GPIO2) is wired to the codec; Waveshare's Arduino audio demo uses it
        .master_mode = false,    // ESP32 is I2S master; codec is slave
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es);
    if (!codec) { ESP_LOGE(TAG, "es8311_codec_new failed"); return ESP_FAIL; }

    esp_codec_dev_cfg_t dc = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = s_data,
    };
    s_out = esp_codec_dev_new(&dc);
    if (!s_out) { ESP_LOGE(TAG, "esp_codec_dev_new(out) failed"); return ESP_FAIL; }
    esp_codec_dev_set_out_vol(s_out, 70);
    // Open at the native slot format (32-bit stereo) to avoid relying on
    // esp_codec_dev's output up-conversion (which produced silence at 16/mono).
    esp_codec_dev_sample_info_t fs = { .sample_rate = SAMPLE_RATE, .channel = 2, .bits_per_sample = 32 };
    if (esp_codec_dev_open(s_out, &fs) != 0) { ESP_LOGE(TAG, "open(out) failed"); return ESP_FAIL; }
    ESP_LOGI(TAG, "ES8311 output ready (16k/32/stereo)");

    // --- Input: ES7210 ADC (mic1) ---
    audio_codec_i2c_cfg_t ic2 = { .port = 0, .addr = ES7210_ADDR, .bus_handle = bsp_i2c_bus() };
    const audio_codec_ctrl_if_t *ctrl2 = audio_codec_new_i2c_ctrl(&ic2);
    es7210_codec_cfg_t es7 = {
        .ctrl_if = ctrl2,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1,
        .mclk_src = ES7210_MCLK_FROM_PAD,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&es7);
    if (!adc) { ESP_LOGE(TAG, "es7210_codec_new failed"); return ESP_FAIL; }
    esp_codec_dev_cfg_t dc2 = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = s_data,
    };
    s_in = esp_codec_dev_new(&dc2);
    if (!s_in) { ESP_LOGE(TAG, "esp_codec_dev_new(in) failed"); return ESP_FAIL; }
    esp_codec_dev_sample_info_t fsi = { .sample_rate = SAMPLE_RATE, .channel = 1, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_in, &fsi) != 0) { ESP_LOGE(TAG, "open(in) failed"); return ESP_FAIL; }
    esp_codec_dev_set_in_gain(s_in, 30.0);
    ESP_LOGI(TAG, "ES7210 input ready (16k/16/mono, mic1)");

    return ESP_OK;
}

esp_err_t bsp_audio_read(int16_t *pcm, size_t samples)
{
    int r = esp_codec_dev_read(s_in, (void *) pcm, samples * sizeof(int16_t));
    return r == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_audio_play(const int16_t *pcm, size_t samples)
{
    // Convert caller's 16-bit mono to the codec's 32-bit stereo format.
    static int32_t buf[512 * 2];
    size_t i = 0;
    while (i < samples) {
        size_t chunk = samples - i;
        if (chunk > 512) chunk = 512;
        for (size_t k = 0; k < chunk; k++) {
            int32_t v = ((int32_t) pcm[i + k]) << 16;   // 16-bit -> 32-bit
            buf[k * 2] = v;        // L
            buf[k * 2 + 1] = v;    // R
        }
        int r = esp_codec_dev_write(s_out, buf, chunk * 2 * sizeof(int32_t));
        if (r != 0) return ESP_FAIL;
        i += chunk;
    }
    return ESP_OK;
}

void bsp_audio_set_volume(int pct)
{
    if (s_out) esp_codec_dev_set_out_vol(s_out, pct);
}
