#!/usr/bin/env python3
"""Benchmark the reference backend and estimate the canonical validation run."""

from __future__ import annotations

import argparse
import json
import platform
import random
import time
from pathlib import Path

from mls_validation import (
    DEFAULT_PARAMETER_SEED,
    DEFAULT_SEQUENCE_SEED,
    LUTLayout,
    ReferenceInput,
    integrate_reference,
    np,
    resolve_reference_backend,
)


TOOL_DIR = Path(__file__).resolve().parent


def random_point(rng: random.Random) -> ReferenceInput:
    return ReferenceInput(rng.random(), 0.1 + 0.9 * rng.random(), rng.random(), rng.random())


def boundary_point(rng: random.Random) -> ReferenceInput:
    return ReferenceInput(
        0.01 + 0.19 * rng.random(),
        0.10 + 0.10 * rng.random(),
        0.75 + 0.25 * rng.random(),
        rng.random(),
    )


def time_group(points: list[ReferenceInput], samples: int, backend: str, seed: int) -> dict:
    start = time.perf_counter()
    checksum = 0.0
    for point in points:
        checksum += integrate_reference(point, samples, seed, backend).value
    elapsed = time.perf_counter() - start
    sample_evaluations = len(points) * samples
    return {
        "points": len(points),
        "samples_per_point": samples,
        "sample_evaluations": sample_evaluations,
        "elapsed_seconds": elapsed,
        "points_per_second": len(points) / elapsed,
        "sample_evaluations_per_second": sample_evaluations / elapsed,
        "checksum": checksum,
    }


def estimate_group(measured: dict, target_points: int, target_samples: int) -> float:
    # Retain measured per-call overhead and scale the sample-dependent portion.
    return (
        measured["elapsed_seconds"]
        * (target_points / measured["points"])
        * (target_samples / measured["samples_per_point"])
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("auto", "numpy", "scalar"), default="auto")
    parser.add_argument("--random-probes", type=int, default=256)
    parser.add_argument("--boundary-probes", type=int, default=64)
    parser.add_argument("--float-texel-probes", type=int, default=128)
    parser.add_argument("--output", type=Path, default=TOOL_DIR / "reports" / "benchmark.json")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if min(args.random_probes, args.boundary_probes, args.float_texel_probes) <= 0:
        raise SystemExit("all probe counts must be positive")
    backend = resolve_reference_backend(args.backend)
    rng = random.Random(DEFAULT_PARAMETER_SEED ^ 0xB16B00B5)
    random_points = [random_point(rng) for _ in range(args.random_probes)]
    boundary_points = [boundary_point(rng) for _ in range(args.boundary_probes)]
    layout = LUTLayout.baseline()
    float_texels = []
    for index in range(args.float_texel_probes):
        flat = index % (16 * 32 * 8 * 4)
        channel = flat % 4
        texel = flat // 4
        ix = texel % 16
        iy = (texel // 16) % 32
        iz = (texel // (16 * 32)) % 8
        float_texels.append(layout.physical_node(ix, iy, iz, channel))

    # Warm imports, ufunc dispatch, and allocator caches before measurements.
    integrate_reference(random_points[0], 1024, DEFAULT_SEQUENCE_SEED, backend)
    measured = {
        "random_16384": time_group(
            random_points, 16384, backend, DEFAULT_SEQUENCE_SEED
        ),
        "near_boundary_65536": time_group(
            boundary_points, 65536, backend, DEFAULT_SEQUENCE_SEED ^ 0xA511E9B3
        ),
        "float_texel_8192": time_group(
            float_texels, 8192, backend, DEFAULT_SEQUENCE_SEED ^ 0x9E3779B9
        ),
    }

    # Exact current canonical configuration. Random bounds and random error
    # share one cached reference evaluation per point.
    workload = {
        "random_reference_unique": {"points": 10000, "samples_per_point": 16384, "rate": "random_16384"},
        "near_boundary": {"points": 512, "samples_per_point": 65536, "rate": "near_boundary_65536"},
        "smooth_positive_roughness": {"points": 192, "samples_per_point": 16384, "rate": "random_16384"},
        "relative_azimuth": {"points": 64 * (5 + 32), "samples_per_point": 16384, "rate": "random_16384"},
        "quantization_fresh_mc": {"points": 256, "samples_per_point": 16384, "rate": "random_16384"},
        "quantization_float_texel_upper_bound": {"points": 256 * 16, "samples_per_point": 8192, "rate": "float_texel_8192"},
    }
    estimated_total = 0.0
    for item in workload.values():
        item["sample_evaluations"] = item["points"] * item["samples_per_point"]
        item["estimated_seconds"] = estimate_group(
            measured[item["rate"]], item["points"], item["samples_per_point"]
        )
        estimated_total += item["estimated_seconds"]

    report = {
        "schema": "MLSMicroShadowValidationBenchmarkV1",
        "backend_requested": args.backend,
        "backend_resolved": backend,
        "python_version": platform.python_version(),
        "platform": platform.platform(),
        "numpy_version": np.__version__ if backend == "numpy" else None,
        "measured": measured,
        "canonical_workload_estimate": {
            "components": workload,
            "total_sample_evaluations": sum(
                item["sample_evaluations"] for item in workload.values()
            ),
            "estimated_seconds": estimated_total,
            "caveat": "Linear estimate from single-process probes; filesystem output, LUT interpolation, cache overlap, and host load can change wall time.",
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"backend: {backend}")
    for name, result in measured.items():
        print(
            f"{name}: {result['sample_evaluations']:,} samples in "
            f"{result['elapsed_seconds']:.3f}s "
            f"({result['sample_evaluations_per_second']:,.0f} samples/s)"
        )
    print(f"canonical estimate: {estimated_total:.1f}s")
    print(f"JSON: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
