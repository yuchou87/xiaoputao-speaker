#pragma once
#include "esp_err.h"

typedef void (*bsp_sr_wake_cb_t)(void);

// Start esp-sr AFE + WakeNet. Feeds the ES7210 mic (mono "M" format) and
// invokes cb() on each wake-word detection. Requires bsp_audio_init() first.
esp_err_t bsp_sr_start(bsp_sr_wake_cb_t cb);
