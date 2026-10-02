import hashlib
import importlib.util
import io
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("release", Path(__file__).parents[1] / "tools" / "release_artifacts.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def app(version="0.10"):
    payload = bytearray(256)
    struct.pack_into("<I", payload, 0, 0xABCD5432)
    payload[16:16 + len(version)] = version.encode()
    payload[48:63] = b"soft_decoder_v3"
    payload[112:116] = b"v6.0"
    header = bytearray(24)
    header[:4] = bytes((0xE9, 1, 2, 0x2F))
    struct.pack_into("<H", header, 12, 9)
    header[23] = 1
    image = header + struct.pack("<II", 0x3C000020, len(payload)) + payload
    checksum = 0xEF
    for value in payload:
        checksum ^= value
    image.extend(b"\0" * ((len(image) | 15) - len(image)))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return bytes(image)


def partitions():
    entries = bytearray()
    for name, kind, subtype, offset, size in (
        ("otadata", 1, 0, 0x1A000, 0x2000),
        ("ota_0", 0, 16, 0x20000, release.SLOT_SIZE),
        ("ota_1", 0, 17, 0x200000, release.SLOT_SIZE),
    ):
        entries.extend(struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, name.encode(), 0))
    entries.extend(b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries).digest())
    return bytes(entries) + b"\xff" * (0xC00 - len(entries))


def wav():
    chunks = b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 22050, 44100, 2, 16)
    chunks += b"data" + struct.pack("<I", 2) + b"\0\0"
    return b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks


def package(names=(b"slot1.wav",)):
    firmware = app()
    output = io.BytesIO()
    output.write(b"AURAOTA2" + struct.pack("<II", len(firmware), len(names)) + firmware)
    for name in names:
        data = wav()
        output.write(struct.pack("<HHI", len(name), 0, len(data)) + name + data)
    return output.getvalue()


class ReleaseTests(unittest.TestCase):
    def test_app_role_version_and_checksum(self):
        info = release.image_info(app())
        self.assertEqual((info["role"], info["version"]), ("application", "0.10"))

    def test_corrupt_segment_and_hash_rejected(self):
        for offset in (64, -1):
            damaged = bytearray(app())
            damaged[offset] ^= 1
            with self.assertRaises(ValueError):
                release.image_info(damaged)

    def test_trailing_bytes_and_wrong_chip_rejected(self):
        with self.assertRaises(ValueError):
            release.image_info(app() + b"\xff")
        damaged = bytearray(app())
        damaged[12] = 0
        with self.assertRaisesRegex(ValueError, "ESP32-S3"):
            release.image_info(damaged)

    def test_partition_md5_and_offsets(self):
        info = release.partition_info(partitions())
        self.assertEqual(info["entries"][1]["offset"], 0x20000)
        damaged = bytearray(partitions())
        damaged[8] ^= 1
        with self.assertRaises(ValueError):
            release.partition_info(damaged)

    def test_container_full_consumption(self):
        self.assertEqual(release.container_info(package())["files"], 1)
        for damaged in (package()[:-1], package() + b"\0"):
            with self.assertRaises(ValueError):
                release.container_info(damaged)

    def test_container_duplicates_and_slot_limits(self):
        for names in ((b"slot1.wav", b"slot1.wav"), (b"slot21.wav",), tuple(b"x%d.wav" % i for i in range(21))):
            with self.assertRaises(ValueError):
                release.container_info(package(names))

    def test_wav_complete_payload_required(self):
        release.wav_info(wav())
        with self.assertRaises(ValueError):
            release.wav_info(wav()[:-1])


if __name__ == "__main__":
    unittest.main()
