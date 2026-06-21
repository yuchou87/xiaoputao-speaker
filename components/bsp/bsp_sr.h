#pragma once
#include "esp_err.h"

typedef void (*bsp_sr_wake_cb_t)(void);

// Start esp-sr AFE + WakeNet. Feeds the ES7210 mic (mono "M" format) and
// invokes cb() on each wake-word detection. Requires bsp_audio_init() first.
esp_err_t bsp_sr_start(bsp_sr_wake_cb_t cb);

// Request a ~2s "echo": the feed task captures the next 2s of mic audio and
// plays it back on the speaker. Safe to call from the wake callback (avoids a
// second concurrent mic reader). No-op if an echo is already in progress.
void bsp_sr_echo(void);
