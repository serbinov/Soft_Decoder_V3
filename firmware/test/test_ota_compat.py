"""Tests for the OTA/platform compatibility guard (tools/check_ota_compat.py)."""

import contextlib
import copy
import importlib.util
import io
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECKER_PATH = ROOT / "tools" / "check_ota_compat.py"

_spec = importlib.util.spec_from_file_location("check_ota_compat", CHECKER_PATH)
checker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(checker)


def _read(path):
    return path.read_text(encoding="utf-8-sig")


def _load_repo():
    golden = json.loads(_read(ROOT / "tools" / "ota_compat.json"))
    partitions = checker.parse_partitions(_read(ROOT / "partitions.csv"))
    sdkconfig_path = ROOT / "sdkconfig"
    if not sdkconfig_path.is_file():
        sdkconfig_path = ROOT / "sdkconfig.defaults"
    sdkconfig = checker.parse_sdkconfig(_read(sdkconfig_path))
    header = checker.parse_platform_header(_read(ROOT / "main" / "platform_compat.h"))
    return golden, partitions, sdkconfig, header


class OtaCompatTest(unittest.TestCase):
    def test_repo_matches_golden_contract(self):
        golden, partitions, sdkconfig, header = _load_repo()
        self.assertEqual(checker.compare(golden, partitions, sdkconfig, header), [])

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            exit_code = checker.main(["--root", str(ROOT)])
        self.assertEqual(exit_code, 0)
        self.assertIn("OK: OTA layout v1 matches the golden contract.", output.getvalue())

    def test_partition_offset_drift_requires_usb(self):
        golden, partitions, sdkconfig, header = _load_repo()
        broken = copy.deepcopy(golden)
        ota_0 = next(p for p in broken["partitions"] if p["name"] == "ota_0")
        ota_0["offset"] = "0x21000"
        errors = checker.compare(broken, partitions, sdkconfig, header)
        self.assertTrue(
            any(e.startswith("USB-REQUIRED:") and "ota_0 offset" in e for e in errors),
            errors,
        )

    def test_secure_boot_drift_requires_usb(self):
        golden, partitions, sdkconfig, header = _load_repo()
        broken = copy.deepcopy(golden)
        broken["secure_boot"] = True
        errors = checker.compare(broken, partitions, sdkconfig, header)
        self.assertTrue(
            any(e.startswith("USB-REQUIRED:") and "secure boot" in e for e in errors),
            errors,
        )

    def test_header_constants_match_partition_table(self):
        golden, partitions, sdkconfig, header = _load_repo()
        self.assertEqual(checker.compare(golden, partitions, sdkconfig, header), [])

        expected = {}
        for partition in partitions:
            expected[partition["name"]] = partition
        self.assertEqual(
            header["PLATFORM_OTADATA_OFFSET"], expected["otadata"]["offset"]
        )
        self.assertEqual(header["PLATFORM_OTADATA_SIZE"], expected["otadata"]["size"])
        self.assertEqual(
            header["PLATFORM_OTA_0_OFFSET"], expected["ota_0"]["offset"]
        )
        self.assertEqual(header["PLATFORM_OTA_SLOT_SIZE"], expected["ota_0"]["size"])
        self.assertEqual(
            header["PLATFORM_OTA_1_OFFSET"], expected["ota_1"]["offset"]
        )
        self.assertEqual(
            header["PLATFORM_PART_TABLE_OFFSET"],
            int(golden["partition_table_offset"], 0),
        )


if __name__ == "__main__":
    unittest.main()
