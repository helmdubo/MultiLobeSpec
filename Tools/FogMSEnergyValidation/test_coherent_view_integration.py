"""Independent CPU contracts for four persistent FogMS view subrays.

The reference uses radiative-transfer operators and analytic/Simpson oracles.
It does not parse shader text, import Unreal, or claim shader/GPU validation.
The shader under review is FogMS_Reconstruction.ush::FogMS_IntegrateCoherentLayer.
Fixed quadrature is not temporal AA and cannot resolve arbitrary thin features.

Run: python -P test_coherent_view_integration.py --json <new-receipt.json>
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import sys
import unittest


ZERO = (0.0, 0.0, 0.0)
GAUSS = (0.5 - 1.0 / math.sqrt(12.0), 0.5 + 1.0 / math.sqrt(12.0))
METRICS = {}


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def scale(v, k):
    return tuple(k * x for x in v)


@dataclass(frozen=True)
class Transfer:
    """Front-to-back radiance transfer: result = emission + T * background."""
    emission: tuple = ZERO
    t: float = 1.0

    def over(self, behind):
        return Transfer(add(self.emission, scale(behind.emission, self.t)), self.t * behind.t)

    def faded(self, amount):
        # Native near-fade is a mixture of a complete layer and vacuum.
        return Transfer(scale(self.emission, amount), 1.0 - amount + amount * self.t)

    def compose_background(self, background):
        return add(self.emission, scale(background, self.t))


def slab(sigma, source_preexposed, length):
    """Closed-form constant-source slab, retaining the native 1e-5 floor.

    expm1 avoids CPU cancellation. This is an equation oracle, not an fp32
    instruction emulator; native exp/subtract rounding still requires GPU QA.
    """
    tau = sigma * length
    gain = -math.expm1(-tau) / max(sigma, 1e-5)
    return Transfer(scale(source_preexposed, gain), math.exp(-tau))


def empty_box(ray, z):
    return 0.0, (1.0, 1.0, 1.0), ZERO


@dataclass(frozen=True)
class Layer:
    length: float
    native_sigma: float = 0.0
    native_source_preexposed: tuple = ZERO
    box: object = empty_box
    fade: float = 1.0
    # Only Box uses actual oblique subray length; native data keeps its native
    # center-ray measure. This preserves native parity with an empty Box.
    box_length_ratios: tuple = (1.0, 1.0, 1.0, 1.0)


def coherent_column(layers, exposure=1.0):
    """Four XY identities survive every layer; average only cumulative L/T."""
    rays = [Transfer() for _ in range(4)]
    start = 0.0
    has_box = False
    for layer in layers:
        for ray in range(4):
            segment = Transfer()
            ratio = layer.box_length_ratios[ray]
            for u in GAUSS:
                sigma, albedo, incident = layer.box(ray, start + u * layer.length)
                has_box |= sigma > 0.0
                box_source = tuple(sigma * min(1.0, max(0.0, a)) * max(0.0, j)
                                   for a, j in zip(albedo, incident))
                total_source = add(layer.native_source_preexposed, scale(box_source, exposure * ratio))
                segment = segment.over(slab(layer.native_sigma + ratio * sigma,
                                             total_source, layer.length / 2.0))
            rays[ray] = rays[ray].over(segment.faded(layer.fade))
        start += layer.length
    mean = Transfer(tuple(sum(r.emission[c] for r in rays) / 4.0 for c in range(3)),
                    sum(r.t for r in rays) / 4.0)
    return mean, tuple(rays), has_box


def simpson(f, start, stop, intervals=32768):
    assert intervals % 2 == 0
    h = (stop - start) / intervals
    return h / 3.0 * (f(start) + f(stop)
                      + 4.0 * sum(f(start + i * h) for i in range(1, intervals, 2))
                      + 2.0 * sum(f(start + i * h) for i in range(2, intervals, 2)))


class CoherentViewIntegration(unittest.TestCase):
    def assert_rgb(self, actual, expected, tolerance=1e-11):
        for x, y in zip(actual, expected):
            self.assertAlmostEqual(x, y, delta=tolerance * max(1.0, abs(y)))

    def test_half_clear_half_opaque_preserves_jensen_coverage(self):
        def box(ray, z):
            return (0.0 if ray < 2 else 16.0), (1.0,) * 3, ZERO
        result, _, _ = coherent_column([Layer(1.0, box=box)])
        expected = 0.5 * (1.0 + math.exp(-16.0))
        averaged_density_t = math.exp(-8.0)
        self.assertAlmostEqual(result.t, expected, places=14)
        self.assertGreater(result.t - averaged_density_t, 0.49)
        METRICS['jensen'] = {'coherent_t': result.t, 'analytic_coverage_t': expected,
                             'exp_mean_tau': averaged_density_t}

    def test_aligned_and_alternating_holes_keep_ray_identity(self):
        def aligned(ray, z):
            return (0.0 if ray < 2 else 10.0), (1.0,) * 3, ZERO
        def alternating(ray, z):
            clear = (ray < 2) if z < 1.0 else (ray >= 2)
            return (0.0 if clear else 10.0), (1.0,) * 3, ZERO
        a, _, _ = coherent_column([Layer(1.0, box=aligned)] * 2)
        b, _, _ = coherent_column([Layer(1.0, box=alternating)] * 2)
        wrong_average_each_layer = (0.5 * (1.0 + math.exp(-10.0))) ** 2
        self.assertAlmostEqual(a.t, 0.5 * (1.0 + math.exp(-20.0)), places=14)
        self.assertAlmostEqual(b.t, math.exp(-10.0), places=14)
        self.assertGreater(a.t - b.t, 0.49)
        self.assertGreater(abs(a.t - wrong_average_each_layer), 0.24)
        self.assertGreater(abs(b.t - wrong_average_each_layer), 0.24)
        METRICS['ray_identity'] = {'aligned_t': a.t, 'alternating_t': b.t,
                                   'wrong_per_layer_average_t': wrong_average_each_layer}

    def test_homogeneous_native_and_box_match_analytic_mixed_slab(self):
        sigma_n, sigma_b, length, exposure = 0.13, 0.27, 4.0, 2.5
        native_world_source = (0.04, 0.08, 0.12)
        albedo, incident = (0.9, 0.6, 0.3), (2.0, 3.0, 4.0)
        box = lambda ray, z: (sigma_b, albedo, incident)
        native_pre = scale(native_world_source, exposure)
        result, _, _ = coherent_column([Layer(length, sigma_n, native_pre, box)], exposure)
        expected_source = tuple(exposure * (n + sigma_b * a * j)
                                for n, a, j in zip(native_world_source, albedo, incident))
        expected = slab(sigma_n + sigma_b, expected_source, length)
        self.assertAlmostEqual(result.t, expected.t, places=14)
        self.assert_rgb(result.emission, expected.emission)
        self.assert_rgb(result.compose_background((1.2, 3.0, 0.7)),
                        expected.compose_background((1.2, 3.0, 0.7)))

    def test_oblique_ratio_changes_box_measure_not_native_measure(self):
        ratios = (1.0, 1.1, 1.3, 1.7)
        box = lambda ray, z: (0.25, (0.8,) * 3, (3.0,) * 3)
        native_q = (0.12,) * 3
        _, rays, _ = coherent_column([Layer(2.0, 0.2, native_q, box, box_length_ratios=ratios)])
        for ratio, ray in zip(ratios, rays):
            expected = slab(0.2 + ratio * 0.25, add(native_q, (ratio * 0.6,) * 3), 2.0)
            self.assertAlmostEqual(ray.t, expected.t, places=14)
            self.assert_rgb(ray.emission, expected.emission)

    def test_zero_box_matches_native_column_including_tiny_extinction(self):
        layers = [Layer(1.0, 0.0, (1.0,) * 3, fade=0.0),
                  Layer(0.7, 1e-8, (1.0, 2.0, 4.0), fade=0.13),
                  Layer(3.2, 0.4, (0.7, 0.3, 0.2), fade=0.6),
                  Layer(0.9, 0.07, (0.2, 0.8, 1.1), fade=1.0)]
        expected = Transfer()
        for layer in layers:
            expected = expected.over(slab(layer.native_sigma, layer.native_source_preexposed,
                                          layer.length).faded(layer.fade))
        actual, rays, has_box = coherent_column(layers, exposure=321.0)
        self.assertFalse(has_box)
        self.assertAlmostEqual(actual.t, expected.t, places=14)
        self.assert_rgb(actual.emission, expected.emission)
        for ray in rays:
            self.assert_rgb(ray.emission, expected.emission)
        METRICS['zero_box'] = {'max_rgb_error': max(abs(a-b) for a,b in zip(actual.emission, expected.emission)),
                               't_error': abs(actual.t-expected.t), 'native_extinction_floor_retained': True}

    def test_fade_is_applied_once_to_whole_layer(self):
        fade, sigma = 0.3, 2.0
        box = lambda ray, z: (sigma, (1.0,) * 3, (3.0,) * 3)
        actual, _, _ = coherent_column([Layer(1.0, box=box, fade=fade)])
        expected = slab(sigma, (6.0,) * 3, 1.0).faded(fade)
        incorrect_half_fade = slab(sigma, (6.0,) * 3, 0.5).faded(fade)
        incorrect = incorrect_half_fade.over(incorrect_half_fade)
        self.assertAlmostEqual(actual.t, expected.t, places=14)
        self.assert_rgb(actual.emission, expected.emission)
        self.assertGreater(abs(actual.t - incorrect.t), 0.05)
        METRICS['fade_once'] = {'actual_t': actual.t, 'expected_t': expected.t,
                                'incorrect_per_half_t': incorrect.t}

    def test_preexposure_scales_rgb_once_and_never_transmittance(self):
        box = lambda ray, z: (0.1 + ray * 0.04, (0.8, 0.4, 0.2), (2.0, 4.0, 8.0))
        reference, _, _ = coherent_column([Layer(3.0, 0.15, (0.1, 0.2, 0.3), box)], exposure=1.0)
        for exposure in (0.001, 0.3, 2.0, 128.0):
            layers = [Layer(3.0, 0.15, scale((0.1, 0.2, 0.3), exposure), box)]
            actual, _, _ = coherent_column(layers, exposure)
            self.assertAlmostEqual(actual.t, reference.t, places=14)
            self.assert_rgb(actual.emission, scale(reference.emission, exposure))

    def test_uniform_radiance_furnace_no_energy_gain_with_holes_and_fade(self):
        radiance = (1.3, 2.1, 4.0)
        box = lambda ray, z: ((0.0 if ray < 2 else 0.7 + z * 0.1), (1.0,) * 3, radiance)
        layers = [Layer(1.1, 0.2, scale(radiance, 0.2), box, fade=0.2),
                  Layer(2.3, 0.3, scale(radiance, 0.3), box, fade=0.7)]
        result, _, _ = coherent_column(layers)
        self.assert_rgb(result.compose_background(radiance), radiance)

    def test_homogeneous_layer_subdivision_invariance(self):
        box = lambda ray, z: (0.3 + ray * 0.1, (0.8,) * 3, (2.0, 3.0, 4.0))
        reference, _, _ = coherent_column([Layer(4.0, 0.1, (0.2, 0.4, 0.6), box)])
        for count in (2, 4, 16, 64):
            result, _, _ = coherent_column([Layer(4.0 / count, 0.1, (0.2, 0.4, 0.6), box)] * count)
            self.assertAlmostEqual(result.t, reference.t, places=13)
            self.assert_rgb(result.emission, reference.emission)

    def test_smooth_source_subdivision_converges_to_integral_oracle(self):
        length = 3.0
        sigma = lambda z: 0.2 + 0.1 * z
        source = lambda z: 0.4 + 0.2 * math.sin(1.3 * z)
        optical_depth = lambda z: 0.2 * z + 0.05 * z * z
        expected_l = simpson(lambda z: source(z) * math.exp(-optical_depth(z)), 0.0, length)
        expected_t = math.exp(-optical_depth(length))
        box = lambda ray, z: (sigma(z), (1.0,) * 3, (source(z) / sigma(z),) * 3)
        errors = []
        for count in (1, 2, 4, 8, 16, 32, 64):
            actual, _, _ = coherent_column([Layer(length / count, box=box)] * count)
            self.assertAlmostEqual(actual.t, expected_t, places=13)
            errors.append(abs(actual.emission[0] - expected_l))
        self.assertTrue(all(b < a for a, b in zip(errors, errors[1:])), errors)
        self.assertLess(errors[-1], errors[0] / 500.0)
        self.assertLess(errors[-1], 1e-5)
        METRICS['subdivision'] = {'layer_counts': [1, 2, 4, 8, 16, 32, 64],
                                  'rgb_errors': errors, 'oracle_l': expected_l,
                                  'oracle_t': expected_t, 'oracle': '32768-interval Simpson; analytic optical depth'}

    def test_fixed_quadrature_admits_unresolved_thin_features(self):
        box = lambda ray, z: (20.0 if 0.46 < z < 0.54 else 0.0, (1.0,) * 3, ZERO)
        coarse, _, _ = coherent_column([Layer(1.0, box=box)])
        refined, _, _ = coherent_column([Layer(1.0 / 32.0, box=box)] * 32)
        analytic_t = math.exp(-20.0 * 0.08)
        self.assertEqual(coarse.t, 1.0)  # Both fixed nodes miss this sheet.
        self.assertLess(abs(refined.t - analytic_t), abs(coarse.t - analytic_t))
        METRICS['fixed_quadrature_limit'] = {'coarse_t': coarse.t, 'refined_t': refined.t,
                                            'analytic_t': analytic_t,
                                            'is_temporal_aa': False}


class ReceiptResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.checks = []

    def addSuccess(self, test):
        super().addSuccess(test)
        self.checks.append({'name': test.id(), 'status': 'PASS'})

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self.checks.append({'name': test.id(), 'status': 'FAIL', 'detail': self._exc_info_to_string(err, test)})

    def addError(self, test, err):
        super().addError(test, err)
        self.checks.append({'name': test.id(), 'status': 'ERROR', 'detail': self._exc_info_to_string(err, test)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--json', type=Path, required=True, help='New immutable CPU receipt path; existing files are rejected.')
    args = parser.parse_args()
    if args.json.exists():
        parser.error('Refusing to overwrite existing receipt: ' + str(args.json))
    result = unittest.TextTestRunner(verbosity=2, resultclass=ReceiptResult).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(CoherentViewIntegration))
    receipt = {
        'schema': 'fogms-coherent-view-integration-cpu-v1',
        'timestamp_utc': datetime.now(timezone.utc).isoformat(),
        'status': 'PASS' if result.wasSuccessful() else 'FAIL',
        'tests_run': result.testsRun,
        'checks': result.checks,
        'metrics': METRICS,
        'limitations': [
            'Independent double-precision equation tests; not shader compilation, GPU execution, or fp32 rounding validation.',
            'Four fixed XY rays and two depth nodes retain unresolved spatial frequencies; this is not temporal AA.',
            'Native 1e-5 source denominator floor and whole-layer near-fade are preserved as compatibility approximations.',
            'Native source input is already pre-exposed; only Box source receives the explicit exposure multiplier.',
            'Scene capture, visual sun-edge acceptance, performance and lifetime validation remain separate GPU gates.',
        ],
    }
    args.json.parent.mkdir(parents=True, exist_ok=True)
    with args.json.open('x', encoding='utf-8') as out:
        json.dump(receipt, out, indent=2)
        out.write('\n')
    print('CPU receipt:', args.json.resolve())
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
