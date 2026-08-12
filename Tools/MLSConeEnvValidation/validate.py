#!/usr/bin/env python3
"""Validate a future generated CARD-09 cone EnvBRDF artifact."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from cone_env import PackedConeEnvLUT, resolve_backend, validate_lut


TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=REPO_ROOT / "Resources" / "Generated" / "MLS_ConeEnvBRDF.manifest.json",
    )
    parser.add_argument(
        "--include",
        type=Path,
        default=REPO_ROOT / "Resources" / "Generated" / "MLS_ConeEnvBRDF.ush",
    )
    parser.add_argument("--backend", choices=("auto", "numpy", "scalar"), default="auto")
    parser.add_argument("--random-points", type=int, default=256)
    parser.add_argument("--reference-samples", type=int, default=4096)
    parser.add_argument("--output-dir", type=Path, default=TOOL_DIR / "reports" / "latest")
    args = parser.parse_args()
    if args.random_points <= 0 or args.reference_samples <= 0:
        parser.error("validation counts must be positive")
    backend = resolve_backend(args.backend)
    lut = PackedConeEnvLUT.load(args.manifest, args.include)
    report = validate_lut(lut, args.random_points, args.reference_samples, backend)
    rows = report["heldOutRandom"].pop("rows")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    report_path = args.output_dir / "report.json"
    csv_path = args.output_dir / "held_out.csv"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"backend: {backend}")
    print(f"monotonic violations: {report['monotonicity']['violationCount']}")
    print(f"A errors: {report['heldOutRandom']['A']}")
    print(f"B errors: {report['heldOutRandom']['B']}")
    print(f"JSON: {report_path.resolve()}")
    print(f"CSV: {csv_path.resolve()}")
    return 1 if report["monotonicity"]["violationCount"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
