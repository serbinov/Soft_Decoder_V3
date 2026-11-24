"""OTA/platform compatibility guard for the frozen layout contract (v1).

Compares firmware/partitions.csv, firmware/sdkconfig(.defaults),
firmware/main/platform_compat.h, and optional build images against
firmware/tools/ota_compat.json. Stdlib only; never writes files.

Any drift means an in-place OTA update is unsafe and a full USB flash is
required, so every mismatch is reported with the USB-REQUIRED: prefix.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve()
DEFAULT_ROOT = SCRIPT.parents[1]

TYPE_MAP = {"app": 0, "data": 1}
SUBTYPE_MAP = {
    "nvs": 2,
    "phy": 1,
    "ota": 0,
    "ota_0": 16,
    "ota_1": 17,
    "coredump": 3,
}

REQUIRED_MACROS = (
    "PLATFORM_LAYOUT_VERSION",
    "PLATFORM_BOOTLOADER_MAX",
    "PLATFORM_PART_TABLE_OFFSET",
    "PLATFORM_OTADATA_OFFSET",
    "PLATFORM_OTADATA_SIZE",
    "PLATFORM_OTA_0_OFFSET",
    "PLATFORM_OTA_1_OFFSET",
    "PLATFORM_OTA_SLOT_SIZE",
)

FLASH_MODE_FLAGS = ("DIO", "QIO", "DOUT", "QOUT")
FLASH_FREQ_FLAGS = ("20M", "26M", "40M", "80M", "120M")
FLASH_SIZE_FLAGS = ("1MB", "2MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB")


def _to_int(value):
    """Parse an int from JSON/CSV/header text (accepts 0x hex and u/L suffixes)."""
    if isinstance(value, int):
        return value
    text = str(value).strip().strip('"')
    text = re.sub(r"[uUlL]+$", "", text)
    return int(text, 0)


def _unquote(value):
    return str(value).strip().strip('"')


def parse_partitions(text):
    """Parse partitions.csv into [{name,type,subtype,offset,size}]."""
    partitions = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 5:
            continue
        name = fields[0]
        if name.lower() == "name":
            continue
        type_text = fields[1].lower()
        subtype_text = fields[2].lower()
        if type_text in TYPE_MAP:
            part_type = TYPE_MAP[type_text]
        else:
            part_type = _to_int(type_text)
        if subtype_text in SUBTYPE_MAP:
            subtype = SUBTYPE_MAP[subtype_text]
        else:
            subtype = _to_int(subtype_text)
        partitions.append(
            {
                "name": name,
                "type": part_type,
                "subtype": subtype,
                "offset": _to_int(fields[3]),
                "size": _to_int(fields[4]),
            }
        )
    return partitions


def parse_sdkconfig(text):
    """Extract the OTA-relevant ESP-IDF configuration.

    Missing ESP-IDF defaults are modelled explicitly so a clean checkout (only
    sdkconfig.defaults exists) compares the same as a generated sdkconfig.
    """
    values = {}
    for raw in text.splitlines():
        line = raw.strip()
        not_set = re.match(r"^#\s*(CONFIG_[A-Za-z0-9_]+)\s+is not set$", line)
        if not_set:
            values[not_set.group(1)] = "n"
            continue
        assigned = re.match(r"^(CONFIG_[A-Za-z0-9_]+)\s*=\s*(.*)$", line)
        if assigned:
            values[assigned.group(1)] = assigned.group(2).strip()

    def flag(name):
        return values.get(name) == "y"

    def suffix(prefix, choices):
        for choice in choices:
            if flag(prefix + choice):
                return choice.lower()
        return None

    flash_size = values.get("CONFIG_ESPTOOLPY_FLASHSIZE")
    if flash_size is not None:
        flash_size = _unquote(flash_size)
    else:
        flash_size = suffix("CONFIG_ESPTOOLPY_FLASHSIZE_", FLASH_SIZE_FLAGS)
        if flash_size is not None:
            flash_size = flash_size.upper()

    flash_mode = values.get("CONFIG_ESPTOOLPY_FLASHMODE")
    if flash_mode is not None:
        flash_mode = _unquote(flash_mode).lower()
    else:
        flash_mode = suffix("CONFIG_ESPTOOLPY_FLASHMODE_", FLASH_MODE_FLAGS) or "dio"

    flash_freq = values.get("CONFIG_ESPTOOLPY_FLASHFREQ")
    if flash_freq is not None:
        flash_freq = _unquote(flash_freq).lower()
    else:
        flash_freq = suffix("CONFIG_ESPTOOLPY_FLASHFREQ_", FLASH_FREQ_FLAGS) or "80m"

    if flag("CONFIG_SPIRAM_MODE_QUAD"):
        spiram_mode = "quad"
    elif flag("CONFIG_SPIRAM_MODE_OCT"):
        spiram_mode = "oct"
    else:
        spiram_mode = None

    bootloader_offset = values.get("CONFIG_BOOTLOADER_OFFSET_IN_FLASH")
    bootloader_offset = (
        _to_int(bootloader_offset) if bootloader_offset is not None else 0x0
    )

    part_table_offset = values.get("CONFIG_PARTITION_TABLE_OFFSET")
    part_table_offset = (
        _to_int(part_table_offset) if part_table_offset is not None else None
    )

    return {
        "flash_size": flash_size,
        "flash_mode": flash_mode,
        "flash_freq": flash_freq,
        "spiram_mode": spiram_mode,
        "secure_boot": flag("CONFIG_SECURE_BOOT"),
        "flash_encryption": flag("CONFIG_SECURE_FLASH_ENC_ENABLED"),
        "rollback_enable": flag("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE"),
        "bootloader_offset": bootloader_offset,
        "partition_table_offset": part_table_offset,
    }


def parse_platform_header(text):
    """Parse the PLATFORM_* constants from platform_compat.h."""
    header = {}
    for raw in text.splitlines():
        match = re.match(r"^\s*#define\s+(PLATFORM_[A-Za-z0-9_]+)\s+(\S+)", raw)
        if match:
            header[match.group(1)] = _to_int(match.group(2))
    return header


def _partition(partitions, name):
    for entry in partitions:
        if entry["name"] == name:
            return entry
    return None


def _compare(errors, label, expected, actual):
    if expected != actual:
        errors.append(
            f"USB-REQUIRED: {label}: expected {expected!r}, got {actual!r}"
        )


def _compare_header(errors, header, macro, expected):
    actual = header.get(macro)
    if actual != expected:
        actual_text = f"{actual:#x}" if isinstance(actual, int) else repr(actual)
        errors.append(
            f"USB-REQUIRED: {macro}: expected {expected!r}, got {actual_text}"
        )


def compare(golden, actual_partitions, sdkconfig, header):
    """Return a list of USB-REQUIRED errors (empty when everything matches)."""
    errors = []

    _compare(errors, "schema", 1, golden.get("schema"))
    layout_version = golden.get("layout_version")
    _compare(errors, "layout_version", 1, layout_version)

    flash = golden.get("flash", {})
    _compare(errors, "flash size", flash.get("size"), sdkconfig.get("flash_size"))
    _compare(errors, "flash mode", flash.get("mode"), sdkconfig.get("flash_mode"))
    _compare(errors, "flash freq", flash.get("freq"), sdkconfig.get("flash_freq"))
    _compare(errors, "spiram mode", golden.get("spiram_mode"), sdkconfig.get("spiram_mode"))
    _compare(errors, "secure boot", golden.get("secure_boot"), sdkconfig.get("secure_boot"))
    _compare(
        errors,
        "flash encryption",
        golden.get("flash_encryption"),
        sdkconfig.get("flash_encryption"),
    )
    _compare(
        errors,
        "rollback enable",
        golden.get("rollback_enable"),
        sdkconfig.get("rollback_enable"),
    )
    _compare(
        errors,
        "bootloader offset",
        _to_int(golden.get("bootloader_offset", "0x0")),
        sdkconfig.get("bootloader_offset"),
    )
    _compare(
        errors,
        "partition table offset",
        _to_int(golden.get("partition_table_offset", "0x0")),
        sdkconfig.get("partition_table_offset"),
    )

    golden_partitions = golden.get("partitions", [])
    if len(golden_partitions) != len(actual_partitions):
        errors.append(
            "USB-REQUIRED: partition count: expected "
            f"{len(golden_partitions)}, got {len(actual_partitions)}"
        )
    for expected in golden_partitions:
        name = expected["name"]
        actual = _partition(actual_partitions, name)
        if actual is None:
            errors.append(f"USB-REQUIRED: partition {name}: missing from partitions.csv")
            continue
        _compare(errors, f"partition {name} type", expected["type"], actual["type"])
        _compare(errors, f"partition {name} subtype", expected["subtype"], actual["subtype"])
        _compare(
            errors,
            f"partition {name} offset",
            _to_int(expected["offset"]),
            actual["offset"],
        )
        _compare(
            errors,
            f"partition {name} size",
            _to_int(expected["size"]),
            actual["size"],
        )

    for macro in REQUIRED_MACROS:
        if macro not in header:
            errors.append(f"USB-REQUIRED: {macro}: missing from platform_compat.h")

    _compare_header(errors, header, "PLATFORM_LAYOUT_VERSION", layout_version)
    _compare_header(
        errors,
        header,
        "PLATFORM_BOOTLOADER_MAX",
        _to_int(golden.get("bootloader_max", "0x0")),
    )
    _compare_header(
        errors,
        header,
        "PLATFORM_PART_TABLE_OFFSET",
        _to_int(golden.get("partition_table_offset", "0x0")),
    )

    otadata = _partition(actual_partitions, "otadata")
    if otadata is not None:
        _compare_header(errors, header, "PLATFORM_OTADATA_OFFSET", otadata["offset"])
        _compare_header(errors, header, "PLATFORM_OTADATA_SIZE", otadata["size"])
    ota_0 = _partition(actual_partitions, "ota_0")
    if ota_0 is not None:
        _compare_header(errors, header, "PLATFORM_OTA_0_OFFSET", ota_0["offset"])
        _compare_header(errors, header, "PLATFORM_OTA_SLOT_SIZE", ota_0["size"])
    ota_1 = _partition(actual_partitions, "ota_1")
    if ota_1 is not None:
        _compare_header(errors, header, "PLATFORM_OTA_1_OFFSET", ota_1["offset"])

    return errors


def _check_images(errors, root, golden):
    build = root / "build"
    bootloader_bin = build / "bootloader" / "bootloader.bin"
    bootloader_max = _to_int(golden.get("bootloader_max", "0x0"))
    if bootloader_bin.is_file():
        size = bootloader_bin.stat().st_size
        if size > bootloader_max:
            errors.append(
                f"USB-REQUIRED: bootloader size {size} exceeds {bootloader_max}"
            )

    app_bin = build / "soft_decoder_v3.bin"
    ota_0 = None
    for entry in golden.get("partitions", []):
        if entry["name"] == "ota_0":
            ota_0 = entry
            break
    if app_bin.is_file() and ota_0 is not None:
        slot_size = _to_int(ota_0["size"])
        size = app_bin.stat().st_size
        if size > slot_size:
            errors.append(
                f"USB-REQUIRED: application size {size} exceeds ota_0 slot "
                f"{slot_size}"
            )
        else:
            free_percent = (slot_size - size) * 100.0 / slot_size
            minimum = golden.get("app_min_free_percent", 0)
            if free_percent < minimum:
                errors.append(
                    "USB-REQUIRED: ota_0 free space "
                    f"{free_percent:.1f}% is below the required {minimum}%"
                )


def _read(path):
    return path.read_text(encoding="utf-8-sig")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=str(DEFAULT_ROOT),
        help="firmware root (default: parent of the tools directory)",
    )
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()

    errors = []
    try:
        golden = json.loads(_read(root / "tools" / "ota_compat.json"))
    except (OSError, ValueError) as exc:
        print(f"USB-REQUIRED: cannot load golden contract: {exc}")
        return 2

    try:
        actual_partitions = parse_partitions(_read(root / "partitions.csv"))
    except OSError as exc:
        print(f"USB-REQUIRED: cannot read partitions.csv: {exc}")
        return 2

    sdkconfig_path = root / "sdkconfig"
    if not sdkconfig_path.is_file():
        sdkconfig_path = root / "sdkconfig.defaults"
    try:
        sdkconfig = parse_sdkconfig(_read(sdkconfig_path))
    except OSError as exc:
        print(f"USB-REQUIRED: cannot read {sdkconfig_path.name}: {exc}")
        return 2

    try:
        header = parse_platform_header(_read(root / "main" / "platform_compat.h"))
    except OSError as exc:
        print(f"USB-REQUIRED: cannot read platform_compat.h: {exc}")
        return 2

    errors.extend(compare(golden, actual_partitions, sdkconfig, header))
    _check_images(errors, root, golden)

    if errors:
        for error in errors:
            print(error)
        return 2

    print("OK: OTA layout v1 matches the golden contract.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
