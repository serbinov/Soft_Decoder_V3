"""Publish and verify offline ESP32-S3 release images. Never accesses hardware."""
import argparse
import csv
import hashlib
import json
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

FLASH_SIZE = 4 * 1024 * 1024
SLOT_SIZE = 0x1E0000
ASP_MAGIC = b"AURASP01"
ASP_PW_MAGIC = b"AURAPW01"
ASP_PW2_MAGIC = b"AURAPW02"
ASP_MAX_SIZE = 15 * 1024 * 1024
ASP_MAX_FILES = 400
ASP_SCHEME_NAME = "__aura_scheme__"
ASP_SCHEME_MAX = 131072
FLASH_FILES = {
    "bootloader.bin": (0x0, "bootloader/bootloader.bin", "bootloader"),
    "partitions.bin": (0x8000, "partition_table/partition-table.bin", "partitions"),
    "ota_data_initial.bin": (0x1A000, "ota_data_initial.bin", "otadata"),
    "firmware.bin": (0x20000, "soft_decoder_v3.bin", "application"),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def image_info(data):
    require(len(data) >= 24 and data[0] == 0xE9, "Not an ESP image")
    require(1 <= data[1] <= 16, "Invalid ESP segment count")
    require(struct.unpack_from("<H", data, 12)[0] == 9, "Image is not ESP32-S3")
    require(data[2] == 2 and data[3] == 0x2F, "Expected DIO / 4MB / 80MHz image")
    require(data[23] in (0, 1), "Invalid appended-hash flag")
    pos, checksum, first = 24, 0xEF, b""
    for index in range(data[1]):
        require(pos + 8 <= len(data), "Truncated ESP segment header")
        _, length = struct.unpack_from("<II", data, pos)
        pos += 8
        require(length % 4 == 0 and pos + length <= len(data), "Invalid ESP segment size")
        segment = data[pos:pos + length]
        if index == 0:
            first = segment
        for value in segment:
            checksum ^= value
        pos += length
    footer = pos | 15
    require(footer < len(data), "Missing ESP checksum")
    require(data[footer] == checksum, "ESP checksum mismatch")
    end = footer + 1
    require(all(value == 0 for value in data[pos:footer]), "Invalid ESP checksum padding")
    if data[23]:
        require(end + 32 <= len(data), "Missing ESP SHA256")
        require(data[end:end + 32] == hashlib.sha256(data[:end]).digest(), "ESP SHA256 mismatch")
        end += 32
    require(end == len(data), "Unexpected data after ESP image (not a raw application)")
    info = {"role": "bootloader", "chip": "esp32s3", "size": len(data),
            "sha256": sha256(data), "image_hash_appended": bool(data[23])}
    if len(first) >= 144 and struct.unpack_from("<I", first)[0] == 0xABCD5432:
        def text(offset, size):
            return first[offset:offset + size].split(b"\0", 1)[0].decode("utf-8")
        info.update(role="application", version=text(16, 32), project=text(48, 32),
                    idf=text(112, 32))
        require(info["project"] == "soft_decoder_v3", "Unexpected application project")
        require(len(data) <= SLOT_SIZE, "Application exceeds OTA partition")
    return info


def partition_info(data):
    require(0 < len(data) <= 0x1000 and len(data) % 32 == 0, "Invalid partition binary size")
    entries, raw, digest_seen = [], bytearray(), False
    for pos in range(0, len(data), 32):
        block = data[pos:pos + 32]
        magic = struct.unpack_from("<H", block)[0]
        if magic == 0x50AA:
            require(not digest_seen, "Partition after MD5 footer")
            _, kind, subtype, offset, size, label, flags = struct.unpack("<HBBII16sI", block)
            name = label.split(b"\0", 1)[0].decode("utf-8")
            require(name and size and offset + size <= FLASH_SIZE, "Partition exceeds flash")
            require(offset % (0x10000 if kind == 0 else 0x1000) == 0, "Unaligned partition")
            entries.append(dict(name=name, type=kind, subtype=subtype, offset=offset, size=size, flags=flags))
            raw.extend(block)
        elif magic == 0xEBEB:
            require(not digest_seen and block[2:16] == b"\xff" * 14, "Invalid MD5 footer")
            require(block[16:] == hashlib.md5(raw).digest(), "Partition MD5 mismatch")
            digest_seen = True
        else:
            require(block == b"\xff" * 32 and data[pos:] == b"\xff" * (len(data) - pos),
                    "Invalid partition padding")
            break
    require(digest_seen and entries, "Partition table has no verified MD5")
    ordered = sorted(entries, key=lambda entry: entry["offset"])
    require(len({entry["name"] for entry in entries}) == len(entries), "Duplicate partition names")
    for previous, current in zip(ordered, ordered[1:]):
        require(previous["offset"] + previous["size"] <= current["offset"], "Overlapping partitions")
    by_name = {entry["name"]: entry for entry in entries}
    for name, offset, size in (("otadata", 0x1A000, 0x2000), ("ota_0", 0x20000, SLOT_SIZE),
                               ("ota_1", 0x200000, SLOT_SIZE)):
        require(name in by_name and by_name[name]["offset"] == offset and by_name[name]["size"] == size,
                f"{name} is incompatible with the flasher's fixed offsets")
    require(by_name["otadata"]["type"] == 1 and by_name["otadata"]["subtype"] == 0, "Not OTA data")
    require(by_name["ota_0"]["type"] == 0 and by_name["ota_0"]["subtype"] == 16, "Not ota_0")
    require(by_name["ota_1"]["type"] == 0 and by_name["ota_1"]["subtype"] == 17, "Not ota_1")
    return {"role": "partitions", "size": len(data), "sha256": sha256(data), "entries": entries}


def source_partitions(path):
    kinds = {"app": 0, "data": 1}
    subtypes = {"nvs": 2, "phy": 1, "ota": 0, "ota_0": 16, "ota_1": 17, "coredump": 3}
    def number(value):
        value = value.strip()
        if value[-1:].upper() in ("K", "M"):
            return int(value[:-1], 0) * (1024 if value[-1:].upper() == "K" else 1024 * 1024)
        return int(value, 0)
    result = []
    for row in csv.reader(line for line in path.read_text(encoding="utf-8-sig").splitlines()
                          if line.strip() and not line.lstrip().startswith("#")):
        name, kind, subtype, offset, size, *flags = (value.strip() for value in row)
        require(not flags or not flags[0], "Partition flags need explicit flasher support")
        result.append(dict(name=name, type=kinds[kind], subtype=subtypes[subtype],
                           offset=number(offset), size=number(size), flags=0))
    return result


def wav_info(data):
    require(12 <= len(data) <= 2 * 1024 * 1024, "Invalid packaged WAV size")
    require(data[:4] == b"RIFF" and data[8:12] == b"WAVE", "Not RIFF/WAVE")
    end = struct.unpack_from("<I", data, 4)[0] + 8
    require(end == len(data), "RIFF length mismatch")
    pos, fmt, samples = 12, None, None
    while pos < end:
        require(pos + 8 <= end, "Truncated WAV chunk")
        name, length = struct.unpack_from("<4sI", data, pos)
        pos += 8
        require(pos + length + (length & 1) <= end, "WAV chunk exceeds RIFF")
        if name == b"fmt ":
            require(fmt is None and length >= 16, "Invalid/duplicate WAV fmt")
            fmt = struct.unpack_from("<HHIIHH", data, pos)
        elif name == b"data":
            require(samples is None, "Duplicate WAV data")
            samples = length
        pos += length + (length & 1)
    require(fmt is not None and samples is not None and samples > 0, "Missing WAV fmt/data")
    kind, channels, rate, byte_rate, align, bits = fmt
    require(kind == 1 and channels in (1, 2) and 1 <= rate <= 192000 and bits == 16,
            "Unsupported WAV encoding")
    require(align == channels * 2 and byte_rate == rate * align and samples % align == 0,
            "Invalid PCM frame alignment")


def asp_info(data, legacy=False):
    """Validate an AURA Sound Pack (.asp) that is shipped with a release."""
    require(16 <= len(data) <= ASP_MAX_SIZE, "Invalid AURA sound pack size")
    if data.startswith(ASP_PW_MAGIC):
        # AURAPW01 (XOR + 32-bit tag) is import-only legacy crypto. A release
        # must not publish it silently; an explicit --legacy opt-in is required.
        require(legacy, "Legacy AURAPW01 sound pack rejected; pass --legacy to inspect it")
        require(len(data) >= 24, "Corrupted legacy AURAPW01 sound pack")
        return dict(role="sound_pack", format="AURAPW01", encrypted=True, legacy=True,
                    size=len(data), sha256=sha256(data))
    if data.startswith(ASP_PW2_MAGIC):
        require(len(data) >= 44 + 32 + 16, "Corrupted AURAPW02 sound pack")
        version, kdf, cipher_id, mac_id = data[8], data[9], data[10], data[11]
        require((version, kdf, cipher_id, mac_id) == (2, 2, 1, 1),
                "Unsupported AURAPW02 header")
        log_n, r, p = data[12], data[13], data[14]
        require(12 <= log_n <= 14 and r == 8 and 1 <= p <= 2,
                "Invalid AURAPW02 KDF parameters")
        require(128 * (1 << log_n) * r <= 32 * 1024 * 1024,
                "AURAPW02 KDF exceeds the decoder memory limit")
        return dict(role="sound_pack", format="AURAPW02", encrypted=True, legacy=False,
                    size=len(data), sha256=sha256(data))
    require(data[:8] == ASP_MAGIC, "Not an AURA sound pack")
    version, count, data_off = struct.unpack_from("<HHI", data, 8)
    require(version == 1, "Unsupported AURA sound pack version")
    require(1 <= count <= ASP_MAX_FILES, "Invalid AURA sound pack entry count")
    require(16 <= data_off <= len(data), "Invalid AURA sound pack data offset")
    pos, running, names = 16, data_off, set()
    for _ in range(count):
        require(pos + 16 <= data_off, "Truncated AURA sound pack index")
        name_len, volume, channels, bits, rate, offset, length = struct.unpack_from("<BBBBIII", data, pos)
        pos += 16
        require(1 <= name_len <= 63 and pos + name_len <= data_off,
                "Invalid AURA sound pack entry name")
        try:
            name = data[pos:pos + name_len].decode("utf-8")
        except UnicodeDecodeError:
            raise ValueError("Invalid UTF-8 AURA sound pack name")
        require("\0" not in name and "/" not in name and "\\" not in name and ".." not in name,
                "Unsafe AURA sound pack name")
        require(name not in names, "Duplicate AURA sound pack name")
        names.add(name)
        pos += name_len
        require(volume <= 100, "Invalid AURA sound pack volume")
        require(bits in (0, 4, 16), "Unsupported AURA sound pack encoding")
        if bits == 0:
            require(name == ASP_SCHEME_NAME and channels == 0 and rate == 0 and
                    0 < length <= ASP_SCHEME_MAX,
                    "Invalid AURA sound pack scheme entry")
        else:
            require(channels in (1, 2) and 1 <= rate <= 192000 and length > 0,
                    "Invalid AURA sound pack audio entry")
        require(offset == running and length <= len(data) - running,
                "Invalid AURA sound pack payload offsets")
        running += length
    require(pos == data_off, "AURA sound pack index length mismatch")
    require(running == len(data), "Trailing AURA sound pack bytes")
    return dict(role="sound_pack", format="AURASP01", encrypted=False, legacy=False,
                size=len(data), sha256=sha256(data), files=count)


def container_info(data):
    require(len(data) <= 8 * 1024 * 1024 and len(data) >= 16, "Invalid AURAOTA2 size")
    length, count = struct.unpack_from("<II", data, 8)
    require(16 <= length <= SLOT_SIZE and count <= 20 and 16 + length <= len(data), "Invalid OTA lengths")
    app = image_info(data[16:16 + length])
    require(app["role"] == "application", "OTA payload is not an application")
    pos, names, slots = 16 + length, set(), set()
    for _ in range(count):
        require(pos + 8 <= len(data), "Truncated OTA file header")
        name_len, label_len, file_len = struct.unpack_from("<HHI", data, pos)
        pos += 8
        require(1 <= name_len <= 63 and label_len <= 63 and 0 < file_len <= 2 * 1024 * 1024,
                "Packaged file exceeds receiver limits")
        require(pos + name_len + label_len + file_len <= len(data), "Truncated OTA file")
        name = data[pos:pos + name_len].decode("utf-8")
        label = data[pos + name_len:pos + name_len + label_len].decode("utf-8")
        require("\0" not in name + label and "/" not in name and "\\" not in name and ".." not in name,
                "Unsafe OTA filename")
        normalized = "".join(c if c.isascii() and (c.isalnum() or c in "._-") else "_" for c in name)
        require(normalized.endswith(".wav") and normalized not in names, "Duplicate/invalid OTA name")
        names.add(normalized)
        match = re.fullmatch(r"slot([0-9]+)\.wav", normalized)
        slot = int(match.group(1)) if match else next((i for i in range(1, 21) if i not in slots), 0)
        require(1 <= slot <= 20 and slot not in slots, "Duplicate/invalid OTA slot")
        slots.add(slot)
        start = pos + name_len + label_len
        wav_info(data[start:start + file_len])
        pos = start + file_len
    require(pos == len(data), "Trailing OTA container bytes")
    return dict(role="AURAOTA2", size=len(data), sha256=sha256(data), version=app["version"],
                application_sha256=app["sha256"], files=count)


def inspect(path, legacy=False):
    data = path.read_bytes()
    if data.startswith((ASP_MAGIC, ASP_PW_MAGIC, ASP_PW2_MAGIC)):
        return asp_info(data, legacy)
    if data.startswith(b"AURAOTA2"):
        return container_info(data)
    if data[:1] == b"\xe9":
        return image_info(data)
    if data[:2] == b"\xaa\x50":
        return partition_info(data)
    if path.name == "ota_data_initial.bin":
        require(data == b"\xff" * 0x2000, "Initial OTA data must be 8192 erased bytes")
        return dict(role="otadata", size=len(data), sha256=sha256(data))
    raise ValueError("Unknown BIN/ASP format")


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".publish-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def publish(args):
    build, release = args.build.resolve(), args.release.resolve()
    version = args.version.read_text(encoding="utf-8-sig").strip()
    require(re.fullmatch(r"[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:[-+][A-Za-z0-9.-]+)?", version), "Invalid version")
    plan = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    require(plan["extra_esptool_args"]["chip"] == "esp32s3", "Wrong build target")
    require(plan["flash_settings"] == dict(flash_mode="dio", flash_size="4MB", flash_freq="80m"),
            "Flash settings differ from web_flasher")
    expected = {offset: source for offset, source, _ in FLASH_FILES.values()}
    actual = {int(offset, 0): source.replace("\\", "/") for offset, source in plan["flash_files"].items()}
    require(actual == expected, "Generated offsets/files differ from web_flasher")
    payloads, manifest = {}, {"schema": 1, "version": version, "chip": "esp32s3",
                              "flash_size": FLASH_SIZE, "files": []}
    for name, (offset, source, role) in FLASH_FILES.items():
        info = inspect(build / source)
        require(info["role"] == role, f"Wrong role for {source}")
        require(info["size"] <= (0x8000 if role == "bootloader" else FLASH_SIZE), "Bootloader overlaps partitions")
        if role == "application":
            require(info["version"] == version, "Built app version is stale; rebuild before publishing")
            require(info["idf"].startswith("v6.0"), "Expected ESP-IDF 6.0")
        if role == "partitions":
            require(info["entries"] == source_partitions(args.partitions), "Binary/source partitions differ")
        payloads[release / "flash_download_tool" / name] = (build / source).read_bytes()
        manifest["files"].append(dict(path="flash_download_tool/" + name, offset=offset, **info))
    # One versioned raw application is published; the same file is used for
    # USB flashing and for the device's web OTA page.
    app = payloads[release / "flash_download_tool" / "firmware.bin"]
    name = f"ADDITIPUS_AURA-X_v{version}.bin"
    payloads[release / name] = app
    manifest["files"].append(dict(path=name, offset=None, **image_info(app)))
    # Sound packs are deliverables too: publish the hashes of every pack that is
    # already present, so later verification notices tampering or truncation.
    for path in sorted(release.glob("*.asp")):
        data = path.read_bytes()
        info = asp_info(data, legacy=getattr(args, "legacy", False))
        payloads[path] = data
        manifest["files"].append(dict(path=path.name, offset=None, **info))
    manifest_path = release / "manifest.json"
    # Invalidate the previous publication marker before updating any fixed-name file.
    if manifest_path.exists():
        manifest_path.unlink()
    for path, data in payloads.items():
        atomic_write(path, data)
        require(path.read_bytes() == data, f"Publication copy mismatch: {path}")
    # Older releases also published `*_OTA.bin` duplicates; the versioned BIN is
    # now the only application deliverable, so drop such stale copies.
    for duplicate in release.glob("ADDITIPUS_AURA-X_v*_OTA.bin"):
        if duplicate.is_file():
            print(f"Removed stale duplicate: {duplicate.name}")
            duplicate.unlink()
    atomic_write(manifest_path, (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    checksums = {path.relative_to(release).as_posix() for path in payloads}
    checksums.add("manifest.json")
    write_checksums(release, checksums)
    verify(args)


def write_checksums(release, relative_paths):
    lines = []
    for relative in sorted(relative_paths):
        path = release / relative
        lines.append(f"{sha256(path.read_bytes())}  {relative}")
    atomic_write(release / "SHA256SUMS.txt", ("\n".join(lines) + "\n").encode("utf-8"))


def verify_checksums(release, expected):
    path = release / "SHA256SUMS.txt"
    if not path.is_file():
        return []
    errors, covered = [], set()
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split(None, 1)
            require(len(parts) == 2, "Malformed SHA256SUMS.txt line")
            digest, name = parts[0].lower(), parts[1].strip().lstrip("*")
            require(re.fullmatch(r"[0-9a-f]{64}", digest), "Malformed SHA256SUMS.txt digest")
            target = (release / name).resolve()
            try:
                target.relative_to(release)
            except ValueError:
                raise ValueError(f"Checksum path escapes the release directory: {name}")
            require(target.is_file(), f"Checksum file is missing: {name}")
            require(sha256(target.read_bytes()) == digest, f"Checksum mismatch: {name}")
            covered.add(name)
        require(covered, "SHA256SUMS.txt is empty")
        missing = set(expected) - covered
        require(not missing, "SHA256SUMS.txt does not cover: " + ", ".join(sorted(missing)))
        print(f"OK SHA256SUMS.txt: {len(covered)} files")
    except (ValueError, OSError, UnicodeError) as error:
        errors.append(f"SHA256SUMS.txt: {error}")
    return errors


def verify(args):
    release = args.release.resolve()
    legacy = getattr(args, "legacy", False)
    manifest = json.loads((release / "manifest.json").read_text(encoding="utf-8"))
    require(manifest["schema"] == 1, "Unsupported release manifest")
    require(manifest["chip"] == "esp32s3" and manifest["flash_size"] == FLASH_SIZE, "Wrong release target")
    errors, seen = [], set()
    expected = {record["path"]: record for record in manifest["files"]}
    require(len(expected) == len(manifest["files"]), "Duplicate manifest paths")
    for name, (offset, _, role) in FLASH_FILES.items():
        record = expected.get("flash_download_tool/" + name)
        require(record is not None and record["offset"] == offset and record["role"] == role,
                f"Manifest flash plan mismatch: {name}")
    app_hash = expected["flash_download_tool/firmware.bin"]["sha256"]

    def parse(path):
        return inspect(path, legacy) if path.suffix.lower() in (".bin", ".asp") else None

    def describe(info):
        text = f"{info['role']}, {info['size']} bytes"
        if "version" in info:
            text += f", version {info['version']}"
        if info.get("format"):
            text += f", {info['format']}"
        return text

    # Every manifest entry is a deliverable: verify its bytes and, for BIN/ASP
    # files, its internal structure as well.
    for relative in sorted(expected):
        record = expected[relative]
        path = release / relative
        try:
            require(path.is_file(), "missing release file")
            data = path.read_bytes()
            require(len(data) == record["size"], "manifest size mismatch")
            require(sha256(data) == record["sha256"], "manifest hash mismatch")
            info = parse(path)
            if info is not None:
                require(info["role"] == record["role"], "manifest role mismatch")
                if info.get("legacy"):
                    print(f"WARNING: {relative} uses legacy AURAPW01 encryption", file=sys.stderr)
                if info["role"] in ("application", "AURAOTA2"):
                    require(info["version"] == manifest["version"], "manifest/application version mismatch")
                if info["role"] == "AURAOTA2":
                    require(info["application_sha256"] == app_hash,
                            "combined OTA does not contain the published application")
            seen.add(relative)
            print(f"OK {relative}: " + (describe(info) if info else record["role"]))
        except (ValueError, UnicodeError, struct.error) as error:
            errors.append(f"{relative}: {error}")

    # Optional binaries/packs in the release directory are validated too so an
    # unlisted but stale artifact cannot be selected for flashing.
    candidates = sorted(release.rglob("*.bin")) + sorted(release.rglob("*.asp"))
    for path in candidates:
        relative = path.relative_to(release).as_posix()
        if relative in expected:
            continue
        try:
            info = inspect(path, legacy)
            if info["role"] in ("application", "AURAOTA2"):
                match = re.search(r"_v(.+?)(?:_with_sounds)?\.bin$", path.name)
                require(match is not None and match[1] == info["version"],
                        "Historical filename/version mismatch")
                if info["version"] == manifest["version"]:
                    embedded = info.get("application_sha256", info["sha256"])
                    require(embedded == app_hash,
                            "Same-version optional BIN contains a stale application")
                status = "OPTIONAL" if info["version"] == manifest["version"] else "HISTORICAL"
            elif info["role"] == "sound_pack":
                status = "OPTIONAL"
                if info.get("legacy"):
                    print(f"WARNING: {relative} uses legacy AURAPW01 encryption", file=sys.stderr)
            else:
                raise ValueError("Unlisted binary cannot be selected for flashing")
            seen.add(relative)
            print(f"{status} {relative}: {describe(info)}")
        except (ValueError, UnicodeError, struct.error) as error:
            errors.append(f"{relative}: {error}")

    errors.extend(f"Missing release file: {name}" for name in expected.keys() - seen)
    errors.extend(verify_checksums(release, expected))
    require(not errors, "\n".join(errors))
    print(f"Verified all {len(seen)} release BIN/ASP files; current version {manifest['version']}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("publish", "verify", "inspect"))
    parser.add_argument("--release", type=Path)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--version", type=Path)
    parser.add_argument("--partitions", type=Path)
    parser.add_argument("--legacy", action="store_true",
                        help="allow legacy AURAPW01 sound packs (import-only, insecure)")
    args = parser.parse_args()
    try:
        if args.command == "inspect":
            require(args.input is not None, "Inspect needs --input")
            print(json.dumps(inspect(args.input, args.legacy), ensure_ascii=False))
        elif args.command == "publish":
            require(args.build is not None and args.version is not None and args.partitions is not None,
                    "Publish needs --build, --version and --partitions")
            require(args.release is not None, "Publish needs --release")
            publish(args)
        else:
            require(args.release is not None, "Verify needs --release")
            verify(args)
    except (ValueError, OSError, KeyError, UnicodeError, struct.error) as error:
        print(f"RELEASE ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
