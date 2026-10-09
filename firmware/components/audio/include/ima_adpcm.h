#ifndef IMA_ADPCM_H
#define IMA_ADPCM_H

#include <stddef.h>
#include <stdint.h>

/* Microsoft IMA ADPCM (WAVE_FORMAT_IMA_ADPCM, 0x0011), mono, as produced by
 * tools/pack_sounds.py and by common PC encoders.
 *
 * A mono block is IMA_ADPCM_BLOCK_ALIGN (256) bytes: a 4-byte preamble
 * (int16 predictor, u8 index, u8 reserved) followed by 252 data bytes = 504
 * nibbles. The preamble sample is the block's first output sample, so one block
 * yields IMA_ADPCM_SAMPLES_PER_BLOCK (505) samples. Blocks are self-contained,
 * so decoding never depends on an earlier block. */

#define IMA_ADPCM_BITS              4
#define IMA_ADPCM_BLOCK_ALIGN       256
#define IMA_ADPCM_SAMPLES_PER_BLOCK 505

/* Decode one mono ADPCM block (in_len bytes, normally IMA_ADPCM_BLOCK_ALIGN)
 * into out[], which must hold at least 1 + (in_len - 4) * 2 samples. Returns
 * the number of samples written, or -1 on invalid input. */
int ima_adpcm_decode_block(const uint8_t *in, size_t in_len,
                           int16_t *out, size_t out_cap);

/* Sample count a `data_len`-byte payload decodes to (blocks * 505). */
static inline uint32_t ima_adpcm_samples_for(uint32_t data_len)
{
    return (data_len / IMA_ADPCM_BLOCK_ALIGN) * IMA_ADPCM_SAMPLES_PER_BLOCK;
}

#endif /* IMA_ADPCM_H */
