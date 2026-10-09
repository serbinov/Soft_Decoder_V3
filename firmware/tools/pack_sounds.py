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
import os
import struct
import sys

MAGIC = b"AURASP01"
VERSION = 1
MAX_FILES = 400
NAME_MAX = 63

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


def pack(src_dir: str, out_path: str, rate: int, stereo: bool,
         default_volume: int, order: str, adpcm: bool) -> int:
    if not os.path.isdir(src_dir):
        raise SystemExit("source folder not found: %s" % src_dir)
    files = [f for f in os.listdir(src_dir) if f.lower().endswith(".wav")]
    if not files:
        raise SystemExit("no .wav files in %s" % src_dir)
    if len(files) > MAX_FILES:
        raise SystemExit("too many files: %d (max %d)" % (len(files), MAX_FILES))
    if order == "mtime":
        files.sort(key=lambda f: os.path.getmtime(os.path.join(src_dir, f)))
    else:
        files.sort()

    overrides = _load_volume_overrides(src_dir, default_volume)
    used_names = set()
    entries = []  # (name, volume, rate, channels, payload_bytes)
    for fname in files:
        path = os.path.join(src_dir, fname)
        base = fname[:-4] if fname.lower().endswith(".wav") else fname
        name = _unique_name(_truncate_utf8(_sanitize(base), NAME_MAX) or "sound", used_names)
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
    tmp = out_path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(blob)
    os.replace(tmp, out_path)

    total = len(blob)
    print("Packed %d file(s), %d B -> %s (%s)" %
          (len(entries), total, out_path,
           "IMA ADPCM" if adpcm else "PCM16"))
    for name, vol, erate, ech, ebits, payload in entries:
        print("  %-40s %6d B  %d Hz  %dch  %s  vol=%d" %
              (name, len(payload), erate, ech, "ADPCM" if ebits == 4 else "PCM16", vol))
    return 0


def main(argv) -> int:
    ap = argparse.ArgumentParser(description="Pack WAV files into an AURA Sound Pack (.asp)")
    ap.add_argument("src_dir", help="folder with .wav files")
    ap.add_argument("-o", "--output", default=None, help="output .asp path")
    ap.add_argument("--rate", type=int, default=22050, help="target sample rate (default 22050)")
    ap.add_argument("--stereo", action="store_true", help="keep stereo (default mono)")
    ap.add_argument("--pcm", action="store_true",
                    help="store uncompressed PCM16 instead of IMA ADPCM (~4x larger)")
    ap.add_argument("--volume", type=int, default=100, help="default per-file volume 0..100")
    ap.add_argument("--order", choices=("name", "mtime"), default="name", help="index order")
    args = ap.parse_args(argv)
    adpcm = not args.pcm
    if adpcm and args.stereo:
        ap.error("--stereo cannot be combined with IMA ADPCM (ADPCM is mono)")
    out = args.output or (args.src_dir.rstrip("/\\") + ".asp")
    return pack(args.src_dir, out, max(1, args.rate), args.stereo,
                max(0, min(100, args.volume)), args.order, adpcm)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
