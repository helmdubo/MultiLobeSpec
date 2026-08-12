from __future__ import annotations

import json
import math
import tempfile
import unittest
from pathlib import Path

from cone_env import (
    AB,
    DIMENSIONS,
    PackedConeEnvLUT,
    adjust_specular_cone,
    apply_ab,
    canonical_manifest,
    cone_node,
    data_sha256,
    error_metrics,
    flat_index,
    generate_packed,
    integrate_ue_numpy,
    integrate_ue_scalar,
    no_v_node,
    np,
    pack_rg16,
    render_include,
    roughness_node,
    unpack_rg16,
    validate_lut,
)


class OracleTests(unittest.TestCase):
    def test_c_zero_matches_ue57_unoccluded_golden(self) -> None:
        # Golden double-precision translations of the UE 5.7 SystemTextures.cpp
        # estimator at its native 128 samples. c=0 must preserve NoL>0 exactly.
        cases = (
            (0.25, 0.10, AB(0.7419709450247717, 0.22975512714084678)),
            (0.50, 0.50, AB(0.7441921095543897, 0.01882633324286235)),
            (0.90, 0.95, AB(0.37651051548809855, 0.000209970821742712)),
        )
        for no_v, roughness, expected in cases:
            actual = integrate_ue_scalar(no_v, roughness, 0.0, 128)
            self.assertAlmostEqual(actual.a, expected.a, places=14)
            self.assertAlmostEqual(actual.b, expected.b, places=14)

    def test_c_one_is_exact_zero(self) -> None:
        for no_v, roughness in ((0.0, 0.0), (0.1, 0.9), (0.5, 0.5), (1.0, 1.0)):
            self.assertEqual(integrate_ue_scalar(no_v, roughness, 1.0, 128), AB(0.0, 0.0))

    def test_reference_is_finite_bounded_and_monotone_nonincreasing_in_c(self) -> None:
        for no_v in (0.01, 0.25, 0.5, 0.99):
            for roughness in (0.01, 0.2, 0.6, 0.99):
                values = [
                    integrate_ue_scalar(no_v, roughness, cone_node(k), 512)
                    for k in range(DIMENSIONS[2])
                ]
                for value in values:
                    self.assertTrue(math.isfinite(value.a) and math.isfinite(value.b))
                    self.assertTrue(0.0 <= value.a <= 1.0)
                    self.assertTrue(0.0 <= value.b <= 1.0)
                for previous, current in zip(values, values[1:]):
                    self.assertLessEqual(current.a, previous.a)
                    self.assertLessEqual(current.b, previous.b)

    @unittest.skipUnless(np is not None, "NumPy is optional")
    def test_scalar_numpy_parity(self) -> None:
        for point in (
            (0.01, 0.02, 0.0),
            (0.2, 0.8, 2.0 / 7.0),
            (0.5, 0.5, 0.5),
            (0.99, 0.99, 6.0 / 7.0),
            (1.0, 0.1, 1.0),
        ):
            scalar = integrate_ue_scalar(*point, 4096)
            vector = integrate_ue_numpy(*point, 4096)
            self.assertAlmostEqual(scalar.a, vector.a, delta=2.0e-13)
            self.assertAlmostEqual(scalar.b, vector.b, delta=2.0e-13)

    def test_adjust_specular_cone_mapping_and_endpoints(self) -> None:
        self.assertEqual(adjust_specular_cone(0.0, 1.0, 1.0), 0.0)
        self.assertEqual(adjust_specular_cone(1.0, 1.0, 1.0), 1.0)
        self.assertEqual(adjust_specular_cone(0.8, 0.0, 1.0), 0.0)
        self.assertEqual(adjust_specular_cone(0.8, 1.0, 0.0), 0.0)
        middle = adjust_specular_cone(0.8, 0.7, 0.6)
        self.assertTrue(0.0 < middle < 0.8)
        self.assertAlmostEqual(apply_ab(0.04, AB(0.7, 0.02)), 0.048, places=15)

    def test_rg16_quantization_bound(self) -> None:
        errors = []
        for index in range(1001):
            value = index / 1000.0
            source = AB(value, 1.0 - value)
            decoded = unpack_rg16(pack_rg16(source))
            errors.extend((abs(decoded.a - source.a), abs(decoded.b - source.b)))
        self.assertLessEqual(max(errors), 0.5 / 65535.0 + 1.0e-15)
        self.assertLess(error_metrics(errors)["mean"], 0.25 / 65535.0)


class PackedLUTTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        root = Path(cls.temporary.name)
        cls.manifest_path = root / "MLS_ConeEnvBRDF.manifest.json"
        cls.include_path = root / "MLS_ConeEnvBRDF.ush"
        backend = "numpy" if np is not None else "scalar"
        cls.packed = generate_packed(128, backend)
        cls.manifest_path.write_text(
            json.dumps(canonical_manifest(128, cls.packed)), encoding="utf-8"
        )
        cls.include_path.write_text(render_include(cls.packed), encoding="utf-8")
        cls.lut = PackedConeEnvLUT.load(cls.manifest_path, cls.include_path)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_loader_hash_count_and_node_endpoints(self) -> None:
        self.assertEqual(len(self.lut.packed), 32 * 32 * 8)
        self.assertEqual(data_sha256(self.lut.packed), self.lut.manifest["dataSHA256"])
        self.assertEqual(cone_node(0), 0.0)
        self.assertEqual(cone_node(7), 1.0)
        self.assertEqual(no_v_node(0), 0.5 / 32.0)
        self.assertEqual(roughness_node(31), 31.5 / 32.0)

    def test_linear_index_golden_no_v_fastest_then_roughness_then_cone(self) -> None:
        self.assertEqual(flat_index(0, 0, 0), 0)
        self.assertEqual(flat_index(31, 0, 0), 31)
        self.assertEqual(flat_index(0, 1, 0), 32)
        self.assertEqual(flat_index(0, 0, 1), 32 * 32)
        self.assertEqual(flat_index(31, 31, 7), 32 * 32 * 8 - 1)
        for no_v, roughness, cone in ((3, 5, 2), (17, 29, 6), (31, 8, 4)):
            expected = no_v + 32 * (roughness + 32 * cone)
            self.assertEqual(flat_index(no_v, roughness, cone), expected)

    def test_generated_c_one_plane_is_zero_and_c_zero_is_unoccluded(self) -> None:
        for roughness_index in (0, 7, 15, 31):
            for view_index in (0, 7, 15, 31):
                zero = self.lut.texel(view_index, roughness_index, 7)
                self.assertEqual(zero, AB(0.0, 0.0))
                expected = integrate_ue_scalar(
                    no_v_node(view_index), roughness_node(roughness_index), 0.0, 128
                )
                actual = self.lut.texel(view_index, roughness_index, 0)
                self.assertLessEqual(abs(actual.a - expected.a), 0.5 / 65535.0 + 1e-15)
                self.assertLessEqual(abs(actual.b - expected.b), 0.5 / 65535.0 + 1e-15)

    def test_manual_trilinear_matches_sampler(self) -> None:
        no_v, roughness, cone = 0.347, 0.621, 0.43
        # Explicitly reproduce center-axis coordinate mapping, separately from
        # PackedConeEnvLUT.sample.
        coords = []
        for value, size, centered in (
            (no_v, 32, True),
            (roughness, 32, True),
            (cone, 8, False),
        ):
            coordinate = value * size - 0.5 if centered else value * (size - 1)
            coordinate = min(max(coordinate, 0.0), size - 1.0)
            lower = int(math.floor(coordinate))
            coords.append((lower, min(lower + 1, size - 1), coordinate - lower))
        vx, ry, cz = coords
        expected_a = 0.0
        expected_b = 0.0
        for iz, wz in ((cz[0], 1 - cz[2]), (cz[1], cz[2])):
            for iy, wy in ((ry[0], 1 - ry[2]), (ry[1], ry[2])):
                for ix, wx in ((vx[0], 1 - vx[2]), (vx[1], vx[2])):
                    value = unpack_rg16(self.packed[flat_index(ix, iy, iz)])
                    expected_a += wx * wy * wz * value.a
                    expected_b += wx * wy * wz * value.b
        actual = self.lut.sample(no_v, roughness, cone)
        self.assertAlmostEqual(actual.a, expected_a, places=15)
        self.assertAlmostEqual(actual.b, expected_b, places=15)

    def test_generated_lut_is_monotone_and_held_out_error_is_reported(self) -> None:
        backend = "numpy" if np is not None else "scalar"
        report = validate_lut(self.lut, random_points=48, reference_samples=4096, backend=backend)
        self.assertEqual(report["monotonicity"]["violationCount"], 0)
        for channel in ("A", "B"):
            metrics = report["heldOutRandom"][channel]
            self.assertEqual(metrics["count"], 48)
            self.assertTrue(all(math.isfinite(metrics[key]) for key in ("mean", "p95", "p99", "max")))
            self.assertGreaterEqual(metrics["max"], metrics["mean"])
        repeated = validate_lut(
            self.lut, random_points=48, reference_samples=4096, backend=backend
        )
        self.assertEqual(report["heldOutRandom"], repeated["heldOutRandom"])

    def test_malformed_manifest_and_payload_fail_closed(self) -> None:
        original = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            include = root / "x.ush"
            include.write_text(render_include(self.packed), encoding="utf-8")
            for key, bad in (
                ("dimensions", [32, 32, 7]),
                ("coneAcceptance", "NoL_ge_AdjustedCosCone"),
                ("quantization", "RG8_UNORM"),
                ("dataSHA256", "0" * 64),
            ):
                manifest = dict(original)
                manifest[key] = bad
                path = root / f"{key}.json"
                path.write_text(json.dumps(manifest), encoding="utf-8")
                with self.subTest(key=key), self.assertRaises(ValueError):
                    PackedConeEnvLUT.load(path, include)


if __name__ == "__main__":
    unittest.main()
