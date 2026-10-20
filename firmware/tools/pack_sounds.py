#!/usr/bin/env python3
"""Pack a folder of WAV files into an AURA Sound Pack (.asp).

The device output is fixed mono 22050 Hz PCM16, so this packer downmixes and
resamples every input to that format and stores the raw PCM16 payloads plus a
compact index. The resulting container is streamed straight from the web
upload and unpacked on the device into individual /userdata/audio/*.wav files.

Container layout (little-endian), see components/web/include/audio_pack.h:
  Header (16 B): magic "AURASP01", u16 version, u16 count, u32 data_off
  Index  (per entry): u8 name_len, u8 volume, u8 channels, u8 bits, u32 rate,
                      u32 data_off, u32 data_len, name bytes
  Payloads: raw PCM16, back to back.

Usage:
  python pack_sounds.py SRC_DIR [-o out.asp] [--rate 22050] [--pcm] [--stereo]
                                [--volume N] [--order name]

Options:
  SRC_DIR        Folder with the .wav files (scanned non-recursively).
  -o, --output   Output .asp path (default: SRC_DIR + ".asp").
  --rate         Sample-rate ceiling in Hz (default 22050); higher-rate inputs
                 are resampled down, lower-rate inputs are left untouched.
  --pcm          Uncompressed PCM16 instead of IMA ADPCM (default is ADPCM).
  --stereo       Keep stereo instead of downmixing to mono (PCM only).
  --volume       Default per-file volume 0..100 (default 100).
  --order        name (default) or mtime; controls index order.

Per-file volumes: an optional "volumes.txt" in SRC_DIR with lines
"<name>=<volume>" (name may include or omit ".wav") overrides --volume.
"""
from __future__ import annotations

import argparse
import binascii
import hashlib
import hmac
import json
import os
import struct
import sys

MAGIC = b"AURASP01"
PW_MAGIC = b"AURAPW01"
VERSION = 1
MAX_FILES = 400
NAME_MAX = 63
SCHEME_ENTRY_NAME = "__aura_scheme__"
SCHEME_MAX_BYTES = 131072


def _fnv32(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = ((h * 0x01000193) & 0xFFFFFFFF)
    return h


def _prng_keystream(pw_bytes: bytes, salt: bytes, length: int) -> bytes:
    seed = pw_bytes + salt
    s0 = _fnv32(seed) or 0x9E3779B9
    s1 = _fnv32(salt) ^ 0x85EBCA6B
    out = bytearray(length)
    for i in range(length):
        s0 ^= (s0 << 13) & 0xFFFFFFFF
        s0 ^= (s0 >> 17) & 0xFFFFFFFF
        s0 ^= (s0 << 5) & 0xFFFFFFFF
        s1 = (s1 * 1664525 + 1013904223) & 0xFFFFFFFF
        out[i] = ((s0 ^ s1) >> ((i & 3) * 8)) & 0xFF
    return bytes(out)


PW_MAGIC_V2 = b"AURAPW02"
PW_VERSION = 2
KDF_SCRYPT = 2
CIPHER_CHACHA20 = 1
MAC_HMAC_SHA256 = 1
SCRYPT_LOG_N = 13
SCRYPT_R = 8
SCRYPT_P = 1
SALT_LEN = 16
NONCE_LEN = 12
TAG_LEN = 32
HDR2_LEN = 44


def _rotl32(value: int, bits: int) -> int:
    return ((value << bits) & 0xFFFFFFFF) | (value >> (32 - bits))


def _chacha20_block(key: bytes, counter: int, nonce: bytes) -> bytes:
    if len(key) != 32 or len(nonce) != 12:
        raise ValueError("invalid ChaCha20 key/nonce")
    state = [
        0x61707865, 0x3320646E, 0x79622D32, 0x6B206574,
        *struct.unpack("<8I", key),
        counter & 0xFFFFFFFF,
        *struct.unpack("<3I", nonce),
    ]
    x = state[:]
    def qr(a: int, b: int, c: int, d: int) -> None:
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF
        x[d] = _rotl32(x[d] ^ x[a], 16)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF
        x[b] = _rotl32(x[b] ^ x[c], 12)
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF
        x[d] = _rotl32(x[d] ^ x[a], 8)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF
        x[b] = _rotl32(x[b] ^ x[c], 7)
    for _ in range(10):
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
    return struct.pack("<16I", *((x[i] + state[i]) & 0xFFFFFFFF for i in range(16)))


def _chacha20_xor(key: bytes, nonce: bytes, data: bytes) -> bytes:
    out = bytearray(len(data))
    counter = 0
    for offset in range(0, len(data), 64):
        block = _chacha20_block(key, counter, nonce)
        chunk = data[offset:offset + 64]
        for index, value in enumerate(chunk):
            out[offset + index] = value ^ block[index]
        counter = (counter + 1) & 0xFFFFFFFF
    return bytes(out)


def _derive_keys(password: str, salt: bytes, log_n: int, r: int, p: int) -> tuple[bytes, bytes]:
    material = hashlib.scrypt(password.encode("utf-8"), salt=salt, n=1 << log_n, r=r, p=p,
                              dklen=64, maxmem=128 * 1024 * 1024)
    return material[:32], material[32:]


def _encrypt_legacy(plain: bytes, password: str) -> bytes:
    """AURAPW01, import-only compatibility for packs made by older releases."""
    pw_bytes = password.encode("utf-8")
    salt = os.urandom(4)
    tag = _fnv32(pw_bytes + salt)
    ks = _prng_keystream(pw_bytes, salt, len(plain))
    cipher = bytes(a ^ b for a, b in zip(plain, ks))
    return PW_MAGIC + salt + struct.pack("<I", tag) + cipher


def encrypt_asp(plain: bytes, password: str) -> bytes:
    """Authenticated password protection: scrypt + ChaCha20 + HMAC-SHA256."""
    if not password:
        return plain
    if len(password) < 8:
        raise ValueError("password must be at least 8 characters")
    salt = os.urandom(SALT_LEN)
    nonce = os.urandom(NONCE_LEN)
    enc_key, mac_key = _derive_keys(password, salt, SCRYPT_LOG_N, SCRYPT_R, SCRYPT_P)
    cipher = _chacha20_xor(enc_key, nonce, plain)
    header = (PW_MAGIC_V2 + bytes([PW_VERSION, KDF_SCRYPT,
                                   CIPHER_CHACHA20, MAC_HMAC_SHA256,
                                   SCRYPT_LOG_N, SCRYPT_R, SCRYPT_P, 0]) +
              salt + nonce)
    tag = hmac.new(mac_key, header + cipher, hashlib.sha256).digest()
    return header + cipher + tag


def _decrypt_legacy(data: bytes, password: str) -> bytes:
    if len(data) < 24:
        raise SystemExit("corrupted encrypted .asp container")
    pw_bytes = (password or "").encode("utf-8")
    salt = data[8:12]
    expected_tag = struct.unpack_from("<I", data, 12)[0]
    if _fnv32(pw_bytes + salt) != expected_tag:
        raise SystemExit("wrong sound pack password")
    cipher = data[16:]
    ks = _prng_keystream(pw_bytes, salt, len(cipher))
    return bytes(a ^ b for a, b in zip(cipher, ks))


def decrypt_asp(data: bytes, password: str) -> bytes:
    if data.startswith(PW_MAGIC):
        return _decrypt_legacy(data, password)
    if not data.startswith(PW_MAGIC_V2):
        return data
    if len(data) < HDR2_LEN + TAG_LEN:
        raise SystemExit("corrupted encrypted .asp container")
    version, kdf, cipher_id, mac_id = data[8], data[9], data[10], data[11]
    log_n, r, p = data[12], data[13], data[14]
    if (version, kdf, cipher_id, mac_id) != (PW_VERSION, KDF_SCRYPT,
                                             CIPHER_CHACHA20, MAC_HMAC_SHA256):
        raise SystemExit("unsupported encrypted .asp format")
    if not 12 <= log_n <= 14 or r != 8 or not 1 <= p <= 2 or 128 * (1 << log_n) * r > 32 * 1024 * 1024:
        raise SystemExit("invalid .asp KDF parameters")
    salt = data[16:32]
    nonce = data[32:44]
    mac_off = len(data) - TAG_LEN
    enc_key, mac_key = _derive_keys(password or "", salt, log_n, r, p)
    expected = hmac.new(mac_key, data[:mac_off], hashlib.sha256).digest()
    if not hmac.compare_digest(expected, data[mac_off:]):
        raise SystemExit("wrong password or damaged sound pack")
    return _chacha20_xor(enc_key, nonce, data[HDR2_LEN:mac_off])

# IMA/DVI ADPCM tables (Microsoft WAVE_FORMAT_IMA_ADPCM).
_IMA_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8,
                    -1, -1, -1, -1, 2, 4, 6, 8)
_IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767)
IMA_BLOCK_ALIGN = 256          # 4-byte preamble + 252 data bytes
IMA_SAMPLES_PER_BLOCK = 505    # 1 preamble sample + 504 nibbles


def _ima_encode(samples):
    """Encode mono int16 samples to MS IMA ADPCM (blocks of 256 bytes)."""
    out = bytearray()
    index = 0
    total = len(samples)
    pos = 0
    while pos < total:
        block = samples[pos:pos + IMA_SAMPLES_PER_BLOCK]
        predictor = int(block[0])
        if predictor > 32767:
            predictor = 32767
        elif predictor < -32768:
            predictor = -32768
        out += struct.pack("<hBB", predictor, index, 0)
        cur = 0
        for k in range(1, IMA_SAMPLES_PER_BLOCK):
            s = int(block[k]) if k < len(block) else int(block[-1])
            step = _IMA_STEP_TABLE[index]
            diff = s - predictor
            sign = 8 if diff < 0 else 0
            if sign:
                diff = -diff
            code = 0
            if diff >= step:
                code |= 4
                diff -= step
            half = step >> 1
            if diff >= half:
                code |= 2
                diff -= half
            quarter = step >> 2
            if diff >= quarter:
                code |= 1
            code |= sign
            # Reconstruct exactly like the decoder so the predictor stays in sync.
            delta = step >> 3
            if code & 1:
                delta += step >> 2
            if code & 2:
                delta += step >> 1
            if code & 4:
                delta += step
            predictor = predictor - delta if (code & 8) else predictor + delta
            if predictor > 32767:
                predictor = 32767
            elif predictor < -32768:
                predictor = -32768
            index += _IMA_INDEX_TABLE[code]
            if index < 0:
                index = 0
            elif index > 88:
                index = 88
            if k & 1:
                cur = code
            else:
                out.append(cur | (code << 4))
        pos += IMA_SAMPLES_PER_BLOCK
    return bytes(out)


def _sanitize(name: str) -> str:
    """Match the device sanitiser (components/web/src/web_util.c)."""
    out = []
    for ch in name:
        o = ord(ch)
        if ch.isascii() and ch.isalnum():
            out.append(ch)
        elif o >= 0x80 or ch in ("_", "-", "."):
            out.append(ch)
        else:
            out.append("_")
    return "".join(out)


def _valid_entry_name(name: str) -> bool:
    """Reject names that the device would sanitize or that escape a directory."""
    if not name or len(name.encode("utf-8")) > NAME_MAX:
        return False
    if name in (".", "..") or "\x00" in name or "/" in name or "\\" in name:
        return False
    return name == _sanitize(name)


def _validate_pack_entries(entries: list, data_off: int, total_len: int) -> None:
    """Mirror the container checks in audio_pack.c before writing anything."""
    seen = set()
    expected = data_off
    audio_count = 0
    for name, vol, ch, bits, rate, eoff, elen in entries:
        if not _valid_entry_name(name):
            raise SystemExit("unsafe or invalid entry name: %r" % name)
        if vol > 100:
            raise SystemExit("invalid volume for %r" % name)
        if bits == 0:
            if name != SCHEME_ENTRY_NAME or ch != 0 or rate != 0 or elen < 1 or elen > SCHEME_MAX_BYTES:
                raise SystemExit("invalid scheme entry")
        elif bits == 16:
            if ch < 1 or ch > 2 or rate < 1 or rate > 192000:
                raise SystemExit("invalid PCM entry %r" % name)
            frame = ch * 2
            if elen < frame or elen % frame:
                raise SystemExit("invalid PCM payload size for %r" % name)
        elif bits == 4:
            if ch != 1 or rate < 1 or rate > 192000:
                raise SystemExit("invalid ADPCM entry %r" % name)
            if elen < IMA_BLOCK_ALIGN or elen % IMA_BLOCK_ALIGN:
                raise SystemExit("invalid ADPCM payload size for %r" % name)
        else:
            raise SystemExit("unsupported bit depth %d for %r" % (bits, name))
        key = name.lower()
        if key in seen:
            raise SystemExit("duplicate entry name: %r" % name)
        seen.add(key)
        if eoff != expected:
            raise SystemExit("non-contiguous payload offset for %r" % name)
        if eoff + elen > total_len:
            raise SystemExit("payload outside the container for %r" % name)
        expected += elen
        if bits != 0:
            audio_count += 1
    if expected != total_len:
        raise SystemExit("container size mismatch")
    if audio_count == 0:
        raise SystemExit("pack contains no audio entries")


def _safe_wav_path(out_dir: str, name: str) -> str:
    root = os.path.realpath(out_dir)
    dest = os.path.realpath(os.path.join(root, name + ".wav"))
    try:
        inside = os.path.commonpath([root, dest]) == root
    except ValueError:
        inside = False
    if not inside:
        raise SystemExit("entry escapes the output directory: %r" % name)
    return dest



def _truncate_utf8(s: str, max_bytes: int) -> str:
    data = s.encode("utf-8")
    if len(data) <= max_bytes:
        return s
    data = data[:max_bytes]
    # Do not split a multi-byte sequence.
    while data and (data[-1] & 0xC0) == 0x80:
        data = data[:-1]
    return data.decode("utf-8", "ignore")


def _slice_utf8_tail(s: str, suffix: str, max_bytes: int) -> str:
    """Return `s` plus `suffix`, UTF-8 truncated to max_bytes (suffix kept)."""
    sfx = suffix.encode("utf-8")
    room = max_bytes - len(sfx)
    if room < 1:
        return _truncate_utf8(s, max_bytes)
    return _truncate_utf8(s, room) + suffix


class WavError(Exception):
    pass


def _read_wav(path: str):
    """Return (channels, sample_rate, mono_or_stereo_int16_frames)."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise WavError("not a RIFF/WAVE file")
    pos = 12
    fmt = None
    payload = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        csize = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if body + csize > len(data):
            raise WavError("truncated chunk")
        if cid == b"fmt ":
            fmt = data[body:body + csize]
        elif cid == b"data":
            payload = data[body:body + csize]
        pos = body + csize + (csize & 1)
    if fmt is None or payload is None:
        raise WavError("missing fmt or data chunk")
    if len(fmt) < 16:
        raise WavError("short fmt chunk")
    audio_format, channels, sample_rate, _byte_rate, _align, bits = struct.unpack_from(
        "<HHIIHH", fmt, 0)
    if audio_format == 0xFFFE and len(fmt) >= 26:
        audio_format = struct.unpack_from("<H", fmt, 24)[0]
    if channels < 1:
        raise WavError("no channels")
    if audio_format == 1:
        return channels, sample_rate, _decode_pcm(payload, channels, bits)
    if audio_format == 3 and bits == 32:
        return channels, sample_rate, _decode_float32(payload, channels)
    raise WavError("unsupported format %d, %d-bit" % (audio_format, bits))


def _decode_pcm(payload: bytes, channels: int, bits: int):
    if bits == 8:
        samples = [(b - 128) * 256 for b in payload]
    elif bits == 16:
        samples = list(struct.unpack("<%dh" % (len(payload) // 2), payload))
    elif bits == 24:
        samples = []
        for i in range(0, len(payload) - 2, 3):
            v = payload[i] | (payload[i + 1] << 8) | (payload[i + 2] << 16)
            if v & 0x800000:
                v -= 0x1000000
            samples.append(v >> 8)
    elif bits == 32:
        samples = [v >> 16 for v in struct.unpack("<%di" % (len(payload) // 4), payload)]
    else:
        raise WavError("unsupported PCM width %d" % bits)
    if channels * (bits // 8) and len(samples) % channels:
        samples = samples[:len(samples) - (len(samples) % channels)]
    return samples


def _decode_float32(payload: bytes, channels: int):
    raw = struct.unpack("<%df" % (len(payload) // 4), payload)
    out = []
    for v in raw:
        if v > 1.0:
            v = 1.0
        elif v < -1.0:
            v = -1.0
        out.append(int(round(v * 32767.0)))
    if len(out) % channels:
        out = out[:len(out) - (len(out) % channels)]
    return out


def _to_mono(samples, channels: int):
    if channels == 1:
        return samples
    if channels == 2:
        return [(samples[i] + samples[i + 1]) // 2 for i in range(0, len(samples) - 1, 2)]
    mono = []
    for i in range(0, len(samples) - channels + 1, channels):
        mono.append(sum(samples[i:i + channels]) // channels)
    return mono


def _to_stereo(samples, channels: int):
    if channels == 2:
        return samples
    if channels == 1:
        return [s for s in samples for _ in (0, 1)]
    out = []
    for i in range(0, len(samples) - channels + 1, channels):
        frame = samples[i:i + channels]
        out.append(frame[0])
        out.append(frame[-1])
    return out


def _resample(samples, src_rate: int, dst_rate: int):
    if src_rate == dst_rate or not samples:
        return samples
    ratio = dst_rate / float(src_rate)
    n_out = int(len(samples) * ratio)
    if n_out < 1:
        return samples[:1]
    out = [0] * n_out
    last = len(samples) - 1
    for i in range(n_out):
        x = i / ratio
        i0 = int(x)
        if i0 >= last:
            out[i] = samples[last]
            continue
        frac = x - i0
        out[i] = int(samples[i0] + (samples[i0 + 1] - samples[i0]) * frac)
    return out


def _clamp16(v: int) -> int:
    if v > 32767:
        return 32767
    if v < -32768:
        return -32768
    return v


def _unique_name(base: str, used: set) -> str:
    if base not in used:
        used.add(base)
        return base
    for n in range(2, 10000):
        cand = _slice_utf8_tail(base, "_%d" % n, NAME_MAX)
        if cand not in used:
            used.add(cand)
            return cand
    raise SystemExit("cannot make a unique name for %r" % base)


def _load_volume_overrides(src_dir: str, default_volume: int):
    vols = {}
    path = os.path.join(src_dir, "volumes.txt")
    if not os.path.isfile(path):
        return vols
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            sep = "=" if "=" in line else (" " if " " in line else None)
            if sep is None:
                continue
            key, _, val = line.partition(sep)
            key = key.strip()
            if key.lower().endswith(".wav"):
                key = key[:-4]
            try:
                v = int(val.strip())
            except ValueError:
                continue
            vols[_sanitize(key)] = max(0, min(100, v))
    return vols


def _build_wav_header(data_len: int, sample_rate: int, channels: int, bits: int) -> bytes:
    """Build the exact canonical WAV header that the device writes on unpack."""
    if bits == 4:
        block_align = IMA_BLOCK_ALIGN
        spb = IMA_SAMPLES_PER_BLOCK
        byte_rate = (sample_rate * block_align) // spb
        total_samples = (data_len // block_align) * spb
        return struct.pack(
            "<4sI4s4sIHHIIHHHH4sII4sI",
            b"RIFF", 52 + data_len, b"WAVE",
            b"fmt ", 20, 0x0011, 1, sample_rate, byte_rate, block_align, 4, 2, spb,
            b"fact", 4, total_samples,
            b"data", data_len,
        )
    block_align = channels * 2
    byte_rate = sample_rate * block_align
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + data_len, b"WAVE",
        b"fmt ", 16, 1, channels, sample_rate, byte_rate, block_align, 16,
        b"data", data_len,
    )


def _compute_asset_manifest(name: str, sample_rate: int, channels: int, bits: int, payload: bytes) -> dict:
    """Compute the exact sg_asset_t manifest for a packed WAV entry."""
    hdr = _build_wav_header(len(payload), sample_rate, channels, bits)
    full = hdr + payload
    crc = binascii.crc32(full) & 0xFFFFFFFF
    if bits == 4:
        duration_ms = ((len(payload) // IMA_BLOCK_ALIGN) * IMA_SAMPLES_PER_BLOCK * 1000) // sample_rate
    else:
        duration_ms = (len(payload) * 1000) // (sample_rate * channels * 2)
    return {
        "file": name + ".wav",
        "size": len(full),
        "sampleRate": sample_rate,
        "channels": channels,
        "bits": bits,
        "durationMs": max(1, int(duration_ms)),
        "crc32": "%08x" % crc,
    }


def _find_scheme_file(src_dir: str, explicit_path: str | None) -> str | None:
    if explicit_path:
        if not os.path.isfile(explicit_path):
            raise SystemExit("scheme file not found: %s" % explicit_path)
        return explicit_path
    for candidate in ("scheme.json", "sound_graph.json", "project.json"):
        p = os.path.join(src_dir, candidate)
        if os.path.isfile(p):
            return p
    return None


def _prepare_scheme_payload(scheme_path: str, entries: list) -> bytes:
    """Load a sound-graph JSON, populate its assets[] manifest from the packed WAVs, and serialize."""
    with open(scheme_path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    if not isinstance(doc, dict) or doc.get("format") != "sound-graph":
        raise SystemExit("%s: expected JSON with \"format\": \"sound-graph\"" % scheme_path)
    manifest_by_file = {}
    for name, _vol, erate, ech, ebits, payload in entries:
        m = _compute_asset_manifest(name, erate, ech, ebits, payload)
        manifest_by_file[m["file"]] = m
    referenced = []
    seen = set()
    for st in doc.get("states", []):
        f = st.get("file", "") if isinstance(st, dict) else ""
        if f and f not in seen:
            if f not in manifest_by_file:
                raise SystemExit("%s: state '%s' references WAV '%s' not present in pack" %
                                 (scheme_path, st.get("id", "?"), f))
            seen.add(f)
            referenced.append(manifest_by_file[f])
    doc["assets"] = referenced
    raw = json.dumps(doc, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(raw) > SCHEME_MAX_BYTES:
        raise SystemExit("%s: serialized scheme exceeds %d B (%d B)" %
                         (scheme_path, SCHEME_MAX_BYTES, len(raw)))
    return raw


def unpack(asp_path: str, out_dir: str, password: str = "") -> int:
    """Unpack an .asp container into WAV files, volumes.txt, and scheme.json (if bundled)."""
    if not os.path.isfile(asp_path):
        raise SystemExit("pack file not found: %s" % asp_path)
    with open(asp_path, "rb") as fh:
        data = fh.read()
    if data.startswith((PW_MAGIC, PW_MAGIC_V2)):
        data = decrypt_asp(data, password)
    if len(data) < 16 or data[:8] != MAGIC:
        raise SystemExit("not a valid AURASP01 container: %s" % asp_path)
    version, count, data_off = struct.unpack_from("<HHI", data, 8)
    if version != VERSION or count == 0 or count > MAX_FILES:
        raise SystemExit("unsupported version or entry count in %s" % asp_path)
    pos = 16
    entries = []
    for _ in range(count):
        if pos + 16 > data_off:
            raise SystemExit("truncated index in %s" % asp_path)
        nlen, vol, ch, bits, rate, eoff, elen = struct.unpack_from("<BBBBIII", data, pos)
        pos += 16
        if nlen < 1 or nlen > NAME_MAX or pos + nlen > data_off:
            raise SystemExit("truncated entry name in %s" % asp_path)
        try:
            name = data[pos:pos + nlen].decode("utf-8")
        except UnicodeDecodeError:
            raise SystemExit("invalid UTF-8 entry name in %s" % asp_path)
        pos += nlen
        entries.append((name, vol, ch, bits, rate, eoff, elen))
    if pos != data_off:
        raise SystemExit("index length mismatch in %s" % asp_path)
    _validate_pack_entries(entries, data_off, len(data))
    os.makedirs(out_dir, exist_ok=True)
    vol_lines = []
    wav_written = 0
    scheme_written = False
    for name, vol, ch, bits, rate, eoff, elen in entries:
        payload = data[eoff:eoff + elen]
        if bits == 0 and name == SCHEME_ENTRY_NAME:
            scheme_out = os.path.join(out_dir, "scheme.json")
            try:
                parsed = json.loads(payload.decode("utf-8"))
                with open(scheme_out, "w", encoding="utf-8") as sf:
                    json.dump(parsed, sf, ensure_ascii=False, indent=2)
            except Exception:
                with open(scheme_out, "wb") as sf:
                    sf.write(payload)
            scheme_written = True
            continue
        hdr = _build_wav_header(elen, rate, ch, bits)
        wav_out = _safe_wav_path(out_dir, name)
        with open(wav_out, "wb") as wf:
            wf.write(hdr)
            wf.write(payload)
        vol_lines.append("%s=%d\n" % (name, vol))
        wav_written += 1
    if vol_lines:
        with open(os.path.join(out_dir, "volumes.txt"), "w", encoding="utf-8") as vf:
            vf.writelines(vol_lines)
    print("Unpacked %d WAV file(s)%s -> %s" %
          (wav_written, " + scheme.json" if scheme_written else "", out_dir))
    return 0


def _load_only_names(path: str) -> set[str]:
    """Read sanitized sound names (with or without .wav) for a filtered pack."""
    names = set()
    with open(path, "r", encoding="utf-8-sig") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            names.add(s[:-4] if s.lower().endswith(".wav") else s)
    return names


def pack(src_dir: str, out_path: str, rate: int, stereo: bool,
         default_volume: int, order: str, adpcm: bool,
         scheme_path: str | None = None, password: str = "",
         only_from: str | None = None) -> int:
    if not os.path.isdir(src_dir):
        raise SystemExit("source folder not found: %s" % src_dir)
    only = _load_only_names(only_from) if only_from else None
    files = [f for f in os.listdir(src_dir) if f.lower().endswith(".wav")]
    if not files:
        raise SystemExit("no .wav files in %s" % src_dir)
    if only is None and len(files) > MAX_FILES:
        raise SystemExit("too many files: %d (max %d)" % (len(files), MAX_FILES))
    if order == "mtime":
        files.sort(key=lambda f: os.path.getmtime(os.path.join(src_dir, f)))
    else:
        files.sort()

    overrides = _load_volume_overrides(src_dir, default_volume)
    used_names = set()
    included_names = set()
    entries = []  # (name, volume, rate, channels, payload_bytes)
    for fname in files:
        path = os.path.join(src_dir, fname)
        base = fname[:-4] if fname.lower().endswith(".wav") else fname
        name = _unique_name(_truncate_utf8(_sanitize(base), NAME_MAX) or "sound", used_names)
        if only is not None and name not in only:
            continue
        included_names.add(name)
        if len(entries) >= MAX_FILES:
            raise SystemExit("too many selected files (max %d)" % MAX_FILES)
        try:
            channels, src_rate, samples = _read_wav(path)
        except WavError as exc:
            raise SystemExit("%s: %s" % (fname, exc))
        if src_rate <= 0:
            raise SystemExit("%s: invalid sample rate" % fname)
        if stereo:
            samples = _to_stereo(samples, channels)
            out_channels = 2
        else:
            samples = _to_mono(samples, channels)
            out_channels = 1
        # Only resample *down* to the ceiling: upsampling a lower-rate file
        # would double its size with no gain (the device mixer resamples any
        # rate to its fixed 22050 Hz output).
        out_rate = rate if src_rate > rate else src_rate
        samples = _resample(samples, src_rate, out_rate)
        samples = [_clamp16(int(s)) for s in samples]
        if not samples:
            raise SystemExit("%s: no audio frames" % fname)
        if adpcm:
            payload = _ima_encode(samples)
            out_bits = 4
        else:
            payload = struct.pack("<%dh" % len(samples), *samples)
            out_bits = 16
        vol = overrides.get(name, default_volume)
        entries.append((name, vol, out_rate, out_channels, out_bits, payload))

    if only is not None:
        missing = sorted(only - included_names)
        if missing:
            raise SystemExit("requested sounds not found: %s" % ", ".join(missing))
    if not entries:
        raise SystemExit("no selected .wav files")

    resolved_scheme = _find_scheme_file(src_dir, scheme_path)
    if resolved_scheme:
        scheme_bytes = _prepare_scheme_payload(resolved_scheme, entries)
        entries.append((SCHEME_ENTRY_NAME, 0, 0, 0, 0, scheme_bytes))
    if len(entries) > MAX_FILES:
        raise SystemExit("too many entries: %d (max %d including the bundled scheme)" %
                         (len(entries), MAX_FILES))

    header_off = 16
    index_len = sum(16 + len(e[0].encode("utf-8")) for e in entries)
    data_off = header_off + index_len

    chunks = []
    chunks.append(MAGIC)
    chunks.append(struct.pack("<HHI", VERSION, len(entries), data_off))
    running = data_off
    for name, vol, erate, ech, ebits, payload in entries:
        nb = name.encode("utf-8")
        chunks.append(struct.pack("<BBBBIII", len(nb), vol, ech, ebits, erate, running, len(payload)))
        chunks.append(nb)
        running += len(payload)
    for _name, _vol, _rate, _ch, _bits, payload in entries:
        chunks.append(payload)

    blob = b"".join(chunks)
    if password:
        blob = encrypt_asp(blob, password)
    tmp = out_path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(blob)
    os.replace(tmp, out_path)

    total = len(blob)
    wav_entries = [e for e in entries if e[4] != 0]
    print("Packed %d WAV file(s)%s%s, %d B -> %s (%s)" %
          (len(wav_entries),
           " + sound scheme (%s)" % os.path.basename(resolved_scheme) if resolved_scheme else "",
           " [password-protected]" if password else "",
           total, out_path,
           "IMA ADPCM" if adpcm else "PCM16"))
    for name, vol, erate, ech, ebits, payload in entries:
        if ebits == 0:
            print("  %-40s %6d B  JSON sound-graph scheme" % (name, len(payload)))
        else:
            print("  %-40s %6d B  %d Hz  %dch  %s  vol=%d" %
                  (name, len(payload), erate, ech, "ADPCM" if ebits == 4 else "PCM16", vol))
    return 0


def main(argv) -> int:
    # Non-ASCII sound names must not crash a cp1251 Windows console.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="Pack/unpack WAV files and sound scheme in an AURA Sound Pack (.asp)")
    ap.add_argument("src", nargs="?", help="folder with .wav files (or .asp path with --unpack)")
    ap.add_argument("-o", "--output", default=None, help="output .asp path (or output folder with --unpack)")
    ap.add_argument("--scheme", default=None, help="optional sound-graph JSON file to bundle into the .asp")
    ap.add_argument("--password", default="", help="optional password (min 8 chars) for authenticated encryption/decryption")
    ap.add_argument("--unpack", action="store_true", help="unpack an .asp file into a folder")
    ap.add_argument("--rate", type=int, default=22050, help="target sample rate (default 22050)")
    ap.add_argument("--stereo", action="store_true", help="keep stereo (default mono)")
    ap.add_argument("--pcm", action="store_true",
                    help="store uncompressed PCM16 instead of IMA ADPCM (~4x larger)")
    ap.add_argument("--volume", type=int, default=100, help="default per-file volume 0..100")
    ap.add_argument("--order", choices=("name", "mtime"), default="name", help="index order")
    ap.add_argument("--only-from", default=None,
                    help="text file with sanitized sound names (one per line) to include")
    args = ap.parse_args(argv)
    if not args.src:
        ap.error("src path is required")
    if args.unpack:
        out_dir = args.output or (args.src[:-4] if args.src.lower().endswith(".asp") else args.src + "_unpacked")
        return unpack(args.src, out_dir, args.password)
    if args.password and len(args.password) < 8:
        ap.error("--password must be at least 8 characters")
    adpcm = not args.pcm
    if adpcm and args.stereo:
        ap.error("--stereo cannot be combined with IMA ADPCM (ADPCM is mono)")
    out = args.output or (args.src.rstrip("/\\") + ".asp")
    return pack(args.src, out, max(1, args.rate), args.stereo,
                max(0, min(100, args.volume)), args.order, adpcm, args.scheme, args.password,
                args.only_from)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
