#pragma once
#include <stddef.h>
#include <stdint.h>

/**
 * Resample mono PCM from 24000 Hz to 16000 Hz using linear interpolation.
 *
 * Ratio: 2/3 (each 3 input samples produce 2 output samples).
 *
 * @param in         Input samples (24 kHz mono int16).
 * @param in_samples Number of input samples.
 * @param out        Output buffer. Caller must ensure capacity >= in_samples*2/3 + 2.
 * @return           Number of output samples written.
 */
size_t resample_24k_to_16k(const int16_t *in, size_t in_samples, int16_t *out);
