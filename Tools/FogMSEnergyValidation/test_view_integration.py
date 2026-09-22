"""Independent CPU contracts for current-frame Box / native-history separation.

No Unreal, GPU, texture or shader compiler is involved. The tests establish the
intended homogeneous-segment mathematics and normalized-froxel quadrature; they
do not prove HLSL execution, camera filtering, geometry visibility or performance.
The existing native denominator floor is tested separately from the exact ODE.

Run: python -B test_view_integration.py --output <receipt.json>
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import sys
import unittest


ZERO = (0.0, 0.0, 0.0)
# Independent tensor-product construction, rather than the shader's bit pattern.
NODES = (0.5 - 1 / math.sqrt(12), 0.5 + 1 / math.sqrt(12))
POINTS = tuple(itertools.product(NODES, repeat=3))


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def scale(a, factor):
    return tuple(x * factor for x in a)


def exact_segment(extinction, source, length, background=ZERO):
    """Solution of dI/ds = source - extinction*I for constant coefficients."""
    transmission = math.exp(-extinction * length)
    response = -math.expm1(-extinction * length) / extinction if extinction else length
    return add(scale(background, transmission), scale(source, response)), transmission


def native_segment(extinction, source, length, background=ZERO):
    """Native segment expression, fade=1 and already pre-exposed source."""
    transmission = math.exp(-extinction * length)
    response = (1 - transmission) / max(extinction, 1e-5)
    return add(scale(background, transmission), scale(source, response)), transmission


def blend_medium(current, previous, history_weight):
    """Medium is (RGB volumetric source, scalar extinction), not integrated RGB."""
    q = add(scale(current[0], 1 - history_weight), scale(previous[0], history_weight))
    sigma = current[1] * (1 - history_weight) + previous[1] * history_weight
    return q, sigma


def add_medium(native, box):
    return add(native[0], box[0]), native[1] + box[1]


def separated_frame(native_current, native_history, box_current, history_weight):
    next_native_history = blend_medium(native_current, native_history, history_weight)
    return add_medium(next_native_history, box_current), next_native_history


def quadrature_medium(density, albedo, incident):
    sigmas = [density(*p) for p in POINTS]
    q = tuple(math.fsum(s * albedo[c] * incident(*p)[c]
                        for p, s in zip(POINTS, sigmas)) / len(POINTS)
              for c in range(3))
    return q, math.fsum(sigmas) / len(POINTS)


class ViewIntegrationContract(unittest.TestCase):
    def assert_vector_close(self, actual, expected, tolerance=2e-12):
        for a, e in zip(actual, expected):
            self.assertLessEqual(abs(a - e), tolerance * max(1.0, abs(e)))

    def test_mixed_medium_matches_joint_beer_source_integral(self):
        rng = random.Random(32109)
        for _ in range(256):
            n_sigma, b_sigma = (10 ** rng.uniform(-3, 0) for _ in range(2))
            n_source = tuple(rng.random() * n_sigma for _ in range(3))
            b_source = tuple(rng.random() * b_sigma for _ in range(3))
            background = tuple(rng.random() * 3 for _ in range(3))
            length = 10 ** rng.uniform(-1, 2)
            mixed = add_medium((n_source, n_sigma), (b_source, b_sigma))
            actual, actual_t = native_segment(mixed[1], mixed[0], length, background)
            expected, expected_t = exact_segment(mixed[1], mixed[0], length, background)
            self.assert_vector_close(actual, expected)
            self.assertAlmostEqual(actual_t, expected_t, places=15)

    def test_media_are_mixed_before_exponential_not_added_as_images(self):
        # Opaque Box absorption must also attenuate the native medium's source.
        native = ((2.0, 1.0, 0.5), 0.2)
        absorbing_box = (ZERO, 0.8)
        combined = add_medium(native, absorbing_box)
        correct, transmission = native_segment(combined[1], combined[0], 2.0)
        independent_native, _ = native_segment(native[1], native[0], 2.0)
        self.assertLess(correct[0], independent_native[0] * 0.6)
        self.assertAlmostEqual(transmission, math.exp(-2.0))
        # A partition of the same homogeneous mixture must not change its image.
        split, _ = native_segment(combined[1], combined[0], 0.7)
        split, _ = native_segment(combined[1], combined[0], 1.3, split)
        self.assert_vector_close(split, correct)

    def test_occupied_to_empty_removes_box_but_retains_native_history(self):
        previous_native = ((0.8, 0.4, 0.2), 0.1)
        current_native = ((0.2, 0.1, 0.05), 0.04)
        previous_box = ((1.5, 1.0, 0.5), 0.7)
        empty_box = (ZERO, 0.0)
        final, stored_history = separated_frame(current_native, previous_native, empty_box, 0.9)
        expected_native = blend_medium(current_native, previous_native, 0.9)
        self.assertEqual(final, expected_native)
        self.assertEqual(stored_history, expected_native)
        self.assertGreater(stored_history[1], current_native[1])
        # The old combined-history recurrence necessarily leaves Box density.
        contaminated = blend_medium(current_native, add_medium(previous_native, previous_box), 0.9)
        self.assertAlmostEqual(contaminated[1] - final[1], 0.9 * previous_box[1])
        actual, actual_t = native_segment(final[1], final[0], 3)
        baseline, baseline_t = native_segment(expected_native[1], expected_native[0], 3)
        self.assertEqual((actual, actual_t), (baseline, baseline_t))

    def test_box_support_never_enters_the_next_history(self):
        native = ((0.3, 0.5, 0.7), 0.2)
        history = native
        for box in (((4.0, 0.0, 2.0), 1.0), (ZERO, 0.0), ((0.0, 3.0, 0.0), 0.7), (ZERO, 0.0)):
            final, history = separated_frame(native, history, box, 0.95)
            self.assert_vector_close(history[0], native[0])
            self.assertAlmostEqual(history[1], native[1])
            self.assert_vector_close(final[0], add(native[0], box[0]))
            self.assertAlmostEqual(final[1], native[1] + box[1])

    def test_transition_reset_prevents_fallback_history_double_count(self):
        native = ((0.2, 0.3, 0.4), 0.1)
        box = ((0.7, 0.5, 0.3), 0.6)
        _, native_history = separated_frame(native, native, box, 0.9)
        # Producer fails: authored Box returns to the old native path, reset=1.
        fallback_current = add_medium(native, box)
        fallback_history = blend_medium(fallback_current, native_history, 0.0)
        self.assertEqual(fallback_history, fallback_current)
        # Producer recovers: reset native history before adding current Box.
        final, new_history = separated_frame(native, fallback_history, box, 0.0)
        self.assertEqual(new_history, native)
        self.assertEqual(final, fallback_current)
        without_reset, _ = separated_frame(native, fallback_history, box, 0.9)
        self.assertGreater(without_reset[1], final[1])

    def test_zero_box_preserves_native_output_exactly(self):
        rng = random.Random(155)
        for _ in range(128):
            native = (tuple(rng.random() for _ in range(3)), rng.random())
            zero_box = quadrature_medium(lambda x, y, z: 0.0, (1, 1, 1), lambda x, y, z: (1e6, 2e6, 3e6))
            combined = add_medium(native, zero_box)
            self.assertEqual(combined, native)
            self.assertEqual(native_segment(combined[1], combined[0], 4), native_segment(native[1], native[0], 4))

    def test_g0_white_furnace_has_no_extra_phase_or_gain(self):
        radiance = (2.0, 3.0, 7.0)
        # For g=0, integral over the sphere of I/(4*pi) is exactly I.
        mean_incident = tuple((4 * math.pi * c) / (4 * math.pi) for c in radiance)
        for sigma in (1e-4, 0.1, 1.0, 100.0):
            box = quadrature_medium(lambda x, y, z: sigma, (1, 1, 1), lambda x, y, z: mean_incident)
            final, _ = native_segment(box[1], box[0], 2.0, radiance)
            self.assert_vector_close(final, radiance)

    def test_preexposure_is_applied_once_and_not_to_extinction(self):
        native_source = (0.4, 0.2, 0.8)
        box_source = (0.3, 0.7, 0.5)
        background = (0.2, 1.3, 0.4)
        sigma = 0.6
        physical, transmission = exact_segment(sigma, add(native_source, box_source), 3, background)
        for previous_exposure, current_exposure in ((0.1, 4.0), (20.0, 0.03), (1.0, 1.0)):
            stored_previous = scale(native_source, previous_exposure)
            converted_native = scale(stored_previous, current_exposure / previous_exposure)
            current_source = add(converted_native, scale(box_source, current_exposure))
            actual, actual_t = native_segment(sigma, current_source, 3, scale(background, current_exposure))
            self.assert_vector_close(actual, scale(physical, current_exposure))
            self.assertEqual(actual_t, transmission)

    def test_eight_point_tensor_moments(self):
        self.assertEqual(len(set(POINTS)), 8)
        self.assertTrue(all(0 < c < 1 for p in POINTS for c in p))
        # Tensor Gauss is degree three in each index coordinate, including xyz.
        # This is NOT an exactness claim after log-depth/projection or threshold.
        for a, b, c in itertools.product(range(4), repeat=3):
            actual = math.fsum(x ** a * y ** b * z ** c for x, y, z in POINTS) / 8
            self.assertAlmostEqual(actual, 1 / ((a + 1) * (b + 1) * (c + 1)), places=14)
        fourth = math.fsum(p[0] ** 4 for p in POINTS) / 8
        self.assertGreater(abs(fourth - 1 / 5), 1e-3)

    def test_correlated_source_not_product_of_filtered_means(self):
        q, sigma = quadrature_medium(lambda x, y, z: x, (1, 0.5, 0.25), lambda x, y, z: (x, x, x))
        self.assertAlmostEqual(sigma, 0.5)
        self.assert_vector_close(q, (1 / 3, 1 / 6, 1 / 12))
        self.assertGreater(q[0] - sigma * 0.5, 0.08)

    def test_positive_weights_preserve_extinction_and_source_bounds(self):
        rng = random.Random(92026)
        for _ in range(256):
            density = [10 ** rng.uniform(-4, 2) * rng.random() for _ in POINTS]
            albedo = tuple(rng.random() for _ in range(3))
            incident = [tuple(rng.random() * 10 for _ in range(3)) for _ in POINTS]
            index = {point: i for i, point in enumerate(POINTS)}
            q, sigma = quadrature_medium(lambda *p: density[index[p]], albedo, lambda *p: incident[index[p]])
            self.assertGreaterEqual(sigma, min(density))
            self.assertLessEqual(sigma, max(density))
            for c in range(3):
                self.assertGreaterEqual(q[c], 0)
                self.assertLessEqual(q[c], sigma * albedo[c] * max(j[c] for j in incident) + 1e-12)
            integrated, transmission = native_segment(sigma, q, 3)
            self.assertTrue(all(math.isfinite(c) and c >= 0 for c in integrated))
            self.assertTrue(0 <= transmission <= 1)

    def test_native_tiny_extinction_floor_is_an_explicit_limit(self):
        # Preserve the existing native equation; do not claim continuum energy
        # exactness below its 1e-5/cm denominator floor or for pure emission.
        sigma = 1e-7
        source = (sigma, sigma, sigma)
        actual, _ = native_segment(sigma, source, 10)
        exact, _ = exact_segment(sigma, source, 10)
        self.assertAlmostEqual(actual[0] / exact[0], sigma / 1e-5, places=10)
        vacuum, transmission = native_segment(0, ZERO, 100, (2, 3, 4))
        self.assertEqual((vacuum, transmission), ((2.0, 3.0, 4.0), 1.0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='Write a JSON CPU-only receipt.')
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ViewIntegrationContract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    root = Path(__file__).resolve().parents[2]
    paths = ('Shaders/Private/FogMS_Reconstruction.ush', 'Source/MultiLobeSpec/Private/MultiLobeShaderPatcher.cpp')
    receipt = {
        'status': 'PASS' if result.wasSuccessful() else 'FAIL',
        'tests_run': result.testsRun,
        'failures': [(test.id(), trace) for test, trace in result.failures],
        'errors': [(test.id(), trace) for test, trace in result.errors],
        'kind': 'independent_cpu_analytic_contract',
        'shader_execution_verified': False,
        'source_sha256_at_test_time': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths},
        'limits': [
            'No shader compilation, native GPU execution, RDG synchronization or performance verification.',
            'Eight-node exactness holds in normalized index coordinates, not after log-depth/projection or nonlinear density threshold.',
            'Exact homogeneous Beer/source comparison uses extinction >= native 1e-5/cm denominator floor and near-fade=1.',
            'History is a scalar-cell recurrence model; it does not simulate spatial reprojection or subsequent TSR/TAA.',
            'No claim that current-frame quadrature eliminates spatial aliasing or depth interpolation at opaque surfaces.',
        ],
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': receipt['status'], 'tests_run': result.testsRun, 'gpu_verified': False}))
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
