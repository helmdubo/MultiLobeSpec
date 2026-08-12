from __future__ import annotations

import json
import hashlib
import math
import tempfile
import unittest
from pathlib import Path

from mls_validation import (
    LUTLayout,
    PackedLUT,
    ReferenceInput,
    ValidationConfig,
    analytic_smooth_limit,
    integrate_reference,
    integrate_reference_numpy,
    integrate_reference_scalar,
    numpy_available,
    sample_ggx_vndf_dupuy_benyoub_isotropic,
    step_positive,
    run_validation,
    xor_scrambled_hammersley32,
)


class ReferenceMathTests(unittest.TestCase):
    def test_hammersley_is_deterministic_and_inside_open_unit_square(self) -> None:
        first = [xor_scrambled_hammersley32(i, 16, 1337) for i in range(16)]
        second = [xor_scrambled_hammersley32(i, 16, 1337) for i in range(16)]
        self.assertEqual(first, second)
        self.assertTrue(all(0.0 < x < 1.0 and 0.0 < y < 1.0 for x, y in first))
        self.assertEqual(len(set(first)), 16)

    def test_literal_sampler_returns_unit_upper_normal(self) -> None:
        view = (math.sqrt(1.0 - 0.3 * 0.3), 0.0, 0.3)
        for index in range(64):
            micro_normal = sample_ggx_vndf_dupuy_benyoub_isotropic(
                view, 0.49, xor_scrambled_hammersley32(index, 64, 7)
            )
            length = math.sqrt(sum(component * component for component in micro_normal))
            self.assertAlmostEqual(length, 1.0, places=12)
            self.assertGreater(micro_normal[2], 0.0)

    def test_step_positive_is_strict(self) -> None:
        self.assertEqual(step_positive(-1.0), 0.0)
        self.assertEqual(step_positive(0.0), 0.0)
        self.assertEqual(step_positive(1.0e-300), 1.0)

    def test_endpoints_are_analytic_and_exact(self) -> None:
        for visibility, expected in ((0.0, 0.0), (1.0, 1.0)):
            result = integrate_reference(
                ReferenceInput(visibility, 0.6, 0.4, 0.2), sample_count=1
            )
            self.assertEqual(result.value, expected)

    def test_zero_roughness_matches_strict_smooth_limit(self) -> None:
        for visibility, no_l in ((0.1, 0.8), (0.1, 0.99), (0.8, 0.3), (0.8, 0.7)):
            point = ReferenceInput(visibility, 0.0, no_l, 0.4)
            actual = integrate_reference(point, sample_count=32).value
            self.assertEqual(actual, analytic_smooth_limit(visibility, no_l))

    @unittest.skipUnless(numpy_available(), "NumPy is optional")
    def test_numpy_backend_matches_scalar_representative_grazing_and_azimuth(self) -> None:
        points = (
            ReferenceInput(0.37, 0.61, 0.72, 0.44, 0.0),
            ReferenceInput(0.12, 0.17, 0.94, 1.0e-6, math.pi / 4.0),
            ReferenceInput(0.83, 1.0, 0.03, 0.02, math.pi),
            ReferenceInput(0.25, 0.1, 0.999999, 0.999999, 2.0 * math.pi),
            ReferenceInput(0.61, 0.47, 0.45, 0.0, math.radians(135.0)),
        )
        for point in points:
            with self.subTest(point=point):
                scalar = integrate_reference_scalar(point, 4096, 0xC0FFEE11)
                vectorized = integrate_reference_numpy(point, 4096, 0xC0FFEE11)
                self.assertEqual(
                    scalar.positive_hemisphere_samples,
                    vectorized.positive_hemisphere_samples,
                )
                self.assertAlmostEqual(scalar.value, vectorized.value, delta=1.0e-12)
                self.assertTrue(
                    math.isclose(
                        scalar.cone_sum,
                        vectorized.cone_sum,
                        rel_tol=5.0e-13,
                        abs_tol=1.0e-12,
                    )
                )
                self.assertTrue(
                    math.isclose(
                        scalar.hemisphere_sum,
                        vectorized.hemisphere_sum,
                        rel_tol=5.0e-13,
                        abs_tol=1.0e-12,
                    )
                )

    @unittest.skipUnless(numpy_available(), "NumPy is optional")
    def test_numpy_backend_preserves_exact_endpoints_and_smooth_step_boundary(self) -> None:
        self.assertEqual(
            integrate_reference(ReferenceInput(0.0, 0.5, 0.2, 0.0), 8, backend="numpy").value,
            0.0,
        )
        self.assertEqual(
            integrate_reference(ReferenceInput(1.0, 0.5, 0.0, 0.0), 8, backend="numpy").value,
            1.0,
        )
        boundary = ReferenceInput(0.75, 0.0, 0.5, 0.5)
        self.assertEqual(integrate_reference(boundary, 8, backend="numpy").value, 0.0)

    @unittest.skipUnless(numpy_available(), "NumPy is optional")
    def test_symmetric_ratio_layout_points_preserve_scalar_vector_parity(self) -> None:
        layout = LUTLayout(
            (7, 9, 5),
            (0.1, 0.4, 0.7, 1.0),
            visibility_warp="symmetricRatioSquare",
            no_l_warp="smoothLimitBoundarySignedSquare",
        )
        cases = (
            layout.physical_node(0, 4, 0, 1),
            layout.physical_node(1, 2, 1, 1),
            layout.physical_node(3, 4, 2, 1),
            layout.physical_node(5, 6, 3, 1),
            layout.physical_node(6, 4, 4, 1),
        )
        for base in cases:
            point = ReferenceInput(
                base.visibility,
                base.roughness,
                base.no_l,
                base.no_v,
                math.radians(135.0),
            )
            with self.subTest(point=point):
                scalar = integrate_reference_scalar(point, 4096, 0xC0FFEE11)
                vectorized = integrate_reference_numpy(point, 4096, 0xC0FFEE11)
                self.assertEqual(
                    scalar.positive_hemisphere_samples,
                    vectorized.positive_hemisphere_samples,
                )
                self.assertAlmostEqual(scalar.value, vectorized.value, delta=1.0e-12)

    @unittest.skipUnless(numpy_available(), "NumPy is optional")
    def test_v2_banked_physical_nodes_preserve_scalar_vector_parity(self) -> None:
        layout = LUTLayout(
            (7, 9, 5),
            (0.1, 0.18, 0.3, 0.45, 0.6, 0.75, 1.0),
            visibility_warp="symmetricRatioSquare",
            no_l_warp="smoothLimitBoundarySignedSquare",
            roughness_banks=((0, 1, 2, 3), (3, 4, 5, 6)),
            roughness_bank_split=0.45,
            roughness_bank_selection="SingleBankPiecewise",
        )
        cases = (
            layout.physical_node(1, 2, 1, 0, 0),
            layout.physical_node(3, 4, 2, 3, 0),
            layout.physical_node(3, 4, 2, 0, 1),
            layout.physical_node(5, 6, 3, 3, 1),
        )
        for point in cases:
            with self.subTest(point=point):
                scalar = integrate_reference_scalar(point, 4096, 0xC0FFEE11)
                vectorized = integrate_reference_numpy(point, 4096, 0xC0FFEE11)
                self.assertEqual(
                    scalar.positive_hemisphere_samples,
                    vectorized.positive_hemisphere_samples,
                )
                self.assertAlmostEqual(scalar.value, vectorized.value, delta=1.0e-12)


class PackedLUTTests(unittest.TestCase):
    def _make_lut(
        self,
        root: Path,
        linear_axes: bool = False,
        boundary_no_l: bool = False,
        symmetric_visibility: bool = False,
    ) -> PackedLUT:
        manifest = {
            "algorithm": "GenericVNDFBasedMicroShadowing",
            "distribution": "IsotropicGGX",
            "vndfSampler": "DupuyBenyoub2023",
            "smithCorrelation": "HeightCorrelated",
            "visibilityWeighting": "CosineWeightedProjectedSolidAngle",
            "relativeAzimuth": "coplanar_same_azimuth",
            "dimensions": [2, 2, 2],
            "roughnessNodes": [0.1, 0.4, 0.7, 1.0],
            "axisWarp": (
                {
                    "visibility": (
                        "symmetricRatioSquare" if symmetric_visibility else "square"
                    ),
                    "NoL": "smoothLimitBoundarySignedSquare",
                    "NoV": "square",
                }
                if boundary_no_l
                else (
                    {"visibility": "linear", "NoL": "linear", "NoV": "linear"}
                    if linear_axes
                    else {"visibility": "square", "NoL": "oneMinusSquare", "NoV": "square"}
                )
            ),
            "roughnessInterpolation": "roughness",
            "linearIndexOrder": ["Visibility", "NoL", "NoV"],
            "quantization": "RGBA8_UNORM",
        }
        manifest_path = root / "fixture.manifest.json"
        include_path = root / "fixture.ush"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

        values = []
        for iz in range(2):
            for iy in range(2):
                for ix in range(2):
                    channels = [10 + 20 * ix + 30 * iy + 40 * iz + channel for channel in range(4)]
                    values.append(sum(value << (8 * channel) for channel, value in enumerate(channels)))
        include_path.write_text(
            "static const uint MLS_VNDF_LUT_PACKED[8] = {\n"
            + ",\n".join(f"0x{value:08X}u" for value in values)
            + "\n};\n",
            encoding="utf-8",
        )
        return PackedLUT.load(manifest_path, include_path)

    def _make_v2_lut(
        self,
        root: Path,
        manifest_changes: dict | None = None,
        packed_count: int = 16,
        interpolation: str = "roughness",
    ) -> PackedLUT:
        manifest = {
            "algorithm": "GenericVNDFBasedMicroShadowing",
            "distribution": "IsotropicGGX",
            "vndfSampler": "DupuyBenyoub2023",
            "smithCorrelation": "HeightCorrelated",
            "visibilityWeighting": "CosineWeightedProjectedSolidAngle",
            "relativeAzimuth": "coplanar_same_azimuth",
            "dimensions": [2, 2, 2],
            "roughnessNodes": [0.1, 0.18, 0.3, 0.45, 0.6, 0.75, 1.0],
            "roughnessBanks": [[0, 1, 2, 3], [3, 4, 5, 6]],
            "roughnessBankSplit": 0.45,
            "roughnessBankSelection": "SingleBankPiecewise",
            "axisWarp": {
                "visibility": "symmetricRatioSquare",
                "NoL": "smoothLimitBoundarySignedSquare",
                "NoV": "square",
            },
            "roughnessInterpolation": interpolation,
            "linearIndexOrder": ["Visibility", "NoL", "NoV"],
            "quantization": "RGBA8_UNORM",
            "channelPacking": "RGBA_LittleEndian",
        }
        if manifest_changes:
            for key, value in manifest_changes.items():
                if value is None:
                    manifest.pop(key, None)
                else:
                    manifest[key] = value
        manifest_path = root / "fixture-v2.manifest.json"
        include_path = root / "fixture-v2.ush"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

        values = []
        for bank in range(2):
            for iz in range(2):
                for iy in range(2):
                    for ix in range(2):
                        channels = [
                            10
                            + bank * 100
                            + 20 * ix
                            + 30 * iy
                            + 40 * iz
                            + channel
                            for channel in range(4)
                        ]
                        values.append(
                            sum(value << (8 * channel) for channel, value in enumerate(channels))
                        )
        values = values[:packed_count]
        include_path.write_text(
            f"static const uint MLS_VNDF_LUT_PACKED[{len(values)}] = {{\n"
            + ",\n".join(f"0x{value:08X}u" for value in values)
            + "\n};\n",
            encoding="utf-8",
        )
        return PackedLUT.load(manifest_path, include_path)

    def test_load_and_sample_manual_trilinear(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            lut = self._make_lut(Path(temp))
            # These physical coordinates map to t=(0.5, 0.5, 0.5) under the
            # three required runtime warps.  Roughness 0.25 is halfway R/G.
            point = ReferenceInput(0.25, 0.25, 0.75, 0.25)
            actual = lut.sample_interpolated(point)
            expected_byte = 10 + 10 + 15 + 20 + 0.5
            self.assertAlmostEqual(actual, expected_byte / 255.0, places=14)

    def test_runtime_endpoints_earlyout_and_cpu_crosscheck(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            lut = self._make_lut(Path(temp))
            self.assertEqual(lut.sample_runtime(ReferenceInput(0.0, 0.5, 1.0, 0.5)), 0.0)
            self.assertEqual(lut.sample_runtime(ReferenceInput(1.0, 0.5, 0.0, 0.5)), 1.0)
            self.assertEqual(lut.sample_runtime(ReferenceInput(0.1, 0.5, 0.1, 0.5)), 0.0)
            point = ReferenceInput(0.4, 0.55, 0.7, 0.3)
            self.assertAlmostEqual(lut.sample_runtime(point), lut.sample_cpu_bruteforce(point), places=15)

    def test_linear_manifest_manual_trilinear_and_cpu_parity(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            lut = self._make_lut(Path(temp), linear_axes=True)
            self.assertEqual(lut.layout.visibility_warp, "linear")
            self.assertEqual(lut.layout.no_l_warp, "linear")
            self.assertEqual(lut.layout.no_v_warp, "linear")
            point = ReferenceInput(0.5, 0.25, 0.5, 0.5)
            actual = lut.sample_interpolated(point)
            expected_byte = 10 + 10 + 15 + 20 + 0.5
            self.assertAlmostEqual(actual, expected_byte / 255.0, places=14)
            runtime_point = ReferenceInput(0.55, 0.55, 0.65, 0.35)
            self.assertAlmostEqual(
                lut.sample_runtime(runtime_point),
                lut.sample_cpu_bruteforce(runtime_point),
                places=15,
            )

    def test_smooth_limit_boundary_manifest_mapping_and_trilinear_parity(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            lut = self._make_lut(Path(temp), boundary_no_l=True)
            self.assertEqual(lut.layout.no_l_warp, "smoothLimitBoundarySignedSquare")

            visibility = 0.25
            boundary = math.sqrt(1.0 - visibility)
            point = ReferenceInput(visibility, 0.25, boundary, 0.25)
            actual = lut.sample_interpolated(point)
            expected_byte = 10 + 10 + 15 + 20 + 0.5
            self.assertAlmostEqual(actual, expected_byte / 255.0, places=14)

            for signed in (-0.75, -0.25, 0.0, 0.25, 0.75):
                runtime_t = 0.5 + 0.5 * signed
                no_l = (
                    boundary * (1.0 - signed * signed)
                    if signed < 0.0
                    else boundary + (1.0 - boundary) * signed * signed
                )
                _, _, fraction = lut.layout._coordinate(
                    no_l, 2, "no_l", visibility=visibility
                )
                self.assertAlmostEqual(fraction, runtime_t, places=14)

            runtime_point = ReferenceInput(0.36, 0.55, 0.60, 0.49)
            self.assertAlmostEqual(
                lut.sample_runtime(runtime_point),
                lut.sample_cpu_bruteforce(runtime_point),
                places=15,
            )

    def test_smooth_limit_boundary_physical_nodes_round_trip(self) -> None:
        layout = LUTLayout(
            (5, 7, 4),
            (0.1, 0.4, 0.7, 1.0),
            no_l_warp="smoothLimitBoundarySignedSquare",
        )
        for ix in range(layout.dimensions[0]):
            for iy in range(layout.dimensions[1]):
                point = layout.physical_node(ix, iy, 0, 0)
                signed = 2.0 * iy / (layout.dimensions[1] - 1) - 1.0
                boundary = math.sqrt(1.0 - point.visibility)
                # At B=1 the positive interval has zero length; at B=0 the
                # negative interval has zero length. Multiple grid nodes then
                # intentionally collapse to the same physical endpoint.
                if (boundary == 1.0 and signed > 0.0) or (
                    boundary == 0.0 and signed < 0.0
                ):
                    continue
                lower, upper, fraction = layout._coordinate(
                    point.no_l,
                    layout.dimensions[1],
                    "no_l",
                    visibility=point.visibility,
                )
                reconstructed_t = (lower + fraction) / (layout.dimensions[1] - 1)
                self.assertAlmostEqual(
                    reconstructed_t,
                    iy / (layout.dimensions[1] - 1),
                    places=12,
                )

    def test_symmetric_ratio_visibility_manifest_round_trip_and_trilinear(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            lut = self._make_lut(
                Path(temp),
                boundary_no_l=True,
                symmetric_visibility=True,
            )
            self.assertEqual(lut.layout.visibility_warp, "symmetricRatioSquare")
            self.assertEqual(lut.layout.no_l_warp, "smoothLimitBoundarySignedSquare")

            # At T=0.5 the symmetric forward map is V=0.5. NoL at its own
            # T=0.5 is the smooth-limit boundary B=sqrt(1-V).
            visibility = 0.5
            boundary = math.sqrt(1.0 - visibility)
            point = ReferenceInput(visibility, 0.25, boundary, 0.25)
            expected_byte = 10 + 10 + 15 + 20 + 0.5
            self.assertAlmostEqual(
                lut.sample_interpolated(point), expected_byte / 255.0, places=14
            )
            self.assertAlmostEqual(
                lut.sample_runtime(point), lut.sample_cpu_bruteforce(point), places=15
            )

        layout = LUTLayout(
            (9, 7, 4),
            (0.1, 0.4, 0.7, 1.0),
            visibility_warp="symmetricRatioSquare",
            no_l_warp="smoothLimitBoundarySignedSquare",
        )
        for ix in range(layout.dimensions[0]):
            point = layout.physical_node(ix, 3, 0, 0)
            lower, upper, fraction = layout._coordinate(
                point.visibility, layout.dimensions[0], "visibility"
            )
            reconstructed_t = (lower + fraction) / (layout.dimensions[0] - 1)
            self.assertAlmostEqual(
                reconstructed_t,
                ix / (layout.dimensions[0] - 1),
                places=14,
            )
            self.assertTrue(math.isfinite(point.visibility))
        self.assertEqual(layout.physical_node(0, 3, 0, 0).visibility, 0.0)
        self.assertEqual(layout.physical_node(8, 3, 0, 0).visibility, 1.0)

    def test_v2_single_bank_piecewise_split_endpoints_and_manual_trilinear(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            lut = self._make_v2_lut(Path(temp))
            self.assertEqual(lut.layout.bank_count, 2)
            self.assertEqual(lut.layout.roughness_banks, ((0, 1, 2, 3), (3, 4, 5, 6)))

            visibility = 0.5
            no_l = math.sqrt(1.0 - visibility)
            common = dict(visibility=visibility, no_l=no_l, no_v=0.25)
            # Spatial center contributes 45 byte units. R=.1 is low R;
            # equality at split must still select low A; R=1 selects high A.
            low = lut.sample_interpolated(ReferenceInput(roughness=0.1, **common))
            split = lut.sample_interpolated(ReferenceInput(roughness=0.45, **common))
            high = lut.sample_interpolated(ReferenceInput(roughness=1.0, **common))
            self.assertAlmostEqual(low, (10 + 45) / 255.0, places=14)
            self.assertAlmostEqual(split, (13 + 45) / 255.0, places=14)
            self.assertAlmostEqual(high, (113 + 45) / 255.0, places=14)

            # R=.525 is halfway from high-bank R=.45 (R) to R=.6 (G).
            halfway = ReferenceInput(roughness=0.525, **common)
            expected = (110 + 45 + 0.5) / 255.0
            self.assertAlmostEqual(lut.sample_interpolated(halfway), expected, places=14)
            self.assertAlmostEqual(
                lut.sample_runtime(halfway), lut.sample_cpu_bruteforce(halfway), places=15
            )

            split_bank, split_stencil = lut.layout.roughness_stencil(0.45)
            above_bank, _ = lut.layout.roughness_stencil(math.nextafter(0.45, 1.0))
            self.assertEqual(split_bank, 0)
            self.assertEqual(split_stencil, [(3, 1.0)])
            self.assertEqual(above_bank, 1)

            # The declared payload hash covers both complete bank-major blocks.
            manifest = json.loads(lut.manifest_path.read_text(encoding="utf-8"))
            manifest["dataSHA256"] = hashlib.sha256(
                b"".join(value.to_bytes(4, "little") for value in lut.packed)
            ).hexdigest()
            lut.manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            hashed = PackedLUT.load(lut.manifest_path, lut.include_path)
            self.assertTrue(hashed.hash_report["verified"])
            self.assertEqual(
                hashed.hash_report["matching_interpretations"],
                ["packed_rgba_bytes_little_endian"],
            )

    def test_v2_alpha_interpolation_stays_inside_selected_bank(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            lut = self._make_v2_lut(Path(temp), interpolation="alpha")
            bank, stencil = lut.layout.roughness_stencil(
                math.sqrt((0.45 * 0.45 + 0.6 * 0.6) * 0.5)
            )
            self.assertEqual(bank, 1)
            self.assertAlmostEqual(stencil[0][1], 0.5, places=14)
            self.assertAlmostEqual(stencil[1][1], 0.5, places=14)

    def test_v2_malformed_schema_is_rejected(self) -> None:
        malformed = (
            {"roughnessBanks": None},
            {"roughnessBanks": [[0, 1, 2, 3], [2, 4, 5, 6]]},
            {"roughnessBanks": [[0.0, 1, 2, 3], [3, 4, 5, 6]]},
            {"roughnessBankSplit": 0.449},
            {"roughnessBankSplit": "0.45"},
            {"roughnessBankSelection": "BlendBanks"},
            {"roughnessNodes": [0.1, 0.18, 0.3, 0.45, 0.6, 1.0]},
        )
        for index, changes in enumerate(malformed):
            with self.subTest(changes=changes), tempfile.TemporaryDirectory() as temp:
                with self.assertRaises(ValueError):
                    self._make_v2_lut(Path(temp), changes)
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "dimensions times bank count"):
                self._make_v2_lut(Path(temp), packed_count=8)

    def test_layout_flat_index_visibility_fastest(self) -> None:
        layout = LUTLayout((2, 3, 4), (0.1, 0.4, 0.7, 1.0))
        self.assertEqual(layout.flat_index(1, 0, 0), 1)
        self.assertEqual(layout.flat_index(0, 1, 0), 2)
        self.assertEqual(layout.flat_index(0, 0, 1), 6)

    def test_loaded_artifact_pipeline_writes_json_and_reference_csv(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lut = self._make_lut(root)
            config = ValidationConfig(
                profile="smoke",
                random_points=2,
                boundary_points=1,
                reference_samples=8,
                boundary_reference_samples=8,
                smooth_samples=8,
                azimuth_base_points=1,
                azimuth_random_phis=0,
                quantization_points=1,
                float_reconstruction_samples=8,
            )
            output = root / "report"
            report, rows = run_validation(config, output, lut)
            self.assertEqual(report["artifact"]["status"], "loaded")
            self.assertIn(
                report["implementation"]["reference_backend_resolved"],
                {"numpy", "scalar"},
            )
            self.assertTrue(rows)
            self.assertTrue((output / "report.json").is_file())
            self.assertTrue((output / "reference.csv").is_file())


if __name__ == "__main__":
    unittest.main()
