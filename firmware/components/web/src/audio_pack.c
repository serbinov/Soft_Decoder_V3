#include "audio_pack.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

bool audio_pack_header_parse(const uint8_t *buf, size_t len, audio_pack_header_t *out)
{
    if (buf == NULL || out == NULL || len < AUDIO_PACK_HDR_LEN) {
        return false;
    }
    if (memcmp(buf, AUDIO_PACK_MAGIC, AUDIO_PACK_MAGIC_LEN) != 0) {
        return false;
    }
    uint16_t version = rd16(buf + 8);
    uint16_t count = rd16(buf + 10);
    uint32_t data_off = rd32(buf + 12);
    if (version != AUDIO_PACK_VERSION || count == 0U || count > AUDIO_PACK_MAX_FILES) {
        return false;
    }
    /* Even the shortest possible index is 16 B/entry: the first payload can
     * never start before the fixed header plus that minimum. */
    uint32_t min_data_off = (uint32_t)AUDIO_PACK_HDR_LEN + (uint32_t)count * AUDIO_PACK_ENTRY_HDR_LEN;
    if (data_off < min_data_off) {
        return false;
    }
    out->count = count;
    out->data_off = data_off;
    return true;
}

bool audio_pack_entry_head(const uint8_t buf[AUDIO_PACK_ENTRY_HDR_LEN],
                           uint64_t chunk_end, audio_pack_entry_t *out,
                           uint8_t *name_len)
{
    if (buf == NULL || out == NULL || name_len == NULL) {
        return false;
    }
    uint8_t nlen = buf[0];
    uint8_t volume = buf[1];
    uint8_t channels = buf[2];
    uint8_t bits = buf[3];
    uint32_t sample_rate = rd32(buf + 4);
    uint32_t data_off = rd32(buf + 8);
    uint32_t data_len = rd32(buf + 12);

    if (nlen < 1U || nlen > AUDIO_PACK_NAME_MAX || volume > 100U) {
        return false;
    }
    uint32_t frame;
    if (bits == AUDIO_PACK_BITS_PCM) {
        if (channels < 1U || channels > 2U) {
            return false;
        }
        frame = (uint32_t)channels * 2U;
    } else if (bits == AUDIO_PACK_BITS_ADPCM) {
        if (channels != 1U) {
            return false; /* the packer/decoder support mono ADPCM only */
        }
        frame = AUDIO_PACK_ADPCM_BLOCK;
    } else {
        return false;
    }
    if (sample_rate == 0U || sample_rate > 192000U) {
        return false;
    }
    if (data_len < frame || (data_len % frame) != 0U) {
        return false;
    }
    if ((uint64_t)data_off + (uint64_t)data_len > chunk_end) {
        return false;
    }
    out->volume = volume;
    out->channels = channels;
    out->bits = bits;
    out->sample_rate = sample_rate;
    out->data_off = data_off;
    out->data_len = data_len;
    *name_len = nlen;
    return true;
}

bool audio_pack_entry_name(audio_pack_entry_t *out, const uint8_t *name, uint8_t name_len)
{
    if (out == NULL || name == NULL || name_len < 1U || name_len > AUDIO_PACK_NAME_MAX) {
        return false;
    }
    if (memchr(name, '\0', name_len) != NULL) {
        return false;
    }
    for (uint8_t i = 0; i < name_len; ++i) {
        if (name[i] == '/' || name[i] == '\\') {
            return false;
        }
    }
    if ((name_len == 1U && name[0] == '.') ||
        (name_len == 2U && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    memcpy(out->name, name, name_len);
    out->name[name_len] = '\0';
    return true;
}

size_t audio_pack_wav_header(uint8_t *hdr, size_t cap, uint32_t data_len,
                             uint32_t sample_rate, uint8_t channels, uint8_t bits)
{
    if (hdr == NULL) {
        return 0;
    }
    if (bits == AUDIO_PACK_BITS_ADPCM) {
        if (cap < AUDIO_PACK_WAV_HDR_MAX || channels != 1U) {
            return 0;
        }
        uint32_t block_align = AUDIO_PACK_ADPCM_BLOCK;
        uint32_t samples_per_block = 505U;
        uint32_t byte_rate = sample_rate * block_align / samples_per_block;
        uint32_t total_samples = (data_len / block_align) * samples_per_block;
        memcpy(hdr, "RIFF", 4);
        wr32(hdr + 4, 52U + data_len);      /* 60-byte hdr + data - 8 */
        memcpy(hdr + 8, "WAVE", 4);
        memcpy(hdr + 12, "fmt ", 4);
        wr32(hdr + 16, 20U);                /* fmt payload */
        wr16(hdr + 20, 0x0011U);            /* WAVE_FORMAT_IMA_ADPCM */
        wr16(hdr + 22, 1U);                 /* mono */
        wr32(hdr + 24, sample_rate);
        wr32(hdr + 28, byte_rate);
        wr16(hdr + 32, (uint16_t)block_align);
        wr16(hdr + 34, AUDIO_PACK_BITS_ADPCM);
        wr16(hdr + 36, 2U);                 /* cbSize */
        wr16(hdr + 38, (uint16_t)samples_per_block);
        memcpy(hdr + 40, "fact", 4);
        wr32(hdr + 44, 4U);
        wr32(hdr + 48, total_samples);
        memcpy(hdr + 52, "data", 4);
        wr32(hdr + 56, data_len);
        return AUDIO_PACK_WAV_HDR_MAX;
    }
    if (cap < AUDIO_PACK_WAV_HDR_LEN || channels < 1U || channels > 2U) {
        return 0;
    }
    uint32_t block_align = (uint32_t)channels * 2U;
    uint32_t byte_rate = sample_rate * block_align;
    memcpy(hdr, "RIFF", 4);
    wr32(hdr + 4, 36U + data_len);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    wr32(hdr + 16, 16U);            /* PCM fmt chunk size */
    wr16(hdr + 20, 1U);             /* format = PCM */
    wr16(hdr + 22, channels);
    wr32(hdr + 24, sample_rate);
    wr32(hdr + 28, byte_rate);
    wr16(hdr + 32, (uint16_t)block_align);
    wr16(hdr + 34, 16U);            /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    wr32(hdr + 40, data_len);
    return AUDIO_PACK_WAV_HDR_LEN;
}
