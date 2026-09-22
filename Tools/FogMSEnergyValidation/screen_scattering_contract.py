"""CPU contracts for the native FSSS missing-solar-source repair.

This is algebra/kernel evidence, not a shader compile or GPU acceptance test.
No Unreal imports, project writes or camera operations. Run this file directly.
"""
from __future__ import annotations

import math
import random
import unittest


def solar_radiance(mu, cos_half_angle, outer_space_luminance, atmosphere_t, exposure):
    """Native disk's soft edge and per-light HDR clamp, in one color channel."""
    if mu <= cos_half_angle:
        return 0.0
    edge = min(1.0, max(0.0, 2.0 * (mu - cos_half_angle) / (1.0 - cos_half_angle)))
    return min(64000.0, outer_space_luminance * atmosphere_t * exposure * edge)


def compose_unblurred(fog_l, t, split, background, disk, cloud_l=0.0, cloud_t=1.0):
    source = cloud_l + cloud_t * (fog_l + t * split * (background + disk))
    residual = cloud_t * t * (1.0 - split) * (background + disk)
    return source + residual


def periodic_box_filter(values, radius):
    n = len(values)
    return [sum(values[(i + j) % n] for j in range(-radius, radius + 1))
            / (2 * radius + 1) for i in range(n)]


class ScreenScatteringContracts(unittest.TestCase):
    def test_unblurred_split_is_existing_transport_for_1000_cases(self):
        rng = random.Random(782163)
        for _ in range(1000):
            l, b, d = [rng.random() * 64000.0 for _ in range(3)]
            t, split, cloud_t = [rng.random() for _ in range(3)]
            cloud_l = rng.random() * 100.0
            expected = cloud_l + cloud_t * (l + t * (b + d))
            actual = compose_unblurred(l, t, split, b, d, cloud_l, cloud_t)
            self.assertAlmostEqual(actual, expected, delta=max(1e-10, abs(expected) * 1e-12))

    def test_missing_source_loses_exactly_the_solar_split(self):
        t, split, disk = 0.4, 0.7, 100.0
        broken = t * (1.0 - split) * disk
        fixed = compose_unblurred(0.0, t, split, 0.0, disk)
        self.assertAlmostEqual(fixed - broken, t * split * disk)
        self.assertAlmostEqual(fixed, t * disk)

    def test_no_fog_is_identity(self):
        # Native split vanishes with coverage=0. Neither disk nor scene changes.
        self.assertEqual(compose_unblurred(0.0, 1.0, 0.0, 12.0, 64000.0), 64012.0)

    def test_fsss_disabled_is_identity(self):
        self.assertEqual(compose_unblurred(3.0, 0.25, 0.0, 12.0, 100.0), 31.0)

    def test_fully_opaque_fog_has_no_background_injection(self):
        self.assertEqual(compose_unblurred(7.0, 0.0, 1.0, 12.0, 64000.0), 7.0)

    def test_opaque_native_cloud_blocks_both_solar_branches(self):
        self.assertEqual(compose_unblurred(7.0, 0.8, 0.9, 12.0, 64000.0, 3.0, 0.0), 3.0)

    def test_normalized_blur_preserves_integrated_remaining_disk_energy(self):
        # Uniform medium, no crop: widening redistributes the retained T*D.
        disk = [0.0] * 101
        disk[50] = 1000.0
        t, split = 0.35, 0.8
        scattered = periodic_box_filter([t * split * d for d in disk], 8)
        sharp = [t * (1.0 - split) * d for d in disk]
        self.assertAlmostEqual(sum(scattered) + sum(sharp), t * sum(disk))
        self.assertGreater(scattered[45], 0.0)
        self.assertLess(scattered[50] + sharp[50], t * disk[50])

    def test_native_soft_edge_has_zero_support_outside_disk(self):
        c = math.cos(math.radians(0.535 * 0.5))
        self.assertEqual(solar_radiance(c, c, 100.0, 0.5, 2.0), 0.0)
        self.assertEqual(solar_radiance(c - 1e-6, c, 100.0, 0.5, 2.0), 0.0)
        self.assertAlmostEqual(solar_radiance(c + (1-c)*0.25, c, 100.0, 0.5, 2.0), 50.0, places=8)
        self.assertEqual(solar_radiance(1.0, c, 100.0, 0.5, 2.0), 100.0)

    def test_native_clamp_is_per_light_after_atmospheric_transmission(self):
        c = math.cos(0.01)
        bright = solar_radiance(1.0, c, 1e8, 0.8, 2.0)
        dim = solar_radiance(1.0, c, 10.0, 0.5, 2.0)
        self.assertEqual(bright, 64000.0)
        self.assertEqual(dim, 10.0)
        self.assertEqual(bright + dim, 64010.0)

    def test_absent_light_is_zero_without_invalid_division(self):
        self.assertEqual(solar_radiance(1.0, 1.0, 0.0, 1.0, 1.0), 0.0)

    def test_white_hdr_disk_can_remain_after_dense_fog(self):
        self.assertGreater(64000.0 * math.exp(-5.0), 400.0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
