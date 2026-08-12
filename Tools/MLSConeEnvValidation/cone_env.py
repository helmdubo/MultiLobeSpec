"""Independent CARD-09 cone-aware EnvBRDF oracle and packed LUT support."""

from __future__ import annotations

import hashlib
import json
import math
import random
import re
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence

try:
    import numpy as np
except ImportError:
    np = None


DIMENSIONS = (32, 32, 8)  # NoV, perceptual Roughness, AdjustedCosCone
DEFAULT_SAMPLES = 128
ARRAY_NAME = "MLS_CONE_ENVBRDF_PACKED"
ARRAY_NAME4 = "MLS_CONE_ENVBRDF_PACKED4"
ARRAY_PLANE_PREFIX = "MLS_CONE_ENVBRDF_PLANE_C"
UINT32_SCALE = 1.0 / 4294967296.0


def clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    return min(max(value, low), high)


def reverse_bits32(bits: int) -> int:
    bits &= 0xFFFFFFFF
    bits = ((bits << 16) | (bits >> 16)) & 0xFFFFFFFF
    bits = (((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8)) & 0xFFFFFFFF
    bits = (((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4)) & 0xFFFFFFFF
    bits = (((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2)) & 0xFFFFFFFF
    bits = (((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1)) & 0xFFFFFFFF
    return bits


@dataclass(frozen=True)
class AB:
    a: float
    b: float


def integrate_ue_scalar(
    no_v: float,
    roughness: float,
    adjusted_cos_cone: float,
    sample_count: int = DEFAULT_SAMPLES,
) -> AB:
    """UE 5.7 SystemTextures.cpp PreintegratedGF estimator with cone rejection.

    The unmodified estimator accepts ``NoL > 0``. The cone-aware form accepts
    ``NoL > AdjustedCosCone`` using the same strict comparison and divides both
    accumulators by the common total sample count, never the accepted count.
    """

    if sample_count <= 0:
        raise ValueError("sample_count must be positive")
    no_v = clamp(no_v)
    roughness = clamp(roughness)
    cone = clamp(adjusted_cos_cone)
    view_x = math.sqrt(max(1.0 - no_v * no_v, 0.0))
    m = roughness * roughness
    m2 = m * m
    sum_a = 0.0
    sum_b = 0.0
    for index in range(sample_count):
        e1 = index / sample_count
        e2 = reverse_bits32(index) * UINT32_SCALE
        phi = 2.0 * math.pi * e1
        cos_theta = math.sqrt((1.0 - e2) / (1.0 + (m2 - 1.0) * e2))
        sin_theta = math.sqrt(max(1.0 - cos_theta * cos_theta, 0.0))
        h_x = sin_theta * math.cos(phi)
        h_y = sin_theta * math.sin(phi)
        h_z = cos_theta
        v_dot_h = max(view_x * h_x + no_v * h_z, 0.0)
        l_z = 2.0 * v_dot_h * h_z - no_v
        no_l = max(l_z, 0.0)
        no_h = max(h_z, 0.0)
        # Strict threshold preserves c=0 parity with UE's strict NoL > 0.
        if no_l > cone:
            vis_smith_v = no_l * (no_v * (1.0 - m) + m)
            vis_smith_l = no_v * (no_l * (1.0 - m) + m)
            denominator = vis_smith_v + vis_smith_l
            if denominator <= 0.0 or no_h <= 0.0:
                continue
            visibility = 0.5 / denominator
            no_l_vis_pdf = no_l * visibility * (4.0 * v_dot_h / no_h)
            fresnel_complement = 1.0 - v_dot_h
            fresnel_complement *= (fresnel_complement * fresnel_complement) ** 2
            sum_a += no_l_vis_pdf * (1.0 - fresnel_complement)
            sum_b += no_l_vis_pdf * fresnel_complement
    inverse_count = 1.0 / sample_count
    return AB(clamp(sum_a * inverse_count), clamp(sum_b * inverse_count))


def _reverse_bits32_numpy(bits):
    bits = ((bits << np.uint32(16)) | (bits >> np.uint32(16))).astype(np.uint32)
    bits = (((bits & np.uint32(0x00FF00FF)) << np.uint32(8)) | ((bits & np.uint32(0xFF00FF00)) >> np.uint32(8))).astype(np.uint32)
    bits = (((bits & np.uint32(0x0F0F0F0F)) << np.uint32(4)) | ((bits & np.uint32(0xF0F0F0F0)) >> np.uint32(4))).astype(np.uint32)
    bits = (((bits & np.uint32(0x33333333)) << np.uint32(2)) | ((bits & np.uint32(0xCCCCCCCC)) >> np.uint32(2))).astype(np.uint32)
    bits = (((bits & np.uint32(0x55555555)) << np.uint32(1)) | ((bits & np.uint32(0xAAAAAAAA)) >> np.uint32(1))).astype(np.uint32)
    return bits


_NUMPY_SEQUENCE_CACHE: dict[int, tuple[object, object, object]] = {}


def _numpy_sequence(sample_count: int):
    cached = _NUMPY_SEQUENCE_CACHE.get(sample_count)
    if cached is not None:
        return cached
    indices = np.arange(sample_count, dtype=np.uint32)
    e1 = indices.astype(np.float64) / float(sample_count)
    e2 = _reverse_bits32_numpy(indices).astype(np.float64) * UINT32_SCALE
    phi = 2.0 * math.pi * e1
    cached = (np.cos(phi), np.sin(phi), e2)
    _NUMPY_SEQUENCE_CACHE[sample_count] = cached
    return cached


def integrate_ue_numpy(
    no_v: float,
    roughness: float,
    adjusted_cos_cone: float,
    sample_count: int = DEFAULT_SAMPLES,
) -> AB:
    if np is None:
        raise ValueError("NumPy backend requested but NumPy is not installed")
    if sample_count <= 0:
        raise ValueError("sample_count must be positive")
    no_v = clamp(no_v)
    roughness = clamp(roughness)
    cone = clamp(adjusted_cos_cone)
    view_x = math.sqrt(max(1.0 - no_v * no_v, 0.0))
    m = roughness * roughness
    m2 = m * m
    cos_phi, _, e2 = _numpy_sequence(sample_count)
    cos_theta = np.sqrt((1.0 - e2) / (1.0 + (m2 - 1.0) * e2))
    sin_theta = np.sqrt(np.maximum(1.0 - cos_theta * cos_theta, 0.0))
    h_x = sin_theta * cos_phi
    v_dot_h = np.maximum(view_x * h_x + no_v * cos_theta, 0.0)
    no_l = np.maximum(2.0 * v_dot_h * cos_theta - no_v, 0.0)
    accepted = no_l > cone
    vis_smith_v = no_l * (no_v * (1.0 - m) + m)
    vis_smith_l = no_v * (no_l * (1.0 - m) + m)
    denominator = vis_smith_v + vis_smith_l
    valid = accepted & (denominator > 0.0) & (cos_theta > 0.0)
    no_l_vis_pdf = np.zeros_like(no_l)
    no_l_vis_pdf[valid] = (
        no_l[valid]
        * (0.5 / denominator[valid])
        * (4.0 * v_dot_h[valid] / cos_theta[valid])
    )
    fc = 1.0 - v_dot_h
    fc = fc * (fc * fc) ** 2
    return AB(
        clamp(float(np.sum(no_l_vis_pdf * (1.0 - fc), dtype=np.float64)) / sample_count),
        clamp(float(np.sum(no_l_vis_pdf * fc, dtype=np.float64)) / sample_count),
    )


def resolve_backend(requested: str) -> str:
    requested = requested.lower()
    if requested == "auto":
        return "numpy" if np is not None else "scalar"
    if requested == "numpy" and np is None:
        raise ValueError("NumPy backend requested but NumPy is not installed")
    if requested not in {"numpy", "scalar"}:
        raise ValueError(f"unknown backend {requested!r}")
    return requested


def integrate_ue(
    no_v: float,
    roughness: float,
    adjusted_cos_cone: float,
    sample_count: int = DEFAULT_SAMPLES,
    backend: str = "auto",
) -> AB:
    return (
        integrate_ue_numpy(no_v, roughness, adjusted_cos_cone, sample_count)
        if resolve_backend(backend) == "numpy"
        else integrate_ue_scalar(no_v, roughness, adjusted_cos_cone, sample_count)
    )


def adjust_specular_cone(cos_cone: float, roughness: float, no_v: float) -> float:
    cos_cone = clamp(cos_cone)
    roughness = clamp(roughness)
    no_v = clamp(no_v)
    gloss = clamp((1.0 - roughness) * 1.5)
    gloss2 = gloss * gloss
    gloss4 = gloss2 * gloss2
    gloss8 = gloss4 * gloss4
    one_minus_no_v2 = (1.0 - no_v) * (1.0 - no_v)
    one_minus_no_v4 = one_minus_no_v2 * one_minus_no_v2
    return clamp(cos_cone * (1.0 - gloss8) * (1.0 - one_minus_no_v4))


def apply_ab(f0: float, value: AB) -> float:
    return clamp(f0) * value.a + value.b


def no_v_node(index: int) -> float:
    return (index + 0.5) / DIMENSIONS[0]


def roughness_node(index: int) -> float:
    return (index + 0.5) / DIMENSIONS[1]


def cone_node(index: int) -> float:
    return index / (DIMENSIONS[2] - 1)


def flat_index(no_v_index: int, roughness_index: int, cone_index: int) -> int:
    nx, nr, nc = DIMENSIONS
    if not (0 <= no_v_index < nx and 0 <= roughness_index < nr and 0 <= cone_index < nc):
        raise IndexError("LUT index out of bounds")
    return no_v_index + nx * (roughness_index + nr * cone_index)


def pack_rg16(value: AB) -> int:
    a = int(clamp(value.a) * 65535.0 + 0.5)
    b = int(clamp(value.b) * 65535.0 + 0.5)
    return a | (b << 16)


def unpack_rg16(packed: int) -> AB:
    return AB((packed & 0xFFFF) / 65535.0, ((packed >> 16) & 0xFFFF) / 65535.0)


def generate_packed(sample_count: int = DEFAULT_SAMPLES, backend: str = "auto") -> list[int]:
    backend = resolve_backend(backend)
    values = [0] * math.prod(DIMENSIONS)
    for cone_index in range(DIMENSIONS[2]):
        cone = cone_node(cone_index)
        for roughness_index in range(DIMENSIONS[1]):
            roughness = roughness_node(roughness_index)
            for view_index in range(DIMENSIONS[0]):
                values[flat_index(view_index, roughness_index, cone_index)] = pack_rg16(
                    integrate_ue(
                        no_v_node(view_index), roughness, cone, sample_count, backend
                    )
                )
    return values


def data_sha256(packed: Sequence[int]) -> str:
    return hashlib.sha256(
        b"".join(int(value).to_bytes(4, "little") for value in packed)
    ).hexdigest()


def canonical_manifest(sample_count: int, packed: Sequence[int]) -> dict:
    return {
        "algorithm": "ConeAwareEnvBRDF",
        "referenceEstimator": "UE5.7_SystemTextures_PreintegratedGF",
        "dimensions": list(DIMENSIONS),
        "axisOrder": ["NoV", "Roughness", "AdjustedCosCone"],
        "NoVNodes": "center",
        "roughnessNodes": "center",
        "adjustedCosConeNodes": [cone_node(i) for i in range(DIMENSIONS[2])],
        "importanceSampling": "GGX_NDF_H",
        "coneAcceptance": "NoL_gt_AdjustedCosCone_Strict",
        "normalization": "DivideByCommonTotalSampleCount",
        "output": "AB_ScaleBiasAgainstF0",
        "quantization": "RG16_UNORM",
        "packing": "A_Low16_B_High16",
        "linearIndexOrder": ["NoV", "Roughness", "AdjustedCosCone"],
        "samplesPerCell": sample_count,
        "dataSHA256": data_sha256(packed),
        "generatorVersion": 1,
    }


def render_include(packed: Sequence[int]) -> str:
    if len(packed) % 4:
        raise ValueError("packed word count must be divisible by four")
    words_per_plane = DIMENSIONS[0] * DIMENSIONS[1]
    lines = ["// Generated by Tools/MLSConeEnvValidation/generate.py; do not hand-edit."]
    for cone in range(DIMENSIONS[2]):
        lines.append(f"static const uint4 {ARRAY_PLANE_PREFIX}{cone}[{words_per_plane // 4}] = {{")
        plane_start = cone * words_per_plane
        for offset in range(0, words_per_plane, 4):
            words = ", ".join(
                f"0x{x:08X}u" for x in packed[plane_start + offset : plane_start + offset + 4]
            )
            lines.append(f"    uint4({words}),")
        lines.append("};")
    lines.append("")
    return "\n".join(lines)


def parse_packed(text: str) -> list[int]:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\r\n]*", "", text)
    plane_pattern = re.compile(
        rf"{ARRAY_PLANE_PREFIX}([0-7])\s*\[[^\]]*\]\s*=\s*\{{(.*?)\}}\s*;",
        flags=re.DOTALL,
    )
    plane_matches = sorted(
        ((int(match.group(1)), match.group(2)) for match in plane_pattern.finditer(text)),
        key=lambda item: item[0],
    )
    if plane_matches:
        if [index for index, _ in plane_matches] != list(range(DIMENSIONS[2])):
            raise ValueError("packed cone planes must be exactly C0 through C7")
        body = "\n".join(value for _, value in plane_matches)
    else:
        match = re.search(
            rf"(?:{ARRAY_NAME4}|{ARRAY_NAME})\s*\[[^\]]*\]\s*=\s*\{{(.*?)\}}\s*;",
            text,
            flags=re.DOTALL,
        )
        if not match:
            raise ValueError(f"{ARRAY_NAME} initializer not found")
        body = match.group(1)
    tokens = re.findall(r"(?<![A-Za-z0-9_])(?:0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*", body)
    values = [int(re.sub(r"[uUlL]+$", "", token), 0) for token in tokens]
    if any(value < 0 or value > 0xFFFFFFFF for value in values):
        raise ValueError("packed value is outside uint32")
    return values


def _axis_coordinate(value: float, size: int, centered: bool) -> tuple[int, int, float]:
    value = clamp(value)
    coordinate = value * size - 0.5 if centered else value * (size - 1)
    coordinate = clamp(coordinate, 0.0, float(size - 1))
    lower = int(math.floor(coordinate))
    upper = min(lower + 1, size - 1)
    return lower, upper, coordinate - lower


@dataclass
class PackedConeEnvLUT:
    manifest_path: Path
    include_path: Path
    manifest: dict
    packed: list[int]
    hash_verified: bool

    @classmethod
    def load(cls, manifest_path: Path, include_path: Path) -> "PackedConeEnvLUT":
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        if not isinstance(manifest, dict):
            raise ValueError("manifest root must be an object")
        required = {
            "algorithm": "ConeAwareEnvBRDF",
            "referenceEstimator": "UE5.7_SystemTextures_PreintegratedGF",
            "dimensions": list(DIMENSIONS),
            "axisOrder": ["NoV", "Roughness", "AdjustedCosCone"],
            "NoVNodes": "center",
            "roughnessNodes": "center",
            "adjustedCosConeNodes": [cone_node(i) for i in range(DIMENSIONS[2])],
            "importanceSampling": "GGX_NDF_H",
            "coneAcceptance": "NoL_gt_AdjustedCosCone_Strict",
            "normalization": "DivideByCommonTotalSampleCount",
            "output": "AB_ScaleBiasAgainstF0",
            "quantization": "RG16_UNORM",
            "packing": "A_Low16_B_High16",
            "linearIndexOrder": ["NoV", "Roughness", "AdjustedCosCone"],
        }
        for key, expected in required.items():
            if manifest.get(key) != expected:
                raise ValueError(f"manifest {key} must be {expected!r}")
        packed = parse_packed(include_path.read_text(encoding="utf-8-sig"))
        if len(packed) != math.prod(DIMENSIONS):
            raise ValueError("packed texel count does not match 32*32*8")
        declared = str(manifest.get("dataSHA256", "")).lower()
        actual = data_sha256(packed)
        if not declared or declared != actual:
            raise ValueError("manifest dataSHA256 does not match packed RG16 payload")
        return cls(manifest_path, include_path, manifest, packed, True)

    def texel(self, no_v_index: int, roughness_index: int, cone_index: int) -> AB:
        return unpack_rg16(self.packed[flat_index(no_v_index, roughness_index, cone_index)])

    def sample(self, no_v: float, roughness: float, adjusted_cos_cone: float) -> AB:
        vx = _axis_coordinate(no_v, DIMENSIONS[0], True)
        ry = _axis_coordinate(roughness, DIMENSIONS[1], True)
        cz = _axis_coordinate(adjusted_cos_cone, DIMENSIONS[2], False)
        a = 0.0
        b = 0.0
        for iz, wz in ((cz[0], 1.0 - cz[2]), (cz[1], cz[2])):
            for iy, wy in ((ry[0], 1.0 - ry[2]), (ry[1], ry[2])):
                for ix, wx in ((vx[0], 1.0 - vx[2]), (vx[1], vx[2])):
                    weight = wx * wy * wz
                    if weight <= 0.0:
                        continue
                    value = self.texel(ix, iy, iz)
                    a += weight * value.a
                    b += weight * value.b
        return AB(clamp(a), clamp(b))


def error_metrics(errors: Sequence[float]) -> dict:
    if not errors:
        return {"count": 0, "mean": None, "p95": None, "p99": None, "max": None}
    ordered = sorted(errors)

    def percentile(q: float) -> float:
        position = (len(ordered) - 1) * q
        low = int(math.floor(position))
        high = int(math.ceil(position))
        fraction = position - low
        return ordered[low] * (1.0 - fraction) + ordered[high] * fraction

    return {
        "count": len(ordered),
        "mean": sum(ordered) / len(ordered),
        "p95": percentile(0.95),
        "p99": percentile(0.99),
        "max": ordered[-1],
    }


def validate_lut(
    lut: PackedConeEnvLUT,
    random_points: int,
    reference_samples: int,
    backend: str,
    seed: int = 0xC09E09,
) -> dict:
    backend = resolve_backend(backend)
    violations = []
    for roughness_index in range(DIMENSIONS[1]):
        for view_index in range(DIMENSIONS[0]):
            previous = lut.texel(view_index, roughness_index, 0)
            for cone_index in range(1, DIMENSIONS[2]):
                current = lut.texel(view_index, roughness_index, cone_index)
                if current.a > previous.a + 1.0 / 65535.0 or current.b > previous.b + 1.0 / 65535.0:
                    violations.append(
                        {
                            "NoVIndex": view_index,
                            "RoughnessIndex": roughness_index,
                            "ConeIndex": cone_index,
                            "previous": asdict(previous),
                            "current": asdict(current),
                        }
                    )
                previous = current

    rng = random.Random(seed)
    errors_a = []
    errors_b = []
    rows = []
    for index in range(random_points):
        no_v = rng.random()
        roughness = rng.random()
        cone = rng.random()
        sampled = lut.sample(no_v, roughness, cone)
        reference = integrate_ue(no_v, roughness, cone, reference_samples, backend)
        errors_a.append(abs(sampled.a - reference.a))
        errors_b.append(abs(sampled.b - reference.b))
        rows.append(
            {
                "case": index,
                "NoV": no_v,
                "Roughness": roughness,
                "AdjustedCosCone": cone,
                "lutA": sampled.a,
                "lutB": sampled.b,
                "referenceA": reference.a,
                "referenceB": reference.b,
                "absErrorA": errors_a[-1],
                "absErrorB": errors_b[-1],
            }
        )
    return {
        "schema": "MLSConeEnvValidationReportV1",
        "backend": backend,
        "numpyVersion": np.__version__ if backend == "numpy" else None,
        "artifact": {
            "manifest": str(lut.manifest_path.resolve()),
            "include": str(lut.include_path.resolve()),
            "hashVerified": lut.hash_verified,
        },
        "monotonicity": {
            "tolerance": 1.0 / 65535.0,
            "violationCount": len(violations),
            "violations": violations,
        },
        "heldOutRandom": {
            "pointCount": random_points,
            "referenceSamplesPerPoint": reference_samples,
            "A": error_metrics(errors_a),
            "B": error_metrics(errors_b),
            "rows": rows,
        },
    }
