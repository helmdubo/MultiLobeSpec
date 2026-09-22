"""Numerical contract for FogMS world-space phase animation; no Unreal dependency.

These tests validate translation, wrapped coordinates and temporal state semantics.
They do not replace the native C++ build or a rendered animation/history test.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
from decimal import Decimal, localcontext
import json
import math
from pathlib import Path
import struct
import unittest


def f32(value: float) -> float:
    return struct.unpack("f", struct.pack("f", value))[0]


def fract(value: float) -> float:
    return value - math.floor(value)


def phase(center: float, velocity: float, seconds: float, frequency: float) -> float:
    center_cycles = center * frequency
    travel_cycles = velocity * seconds * frequency
    value = f32(fract(fract(center_cycles) - fract(travel_cycles)))
    return 0.0 if value == 1.0 else value


def uv(point: float, center: float, velocity: float, seconds: float, frequency: float) -> float:
    return fract((point - center) * frequency + phase(center, velocity, seconds, frequency))


def wrap_error(a: float, b: float) -> float:
    return abs((a - b + 0.5) % 1.0 - 0.5)


@dataclass(frozen=True)
class State:
    time: float = 0.0
    manual: bool = False
    manual_time: float = 0.0
    offset: float = 0.0
    velocities: tuple[float, ...] = (20.0, 3.0, -7.0)
    center: tuple[float, ...] = (1000.0, 2000.0, -3000.0)
    extent: tuple[float, ...] = (1000.0, 1000.0, 1000.0)
    animated: bool = True

    def history_key(self) -> tuple:
        return (
            self.animated, self.manual,
            self.manual_time if self.manual else None,
            self.offset, self.velocities, self.center, self.extent,
        )


def resets(before: State, after: State) -> bool:
    return before.history_key() != after.history_key() or after.time < before.time


class DensityAnimationContract(unittest.TestCase):
    def test_positive_wind_moves_pattern_forward(self) -> None:
        # Same feature at P at t=0 appears at P+v*t later, for all three octaves.
        f0 = f32(1.0 / 8000.0)
        for frequency, velocity in ((f0, 80.0), (f32(f0 * 4.0), 93.0), (f32(f0 * 8.0), 88.0)):
            for seconds in (-10000.0, -1.0, 0.0, 1.0, 10000.0):
                initial = uv(1234.0, -1798.0, velocity, 0.0, frequency)
                moved = uv(1234.0 + velocity * seconds, -1798.0, velocity, seconds, frequency)
                self.assertLess(wrap_error(initial, moved), 1.0e-6)

    def test_box_translation_does_not_carry_world_pattern(self) -> None:
        frequency = f32(1.0 / 2000.0)
        for center in (-1.0e10, -9000.0, 0.0, 3.0e10):
            actual = uv(1750.0, center, 25.0, 42.0, frequency)
            expected = fract((1750.0 - 25.0 * 42.0) * frequency)
            self.assertLess(wrap_error(actual, expected), 1.0e-6)

    def test_world_scale_and_rotation_are_not_density_coordinates(self) -> None:
        # Neither extents nor local axes occurs in the world-coordinate sampler.
        states = [replace(State(), extent=e) for e in ((1, 1, 1), (1000, 20, 200), (9, 4, 100000))]
        samples = [uv(1450.0, s.center[0], s.velocities[0], 8.0, f32(1 / 8000)) for s in states]
        self.assertEqual(samples, [samples[0]] * len(samples))

    def test_lwc_negative_time_against_decimal_reference(self) -> None:
        frequency = f32(1.0 / 8000.0)
        with localcontext() as context:
            context.prec = 70
            for center in (-1.0e12 + 137.0, 1.0e12 + 239.0):
                for seconds in (-1.0e7 - 0.125, 0.0, 1.0e7 + 0.25):
                    exact = (Decimal.from_float(center)
                             - Decimal.from_float(83.75) * Decimal.from_float(seconds)) * Decimal.from_float(frequency)
                    exact_fraction = exact % Decimal(1)
                    if exact_fraction < 0:
                        exact_fraction += 1
                    self.assertLess(wrap_error(phase(center, 83.75, seconds, frequency), float(exact_fraction)), 5.0e-8)

    def test_zero_time_matches_static_mapping_bitwise(self) -> None:
        for center in (-1.0e12, -17.0, 0.0, 17.0, 1.0e12):
            frequency = f32(1.0 / 8000.0)
            static = f32(fract(center * frequency))
            if static == 1.0:
                static = 0.0
            self.assertEqual(phase(center, 37.0, 0.0, frequency), static)

    def test_modulo_wrap_is_continuous(self) -> None:
        frequency = f32(1.0 / 8000.0)
        velocity = 80.0
        crossing = 1.0 / (frequency * velocity)
        before = phase(0, velocity, crossing - 1.0e-4, frequency)
        after = phase(0, velocity, crossing + 1.0e-4, frequency)
        self.assertLess(wrap_error(before, after), 3.0e-6)

    def test_continuous_and_frozen_time_do_not_reset_history(self) -> None:
        previous = State(time=3.0)
        for seconds in (3.0, 3.1, 60.0, 86400.0):
            current = replace(previous, time=seconds)
            self.assertFalse(resets(previous, current))
            previous = current
        frozen = State(time=100, manual=True, manual_time=100)
        self.assertFalse(resets(frozen, frozen))

    def test_authored_edits_and_seeks_reset_history(self) -> None:
        before = State(time=40)
        changes = (
            replace(before, time=39), replace(before, offset=1),
            replace(before, velocities=(1, 2, 3)), replace(before, manual=True),
            replace(before, animated=False), replace(before, center=(1, 2, 3)),
            replace(before, extent=(1, 2, 3)),
        )
        for after in changes:
            self.assertTrue(resets(before, after))
        manual = replace(before, manual=True, manual_time=40)
        self.assertTrue(resets(manual, replace(manual, manual_time=41, time=41)))

    def test_freeze_resume_keeps_phase(self) -> None:
        time_offset = -17.0
        frozen_at_world_time = 215.0
        frozen_effective_time = frozen_at_world_time + time_offset
        manual_time = frozen_effective_time - time_offset
        resumed_at_world_time = 835.0
        resumed_offset = manual_time + time_offset - resumed_at_world_time
        self.assertEqual(resumed_at_world_time + resumed_offset, frozen_effective_time)

    def test_same_frame_time_snapshot_is_world_local(self) -> None:
        snapshots: dict[str, tuple[int, float]] = {}

        def get(world: str, frame: int, live_time: float) -> float:
            if snapshots.get(world, (-1, 0))[0] != frame:
                snapshots[world] = (frame, live_time)
            return snapshots[world][1]

        self.assertEqual(get("Editor", 100, 40), 40)
        self.assertEqual(get("Editor", 100, 41), 40)
        self.assertEqual(get("PIE", 100, 0), 0)
        self.assertEqual(get("Editor", 101, 41), 41)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(DensityAnimationContract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    summary = {
        "status": "PASS" if result.wasSuccessful() else "FAIL",
        "tests": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
        "scope": "CPU numerical contract only; native C++ build, MID/packet parity and rendered history acceptance remain required",
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
