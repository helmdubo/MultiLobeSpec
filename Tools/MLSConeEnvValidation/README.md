# CARD-09 cone-aware EnvBRDF oracle

This directory is an independent Python 3 generator and validator for the confirmed MultiLobeSpec CARD-09 cone-aware environment-BRDF contract. It does not import plugin code or modify `Source/` and `Resources/`.

The reference is a literal translation of the UE 5.7 `SystemTextures.cpp` PreintegratedGF estimator:

- GGX NDF half-vector sampling with `E1=i/N` and bit-reversed `E2`;
- the UE visibility/PDF and Fresnel scale/bias (`A`, `B`) equations;
- strict acceptance `NoL > AdjustedCosCone`;
- both accumulators divided by the common total `N`, not accepted samples.

Consequently, `AdjustedCosCone=0` exactly preserves the UE unoccluded `NoL>0` estimator and `AdjustedCosCone=1` produces exact zero.

## Canonical layout and packing

```text
dimensions: 32 NoV x 32 perceptual Roughness x 8 AdjustedCosCone
NoV nodes:       (i + 0.5) / 32
Roughness nodes: (j + 0.5) / 32
Cone nodes:      k / 7, including 0 and 1
index:           NoV + 32 * (Roughness + 32 * Cone)
format:          RG16_UNORM packed in uint, A low 16 bits and B high 16 bits
response:        F0 * A + F90 * B
```

Runtime interpolation clamps NoV/Roughness to their nearest center node and uses endpoint interpolation for AdjustedCosCone. Sampling is manual trilinear over eight packed texels.

The manifest and include contract is implemented fail-closed by `PackedConeEnvLUT.load`. It accepts the original `MLS_CONE_ENVBRDF_PACKED`, one `uint4` array, or the final eight-plane `MLS_CONE_ENVBRDF_PLANE_C0...C7` staging layout and verifies the declared storage grouping. The manifest's `dataSHA256` hashes the complete logical sequence of little-endian packed RG16 words.

## Commands

From this directory:

```powershell
python -B -m unittest discover -s . -p "test_*.py" -v
python -B -m py_compile cone_env.py generate.py validate.py test_cone_env.py
```

Generate an oracle artifact locally:

```powershell
python -B generate.py --backend numpy --samples 128 --output-dir generated
```

Validate it against held-out parameter points and a higher-sample reference:

```powershell
python -B validate.py --backend numpy `
  --manifest generated\MLS_ConeEnvBRDF.manifest.json `
  --include generated\MLS_ConeEnvBRDF.ush `
  --random-points 256 --reference-samples 4096 `
  --output-dir reports\smoke
```

`--backend auto` prefers NumPy when installed and otherwise uses the standard-library scalar oracle. `--backend scalar` forces the dependency-free path. Scalar/NumPy parity is covered by tests.

The validation report contains raw monotonicity violations and separate held-out absolute-error distributions for A and B. No shipping acceptance thresholds are invented here; consumers must set them in the CARD-09 execution/acceptance contract.

## Cone adjustment

`adjust_specular_cone` is a literal CPU translation of `MLS_AdjustSpecularCone` from execution-spec section 8.1. Feed its output—not unadjusted material `CosCone`—to the third LUT axis. The helper preserves documented endpoints for open, fully glossy, grazing, and fully rough/normal-view cases.
