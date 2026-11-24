"""Guards against drift between pinmap.h and the 2nd-stage boot-safe mask.

The bootloader hook drives a fixed set of actuator/load pins low right after
ROM, before app_main runs. If a pin is added, removed or remapped in
pinmap.h, the mask in hooks.c must change with it, otherwise a load can float
or a reserved pin (flash/PSRAM, USB, UART0, external NOR, ...) can be driven.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINMAP = ROOT / "components/pinmap/include/pinmap.h"
HOOKS = ROOT / "bootloader_components/board_boot_safe/hooks.c"

ACTUATOR_PINS = (
    "PIN_MOTOR_IN1", "PIN_MOTOR_IN2", "PIN_ACK_LOAD",
    "PIN_AUDIO_BCLK", "PIN_AUDIO_WS", "PIN_AUDIO_DOUT", "PIN_AUDIO_SD_MODE",
    "PIN_AUX_F0F", "PIN_AUX_F0R", "PIN_AUX1", "PIN_AUX2", "PIN_AUX3",
    "PIN_AUX4", "PIN_AUX5", "PIN_AUX6", "PIN_AUX7",
)

RESERVED = {0, 4, 5, 6, 7, 9, 13, 14, 15, 16, 17, 18, 19, 20, 43, 44, 45} | set(range(26, 33))


def parse_pinmap():
    text = PINMAP.read_text(encoding="utf-8")
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"^\s*#define\s+(PIN_[A-Z0-9_]+)\s+(\d+)\s*$", text, re.MULTILINE)}


def parse_hook_mask():
    text = HOOKS.read_text(encoding="utf-8")
    return text, {int(n) for n in re.findall(r"1ULL\s*<<\s*(\d+)", text)}


class BootSafePinsTest(unittest.TestCase):
    def test_mask_matches_actuator_pins(self):
        pins = parse_pinmap()
        missing_macros = [name for name in ACTUATOR_PINS if name not in pins]
        self.assertEqual(missing_macros, [], "pinmap.h is missing actuator macros")

        text, hook_set = parse_hook_mask()
        self.assertIn("BOARD_BOOT_SAFE_MASK", text)
        self.assertTrue(hook_set, "no 1ULL << n entries found in hooks.c")

        expected = {pins[name] for name in ACTUATOR_PINS}
        self.assertEqual(
            hook_set, expected,
            "boot-safe mask drift: hooks=%s expected=%s" % (sorted(hook_set), sorted(expected)),
        )

    def test_mask_never_drives_reserved_pins(self):
        _, hook_set = parse_hook_mask()
        overlap = hook_set & RESERVED
        self.assertEqual(
            overlap, set(),
            "boot-safe mask drives reserved pins: hooks=%s reserved_overlap=%s"
            % (sorted(hook_set), sorted(overlap)),
        )


if __name__ == "__main__":
    unittest.main()
