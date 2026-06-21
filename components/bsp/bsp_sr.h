#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*bsp_sr_wake_cb_t)(void);

// Callback invoked with each AFE-processed audio chunk while streaming is on.
// pcm16: single-channel 16kHz 16-bit PCM; samples: number of int16_t samples.
typedef void (*bsp_sr_audio_cb_t)(const int16_t *pcm16, size_t samples);

// Start esp-sr AFE + WakeNet. Feeds the ES7210 mic (mono "M" format) and
// invokes cb() on each wake-word detection. Requires bsp_audio_init() first.
esp_err_t bsp_sr_start(bsp_sr_wake_cb_t cb);

// Request a ~2s "echo": the feed task captures the next 2s of mic audio and
// plays it back on the speaker. Safe to call from the wake callback (avoids a
// second concurrent mic reader). No-op if an echo is already in progress.
void bsp_sr_echo(void);

// Register a callback to receive AFE-processed mic audio chunks.
// Pass NULL to clear. Thread-safe (written atomically by caller before enabling
// streaming). Must be set before calling bsp_sr_set_streaming(true).
void bsp_sr_set_audio_cb(bsp_sr_audio_cb_t cb);

// Enable or disable forwarding of AFE-processed audio to the registered
// audio callback. Safe to toggle at runtime from any task.
void bsp_sr_set_streaming(bool on);

// End-of-utterance callback: invoked once (on the detect task) when, while
// streaming, the AFE VAD reports sustained silence after speech. Lets the app
// end the turn itself instead of relying on a remote energy VAD.
typedef void (*bsp_sr_eou_cb_t)(void);
void bsp_sr_set_eou_cb(bsp_sr_eou_cb_t cb);
