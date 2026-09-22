"""Independent CPU contracts for fog-weighted redistribution of full scene HDR.

Run: python -B test_full_hdr_scattering.py --output <receipt.json>

This is an algebraic oracle for positive normalized pyramid/reconstruction
operators, not a rasterizer or a translation of the HLSL. Stencil positions,
GPU FP16 rounding, scene-depth/fog sampling and temporal images are not modeled.
The crucial regression fixture is illumination emitted/scattered BY dense fog
against a black background: filtering C-Lvol would have no signal to spread.
Only image-space invariants are asserted; no physical transport/energy claim.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import random
import sys
import unittest


RGB = tuple[float, float, float]
LUMA = (0.2126, 0.7152, 0.0722)
BINOMIAL_2D = tuple(a * b / 64 for a in (1, 3, 3, 1) for b in (1, 3, 3, 1))


def normalized_average(colors, weights):
    total = math.fsum(weights)
    if total <= 0:
        return None
    return tuple(math.fsum(w * c[channel] for c, w in zip(colors, weights)) / total
                 for channel in range(3))


def first_reduction(colors, spatial_weights, pre_exposure):
    """Karis weights use original scene-linear, not pre-exposed, luminance."""
    weights = [w / (1 + math.fsum(c * l for c, l in zip(color, LUMA)) / pre_exposure)
               for color, w in zip(colors, spatial_weights)]
    # The downsample normalizes at any positive weight sum. The guided receiver
    # instead retains the accepted mass as confidence, without division.
    total = math.fsum(weights)
    return tuple(math.fsum(w * c[channel] for c, w in zip(colors, weights)) / total
                 for channel in range(3))


def guided_average(colors, weights, depth_ranges, receiver_depth):
    accepted = [w * max(0.0, 1 - max(abs(lo - receiver_depth), abs(hi - receiver_depth)) / 0.25)
                for w, (lo, hi) in zip(weights, depth_ranges)]
    # Do NOT divide by accepted mass. Tiny supports must cause tiny changes,
    # rather than jumping between a full blur and an epsilon-triggered fallback.
    weighted_rgb = tuple(math.fsum(w * c[channel] for c, w in zip(colors, accepted))
                         for channel in range(3))
    return weighted_rgb, math.fsum(accepted)


def composite(current: RGB, filtered, transmission, amount, enabled=True, accepted_mass=1.0):
    """Filtered is an unnormalized RGB sum; missing weight stays at current C."""
    coverage = min(1.0, max(0.0, 1 - transmission))
    blend = min(1.0, max(0.0, amount)) * coverage
    if not enabled or blend == 0 or accepted_mass == 0:
        return current
    return tuple(c + blend * (f - accepted_mass * c) for c, f in zip(current, filtered))


def scale(color, factor):
    return tuple(c * factor for c in color)


class FullHDRScatteringContract(unittest.TestCase):
    def assert_rgb_close(self, actual, expected, rel=3e-12):
        for a, e in zip(actual, expected):
            self.assertLessEqual(abs(a - e), rel * max(abs(e), 1.0))

    def test_zero_fog_is_exact_identity_even_with_bright_neighbors(self):
        current = (0.03125, 4.0, 127.0)
        for amount in (0.0, 0.5, 1.0):
            self.assertEqual(composite(current, (60000, 30000, 10000), 1, amount), current)

    def test_off_and_amount_zero_preserve_unbounded_original_hdr(self):
        # The actual early CPU return must avoid even the FP16 prepare pass.
        current = (1e8, 1e6, 1e7)
        self.assertEqual(composite(current, (0, 0, 0), 0, 0), current)
        self.assertEqual(composite(current, (0, 0, 0), 0, 1, enabled=False), current)

    def test_full_opacity_keeps_blur_active(self):
        current = (8, 2, 1)
        target = (2, 3, 4)
        self.assertEqual(composite(current, target, 0, 1), target)
        self.assert_rgb_close(composite(current, target, 0, 0.5), (5, 2.5, 2.5))

    def test_localized_fog_illumination_spreads_on_black_background(self):
        # C is entirely Lvol; there is no transmitted sky/sun/background signal.
        fog_light = [(0.0, 0.0, 0.0)] * 16
        fog_light[5] = (8.0, 4.0, 2.0)
        target = first_reduction(fog_light, BINOMIAL_2D, pre_exposure=1)
        source_after = composite(fog_light[5], target, transmission=0, amount=1)
        neighbor_after = composite((0, 0, 0), target, transmission=0, amount=1)
        self.assertTrue(all(0 < a < b for a, b in zip(source_after, fog_light[5])))
        self.assertTrue(all(x > 0 for x in neighbor_after))
        # The former Q=max(C-Lvol,0) pipeline would have only zeros to filter.
        self.assertEqual(first_reduction([(0, 0, 0)] * 16, BINOMIAL_2D, 1), (0, 0, 0))

    def test_constant_hdr_survives_karis_guidance_and_variable_coverage(self):
        rng = random.Random(2209)
        for _ in range(256):
            color = tuple(rng.random() * 10000 for _ in range(3))
            low = first_reduction([color] * 16, BINOMIAL_2D, 0.5)
            reconstruction, mass = guided_average([low] * 4, (0.1, 0.2, 0.3, 0.4),
                                                  ((10, 10), (10.02, 10.06), (32, 32), (9.9, 10.1)), 10)
            self.assert_rgb_close(composite(color, reconstruction, rng.random(), rng.random(), accepted_mass=mass), color)

    def test_positive_normalized_stages_are_convex_and_finite(self):
        rng = random.Random(4541)
        for _ in range(1000):
            colors = [tuple(rng.random() * 60000 for _ in range(3)) for _ in range(16)]
            target = first_reduction(colors, BINOMIAL_2D, 10 ** rng.uniform(-3, 3))
            current = colors[rng.randrange(16)]
            mass = rng.random()
            result = composite(current, scale(target, mass), rng.random(), rng.random(), accepted_mass=mass)
            for channel, value in enumerate(result):
                self.assertTrue(math.isfinite(value))
                self.assertGreaterEqual(value + 1e-9, min(c[channel] for c in colors))
                self.assertLessEqual(value - 1e-9, max(c[channel] for c in colors))

    def test_foreground_rejects_bright_sky_and_mixed_depth_tiles(self):
        dark = (0.1, 0.2, 0.3)
        colors = [dark, (60000, 60000, 60000), (5000, 8000, 9000)]
        target, mass = guided_average(colors, (0.2, 0.4, 0.4), ((10, 10), (32, 32), (10, 32)), 10)
        self.assertEqual(mass, 0.2)
        self.assert_rgb_close(composite(dark, target, 0, 1, accepted_mass=mass), dark)

    def test_all_rejected_support_falls_back_to_current_even_in_dense_fog(self):
        current = (2, 3, 7)
        target, mass = guided_average([(60000, 60000, 60000)], (1,), ((32, 32),), 10)
        self.assertEqual((target, mass), ((0, 0, 0), 0))
        self.assertEqual(composite(current, target, 0, 1, accepted_mass=mass), current)

    def test_accepted_mass_tends_continuously_to_zero_without_epsilon_jump(self):
        current = (0.2, 0.4, 0.6)
        neighboring = (12, 7, 3)
        previous_error = math.inf
        for mass in (1, 0.1, 1e-3, 1e-5, 1e-6, 1e-9, 1e-12, 0):
            result = composite(current, scale(neighboring, mass), 0, 1, accepted_mass=mass)
            expected = tuple(c + mass * (n - c) for c, n in zip(current, neighboring))
            self.assert_rgb_close(result, expected)
            error = max(abs(a - b) for a, b in zip(result, current))
            self.assertLessEqual(error, previous_error)
            previous_error = error
        self.assertEqual(result, current)

    def test_depth_rejection_boundary_reaches_identity_continuously(self):
        current = (1, 1, 1)
        neighbor = (11, 5, 2)
        for epsilon in (0.1, 0.01, 1e-4, 1e-6, 1e-9, 0):
            # The guide cutoff is 0.25 in log-depth units. Its surviving mass
            # tends to zero linearly instead of normalizing back to a full blur.
            depth = 0.25 - epsilon
            target, mass = guided_average([neighbor], (1,), ((depth, depth),), 0)
            result = composite(current, target, 0, 1, accepted_mass=mass)
            self.assertAlmostEqual(mass, 4 * epsilon, places=14)
            expected = tuple(c + 4 * epsilon * (n - c) for c, n in zip(current, neighbor))
            self.assert_rgb_close(result, expected)

    def test_two_lod_weighted_blend_preserves_partial_support_confidence(self):
        current = (1, 2, 4)
        # One LOD is rejected completely, the other has 40% accepted support.
        neighbor = (9, 3, 1)
        for lod in (0, 0.1, 0.5, 0.9, 1):
            mass = lod * 0.4
            rgb_sum = scale(neighbor, mass)
            actual = composite(current, rgb_sum, 0, 1, accepted_mass=mass)
            expected = tuple((1 - mass) * c + mass * n for c, n in zip(current, neighbor))
            self.assert_rgb_close(actual, expected)

    def test_full_support_matches_original_normalized_blur(self):
        colors = [(float(i), float(i + 1), float(2 * i)) for i in range(16)]
        target, mass = guided_average(colors, BINOMIAL_2D, [(10, 10)] * 16, 10)
        self.assertEqual(mass, 1)
        normalized = normalized_average(colors, BINOMIAL_2D)
        self.assert_rgb_close(composite(colors[0], target, 0.2, 0.7, accepted_mass=mass),
                              composite(colors[0], normalized, 0.2, 0.7))

    def test_coverage_is_current_receiver_not_blurred_neighbor_coverage(self):
        target = (10, 10, 10)
        self.assertEqual(composite((0, 0, 0), target, 1, 1), (0, 0, 0))
        self.assertEqual(composite((0, 0, 0), target, 0.25, 1), (7.5, 7.5, 7.5))

    def test_preexposure_changes_no_karis_visibility_below_fp16_clipping(self):
        rng = random.Random(5444)
        for _ in range(256):
            scene_colors = [tuple(rng.random() * 50 for _ in range(3)) for _ in range(16)]
            current = scene_colors[0]
            target = first_reduction(scene_colors, BINOMIAL_2D, 1)
            baseline = composite(current, target, 0.3, 0.8)
            exposure = 10 ** rng.uniform(-2, 2)
            scaled_target = first_reduction([scale(c, exposure) for c in scene_colors], BINOMIAL_2D, exposure)
            actual = composite(scale(current, exposure), scaled_target, 0.3, 0.8)
            self.assert_rgb_close(actual, scale(baseline, exposure))

    def test_common_rgb_weights_do_not_create_spurious_color_for_gray_input(self):
        colors = [(float(i),) * 3 for i in range(16)]
        target = first_reduction(colors, BINOMIAL_2D, 1)
        self.assertEqual(target[0], target[1])
        self.assertEqual(target[1], target[2])

    def test_local_bounds_do_not_imply_global_image_energy_conservation(self):
        # Explicit counterexample: receiver coverage differs. Document the
        # limitation rather than naming this screen effect a transport solver.
        original = [(0, 0, 0), (10, 10, 10)]
        target = normalized_average(original, (0.5, 0.5))
        result = [composite(original[0], target, 0, 1), composite(original[1], target, 1, 1)]
        self.assertEqual(sum(c[0] for c in original), 10)
        self.assertEqual(sum(c[0] for c in result), 15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FullHDRScatteringContract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    receipt = {
        'status': 'PASS' if result.wasSuccessful() else 'FAIL',
        'tests_run': result.testsRun,
        'failures': [(case.id(), trace) for case, trace in result.failures],
        'errors': [(case.id(), trace) for case, trace in result.errors],
        'kind': 'independent_cpu_full_hdr_confidence_weighted_image_operator_contract',
        'gpu_verified': False,
        'limits': [
            'No GPU, shader compilation, native composition, image comparison, performance or temporal acceptance.',
            'Positive-operator algebra does not prove actual HLSL resource bindings or stencil placement.',
            'FP16 rounding/clipping and native fog/depth sampling are not simulated.',
            'Karis and receiver-varying coverage do not conserve global image energy (explicit counterexample tested).',
            'Opaque-background depth guidance can reject fog blur even when fog itself is in front of that background.',
            'This is an approximate image effect; it establishes no B3 transport, anisotropy or scattering-order property.',
        ],
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': receipt['status'], 'tests_run': result.testsRun, 'gpu_verified': False}))
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
