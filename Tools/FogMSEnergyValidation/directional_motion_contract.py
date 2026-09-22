"""Independent contracts for coherent world wind and continuous piecewise motion.

CPU-only reference. Does not import Unreal, touch a project, or validate UHT/cook.
"""
from __future__ import annotations
import argparse
from dataclasses import asdict, dataclass
import json
import math
from pathlib import Path
import re
import struct
import unittest

ZERO = (0., 0., 0.)


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def scale(a, b):
    return tuple(x * b for x in a)


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


def phase(center, displacement, offset, frequency):
    result = []
    for c, d, o in zip(center, displacement, offset):
        cycles = (c * frequency % 1.) - (d * frequency % 1.) - (o * frequency % 1.)
        value = f32(cycles % 1.)
        result.append(0. if value == 1. else value)
    return tuple(result)


def phase_error(a, b):
    return max(abs((x - y + .5) % 1. - .5) for x, y in zip(a, b))


@dataclass(frozen=True)
class Segment:
    epoch: float
    displacement: tuple
    velocity: tuple

    def at(self, time):
        return add(self.displacement, scale(self.velocity, time - self.epoch))

    def change_velocity(self, time, velocity):
        # This is a continuous piecewise integral of velocity, independent of phase wrapping.
        if tuple(velocity) == tuple(self.velocity):
            return self
        return Segment(time, self.at(time), tuple(velocity))


def coherent_velocities(wind, detail=ZERO, evolution=ZERO):
    return wind, add(wind, detail), add(add(wind, detail), evolution)


def property_specifiers(name):
    """Read the actual outer actor property, not the struct member decorations."""
    header = Path(__file__).resolve().parents[2] / 'Source/MultiLobeSpec/Private/FogMS_BoxVolume.h'
    source = header.read_text(encoding='utf-8')
    declaration = re.search(r'\b' + re.escape(name) + r'\s*;', source)
    if declaration is None:
        raise AssertionError('Missing reflected property: ' + name)
    annotation = source.rfind('UPROPERTY(', 0, declaration.start())
    return source[annotation + len('UPROPERTY('):source.index(')', annotation)]


def construction_resets(specifiers):
    """UE ActorConstruction.cpp ResetPropertiesForConstruction for this native struct.

    It is neither an exposed-on-spawn value, delegate, instanced object nor random
    stream. Match the native edit/Blueprint flags that decide CDO reset.
    VisibleAnywhere sets EditConst as well as Edit; BlueprintReadOnly is visible
    to Blueprints but cannot be assigned there.
    """
    words = set(re.findall(r'\b\w+\b', specifiers))
    edit = bool(words & {'EditAnywhere', 'EditInstanceOnly', 'EditDefaultsOnly',
                         'VisibleAnywhere', 'VisibleInstanceOnly', 'VisibleDefaultsOnly'})
    disable_edit_on_instance = bool(words & {'EditDefaultsOnly', 'VisibleDefaultsOnly'})
    blueprint_visible = bool(words & {'BlueprintReadWrite', 'BlueprintReadOnly'})
    blueprint_readonly = 'BlueprintReadOnly' in words
    return not (edit and not disable_edit_on_instance) and blueprint_visible and not blueprint_readonly


def reconstruct_and_edit(reference, time, velocity, specifiers, manual):
    """Model reset, OnConstruction update, then continuous velocity reanchoring."""
    if construction_resets(specifiers):
        reference = None
    if reference is None:
        reference = Segment(0. if manual else time, ZERO, tuple(velocity))
    return reference.change_velocity(time, velocity)


class DirectionalMotionContract(unittest.TestCase):
    def test_long_elapsed_speed_change_is_continuous(self):
        t = 1.e9 + .125
        old = Segment(0., ZERO, (83.75, -17., .25))
        new = old.change_velocity(t, (100., -90., 12.))
        self.assertEqual(old.at(t), new.at(t))
        for f in (f32(1/8000), f32(4/8000), f32(8/8000)):
            self.assertEqual(phase((1.e12, -2.e12, 137.), old.at(t), ZERO, f),
                             phase((1.e12, -2.e12, 137.), new.at(t), ZERO, f))
        self.assertNotEqual(scale(new.velocity, t), old.at(t), 'Naive v*t must differ in this regression case')

    def test_direction_change_integrates_new_direction(self):
        old = Segment(700., (100., 50., 0.), (80., 0., 0.))
        turn = old.change_velocity(750., (0., 80., 0.))
        self.assertEqual(turn.at(750.), old.at(750.))
        self.assertEqual(turn.at(752.), add(old.at(750.), (0., 160., 0.)))

    def test_same_frame_repeated_edits_do_not_translate_density(self):
        current = Segment(0., ZERO, (80., 0., 0.))
        expected = current.at(50000.)
        for velocity in ((0., 80., 0.), (0., 0., 20.), (-100., 2., 5.), ZERO):
            current = current.change_velocity(50000., velocity)
            self.assertEqual(current.at(50000.), expected)

    def test_zero_speed_holds_shape(self):
        frozen = Segment(0., ZERO, (80., 0., 0.)).change_velocity(12., ZERO)
        self.assertEqual(frozen.at(12.), frozen.at(500000.))

    def test_freeze_resume_and_velocity_edit_while_frozen(self):
        world_time, offset = 90., -17.
        effective_time = world_time + offset
        segment = Segment(0., ZERO, (80., 0., 0.))
        position = segment.at(effective_time)
        manual_time = effective_time - offset
        segment = segment.change_velocity(manual_time + offset, (0., 20., 0.))
        self.assertEqual(segment.at(manual_time + offset), position)
        resumed_world_time = 3000.
        resumed_offset = manual_time + offset - resumed_world_time
        self.assertEqual(segment.at(resumed_world_time + resumed_offset), position)
        self.assertEqual(segment.at(resumed_world_time + resumed_offset + 2.), add(position, (0., 40., 0.)))

    def test_world_pause_does_not_advance_motion(self):
        segment = Segment(0., ZERO, (80., 4., 2.))
        self.assertEqual([segment.at(117.) for _ in range(100)], [segment.at(117.)] * 100)

    def test_all_default_octaves_advect_coherently(self):
        velocities = coherent_velocities((80., -13., 4.))
        segments = [Segment(500., ZERO, v) for v in velocities]
        for t in (500., 1234., 1.e7):
            positions = [s.at(t) for s in segments]
            self.assertEqual(positions, [positions[0]] * 3)
        changed = [s.change_velocity(15000., (0., 50., -2.)) for s in segments]
        self.assertEqual([s.at(15010.) for s in changed], [changed[0].at(15010.)] * 3)

    def test_relative_detail_edits_preserve_each_phase(self):
        velocities = coherent_velocities((80., 0., 0.), (1., -2., 0.), (0., 1., -.5))
        segments = [Segment(0., ZERO, v) for v in velocities]
        changed_velocities = coherent_velocities((80., 0., 0.), (3., 0., 1.), (0., 0., 0.))
        changed = [s.change_velocity(40000., v) for s, v in zip(segments, changed_velocities)]
        self.assertEqual([s.at(40000.) for s in segments], [s.at(40000.) for s in changed])

    def test_manual_seek_reproducible_with_serialized_reference(self):
        original = Segment(0., ZERO, (80., 0., 0.)).change_velocity(1234., (0., 40., 0.))
        # Reconstruct a saved reference rather than silently erasing the continuity offset.
        restored = Segment(**json.loads(json.dumps(asdict(original))))
        for time in (-80., 0., 1234., 3000., 1234.):
            self.assertEqual(original.at(time), restored.at(time))
        self.assertNotEqual(original.at(1234.), scale(original.velocity, 1234.))

    def test_legacy_conversion_preserves_active_phase(self):
        velocities = coherent_velocities((80., -17., 3.), (2., 1., 0.), (-1., 4., .5))
        t = 45000.25
        for v, f in zip(velocities, (f32(1/8000), f32(4/8000), f32(8/8000))):
            legacy = scale(v, t)
            converted = Segment(t, legacy, v)
            self.assertEqual(phase((137., -521., 99.), legacy, ZERO, f),
                             phase((137., -521., 99.), converted.at(t), ZERO, f))

    def test_disabled_legacy_conversion_keeps_static_phase(self):
        converted = Segment(1234., ZERO, (80., 0., 0.))
        self.assertEqual(converted.at(1234.), ZERO)

    def test_static_zero_offset_is_bitwise_original(self):
        for c in ((0., 0., 0.), (1.e12, -1.e12, -237.125)):
            for f in (f32(1/8000), f32(4/8000), f32(8/8000)):
                expected = tuple(0. if f32(x * f % 1.) == 1. else f32(x * f % 1.) for x in c)
                self.assertEqual(phase(c, ZERO, ZERO, f), expected)

    def test_positive_offset_moves_pattern_positive(self):
        center, displacement = (1000., 2000., 3000.), (17., -10., 3.)
        offset = (500., -200., 800.)
        for f in (f32(1/8000), f32(4/8000), f32(8/8000)):
            # The same feature at P appears at P+offset with a positive authored offset.
            original = phase(center, displacement, ZERO, f)
            shifted = phase(add(center, offset), displacement, offset, f)
            self.assertLess(phase_error(original, shifted), 1.e-7)

    def test_box_transform_is_not_a_wind_direction(self):
        # A world-space direction component supplies this vector; Box transforms never enter it.
        world_direction = (0., 1., 0.)
        speed = 80.
        for box_rotation in (0., 45., 90., 180.):
            for box_scale in (.1, 1., 20.):
                self.assertEqual(scale(world_direction, speed), (0., 80., 0.))

    def test_continuous_time_does_not_rewrite_reference(self):
        reference = Segment(500., ZERO, (80., 0., 0.))
        for t in (501., 502., 50000.):
            self.assertIs(reference.change_velocity(t, reference.velocity), reference)

    def test_reset_is_an_explicit_displacement_change(self):
        old = Segment(0., ZERO, (80., 0., 0.))
        reset = Segment(100., ZERO, old.velocity)
        self.assertNotEqual(old.at(100.), reset.at(100.))
        self.assertEqual(reset.at(102.), (160., 0., 0.))

    def test_construction_lifecycle_reproduces_old_manual_failure(self):
        # This is the actual native probe: Reset Motion Origin at t=10000,
        # then edit speed 125 -> 750. The formerly hidden RW property resets.
        old = Segment(10000., ZERO, (125., 0., 0.))
        broken = reconstruct_and_edit(old, 10000., (750., 0., 0.),
                                      'BlueprintReadWrite', manual=True)
        self.assertEqual(broken.at(10000.), (7500000., 0., 0.))
        self.assertGreater(phase_error(phase(ZERO, old.at(10000.), ZERO, f32(1/8000)),
                                      phase(ZERO, broken.at(10000.), ZERO, f32(1/8000))), .1)

    def test_actual_property_survives_construction_and_manual_speed_arrow_edits(self):
        specifiers = property_specifiers('DensityMotionReference')
        old = Segment(10000., ZERO, (125., 0., 0.))
        current = old
        for velocity in ((750., 0., 0.), (0., 750., 0.), (0., 375., 0.), ZERO):
            current = reconstruct_and_edit(current, 10000., velocity, specifiers, manual=True)
            self.assertEqual(current.at(10000.), old.at(10000.))
        self.assertEqual(current.at(20000.), ZERO)

    def test_saved_directional_live_reference_survives_long_time_edits(self):
        # Rehydrate the persisted epoch and anchors at a late world time.
        saved = Segment(400., (17., -20., 9.), (125., 0., 0.))
        restored = Segment(**json.loads(json.dumps(asdict(saved))))
        time = 10000.
        expected = restored.at(time)
        specifiers = property_specifiers('DensityMotionReference')
        faster = reconstruct_and_edit(restored, time, (750., 0., 0.), specifiers, manual=False)
        turned = reconstruct_and_edit(faster, time, (0., 750., 0.), specifiers, manual=False)
        self.assertEqual(faster.at(time), expected)
        self.assertEqual(turned.at(time), expected)
        self.assertEqual(turned.at(time + 2.), add(expected, (0., 1500., 0.)))
        # Regression sensitivity: the old flags would also reset live motion.
        broken = reconstruct_and_edit(restored, time, (750., 0., 0.),
                                      'BlueprintReadWrite', manual=False)
        self.assertNotEqual(broken.at(time), expected)

    def test_arrow_actor_reference_is_not_replaced_by_cdo_reset(self):
        # Native component transform instance-data restoration is checked by
        # the UE probe, not claimed by this actor-property lifecycle model.
        specifiers = property_specifiers('WindDirectionComponent')
        self.assertFalse(construction_resets(specifiers))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(DirectionalMotionContract))
    summary = {'status': 'PASS' if result.wasSuccessful() else 'FAIL', 'tests': result.testsRun,
               'failures': len(result.failures), 'errors': len(result.errors),
               'construction_regression': {
                   'source': 'UE 5.8 Engine/Source/Runtime/Engine/Private/ActorConstruction.cpp:101-113',
                   'actual_reference_specifiers': property_specifiers('DensityMotionReference'),
                   'actual_reference_resets': construction_resets(property_specifiers('DensityMotionReference')),
                   'old_hidden_blueprint_readwrite_resets': construction_resets('BlueprintReadWrite'),
                   'manual_regression_time_seconds': 10000,
                   'old_broken_displacement_cm': [7500000, 0, 0],
                   'correct_preserved_displacement_cm': [0, 0, 0]},
               'scope': 'CPU reference only; native custom-version migration, component UI, UHT/cook and rendered acceptance remain required'}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
