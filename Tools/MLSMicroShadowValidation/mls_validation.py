"""Independent CPU validation for the MLS Generic VNDF micro-shadow LUT.

This module intentionally uses only the Python 3 standard library.  It is not
a binding to the Unreal implementation and does not import generator code from
``Source``.
"""

from __future__ import annotations

import csv
import hashlib
import json
import math
import random
import re
from collections import OrderedDict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence

try:
    import numpy as np
except ImportError:  # The scalar reference deliberately remains dependency-free.
    np = None


TWO_PI = 2.0 * math.pi
UINT32_SCALE = 1.0 / 4294967296.0
DEFAULT_SEQUENCE_NAME = "XorScrambledHammersley32"
DEFAULT_SEQUENCE_SEED = 0xC0FFEE11
DEFAULT_PARAMETER_SEED = 0x4D4C530E


def clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    return min(max(value, low), high)


def normalize3(value: tuple[float, float, float]) -> tuple[float, float, float]:
    length2 = value[0] * value[0] + value[1] * value[1] + value[2] * value[2]
    if length2 <= 1.0e-24:
        return (0.0, 0.0, 1.0)
    inverse_length = 1.0 / math.sqrt(length2)
    return (
        value[0] * inverse_length,
        value[1] * inverse_length,
        value[2] * inverse_length,
    )


def reverse_bits32(bits: int) -> int:
    """Reverse exactly 32 bits, matching an unsigned uint32 implementation."""

    bits &= 0xFFFFFFFF
    bits = ((bits << 16) | (bits >> 16)) & 0xFFFFFFFF
    bits = (((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8)) & 0xFFFFFFFF
    bits = (((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4)) & 0xFFFFFFFF
    bits = (((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2)) & 0xFFFFFFFF
    bits = (((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1)) & 0xFFFFFFFF
    return bits


def xor_scrambled_hammersley32(index: int, count: int, seed: int) -> tuple[float, float]:
    """Return a deterministic 2D Hammersley point with an XOR digital scramble.

    The name is deliberately narrower than "Owen scrambled": XOR with a fixed
    32-bit mask is a digital shift, not a nested Owen scramble.  Half-stratum
    offsets keep both coordinates away from exact endpoints.
    """

    if count <= 0:
        raise ValueError("Hammersley count must be positive")
    if index < 0 or index >= count:
        raise ValueError("Hammersley index must be in [0, count)")
    x = (index + 0.5) / count
    scrambled = reverse_bits32(index) ^ (seed & 0xFFFFFFFF)
    y = (scrambled + 0.5) * UINT32_SCALE
    return x, y


def sample_ggx_vndf_dupuy_benyoub_isotropic(
    view: tuple[float, float, float],
    alpha: float,
    xi: tuple[float, float],
) -> tuple[float, float, float]:
    """Literal isotropic form of Dupuy--Benyoub 2023 Listings 1 and 3.

    It warps the incident direction to the hemispherical configuration,
    samples the spherical cap ``z in (-wiStd.z, 1]``, forms the halfway vector,
    and inverse-transpose warps that normal back to the GGX ellipsoid.
    """

    alpha = max(alpha, 0.0)
    if alpha <= 1.0e-12:
        return (0.0, 0.0, 1.0)

    wi_std = normalize3((view[0] * alpha, view[1] * alpha, view[2]))
    phi = TWO_PI * (xi[0] % 1.0)
    uy = clamp(xi[1])
    z = (1.0 - uy) * (1.0 + wi_std[2]) - wi_std[2]
    sin_theta = math.sqrt(clamp(1.0 - z * z))
    cap = (sin_theta * math.cos(phi), sin_theta * math.sin(phi), z)
    half_std = (cap[0] + wi_std[0], cap[1] + wi_std[1], cap[2] + wi_std[2])
    return normalize3((half_std[0] * alpha, half_std[1] * alpha, half_std[2]))


def lambda_ggx(no_x: float, alpha: float) -> float:
    no_x = max(no_x, 1.0e-6)
    sin2 = max(1.0 - no_x * no_x, 0.0)
    tan2 = sin2 / (no_x * no_x)
    return 0.5 * (math.sqrt(1.0 + alpha * alpha * tan2) - 1.0)


def step_positive(value: float) -> float:
    """The normative strict step: equality is zero."""

    return 1.0 if value > 0.0 else 0.0


@dataclass(frozen=True)
class ReferenceInput:
    visibility: float
    roughness: float
    no_l: float
    no_v: float
    phi_radians: float = 0.0


@dataclass(frozen=True)
class IntegralResult:
    value: float
    cone_sum: float
    hemisphere_sum: float
    positive_hemisphere_samples: int


def integrate_reference_scalar(
    point: ReferenceInput,
    sample_count: int,
    sequence_seed: int = DEFAULT_SEQUENCE_SEED,
) -> IntegralResult:
    """Evaluate the section 3 surface integral with deterministic QMC samples."""

    visibility = clamp(point.visibility)
    if visibility <= 0.0:
        return IntegralResult(0.0, 0.0, 0.0, 0)
    if visibility >= 1.0:
        return IntegralResult(1.0, 0.0, 0.0, 0)
    if sample_count <= 0:
        raise ValueError("sample_count must be positive for an interior visibility")

    roughness = clamp(point.roughness)
    alpha = roughness * roughness
    no_l = clamp(point.no_l)
    no_v = clamp(point.no_v)
    view_sin = math.sqrt(max(1.0 - no_v * no_v, 0.0))
    light_sin = math.sqrt(max(1.0 - no_l * no_l, 0.0))
    view = (view_sin, 0.0, no_v)
    light = (
        light_sin * math.cos(point.phi_radians),
        light_sin * math.sin(point.phi_radians),
        no_l,
    )
    cos_theta = math.sqrt(max(1.0 - visibility, 0.0))
    lambda_v = lambda_ggx(no_v, alpha)
    lambda_l = lambda_ggx(no_l, alpha)
    weight = (1.0 + lambda_v) / max(1.0 + lambda_v + lambda_l, 1.0e-6)

    cone_sum = 0.0
    hemisphere_sum = 0.0
    positive_count = 0
    for sample_index in range(sample_count):
        xi = xor_scrambled_hammersley32(sample_index, sample_count, sequence_seed)
        micro_normal = sample_ggx_vndf_dupuy_benyoub_isotropic(view, alpha, xi)
        n_dot_m = clamp(micro_normal[2])
        m_dot_l = clamp(
            micro_normal[0] * light[0]
            + micro_normal[1] * light[1]
            + micro_normal[2] * light[2]
        )
        hemisphere = step_positive(n_dot_m) * step_positive(m_dot_l) * weight
        cone = (
            step_positive(n_dot_m - cos_theta)
            * step_positive(m_dot_l - cos_theta)
            * weight
        )
        hemisphere_sum += hemisphere
        cone_sum += cone
        if hemisphere > 0.0:
            positive_count += 1

    value = 0.0 if hemisphere_sum <= 1.0e-8 else clamp(cone_sum / hemisphere_sum)
    return IntegralResult(value, cone_sum, hemisphere_sum, positive_count)


def numpy_available() -> bool:
    return np is not None


def resolve_reference_backend(requested: str) -> str:
    requested = requested.lower()
    if requested == "auto":
        return "numpy" if numpy_available() else "scalar"
    if requested == "numpy":
        if not numpy_available():
            raise ValueError("NumPy backend requested, but NumPy is not installed")
        return "numpy"
    if requested == "scalar":
        return "scalar"
    raise ValueError(f"unknown reference backend {requested!r}")


def _reverse_bits32_numpy(bits):
    """Vector uint32 form of reverse_bits32; all masks/shifts stay uint32."""

    bits = ((bits << np.uint32(16)) | (bits >> np.uint32(16))).astype(np.uint32)
    bits = (
        ((bits & np.uint32(0x00FF00FF)) << np.uint32(8))
        | ((bits & np.uint32(0xFF00FF00)) >> np.uint32(8))
    ).astype(np.uint32)
    bits = (
        ((bits & np.uint32(0x0F0F0F0F)) << np.uint32(4))
        | ((bits & np.uint32(0xF0F0F0F0)) >> np.uint32(4))
    ).astype(np.uint32)
    bits = (
        ((bits & np.uint32(0x33333333)) << np.uint32(2))
        | ((bits & np.uint32(0xCCCCCCCC)) >> np.uint32(2))
    ).astype(np.uint32)
    bits = (
        ((bits & np.uint32(0x55555555)) << np.uint32(1))
        | ((bits & np.uint32(0xAAAAAAAA)) >> np.uint32(1))
    ).astype(np.uint32)
    return bits


_NUMPY_SEQUENCE_CACHE: OrderedDict[
    tuple[int, int], tuple[object, object, object]
] = OrderedDict()
_NUMPY_SEQUENCE_CACHE_LIMIT = 8


def _numpy_sequence_terms(sample_count: int, sequence_seed: int):
    """Cache invariant cap azimuth terms and Xi.Y for repeated point sweeps."""

    key = (sample_count, sequence_seed & 0xFFFFFFFF)
    cached = _NUMPY_SEQUENCE_CACHE.get(key)
    if cached is not None:
        _NUMPY_SEQUENCE_CACHE.move_to_end(key)
        return cached
    indices = np.arange(sample_count, dtype=np.uint32)
    xi_x = (indices.astype(np.float64) + 0.5) / float(sample_count)
    reversed_bits = _reverse_bits32_numpy(indices)
    scrambled = reversed_bits ^ np.uint32(sequence_seed & 0xFFFFFFFF)
    xi_y = (scrambled.astype(np.float64) + 0.5) * UINT32_SCALE
    phi = TWO_PI * xi_x
    cached = (np.cos(phi), np.sin(phi), xi_y)
    for array in cached:
        array.flags.writeable = False
    _NUMPY_SEQUENCE_CACHE[key] = cached
    if len(_NUMPY_SEQUENCE_CACHE) > _NUMPY_SEQUENCE_CACHE_LIMIT:
        _NUMPY_SEQUENCE_CACHE.popitem(last=False)
    return cached


def integrate_reference_numpy(
    point: ReferenceInput,
    sample_count: int,
    sequence_seed: int = DEFAULT_SEQUENCE_SEED,
) -> IntegralResult:
    """NumPy-vectorized equivalent of :func:`integrate_reference_scalar`.

    Endpoint branching, the strict ``> 0`` masks, uint32 Hammersley digital
    shift, coordinate mapping, and spherical-cap equations match the scalar
    implementation.  Only independent samples are vectorized; validation
    points remain sequential and deterministic.
    """

    if np is None:
        raise ValueError("NumPy backend requested, but NumPy is not installed")
    visibility = clamp(point.visibility)
    if visibility <= 0.0:
        return IntegralResult(0.0, 0.0, 0.0, 0)
    if visibility >= 1.0:
        return IntegralResult(1.0, 0.0, 0.0, 0)
    if sample_count <= 0:
        raise ValueError("sample_count must be positive for an interior visibility")

    roughness = clamp(point.roughness)
    alpha = roughness * roughness
    no_l = clamp(point.no_l)
    no_v = clamp(point.no_v)
    view_sin = math.sqrt(max(1.0 - no_v * no_v, 0.0))
    light_sin = math.sqrt(max(1.0 - no_l * no_l, 0.0))
    light_x = light_sin * math.cos(point.phi_radians)
    light_y = light_sin * math.sin(point.phi_radians)
    cos_theta = math.sqrt(max(1.0 - visibility, 0.0))
    lambda_v = lambda_ggx(no_v, alpha)
    lambda_l = lambda_ggx(no_l, alpha)
    weight = (1.0 + lambda_v) / max(1.0 + lambda_v + lambda_l, 1.0e-6)

    if alpha <= 1.0e-12:
        # This is the same singularity guard used by the literal scalar sampler.
        hemisphere_count = sample_count if no_l > 0.0 else 0
        cone_count = sample_count if 1.0 > cos_theta and no_l > cos_theta else 0
    else:
        cos_phi, sin_phi, xi_y = _numpy_sequence_terms(sample_count, sequence_seed)

        wi_std_x = view_sin * alpha
        wi_std_z = no_v
        wi_length = math.sqrt(wi_std_x * wi_std_x + wi_std_z * wi_std_z)
        if wi_length <= 1.0e-12:
            wi_std_x, wi_std_z = 0.0, 1.0
        else:
            wi_std_x /= wi_length
            wi_std_z /= wi_length

        z = (1.0 - xi_y) * (1.0 + wi_std_z) - wi_std_z
        sin_theta = np.sqrt(np.clip(1.0 - z * z, 0.0, 1.0))
        half_x = sin_theta * cos_phi + wi_std_x
        half_y = sin_theta * sin_phi
        half_z = z + wi_std_z
        micro_x = half_x * alpha
        micro_y = half_y * alpha
        micro_z = half_z
        micro_length = np.sqrt(micro_x * micro_x + micro_y * micro_y + micro_z * micro_z)
        valid = micro_length > 1.0e-12
        inverse_length = np.divide(
            1.0,
            micro_length,
            out=np.zeros_like(micro_length),
            where=valid,
        )
        micro_x *= inverse_length
        micro_y *= inverse_length
        micro_z = np.where(valid, micro_z * inverse_length, 1.0)

        n_dot_m = np.clip(micro_z, 0.0, 1.0)
        m_dot_l = np.clip(micro_x * light_x + micro_y * light_y + micro_z * no_l, 0.0, 1.0)
        hemisphere_mask = (n_dot_m > 0.0) & (m_dot_l > 0.0)
        cone_mask = (n_dot_m - cos_theta > 0.0) & (m_dot_l - cos_theta > 0.0)
        hemisphere_count = int(np.count_nonzero(hemisphere_mask))
        cone_count = int(np.count_nonzero(cone_mask))

    hemisphere_sum = hemisphere_count * weight
    cone_sum = cone_count * weight
    value = 0.0 if hemisphere_sum <= 1.0e-8 else clamp(cone_sum / hemisphere_sum)
    return IntegralResult(value, cone_sum, hemisphere_sum, hemisphere_count)


def integrate_reference(
    point: ReferenceInput,
    sample_count: int,
    sequence_seed: int = DEFAULT_SEQUENCE_SEED,
    backend: str = "scalar",
) -> IntegralResult:
    resolved = resolve_reference_backend(backend)
    if resolved == "numpy":
        return integrate_reference_numpy(point, sample_count, sequence_seed)
    return integrate_reference_scalar(point, sample_count, sequence_seed)


def analytic_smooth_limit(visibility: float, no_l: float) -> float:
    threshold = math.sqrt(max(1.0 - clamp(visibility), 0.0))
    return step_positive(clamp(no_l) - threshold)


def _canonical_axis_name(value: str) -> str:
    compact = re.sub(r"[^a-z0-9]", "", value.lower())
    names = {
        "visibility": "visibility",
        "materialao": "visibility",
        "nol": "no_l",
        "light": "no_l",
        "nov": "no_v",
        "view": "no_v",
    }
    if compact not in names:
        raise ValueError(f"unknown LUT axis name: {value!r}")
    return names[compact]


@dataclass(frozen=True)
class LUTLayout:
    dimensions: tuple[int, int, int]
    roughness_nodes: tuple[float, ...]
    visibility_warp: str = "square"
    no_l_warp: str = "oneMinusSquare"
    no_v_warp: str = "square"
    roughness_interpolation: str = "roughness"
    fastest_axis_order: tuple[str, str, str] = ("visibility", "no_l", "no_v")
    roughness_banks: tuple[tuple[int, int, int, int], ...] = ((0, 1, 2, 3),)
    roughness_bank_split: float | None = None
    roughness_bank_selection: str = "SingleBankV1"

    @classmethod
    def baseline(cls) -> "LUTLayout":
        return cls((16, 32, 8), (0.1, 0.4, 0.7, 1.0))

    @classmethod
    def from_manifest(cls, manifest: dict) -> tuple["LUTLayout", list[str]]:
        assumptions: list[str] = []
        dimensions_raw = manifest.get("dimensions")
        if not isinstance(dimensions_raw, list) or len(dimensions_raw) != 3:
            raise ValueError("manifest dimensions must be a three-element array")
        dimensions = tuple(int(item) for item in dimensions_raw)
        if any(item <= 0 for item in dimensions):
            raise ValueError("all LUT dimensions must be positive")

        nodes_raw = manifest.get("roughnessNodes")
        if not isinstance(nodes_raw, list):
            raise ValueError("manifest roughnessNodes must be an array")
        nodes = tuple(float(item) for item in nodes_raw)
        if tuple(sorted(nodes)) != nodes or len(set(nodes)) != len(nodes):
            raise ValueError("roughnessNodes must be unique and strictly ascending")

        v2_fields = (
            "roughnessBanks",
            "roughnessBankSplit",
            "roughnessBankSelection",
        )
        present_v2_fields = [field for field in v2_fields if field in manifest]
        if present_v2_fields:
            if len(present_v2_fields) != len(v2_fields):
                missing = [field for field in v2_fields if field not in manifest]
                raise ValueError(
                    "V2 roughness bank schema is incomplete; missing " + ", ".join(missing)
                )
            expected_nodes = (0.1, 0.18, 0.3, 0.45, 0.6, 0.75, 1.0)
            if nodes != expected_nodes:
                raise ValueError(
                    f"SingleBankPiecewise roughnessNodes must be {list(expected_nodes)!r}"
                )
            banks_raw = manifest["roughnessBanks"]
            banks_are_integer_indices = (
                isinstance(banks_raw, list)
                and all(isinstance(bank, list) for bank in banks_raw)
                and all(
                    isinstance(index, int) and not isinstance(index, bool)
                    for bank in banks_raw
                    for index in bank
                )
            )
            if (
                not banks_are_integer_indices
                or banks_raw != [[0, 1, 2, 3], [3, 4, 5, 6]]
            ):
                raise ValueError(
                    "SingleBankPiecewise roughnessBanks must be [[0,1,2,3],[3,4,5,6]]"
                )
            roughness_banks = ((0, 1, 2, 3), (3, 4, 5, 6))
            split_raw = manifest["roughnessBankSplit"]
            if not isinstance(split_raw, (int, float)) or isinstance(split_raw, bool):
                raise ValueError("SingleBankPiecewise roughnessBankSplit must be numeric")
            roughness_bank_split = float(split_raw)
            if roughness_bank_split != 0.45:
                raise ValueError("SingleBankPiecewise roughnessBankSplit must be 0.45")
            roughness_bank_selection = str(manifest["roughnessBankSelection"])
            if roughness_bank_selection != "SingleBankPiecewise":
                raise ValueError(
                    "roughnessBankSelection must be 'SingleBankPiecewise'"
                )
        else:
            if len(nodes) != 4:
                raise ValueError("V1 RGBA8 LUT requires four roughnessNodes")
            roughness_banks = ((0, 1, 2, 3),)
            roughness_bank_split = None
            roughness_bank_selection = "SingleBankV1"

        axis_warp = manifest.get("axisWarp")
        if not isinstance(axis_warp, dict):
            raise ValueError("manifest axisWarp must be an object")
        visibility_warp = str(axis_warp.get("visibility", ""))
        no_l_warp = str(axis_warp.get("NoL", axis_warp.get("noL", "")))
        no_v_warp = str(axis_warp.get("NoV", axis_warp.get("noV", "")))
        visibility_warp_key = visibility_warp.lower()
        no_l_warp_key = re.sub(r"[^a-z]", "", no_l_warp.lower())
        no_v_warp_key = no_v_warp.lower()
        if visibility_warp_key not in {
            "square",
            "linear",
            "symmetricratiosquare",
        }:
            raise ValueError(f"unsupported visibility warp: {visibility_warp!r}")
        if no_l_warp_key not in {
            "oneminussquare",
            "linear",
            "smoothlimitboundarysignedsquare",
        }:
            raise ValueError(f"unsupported NoL warp: {no_l_warp!r}")
        if no_v_warp_key not in {"square", "linear"}:
            raise ValueError(f"unsupported NoV warp: {no_v_warp!r}")

        interpolation_raw = manifest.get("roughnessInterpolation")
        if interpolation_raw is None:
            assumptions.append("roughnessInterpolation absent; assumed linear in perceptual roughness")
            roughness_interpolation = "roughness"
        else:
            compact = re.sub(r"[^a-z0-9]", "", str(interpolation_raw).lower())
            if compact in {"roughness", "perceptualroughness", "linearinroughness"}:
                roughness_interpolation = "roughness"
            elif compact in {"alpha", "roughnesssquared", "linearinalpha", "r2"}:
                roughness_interpolation = "alpha"
            else:
                raise ValueError(f"unsupported roughnessInterpolation: {interpolation_raw!r}")

        order_raw = manifest.get("linearIndexOrder", manifest.get("axisOrder"))
        if order_raw is None:
            assumptions.append("linearIndexOrder absent; assumed Visibility-fastest, then NoL, then NoV")
            order = ("visibility", "no_l", "no_v")
        elif isinstance(order_raw, list):
            order = tuple(_canonical_axis_name(str(item)) for item in order_raw)
        else:
            tokens = re.findall(r"visibility|materialao|no[_ ]?l|no[_ ]?v", str(order_raw), re.I)
            order = tuple(_canonical_axis_name(item) for item in tokens)
        if len(order) != 3 or set(order) != {"visibility", "no_l", "no_v"}:
            raise ValueError(f"linearIndexOrder must name each spatial axis once: {order_raw!r}")

        return (
            cls(
                dimensions=dimensions,
                roughness_nodes=nodes,
                visibility_warp=visibility_warp,
                no_l_warp=no_l_warp,
                no_v_warp=no_v_warp,
                roughness_interpolation=roughness_interpolation,
                fastest_axis_order=order,
                roughness_banks=roughness_banks,
                roughness_bank_split=roughness_bank_split,
                roughness_bank_selection=roughness_bank_selection,
            ),
            assumptions,
        )

    def _axis_size(self, axis: str) -> int:
        return {
            "visibility": self.dimensions[0],
            "no_l": self.dimensions[1],
            "no_v": self.dimensions[2],
        }[axis]

    def flat_index(self, visibility_index: int, no_l_index: int, no_v_index: int) -> int:
        indices = {
            "visibility": visibility_index,
            "no_l": no_l_index,
            "no_v": no_v_index,
        }
        stride = 1
        result = 0
        for axis in self.fastest_axis_order:
            index = indices[axis]
            size = self._axis_size(axis)
            if index < 0 or index >= size:
                raise IndexError(f"{axis} index {index} outside [0, {size})")
            result += index * stride
            stride *= size
        return result

    def _coordinate(
        self,
        value: float,
        size: int,
        axis: str,
        visibility: float | None = None,
    ) -> tuple[int, int, float]:
        value = clamp(value)
        warp = {
            "visibility": self.visibility_warp.lower(),
            "no_l": re.sub(r"[^a-z]", "", self.no_l_warp.lower()),
            "no_v": self.no_v_warp.lower(),
        }.get(axis)
        if warp == "linear":
            runtime_t = value
        elif warp == "square":
            runtime_t = math.sqrt(value)
        elif warp == "symmetricratiosquare":
            sqrt_value = math.sqrt(value)
            sqrt_complement = math.sqrt(max(1.0 - value, 0.0))
            denominator = sqrt_value + sqrt_complement
            runtime_t = sqrt_value / denominator if denominator > 0.0 else 0.0
        elif warp == "oneminussquare":
            runtime_t = 1.0 - math.sqrt(max(1.0 - value, 0.0))
        elif warp == "smoothlimitboundarysignedsquare":
            if visibility is None:
                raise ValueError("smooth-limit-boundary NoL coordinate requires Visibility")
            boundary = math.sqrt(max(1.0 - clamp(visibility), 0.0))
            if value < boundary:
                signed = (
                    -math.sqrt(max((boundary - value) / boundary, 0.0))
                    if boundary > 0.0
                    else 0.0
                )
            else:
                upper_span = 1.0 - boundary
                signed = (
                    math.sqrt(max((value - boundary) / upper_span, 0.0))
                    if upper_span > 0.0
                    else 0.0
                )
            runtime_t = 0.5 + 0.5 * clamp(signed, -1.0, 1.0)
        else:
            raise ValueError(f"unsupported {axis} warp {warp!r}")
        scaled = runtime_t * max(size - 1, 0)
        lower = min(int(math.floor(scaled)), max(size - 1, 0))
        upper = min(lower + 1, max(size - 1, 0))
        return lower, upper, scaled - lower

    def spatial_stencil(
        self, visibility: float, no_l: float, no_v: float
    ) -> list[tuple[int, int, int, float]]:
        vx = self._coordinate(visibility, self.dimensions[0], "visibility")
        ly = self._coordinate(
            no_l, self.dimensions[1], "no_l", visibility=visibility
        )
        vz = self._coordinate(no_v, self.dimensions[2], "no_v")
        result: list[tuple[int, int, int, float]] = []
        for iz, wz in ((vz[0], 1.0 - vz[2]), (vz[1], vz[2])):
            for iy, wy in ((ly[0], 1.0 - ly[2]), (ly[1], ly[2])):
                for ix, wx in ((vx[0], 1.0 - vx[2]), (vx[1], vx[2])):
                    weight = wx * wy * wz
                    if weight > 0.0:
                        result.append((ix, iy, iz, weight))
        return result

    @property
    def bank_count(self) -> int:
        return len(self.roughness_banks)

    def roughness_stencil(self, roughness: float) -> tuple[int, list[tuple[int, float]]]:
        roughness = clamp(roughness, self.roughness_nodes[0], self.roughness_nodes[-1])
        if self.roughness_bank_selection == "SingleBankPiecewise":
            bank = 0 if roughness <= self.roughness_bank_split else 1
        else:
            bank = 0
        bank_nodes = self.roughness_banks[bank]
        coordinate = roughness * roughness if self.roughness_interpolation == "alpha" else roughness
        mapped_nodes = [
            self.roughness_nodes[node_index] ** 2
            if self.roughness_interpolation == "alpha"
            else self.roughness_nodes[node_index]
            for node_index in bank_nodes
        ]
        for upper in range(1, len(mapped_nodes)):
            if coordinate <= mapped_nodes[upper]:
                lower = upper - 1
                width = mapped_nodes[upper] - mapped_nodes[lower]
                fraction = 0.0 if width <= 0.0 else (coordinate - mapped_nodes[lower]) / width
                if fraction <= 0.0:
                    return bank, [(lower, 1.0)]
                if fraction >= 1.0:
                    return bank, [(upper, 1.0)]
                return bank, [(lower, 1.0 - fraction), (upper, fraction)]
        return bank, [(len(mapped_nodes) - 1, 1.0)]

    def physical_node(
        self, ix: int, iy: int, iz: int, channel: int, bank: int = 0
    ) -> ReferenceInput:
        tv = 0.0 if self.dimensions[0] <= 1 else ix / (self.dimensions[0] - 1)
        tl = 0.0 if self.dimensions[1] <= 1 else iy / (self.dimensions[1] - 1)
        tview = 0.0 if self.dimensions[2] <= 1 else iz / (self.dimensions[2] - 1)
        visibility_warp = self.visibility_warp.lower()
        if visibility_warp == "linear":
            visibility = tv
        elif visibility_warp == "square":
            visibility = tv * tv
        elif visibility_warp == "symmetricratiosquare":
            tv2 = tv * tv
            complement2 = (1.0 - tv) * (1.0 - tv)
            denominator = tv2 + complement2
            visibility = tv2 / denominator if denominator > 0.0 else 0.0
        else:
            raise ValueError(f"unsupported Visibility warp {self.visibility_warp!r}")
        no_l_warp = re.sub(r"[^a-z]", "", self.no_l_warp.lower())
        if no_l_warp == "linear":
            no_l = tl
        elif no_l_warp == "oneminussquare":
            no_l = 1.0 - (1.0 - tl) * (1.0 - tl)
        elif no_l_warp == "smoothlimitboundarysignedsquare":
            boundary = math.sqrt(max(1.0 - visibility, 0.0))
            signed = 2.0 * tl - 1.0
            if signed < 0.0:
                no_l = boundary * (1.0 - signed * signed)
            else:
                no_l = boundary + (1.0 - boundary) * signed * signed
        else:
            raise ValueError(f"unsupported NoL warp {self.no_l_warp!r}")
        no_v = tview if self.no_v_warp.lower() == "linear" else tview * tview
        return ReferenceInput(
            visibility=visibility,
            roughness=self.roughness_nodes[self.roughness_banks[bank][channel]],
            no_l=no_l,
            no_v=no_v,
            phi_radians=0.0,
        )


def interpolate_provider(
    layout: LUTLayout,
    point: ReferenceInput,
    provider: Callable[[int, int, int, int, int], float],
) -> float:
    value = 0.0
    bank, roughness_stencil = layout.roughness_stencil(point.roughness)
    for ix, iy, iz, spatial_weight in layout.spatial_stencil(
        point.visibility, point.no_l, point.no_v
    ):
        for channel, roughness_weight in roughness_stencil:
            value += (
                spatial_weight
                * roughness_weight
                * provider(ix, iy, iz, bank, channel)
            )
    return clamp(value)


def _strip_c_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\r\n]*", "", text)


def parse_packed_uints(include_text: str) -> list[int]:
    match = re.search(
        r"MLS_VNDF_LUT_PACKED\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;",
        include_text,
        flags=re.DOTALL,
    )
    if not match:
        raise ValueError("MLS_VNDF_LUT_PACKED initializer was not found")
    body = _strip_c_cpp_comments(match.group(1))
    tokens = re.findall(r"(?<![A-Za-z0-9_])(?:0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*", body)
    values: list[int] = []
    for token in tokens:
        numeric = re.sub(r"[uUlL]+$", "", token)
        value = int(numeric, 0)
        if value < 0 or value > 0xFFFFFFFF:
            raise ValueError(f"packed LUT value is not uint32: {token}")
        values.append(value)
    return values


@dataclass
class PackedLUT:
    manifest_path: Path
    include_path: Path
    manifest: dict
    layout: LUTLayout
    packed: list[int]
    assumptions: list[str]
    hash_report: dict

    @classmethod
    def load(cls, manifest_path: Path, include_path: Path) -> "PackedLUT":
        manifest_bytes = manifest_path.read_bytes()
        manifest = json.loads(manifest_bytes.decode("utf-8-sig"))
        if not isinstance(manifest, dict):
            raise ValueError("manifest root must be an object")
        required_contract = {
            "algorithm": "GenericVNDFBasedMicroShadowing",
            "distribution": "IsotropicGGX",
            "vndfSampler": "DupuyBenyoub2023",
            "smithCorrelation": "HeightCorrelated",
            "visibilityWeighting": "CosineWeightedProjectedSolidAngle",
            "relativeAzimuth": "coplanar_same_azimuth",
            "quantization": "RGBA8_UNORM",
        }
        for field, expected in required_contract.items():
            if manifest.get(field) != expected:
                raise ValueError(
                    f"manifest {field} must be {expected!r}, got {manifest.get(field)!r}"
                )
        layout, assumptions = LUTLayout.from_manifest(manifest)
        if "channelPacking" not in manifest:
            assumptions.append("channelPacking absent; assumed R in least-significant byte, then G, B, A")
        else:
            packing = re.sub(r"[^a-z0-9]", "", str(manifest["channelPacking"]).lower())
            if packing not in {"rgbalittleendian", "rlsb", "rleastsignificantbyte"}:
                raise ValueError(f"unsupported channelPacking: {manifest['channelPacking']!r}")
        include_bytes = include_path.read_bytes()
        include_text = include_bytes.decode("utf-8-sig")
        packed = parse_packed_uints(include_text)
        expected_count = math.prod(layout.dimensions) * layout.bank_count
        if len(packed) != expected_count:
            raise ValueError(
                f"packed texel count {len(packed)} does not match dimensions times bank count {expected_count}"
            )

        little_endian = b"".join(value.to_bytes(4, "little") for value in packed)
        big_endian = b"".join(value.to_bytes(4, "big") for value in packed)
        candidates = {
            "packed_rgba_bytes_little_endian": hashlib.sha256(little_endian).hexdigest(),
            "packed_uints_big_endian": hashlib.sha256(big_endian).hexdigest(),
            "include_file_bytes": hashlib.sha256(include_bytes).hexdigest(),
        }
        declared = str(manifest.get("dataSHA256", "")).lower()
        matches = [name for name, digest in candidates.items() if declared and digest == declared]
        hash_report = {
            "declared_data_sha256": declared or None,
            "computed": candidates,
            "matching_interpretations": matches,
            "verified": bool(matches),
        }
        if not declared:
            assumptions.append("dataSHA256 absent; packed payload integrity could not be checked")
        elif not matches:
            assumptions.append("dataSHA256 did not match supported packed-byte or include-file interpretations")

        return cls(
            manifest_path=manifest_path,
            include_path=include_path,
            manifest=manifest,
            layout=layout,
            packed=packed,
            assumptions=assumptions,
            hash_report=hash_report,
        )

    def channel(self, ix: int, iy: int, iz: int, bank: int, channel: int) -> float:
        if bank < 0 or bank >= self.layout.bank_count:
            raise IndexError(f"roughness bank {bank} outside [0, {self.layout.bank_count})")
        texels_per_bank = math.prod(self.layout.dimensions)
        packed = self.packed[
            bank * texels_per_bank + self.layout.flat_index(ix, iy, iz)
        ]
        return ((packed >> (channel * 8)) & 0xFF) / 255.0

    def sample_interpolated(self, point: ReferenceInput) -> float:
        return interpolate_provider(self.layout, point, self.channel)

    def sample_runtime(self, point: ReferenceInput) -> float:
        visibility = clamp(point.visibility)
        no_l = clamp(point.no_l)
        if visibility <= 0.0:
            return 0.0
        if visibility >= 1.0:
            return 1.0
        if 2.0 * visibility + no_l < 1.0:
            return 0.0
        runtime_point = ReferenceInput(
            visibility=visibility,
            roughness=clamp(point.roughness, 0.1, 1.0),
            no_l=no_l,
            no_v=clamp(point.no_v),
            phi_radians=point.phi_radians,
        )
        return self.sample_interpolated(runtime_point)

    def sample_cpu_bruteforce(self, point: ReferenceInput) -> float:
        """Independent 4D weighted sum used to cross-check staged runtime interpolation."""

        visibility = clamp(point.visibility)
        no_l = clamp(point.no_l)
        if visibility <= 0.0:
            return 0.0
        if visibility >= 1.0:
            return 1.0
        if 2.0 * visibility + no_l < 1.0:
            return 0.0
        point = ReferenceInput(
            visibility=visibility,
            roughness=clamp(point.roughness, 0.1, 1.0),
            no_l=no_l,
            no_v=clamp(point.no_v),
            phi_radians=point.phi_radians,
        )
        # Deliberately duplicate the coordinate and weight construction instead
        # of calling LUTLayout.spatial_stencil/interpolate_provider.  This makes
        # the cross-check capable of catching a staged-interpolator indexing or
        # weight bug.
        visibility_warp = self.layout.visibility_warp.lower()
        if visibility_warp == "linear":
            tv = point.visibility
        elif visibility_warp == "square":
            tv = math.sqrt(point.visibility)
        elif visibility_warp == "symmetricratiosquare":
            sqrt_visibility = math.sqrt(point.visibility)
            sqrt_complement = math.sqrt(max(1.0 - point.visibility, 0.0))
            denominator = sqrt_visibility + sqrt_complement
            tv = sqrt_visibility / denominator if denominator > 0.0 else 0.0
        else:
            raise ValueError(
                f"unsupported Visibility warp {self.layout.visibility_warp!r}"
            )
        no_l_warp = re.sub(r"[^a-z]", "", self.layout.no_l_warp.lower())
        if no_l_warp == "linear":
            tl = point.no_l
        elif no_l_warp == "oneminussquare":
            tl = 1.0 - math.sqrt(max(1.0 - point.no_l, 0.0))
        elif no_l_warp == "smoothlimitboundarysignedsquare":
            boundary = math.sqrt(max(1.0 - point.visibility, 0.0))
            if point.no_l < boundary:
                signed = (
                    -math.sqrt(max((boundary - point.no_l) / boundary, 0.0))
                    if boundary > 0.0
                    else 0.0
                )
            else:
                upper_span = 1.0 - boundary
                signed = (
                    math.sqrt(max((point.no_l - boundary) / upper_span, 0.0))
                    if upper_span > 0.0
                    else 0.0
                )
            tl = 0.5 + 0.5 * clamp(signed, -1.0, 1.0)
        else:
            raise ValueError(f"unsupported NoL warp {self.layout.no_l_warp!r}")
        tview = (
            point.no_v
            if self.layout.no_v_warp.lower() == "linear"
            else math.sqrt(point.no_v)
        )

        def pair(t: float, size: int) -> tuple[int, int, float]:
            q = t * max(size - 1, 0)
            lower = min(int(math.floor(q)), max(size - 1, 0))
            return lower, min(lower + 1, max(size - 1, 0)), q - lower

        vx = pair(tv, self.layout.dimensions[0])
        ly = pair(tl, self.layout.dimensions[1])
        vz = pair(tview, self.layout.dimensions[2])
        if self.layout.roughness_bank_selection == "SingleBankPiecewise":
            rough_bank = 0 if point.roughness <= self.layout.roughness_bank_split else 1
        else:
            rough_bank = 0
        bank_node_indices = self.layout.roughness_banks[rough_bank]
        roughness_coordinate = (
            point.roughness * point.roughness
            if self.layout.roughness_interpolation == "alpha"
            else point.roughness
        )
        mapped_nodes = [
            node * node if self.layout.roughness_interpolation == "alpha" else node
            for node_index in bank_node_indices
            for node in (self.layout.roughness_nodes[node_index],)
        ]
        rough_lower = 0
        rough_upper = 0
        rough_fraction = 0.0
        for candidate in range(1, 4):
            if roughness_coordinate <= mapped_nodes[candidate]:
                rough_lower = candidate - 1
                rough_upper = candidate
                rough_fraction = (
                    (roughness_coordinate - mapped_nodes[rough_lower])
                    / (mapped_nodes[rough_upper] - mapped_nodes[rough_lower])
                )
                break
        else:
            rough_lower = rough_upper = 3
        rough_fraction = clamp(rough_fraction)

        total = 0.0
        for iz, wz in ((vz[0], 1.0 - vz[2]), (vz[1], vz[2])):
            for iy, wy in ((ly[0], 1.0 - ly[2]), (ly[1], ly[2])):
                for ix, wx in ((vx[0], 1.0 - vx[2]), (vx[1], vx[2])):
                    lower_value = self.channel(ix, iy, iz, rough_bank, rough_lower)
                    upper_value = self.channel(ix, iy, iz, rough_bank, rough_upper)
                    channel_value = lower_value * (1.0 - rough_fraction) + upper_value * rough_fraction
                    total += wx * wy * wz * channel_value
        return clamp(total)


class ReconstructedFloatLUT:
    """Lazy independent reconstruction of only the float texels a report touches."""

    def __init__(
        self, layout: LUTLayout, sample_count: int, sequence_seed: int, backend: str = "scalar"
    ):
        if sample_count <= 0:
            raise ValueError("float LUT reconstruction sample count must be positive")
        self.layout = layout
        self.sample_count = sample_count
        self.sequence_seed = sequence_seed
        self.backend = resolve_reference_backend(backend)
        self._cache: dict[tuple[int, int, int, int, int], float] = {}

    def value(self, ix: int, iy: int, iz: int, bank: int, channel: int) -> float:
        key = (ix, iy, iz, bank, channel)
        if key not in self._cache:
            point = self.layout.physical_node(ix, iy, iz, channel, bank)
            self._cache[key] = integrate_reference(
                point, self.sample_count, self.sequence_seed, self.backend
            ).value
        return self._cache[key]

    def quantized_value(
        self, ix: int, iy: int, iz: int, bank: int, channel: int
    ) -> float:
        return round(clamp(self.value(ix, iy, iz, bank, channel)) * 255.0) / 255.0

    def sample_float(self, point: ReferenceInput) -> float:
        return interpolate_provider(self.layout, point, self.value)

    def sample_quantized(self, point: ReferenceInput) -> float:
        return interpolate_provider(self.layout, point, self.quantized_value)

    @property
    def evaluated_texels(self) -> int:
        return len(self._cache)


def error_metrics(errors: Sequence[float]) -> dict:
    if not errors:
        return {"count": 0, "mean": None, "p95": None, "p99": None, "max": None}
    ordered = sorted(float(value) for value in errors)

    def percentile(fraction: float) -> float:
        if len(ordered) == 1:
            return ordered[0]
        position = (len(ordered) - 1) * fraction
        lower = int(math.floor(position))
        upper = int(math.ceil(position))
        weight = position - lower
        return ordered[lower] * (1.0 - weight) + ordered[upper] * weight

    return {
        "count": len(ordered),
        "mean": sum(ordered) / len(ordered),
        "p95": percentile(0.95),
        "p99": percentile(0.99),
        "max": ordered[-1],
    }


RANDOM_THRESHOLDS = {"mean": 0.012, "p95": 0.035, "p99": 0.070, "max": 0.12}
BOUNDARY_THRESHOLDS = {"p95": 0.06, "max": 0.15}


def threshold_result(metrics: dict, thresholds: dict) -> dict:
    checks = {
        key: metrics[key] is not None and metrics[key] <= threshold
        for key, threshold in thresholds.items()
    }
    return {"thresholds": thresholds, "checks": checks, "passed": all(checks.values())}


@dataclass(frozen=True)
class ValidationConfig:
    profile: str
    random_points: int
    boundary_points: int
    reference_samples: int
    boundary_reference_samples: int
    smooth_samples: int
    azimuth_base_points: int
    azimuth_random_phis: int
    quantization_points: int
    float_reconstruction_samples: int
    parameter_seed: int = DEFAULT_PARAMETER_SEED
    sequence_seed: int = DEFAULT_SEQUENCE_SEED
    monotonic_tolerance: float = (1.0 / 255.0) + 1.0e-12
    reference_backend: str = "auto"

    @classmethod
    def profile_defaults(cls, name: str) -> "ValidationConfig":
        if name == "smoke":
            return cls(name, 24, 12, 256, 512, 1024, 4, 4, 6, 256)
        if name == "canonical":
            return cls(name, 10000, 512, 16384, 65536, 16384, 64, 32, 256, 8192)
        if name == "high":
            return cls(name, 20000, 1024, 65536, 65536, 65536, 128, 128, 512, 16384)
        raise ValueError(f"unknown profile {name!r}")


CSV_FIELDS = [
    "suite",
    "case_id",
    "status",
    "visibility",
    "roughness",
    "no_l",
    "no_v",
    "phi_deg",
    "reference",
    "lut",
    "float_lut",
    "rgba8_simulated",
    "abs_error",
    "detail",
]


def _row(suite: str, case_id: str, status: str = "measured", **values: object) -> dict:
    row = {field: "" for field in CSV_FIELDS}
    row.update({"suite": suite, "case_id": case_id, "status": status})
    for key, value in values.items():
        if key in row:
            row[key] = value
    return row


def _point_fields(point: ReferenceInput) -> dict:
    return {
        "visibility": point.visibility,
        "roughness": point.roughness,
        "no_l": point.no_l,
        "no_v": point.no_v,
        "phi_deg": math.degrees(point.phi_radians),
    }


def _random_point(rng: random.Random) -> ReferenceInput:
    return ReferenceInput(rng.random(), 0.1 + 0.9 * rng.random(), rng.random(), rng.random())


def _boundary_point(rng: random.Random) -> ReferenceInput:
    return ReferenceInput(
        0.01 + 0.19 * rng.random(),
        0.10 + 0.10 * rng.random(),
        0.75 + 0.25 * rng.random(),
        rng.random(),
    )


def validate_endpoints(
    lut: PackedLUT | None, config: ValidationConfig, backend: str, rows: list[dict]
) -> dict:
    cases = []
    exact = True
    values = ((0.0, 0.0), (1.0, 1.0))
    for index, (visibility, expected) in enumerate(values):
        for no_l in (0.0, 0.37, 1.0):
            point = ReferenceInput(visibility, 0.47, no_l, 0.23)
            reference = integrate_reference(
                point, config.reference_samples, config.sequence_seed, backend
            ).value
            runtime = lut.sample_runtime(point) if lut else None
            case_pass = reference == expected and (runtime is None or runtime == expected)
            exact = exact and case_pass
            case = {
                "visibility": visibility,
                "no_l": no_l,
                "expected": expected,
                "reference": reference,
                "runtime_lut": runtime,
                "passed": case_pass,
            }
            cases.append(case)
            rows.append(
                _row(
                    "endpoints",
                    f"endpoint_{index}_{no_l:g}",
                    status="pass" if case_pass else "fail",
                    **_point_fields(point),
                    reference=reference,
                    lut=runtime if runtime is not None else "",
                    abs_error=abs(reference - expected),
                    detail=f"expected={expected}",
                )
            )
    return {"passed": exact, "cases": cases}


def validate_smooth_limit(
    lut: PackedLUT | None, config: ValidationConfig, backend: str, rows: list[dict]
) -> dict:
    roughness_values = (0.0, 0.1, 0.03, 0.01, 0.003)
    points = []
    for visibility in (0.05, 0.25, 0.50, 0.90):
        threshold = math.sqrt(1.0 - visibility)
        for offset in (-0.15, -0.05, 0.05, 0.15):
            no_l = clamp(threshold + offset)
            for no_v in (0.1, 0.5, 1.0):
                points.append((visibility, no_l, no_v))
    # Exact discontinuity is a separate strict-step case; positive-roughness
    # convergence is judged only on points with a finite margin from it.
    exact_boundary_point = (0.75, 0.50, 0.50)
    per_roughness: dict[str, dict] = {}
    exact_zero_pass = True
    for roughness in roughness_values:
        errors = []
        for index, (visibility, no_l, no_v) in enumerate(points):
            point = ReferenceInput(visibility, roughness, no_l, no_v)
            actual = integrate_reference(
                point, config.smooth_samples, config.sequence_seed, backend
            ).value
            expected = analytic_smooth_limit(visibility, no_l)
            error = abs(actual - expected)
            errors.append(error)
            if roughness == 0.0:
                exact_zero_pass = exact_zero_pass and error == 0.0
            rows.append(
                _row(
                    "smooth_limit",
                    f"r_{roughness:g}_{index}",
                    status="measured",
                    **_point_fields(point),
                    reference=actual,
                    abs_error=error,
                    detail=f"analytic_step={expected}",
                )
            )
        per_roughness[f"{roughness:.9g}"] = error_metrics(errors)

    exact_boundary = ReferenceInput(
        exact_boundary_point[0], 0.0, exact_boundary_point[1], exact_boundary_point[2]
    )
    exact_boundary_actual = integrate_reference(
        exact_boundary, config.smooth_samples, config.sequence_seed, backend
    ).value
    exact_boundary_expected = analytic_smooth_limit(
        exact_boundary.visibility, exact_boundary.no_l
    )
    exact_boundary_pass = exact_boundary_actual == exact_boundary_expected == 0.0
    exact_zero_pass = exact_zero_pass and exact_boundary_pass
    rows.append(
        _row(
            "smooth_limit",
            "exact_step_boundary",
            status="pass" if exact_boundary_pass else "fail",
            **_point_fields(exact_boundary),
            reference=exact_boundary_actual,
            abs_error=abs(exact_boundary_actual - exact_boundary_expected),
            detail="strict StepPositive equality must be zero",
        )
    )

    positive_means = [per_roughness[f"{roughness:.9g}"]["mean"] for roughness in roughness_values[1:]]
    convergence = {
        "roughness_descending": list(roughness_values[1:]),
        "mean_absolute_errors": positive_means,
        "final_not_worse_than_initial": positive_means[-1] <= positive_means[0],
        "informational_only": True,
    }

    runtime_clamp = {"status": "skipped", "reason": "LUT artifacts not found"}
    if lut:
        clamp_cases = []
        clamp_pass = True
        for index, (visibility, no_l, no_v) in enumerate(points):
            values = []
            for roughness in (0.0, 0.05, 0.1):
                values.append(lut.sample_runtime(ReferenceInput(visibility, roughness, no_l, no_v)))
            case_pass = max(values) - min(values) <= 1.0e-15
            clamp_pass = clamp_pass and case_pass
            clamp_cases.append({"case": index, "values_for_r_0_005_01": values, "passed": case_pass})
        runtime_clamp = {"status": "pass" if clamp_pass else "fail", "cases": clamp_cases}
    return {
        "integrator_r_zero_exact_passed": exact_zero_pass,
        "strict_exact_boundary": {
            "visibility": exact_boundary.visibility,
            "no_l": exact_boundary.no_l,
            "expected": exact_boundary_expected,
            "actual": exact_boundary_actual,
            "passed": exact_boundary_pass,
        },
        "per_roughness_error_vs_analytic_step": per_roughness,
        "positive_roughness_convergence": convergence,
        "runtime_clamped_lut_behavior": runtime_clamp,
    }


def validate_bounds(
    points: Sequence[ReferenceInput],
    lut: PackedLUT | None,
    config: ValidationConfig,
    backend: str,
    rows: list[dict],
    reference_cache: dict[ReferenceInput, float] | None = None,
) -> dict:
    reference_violations = []
    lut_violations = []
    for index, point in enumerate(points):
        reference = integrate_reference(
            point, config.reference_samples, config.sequence_seed, backend
        ).value
        if reference_cache is not None:
            reference_cache[point] = reference
        if not 0.0 <= reference <= 1.0:
            reference_violations.append({"case": index, "value": reference, "point": asdict(point)})
        runtime = lut.sample_runtime(point) if lut else None
        if runtime is not None and not 0.0 <= runtime <= 1.0:
            lut_violations.append({"case": index, "value": runtime, "point": asdict(point)})
        rows.append(
            _row(
                "bounds",
                str(index),
                status="pass" if 0.0 <= reference <= 1.0 and (runtime is None or 0.0 <= runtime <= 1.0) else "fail",
                **_point_fields(point),
                reference=reference,
                lut=runtime if runtime is not None else "",
            )
        )
    return {
        "reference": {
            "passed": not reference_violations,
            "sampled_point_count": len(points),
            "violations": reference_violations,
        },
        "runtime_lut": (
            {
                "status": "pass" if not lut_violations else "fail",
                "sampled_point_count": len(points),
                "violations": lut_violations,
            }
            if lut
            else {"status": "skipped", "reason": "LUT artifacts not found"}
        ),
    }


def validate_error_set(
    name: str,
    points: Iterable[ReferenceInput],
    lut: PackedLUT | None,
    reference_samples: int,
    sequence_seed: int,
    thresholds: dict,
    qualified: bool,
    backend: str,
    rows: list[dict],
    precomputed_references: dict[ReferenceInput, float] | None = None,
) -> dict:
    if lut is None:
        rows.append(_row(name, "not_run", status="skipped", detail="LUT artifacts not found"))
        return {"status": "skipped", "reason": "LUT artifacts not found", "qualified": False}
    errors: list[float] = []
    worst: list[dict] = []
    for index, point in enumerate(points):
        if precomputed_references is not None and point in precomputed_references:
            reference = precomputed_references[point]
        else:
            reference = integrate_reference(
                point, reference_samples, sequence_seed, backend
            ).value
        runtime = lut.sample_runtime(point)
        error = abs(runtime - reference)
        errors.append(error)
        record = {
            "case": index,
            **asdict(point),
            "reference": reference,
            "lut": runtime,
            "absolute_error": error,
        }
        worst.append(record)
        rows.append(
            _row(
                name,
                str(index),
                **_point_fields(point),
                reference=reference,
                lut=runtime,
                abs_error=error,
            )
        )
    metrics = error_metrics(errors)
    threshold = threshold_result(metrics, thresholds)
    return {
        "status": "pass" if qualified and threshold["passed"] else ("fail" if qualified else "informational"),
        "qualified": qualified,
        "reference_samples_per_point": reference_samples,
        "metrics": metrics,
        "acceptance": threshold,
        "worst_cases": sorted(worst, key=lambda item: item["absolute_error"], reverse=True)[:10],
    }


def validate_quantization(
    points: Sequence[ReferenceInput],
    layout: LUTLayout,
    lut: PackedLUT | None,
    config: ValidationConfig,
    backend: str,
    rows: list[dict],
) -> dict:
    reconstructed = ReconstructedFloatLUT(
        layout,
        config.float_reconstruction_samples,
        config.sequence_seed ^ 0x9E3779B9,
        backend,
    )
    float_vs_mc_errors = []
    simulated_quantization_errors = []
    packed_vs_float_errors = []
    trilinear_errors = []
    for index, point in enumerate(points):
        fresh = integrate_reference(
            point, config.reference_samples, config.sequence_seed, backend
        ).value
        float_lut = reconstructed.sample_float(point)
        rgba8_simulated = reconstructed.sample_quantized(point)
        float_vs_mc_errors.append(abs(float_lut - fresh))
        simulated_quantization_errors.append(abs(rgba8_simulated - float_lut))
        packed = None
        if lut:
            table_point = ReferenceInput(
                point.visibility,
                clamp(point.roughness, 0.1, 1.0),
                point.no_l,
                point.no_v,
                point.phi_radians,
            )
            packed = lut.sample_interpolated(table_point)
        if lut:
            packed_vs_float_errors.append(abs(packed - float_lut))
            runtime = lut.sample_runtime(point)
            trilinear_errors.append(abs(runtime - lut.sample_cpu_bruteforce(point)))
        rows.append(
            _row(
                "quantization",
                str(index),
                **_point_fields(point),
                reference=fresh,
                lut=packed if packed is not None else "",
                float_lut=float_lut,
                rgba8_simulated=rgba8_simulated,
                abs_error=abs(rgba8_simulated - float_lut),
                detail="added RGBA8 error from independently reconstructed local float texels",
            )
        )
    quant_metrics = error_metrics(simulated_quantization_errors)
    quant_pass = quant_metrics["mean"] is not None and quant_metrics["mean"] <= 0.003
    result = {
        "mode": "lazy_independent_float_texel_reconstruction",
        "float_reconstruction_samples_per_texel": config.float_reconstruction_samples,
        "evaluated_float_texels": reconstructed.evaluated_texels,
        "float_lut_vs_fresh_mc": error_metrics(float_vs_mc_errors),
        "simulated_rgba8_vs_float_lut": {
            "metrics": quant_metrics,
            "mean_threshold": 0.003,
            "passed": quant_pass,
        },
        "packed_rgba8_vs_reconstructed_float_lut": (
            error_metrics(packed_vs_float_errors)
            if lut
            else {"status": "skipped", "reason": "LUT artifacts not found"}
        ),
        "runtime_trilinear_vs_independent_cpu_weighted_sum": (
            {
                "metrics": error_metrics(trilinear_errors),
                "passed": max(trilinear_errors, default=0.0) <= 1.0e-12,
            }
            if lut
            else {"status": "skipped", "reason": "LUT artifacts not found"}
        ),
    }
    return result


def validate_azimuth(
    base_points: Sequence[ReferenceInput],
    lut: PackedLUT | None,
    config: ValidationConfig,
    backend: str,
    rng: random.Random,
    rows: list[dict],
) -> dict:
    fixed_degrees = [0.0, 45.0, 90.0, 135.0, 180.0]
    canonical_vs_average = []
    canonical_vs_minimum = []
    canonical_vs_maximum = []
    all_reference_errors = []
    cases = []
    for base_index, base in enumerate(base_points):
        phis = [math.radians(value) for value in fixed_degrees]
        phis.extend(TWO_PI * rng.random() for _ in range(config.azimuth_random_phis))
        samples = []
        for phi_index, phi in enumerate(phis):
            point = ReferenceInput(base.visibility, base.roughness, base.no_l, base.no_v, phi)
            reference = integrate_reference(
                point, config.reference_samples, config.sequence_seed, backend
            ).value
            samples.append(reference)
            rows.append(
                _row(
                    "relative_azimuth",
                    f"{base_index}_{phi_index}",
                    **_point_fields(point),
                    reference=reference,
                )
            )
        canonical = (
            lut.sample_runtime(base)
            if lut
            else integrate_reference(
                base, config.reference_samples, config.sequence_seed, backend
            ).value
        )
        average = sum(samples) / len(samples)
        minimum = min(samples)
        maximum = max(samples)
        canonical_vs_average.append(abs(canonical - average))
        canonical_vs_minimum.append(abs(canonical - minimum))
        canonical_vs_maximum.append(abs(canonical - maximum))
        all_reference_errors.extend(abs(canonical - value) for value in samples)
        cases.append(
            {
                "base_point": asdict(base),
                "canonical_source": "packed_4d_lut" if lut else "phi_zero_reference_no_lut",
                "canonical": canonical,
                "azimuth_average": average,
                "azimuth_minimum": minimum,
                "azimuth_maximum": maximum,
                "fixed_phi_degrees": fixed_degrees,
                "continuous_random_phi_count": config.azimuth_random_phis,
            }
        )
    return {
        "status": "measured",
        "canonical_source": "packed_4d_lut" if lut else "phi_zero_reference_no_lut",
        "canonical_error_vs_azimuth_average": error_metrics(canonical_vs_average),
        "canonical_error_vs_min_over_azimuth": error_metrics(canonical_vs_minimum),
        "canonical_error_vs_max_over_azimuth": error_metrics(canonical_vs_maximum),
        "canonical_error_vs_all_sampled_5d_references": error_metrics(all_reference_errors),
        "cases": cases,
    }


def validate_monotonicity(lut: PackedLUT | None, config: ValidationConfig, rows: list[dict]) -> dict:
    if lut is None:
        return {"status": "skipped", "reason": "LUT artifacts not found"}
    layout = lut.layout
    violations = []

    def check(previous: float, current: float, axis: str, indices: tuple[int, ...]) -> None:
        if current + config.monotonic_tolerance < previous:
            violations.append(
                {
                    "axis": axis,
                    "indices": indices,
                    "previous": previous,
                    "current": current,
                    "drop": previous - current,
                }
            )

    for bank in range(layout.bank_count):
        for channel in range(4):
            for iz in range(layout.dimensions[2]):
                for iy in range(layout.dimensions[1]):
                    previous = lut.channel(0, iy, iz, bank, channel)
                    for ix in range(1, layout.dimensions[0]):
                        current = lut.channel(ix, iy, iz, bank, channel)
                        check(
                            previous,
                            current,
                            "visibility",
                            (ix, iy, iz, bank, channel),
                        )
                        previous = current
            for iz in range(layout.dimensions[2]):
                for ix in range(layout.dimensions[0]):
                    previous = lut.channel(ix, 0, iz, bank, channel)
                    for iy in range(1, layout.dimensions[1]):
                        current = lut.channel(ix, iy, iz, bank, channel)
                        check(previous, current, "NoL", (ix, iy, iz, bank, channel))
                        previous = current
    for index, violation in enumerate(violations):
        rows.append(
            _row(
                "monotonicity",
                str(index),
                status="violation",
                abs_error=violation["drop"],
                detail=json.dumps(violation, separators=(",", ":")),
            )
        )
    return {
        "status": "pass" if not violations else "fail",
        "tolerance": config.monotonic_tolerance,
        "violation_count": len(violations),
        "raw_violations": violations,
        "post_filtering_applied": False,
    }


def run_validation(
    config: ValidationConfig,
    output_dir: Path,
    lut: PackedLUT | None,
) -> tuple[dict, list[dict]]:
    resolved_backend = resolve_reference_backend(config.reference_backend)
    rng = random.Random(config.parameter_seed)
    rows: list[dict] = []
    random_points = [_random_point(rng) for _ in range(config.random_points)]
    boundary_points = [_boundary_point(rng) for _ in range(config.boundary_points)]
    quantization_points = [_random_point(rng) for _ in range(config.quantization_points)]
    azimuth_points = [_random_point(rng) for _ in range(config.azimuth_base_points)]

    random_qualified = config.random_points >= 10000 and config.reference_samples >= 16384
    boundary_qualified = config.boundary_points > 0 and config.boundary_reference_samples >= 65536
    random_reference_cache: dict[ReferenceInput, float] = {}
    layout = lut.layout if lut else LUTLayout.baseline()
    artifact = (
        {
            "status": "loaded",
            "manifest_path": str(lut.manifest_path.resolve()),
            "include_path": str(lut.include_path.resolve()),
            "manifest_contract": {
                key: lut.manifest.get(key)
                for key in (
                    "algorithm",
                    "distribution",
                    "vndfSampler",
                    "smithCorrelation",
                    "visibilityWeighting",
                    "dimensions",
                    "roughnessNodes",
                    "axisWarp",
                    "relativeAzimuth",
                    "samplesPerCell",
                    "sequence",
                    "seed",
                    "quantization",
                    "generatorVersion",
                    "roughnessInterpolation",
                    "roughnessBanks",
                    "roughnessBankSplit",
                    "roughnessBankSelection",
                    "linearIndexOrder",
                )
                if key in lut.manifest
            },
            "assumptions_and_warnings": lut.assumptions,
            "hash": lut.hash_report,
        }
        if lut
        else {"status": "not_found", "reason": "manifest and packed include were absent"}
    )

    report = {
        "schema": "MLSMicroShadowValidationReportV1",
        "implementation": {
            "reference_sampler": "DupuyBenyoub2023SphericalCapsIsotropic",
            "sample_sequence": DEFAULT_SEQUENCE_NAME,
            "sample_sequence_version": 1,
            "sequence_description": "2D Hammersley point set; stratum-center X and uint32 reversed-bit Y with fixed XOR digital shift",
            "parameter_sequence": "PythonRandomMersenneTwister_deterministic",
            "strict_step_positive": True,
            "alpha_mapping": "roughness_squared",
            "canonical_relative_azimuth": "coplanar_same_azimuth_phi_zero",
            "reference_backend_requested": config.reference_backend,
            "reference_backend_resolved": resolved_backend,
            "numpy_version": np.__version__ if resolved_backend == "numpy" else None,
            "numpy_sequence_cache_entries_max": (
                _NUMPY_SEQUENCE_CACHE_LIMIT if resolved_backend == "numpy" else None
            ),
            "random_reference_reused_for_bounds_and_error": True,
        },
        "configuration": asdict(config),
        "artifact": artifact,
        "endpoints": validate_endpoints(lut, config, resolved_backend, rows),
        "bounds": validate_bounds(
            random_points,
            lut,
            config,
            resolved_backend,
            rows,
            random_reference_cache,
        ),
        "smooth_limit": validate_smooth_limit(lut, config, resolved_backend, rows),
        "random_error": validate_error_set(
            "random_error",
            random_points,
            lut,
            config.reference_samples,
            config.sequence_seed,
            RANDOM_THRESHOLDS,
            random_qualified,
            resolved_backend,
            rows,
            random_reference_cache,
        ),
        "near_boundary_error": validate_error_set(
            "near_boundary_error",
            boundary_points,
            lut,
            config.boundary_reference_samples,
            config.sequence_seed ^ 0xA511E9B3,
            BOUNDARY_THRESHOLDS,
            boundary_qualified,
            resolved_backend,
            rows,
        ),
        "quantization": validate_quantization(
            quantization_points, layout, lut, config, resolved_backend, rows
        ),
        "relative_azimuth": validate_azimuth(
            azimuth_points, lut, config, resolved_backend, rng, rows
        ),
        "monotonicity": validate_monotonicity(lut, config, rows),
    }

    hard_failures = []
    if not report["endpoints"]["passed"]:
        hard_failures.append("analytical endpoints")
    if not report["smooth_limit"]["integrator_r_zero_exact_passed"]:
        hard_failures.append("integrator r=0 smooth limit")
    if not report["bounds"]["reference"]["passed"]:
        hard_failures.append("reference bounds")
    if lut and report["bounds"]["runtime_lut"].get("status") == "fail":
        hard_failures.append("runtime LUT bounds")
    for key in ("random_error", "near_boundary_error"):
        section = report[key]
        if section.get("qualified") and section.get("status") == "fail":
            hard_failures.append(key)
    if lut and report["quantization"]["runtime_trilinear_vs_independent_cpu_weighted_sum"].get("passed") is False:
        hard_failures.append("runtime trilinear cross-check")
    if not report["quantization"]["simulated_rgba8_vs_float_lut"]["passed"]:
        hard_failures.append("RGBA8 added mean error")
    acceptance_qualified = bool(lut) and random_qualified and boundary_qualified
    if hard_failures:
        overall_status = "fail"
    elif config.profile == "smoke":
        overall_status = "informational"
    elif not acceptance_qualified:
        overall_status = "incomplete"
    else:
        overall_status = "pass"
    report["overall"] = {
        "status": overall_status,
        "hard_failures": hard_failures,
        "acceptance_qualified": acceptance_qualified,
        "note": (
            "Smoke results are diagnostics, not release acceptance."
            if config.profile == "smoke"
            else "Qualified error sections use execution-spec sample minima."
        ),
    }

    output_dir.mkdir(parents=True, exist_ok=True)
    report_path = output_dir / "report.json"
    csv_path = output_dir / "reference.csv"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    return report, rows
