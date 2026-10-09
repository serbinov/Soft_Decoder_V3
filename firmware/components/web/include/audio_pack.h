#ifndef AUDIO_PACK_H
#define AUDIO_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AURA Sound Pack (.asp) v1 -- a minimal binary container that carries up to
 * AUDIO_PACK_MAX_FILES PCM16 WAV payloads plus a compact index. The PC packer
 * (tools/pack_sounds.py) produces it; the device streams it straight from the
 * HTTP body and unpacks every entry into an individual .wav file under
 * /userdata/audio, so no whole-pack copy is stored on the NOR.
 *
 * Layout (little-endian):
 *   Header (16 B):
 *     0x00 char[8] magic "AURASP01"
 *     0x08 u16     version (AUDIO_PACK_VERSION)
 *     0x0A u16     count   (1..AUDIO_PACK_MAX_FILES)
 *     0x0C u32     data_off (absolute offset of the first payload byte)
 *   Index (count entries, sequential):
 *     u8   name_len   (1..AUDIO_PACK_NAME_MAX; base name, no ".wav")
 *     u8   volume     (0..100, per-file default volume)
 *     u8   channels   (1..2)
 *     u8   bits       (16)
 *     u32  sample_rate
 *     u32  data_off   (absolute; entries ascending and back to back)
 *     u32  data_len   (interleaved PCM16 payload bytes)
 *     char name[name_len]
 *   Payloads follow the index at their data_off, back to back; the last
 *   payload ends exactly at the container end.
 *
 * The index may be rebuilt on the device but the payloads are never read as a
 * file: they arrive sequentially in the request body. */

#define AUDIO_PACK_MAGIC         "AURASP01"
#define AUDIO_PACK_MAGIC_LEN     8
#define AUDIO_PACK_VERSION       1
#define AUDIO_PACK_HDR_LEN       16
#define AUDIO_PACK_ENTRY_HDR_LEN 16
#define AUDIO_PACK_MAX_FILES     400
#define AUDIO_PACK_NAME_MAX      63
/* WAV header written on unpack: 44 bytes for PCM16, 60 for mono IMA ADPCM
 * (extra 20-byte fmt + 12-byte fact chunk). */
#define AUDIO_PACK_WAV_HDR_LEN   44
#define AUDIO_PACK_WAV_HDR_MAX   60
/* Per-entry `bits` selects the payload encoding: 16 = PCM16, 4 = IMA ADPCM. */
#define AUDIO_PACK_BITS_PCM      16
#define AUDIO_PACK_BITS_ADPCM    4
#define AUDIO_PACK_ADPCM_BLOCK   256

typedef struct {
    uint32_t count;
    uint32_t data_off;   /* absolute offset of the first payload */
} audio_pack_header_t;

typedef struct {
    char name[AUDIO_PACK_NAME_MAX + 1]; /* base name, no extension */
    uint8_t volume;                     /* 0..100 */
    uint8_t channels;                   /* 1..2 */
    uint16_t bits;                      /* 16 */
    uint32_t sample_rate;
    uint32_t data_off;                  /* absolute payload offset */
    uint32_t data_len;                  /* payload length in bytes */
} audio_pack_entry_t;

/* Parse the fixed 16-byte header. Returns false on bad magic/version, a count
 * outside 1..AUDIO_PACK_MAX_FILES, or a data_off that cannot follow the
 * smallest possible index. */
bool audio_pack_header_parse(const uint8_t *buf, size_t len, audio_pack_header_t *out);

/* Parse the 16-byte fixed entry header. On success fills the scalar fields and
 * reports the name length in *name_len (1..63); the caller then reads that many
 * name bytes and calls audio_pack_entry_name(). Returns false on an invalid
 * format, an impossible name length, or payload bounds outside `chunk_end`. */
bool audio_pack_entry_head(const uint8_t buf[AUDIO_PACK_ENTRY_HDR_LEN],
                           uint64_t chunk_end, audio_pack_entry_t *out,
                           uint8_t *name_len);

/* Attach the name bytes to a parsed entry head. Rejects empty names, embedded
 * NUL, path separators and "."/"..". */
bool audio_pack_entry_name(audio_pack_entry_t *out, const uint8_t *name, uint8_t name_len);

/* Build a canonical WAV header for `data_len` payload bytes at `sample_rate`.
 * `bits` selects PCM16 (16, channels 1..2) or mono IMA ADPCM (4). `hdr` must
 * hold AUDIO_PACK_WAV_HDR_MAX bytes. Returns the header length (44 or 60). */
size_t audio_pack_wav_header(uint8_t *hdr, size_t cap, uint32_t data_len,
                             uint32_t sample_rate, uint8_t channels, uint8_t bits);

#endif /* AUDIO_PACK_H */
