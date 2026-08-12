#!/usr/bin/env python3
"""Command-line entry point for independent MLS micro-shadow validation."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import replace
from pathlib import Path

from mls_validation import PackedLUT, ValidationConfig, run_validation


TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]
DEFAULT_MANIFEST = REPO_ROOT / "Resources" / "Generated" / "MLS_MicroShadowLUT.manifest.json"
DEFAULT_INCLUDE = REPO_ROOT / "Resources" / "Generated" / "MLS_MicroShadowLUT.ush"


def _int_auto(value: str) -> int:
    return int(value, 0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate MultiLobeSpec v0.14 Generic VNDF micro-shadowing."
    )
    parser.add_argument("--profile", choices=("smoke", "canonical", "high"), default="smoke")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--include", type=Path, default=DEFAULT_INCLUDE)
    parser.add_argument("--output-dir", type=Path, default=TOOL_DIR / "reports" / "latest")
    parser.add_argument("--require-lut", action="store_true", help="Fail if generated artifacts are absent.")
    parser.add_argument("--strict", action="store_true", help="Return nonzero for a qualified numerical failure.")
    parser.add_argument(
        "--backend",
        dest="reference_backend",
        choices=("auto", "numpy", "scalar"),
        help="Reference integration backend (default: auto, preferring NumPy when installed).",
    )

    parser.add_argument("--random-points", type=int)
    parser.add_argument("--boundary-points", type=int)
    parser.add_argument("--reference-samples", type=int)
    parser.add_argument("--boundary-reference-samples", type=int)
    parser.add_argument("--smooth-samples", type=int)
    parser.add_argument("--azimuth-base-points", type=int)
    parser.add_argument("--azimuth-random-phis", type=int)
    parser.add_argument("--quantization-points", type=int)
    parser.add_argument("--float-reconstruction-samples", type=int)
    parser.add_argument("--parameter-seed", type=_int_auto)
    parser.add_argument("--sequence-seed", type=_int_auto)
    return parser


def _apply_overrides(config: ValidationConfig, args: argparse.Namespace) -> ValidationConfig:
    names = (
        "random_points",
        "boundary_points",
        "reference_samples",
        "boundary_reference_samples",
        "smooth_samples",
        "azimuth_base_points",
        "azimuth_random_phis",
        "quantization_points",
        "float_reconstruction_samples",
        "parameter_seed",
        "sequence_seed",
        "reference_backend",
    )
    changes = {name: getattr(args, name) for name in names if getattr(args, name) is not None}
    config = replace(config, **changes)
    positive_names = (
        "reference_samples",
        "boundary_reference_samples",
        "smooth_samples",
        "float_reconstruction_samples",
    )
    positive_count_names = (
        "random_points",
        "boundary_points",
        "azimuth_base_points",
        "quantization_points",
    )
    for name in positive_names:
        if getattr(config, name) <= 0:
            raise ValueError(f"--{name.replace('_', '-')} must be positive")
    for name in positive_count_names:
        if getattr(config, name) <= 0:
            raise ValueError(f"--{name.replace('_', '-')} must be positive")
    if config.azimuth_random_phis < 0:
        raise ValueError("--azimuth-random-phis must be nonnegative")
    return config


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        config = _apply_overrides(ValidationConfig.profile_defaults(args.profile), args)
        manifest_exists = args.manifest.is_file()
        include_exists = args.include.is_file()
        if manifest_exists != include_exists:
            raise ValueError(
                "generated LUT is incomplete: manifest and packed include must either both exist or both be absent"
            )
        if args.require_lut and not manifest_exists:
            raise ValueError("generated LUT artifacts are required but were not found")
        lut = PackedLUT.load(args.manifest, args.include) if manifest_exists else None
        report, rows = run_validation(config, args.output_dir, lut)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"validation error: {error}", file=sys.stderr)
        return 2

    print(f"profile: {config.profile}")
    print(f"LUT artifact: {report['artifact']['status']}")
    print(f"overall: {report['overall']['status']}")
    print(f"reference backend: {report['implementation']['reference_backend_resolved']}")
    print(f"rows: {len(rows)}")
    print(f"JSON: {(args.output_dir / 'report.json').resolve()}")
    print(f"CSV:  {(args.output_dir / 'reference.csv').resolve()}")
    if args.strict and report["overall"]["status"] in {"fail", "incomplete"}:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
