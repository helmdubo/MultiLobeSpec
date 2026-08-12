# MultiLobeSpec Generic VNDF validation

This directory contains an independent Python 3 reference and validation harness for the v0.14 **Generic VNDF-Based Micro-Shadowing (Isotropic LUT)** contract. It does not import or execute code from `Source/`. The scalar implementation uses only the standard library; an optional NumPy backend accelerates large validation runs.

The reference follows execution-spec sections 3, 4, and 10:

- literal Dupuy--Benyoub 2023 spherical-cap isotropic GGX VNDF sampling;
- `alpha = Roughness^2`, height-correlated Smith `G2/G1`, strict `StepPositive(x > 0)`, and cone/hemisphere normalization;
- deterministic `XorScrambledHammersley32` sample points. This is a 2D Hammersley point set with a fixed XOR digital shift of the reversed-bit coordinate; it is intentionally **not** described as Owen-scrambled Sobol;
- warped `Visibility`, `NoL`, and `NoV` LUT coordinates, RGBA8 unpacking, spatial trilinear filtering, and roughness interpolation;
- both `linear` axis candidates and the warped `square` / `oneMinusSquare` layouts required for objective layout comparison;
- visibility-dependent `smoothLimitBoundarySignedSquare` NoL candidates centered on the analytical smooth-limit boundary;
- endpoint-safe `symmetricRatioSquare` Visibility candidates paired with square NoV and boundary-aware NoL;
- V2 `SingleBankPiecewise` roughness packing with two bank-major RGBA texels and seven shared roughness nodes, while retaining V1 single-bank compatibility;
- analytical endpoints, the true `Roughness -> 0` integrator limit, runtime `Roughness >= 0.1` clamp behavior, random and near-boundary errors, quantization, raw monotonicity, and fixed plus continuous relative-azimuth sweeps.

## Run

From this directory:

```powershell
python validate.py --profile smoke
python -m unittest discover -s . -p "test_*.py" -v
```

The lightweight `smoke` profile writes deterministic machine-readable files to `reports/latest/report.json` and `reports/latest/reference.csv`. If both standard artifacts exist, they are automatically loaded:

```text
../../Resources/Generated/MLS_MicroShadowLUT.manifest.json
../../Resources/Generated/MLS_MicroShadowLUT.ush
```

Use explicit paths or require their presence when validating copied artifacts:

```powershell
python validate.py --profile smoke `
  --manifest C:\path\MLS_MicroShadowLUT.manifest.json `
  --include C:\path\MLS_MicroShadowLUT.ush `
  --require-lut --strict
```

The packed array must be named `MLS_VNDF_LUT_PACKED`. The loader validates its texel count against `dimensions`. The manifest should state `roughnessInterpolation` and `linearIndexOrder`; for compatibility with the execution-spec example, missing fields are sampled as linear perceptual roughness and `Visibility`-fastest/`NoL`/`NoV`, and the assumptions are recorded in JSON rather than hidden.

## Profiles and acceptance

`smoke` is intentionally quick and is always labeled `informational`; it cannot be used as release acceptance. `canonical` selects the section 10 minima: 10,000 random points at 16,384 fresh reference samples and a targeted near-boundary sweep at 65,536 samples. `high` increases the random sweep/reference counts further. Full profiles are computationally expensive and are meant for unattended validation.

```powershell
python validate.py --profile canonical --require-lut --strict `
  --backend numpy `
  --output-dir reports\canonical
```

`--backend auto` is the default and selects NumPy when available, otherwise the dependency-free scalar implementation. `--backend scalar` forces the compatibility oracle. The resolved backend and NumPy version are written into every report. Scalar/vectorized parity tests cover representative, grazing, smooth-boundary, and relative-azimuth cases.

Benchmark the exact canonical workload shape before a long run:

```powershell
python benchmark.py --backend numpy
```

This performs several million real reference samples at the canonical 16,384/65,536/8,192 counts and writes `reports/benchmark.json`, including a transparent component-by-component wall-time estimate. It is an estimate rather than an acceptance result.

Every count and seed can be overridden. For example, this is a practical intermediate run, still reported as unqualified when it is below the normative minima:

```powershell
python validate.py --profile smoke --require-lut `
  --random-points 1000 --boundary-points 200 `
  --reference-samples 4096 --boundary-reference-samples 8192 `
  --float-reconstruction-samples 4096 `
  --output-dir reports\intermediate
```

Important report semantics:

- `random_error` and `near_boundary_error` are skipped, not passed, when generated LUT artifacts do not exist.
- The quantization section lazily reconstructs independent float texels around sampled queries. `simulated_rgba8_vs_float_lut` isolates UNORM8 rounding; `packed_rgba8_vs_reconstructed_float_lut` also contains bake-sequence/sample-count differences unless reconstruction exactly matches the manifest generator contract.
- The relative-azimuth report uses the packed canonical 4D LUT when present. Without artifacts it explicitly falls back to the `Phi=0` reference and labels that source.
- Runtime early-out and roughness clamp are applied only by the runtime LUT path. The reference integrator remains the unclamped section 3 integral.
- No monotonic post-filter is applied. All packed-data violations beyond the stated tolerance are retained in JSON/CSV.

## Output schema

`report.json` uses schema name `MLSMicroShadowValidationReportV1` and contains the complete configuration, artifact/manifest receipt, metrics, thresholds, qualification flags, worst cases, quantization split, and azimuth statistics. `reference.csv` contains per-case values and raw monotonicity violations suitable for external analysis.

The process exits `2` for malformed or incomplete artifacts. With `--strict`, it exits `1` for qualified numerical failures or an incomplete non-smoke acceptance run, and `0` otherwise. A missing LUT is an immediate error only when `--require-lut` is supplied.

## Source references

- Repository execution spec: `MultiLobeSpec_v0.14_Generic_VNDF_Execution_Spec_RU.md`, sections 3, 4, and 10.
- J. Dupuy and A. Benyoub, *Sampling Visible GGX Normals with Spherical Caps*, Computer Graphics Forum 42(8), 2023, DOI `10.1111/cgf.14867`.
