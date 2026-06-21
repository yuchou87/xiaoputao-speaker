#include "resample.h"

/**
 * Resample 24 kHz mono -> 16 kHz mono via linear interpolation.
 *
 * For output index j, the source position is j * (24000/16000) = j * 1.5.
 * We compute floor/ceil indices and blend proportionally.
 * We stop before reading past in_samples-1 to stay in bounds.
 */
size_t resample_24k_to_16k(const int16_t *in, size_t in_samples, int16_t *out)
{
    if (in_samples == 0) {
        return 0;
    }

    size_t out_count = 0;

    /* Walk output indices; stop when the source floor index reaches the last
     * valid input sample (ceil would be out of bounds). */
    for (size_t j = 0; ; j++) {
        /* Source position in Q1 (scaled by 2 to avoid floats): src_q1 = j*3 */
        size_t src_q1 = j * 3u;          /* = j * 1.5 * 2 */
        size_t floor_idx = src_q1 >> 1;  /* integer part */

        if (floor_idx >= in_samples - 1) {
            /* ceil_idx would be out of range; emit the last sample if exact */
            if (floor_idx == in_samples - 1 && (src_q1 & 1u) == 0) {
                out[out_count++] = in[floor_idx];
            }
            break;
        }

        int32_t s0 = in[floor_idx];
        int32_t s1 = in[floor_idx + 1];

        /* Fractional part is (src_q1 & 1) out of 2 */
        if (src_q1 & 1u) {
            /* frac = 0.5 -> midpoint */
            out[out_count++] = (int16_t)((s0 + s1) >> 1);
        } else {
            /* frac = 0 -> exact sample */
            out[out_count++] = (int16_t)s0;
        }
    }

    return out_count;
}
