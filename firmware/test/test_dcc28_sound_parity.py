"""DCC 28-step -> motor speed -> sound-graph band parity.

The sound editor simulator works in 0..255 units (0..100 % scaled to 255).
The decoder converts a 28-step DCC packet to the 0..126 motor scale and the
sound engine scales that to 0..255. This test checks that every one of the 28
running notches lands in the same default-scheme engine band as the simulator
would choose for the same percentage.
"""

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEME = ROOT / "main" / "data" / "default_scheme.json"


def dcc28_to_motor_speed(step: int) -> int:
    if step <= 0:
        return 0
    if step >= 28:
        return 126
    return (step * 126 + 14) // 28


def motor_to_sound_speed(speed128: int) -> int:
    speed128 = max(0, min(126, speed128))
    return (speed128 * 255 + 63) // 126


def simulator_speed(step: int) -> int:
    """Math.round(step / 28 * 255), as the editor's pctSpeed() does."""
    return (step * 255 + 14) // 28


def load_engine_bands():
    data = json.loads(SCHEME.read_text(encoding="utf-8"))
    bands = []
    for transition in data["transitions"]:
        condition = transition.get("condition") or {}
        if condition.get("type") != "speed":
            continue
        if transition.get("channel", 0) != 0:
            continue
        if transition.get("source") != "eng_off":
            continue
        target = transition.get("target", "")
        if not target.startswith("eng") or not target.endswith("_start_play"):
            continue
        lo, hi = int(condition["min"]), int(condition["max"])
        if lo > 0 and hi >= lo:
            bands.append((lo, hi, target))
    bands.sort()
    return bands


BANDS = load_engine_bands()


def band_for(speed: int):
    if speed <= 0:
        return "idle"
    for lo, hi, target in BANDS:
        if lo <= speed <= hi:
            return target
    raise AssertionError(f"speed {speed} is outside every engine band")


class Dcc28SoundParityTests(unittest.TestCase):
    def test_default_scheme_has_seven_contiguous_bands(self):
        self.assertEqual(7, len(BANDS), BANDS)
        self.assertEqual(1, BANDS[0][0])
        self.assertEqual(255, BANDS[-1][1])
        for (_, prev_hi, _), (lo, _, _) in zip(BANDS, BANDS[1:]):
            self.assertEqual(prev_hi + 1, lo, BANDS)

    def test_all_28_notches_match_simulator_band(self):
        for step in range(1, 29):
            motor = dcc28_to_motor_speed(step)
            firmware = motor_to_sound_speed(motor)
            simulated = simulator_speed(step)
            self.assertEqual(
                band_for(simulated),
                band_for(firmware),
                f"step {step}: firmware {firmware}, simulator {simulated}, bands {BANDS}",
            )

    def test_notches_are_monotonic_and_cover_idle_to_full(self):
        self.assertEqual("idle", band_for(dcc28_to_motor_speed(0)))
        previous = -1
        for step in range(1, 29):
            sound = motor_to_sound_speed(dcc28_to_motor_speed(step))
            self.assertGreater(sound, 0)
            self.assertGreaterEqual(sound, previous)
            previous = sound
        self.assertEqual(255, motor_to_sound_speed(dcc28_to_motor_speed(28)))

    def test_14_step_matches_simulator_bands(self):
        for step in range(1, 15):
            motor = (step * 126 + 7) // 14
            firmware = motor_to_sound_speed(motor)
            simulated = (step * 255 + 7) // 14
            self.assertEqual(
                band_for(simulated),
                band_for(firmware),
                f"14-step {step}: firmware {firmware}, simulator {simulated}",
            )


if __name__ == "__main__":
    unittest.main()
