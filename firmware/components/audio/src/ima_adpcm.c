#include "ima_adpcm.h"

/* Standard IMA/DVI ADPCM step and index adaptation tables. */
static const int16_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int16_t ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

int ima_adpcm_decode_block(const uint8_t *in, size_t in_len,
                           int16_t *out, size_t out_cap)
{
    if (in == NULL || out == NULL || in_len < 4) {
        return -1;
    }
    int32_t predictor = (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    int index = in[2];
    if (index > 88) {
        index = 88;
    }
    size_t n = 0;
    if (n < out_cap) {
        out[n] = (int16_t)predictor;
    }
    n++;
    for (size_t i = 4; i < in_len; ++i) {
        uint8_t byte = in[i];
        for (int half = 0; half < 2; ++half) {
            int nibble = (half == 0) ? (byte & 0x0F) : (byte >> 4);
            int32_t step = ima_step_table[index];
            int32_t diff = step >> 3;
            if (nibble & 1) { diff += step >> 2; }
            if (nibble & 2) { diff += step >> 1; }
            if (nibble & 4) { diff += step; }
            if (nibble & 8) {
                predictor -= diff;
            } else {
                predictor += diff;
            }
            if (predictor > 32767) { predictor = 32767; }
            else if (predictor < -32768) { predictor = -32768; }
            index += ima_index_table[nibble];
            if (index < 0) { index = 0; }
            else if (index > 88) { index = 88; }
            if (n < out_cap) {
                out[n] = (int16_t)predictor;
            }
            n++;
        }
    }
    return (int)n;
}
