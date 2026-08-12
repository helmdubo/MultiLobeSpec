#!/usr/bin/env python3
"""Generate the canonical packed CARD-09 cone EnvBRDF LUT."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from cone_env import canonical_manifest, generate_packed, render_include, resolve_backend


TOOL_DIR = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=128)
    parser.add_argument("--backend", choices=("auto", "numpy", "scalar"), default="auto")
    parser.add_argument("--output-dir", type=Path, default=TOOL_DIR / "generated")
    args = parser.parse_args()
    if args.samples <= 0:
        parser.error("--samples must be positive")
    backend = resolve_backend(args.backend)
    packed = generate_packed(args.samples, backend)
    manifest = canonical_manifest(args.samples, packed)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    include_path = args.output_dir / "MLS_ConeEnvBRDF.ush"
    manifest_path = args.output_dir / "MLS_ConeEnvBRDF.manifest.json"
    include_path.write_text(render_include(packed), encoding="utf-8", newline="\n")
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"backend: {backend}")
    print(f"texels: {len(packed)}")
    print(f"include: {include_path.resolve()}")
    print(f"manifest: {manifest_path.resolve()}")
    print(f"dataSHA256: {manifest['dataSHA256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
