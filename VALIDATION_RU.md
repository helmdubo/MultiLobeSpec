# Validation receipt v0.14

Дата: 2026-08-12. Engine: UE 5.7.4, CL 51494982, source tree `D:\PersonalProjects\UE5\UE_5.7`.

## v0.14.1 Unity Build regression

- Реальный project target `MimirHead_portfolioEditor`, UBT `-ForceUnity`: `Module.MultiLobeSpecEditor.cpp` compile, static/import libraries и DLL — PASS.
- Устранены collisions file-local editor symbols, проявлявшиеся только при unity amalgamation (`C2374`, `C2086`, `C2084`, `C2264`).
- `MultiLobeSpec.*` native Automation Tests: 5/5 PASS, включая исправленный boundary-aware small-LUT contract.
- CPU float/packed LUT samplers возвращают fail-closed zero на неполном interior payload; Visibility endpoints остаются exact 0/1.

## Native build

- Финальный P32 `UnrealEditor Win64 Development`: успешно, 7 actions, 221.77 s; UHT и оба модуля linked.
- Предыдущий полный native build: успешно, 16 actions, 204.92 s.
- Инкрементальная сборка финального cone generator: успешно, 5 actions, 5.74 s.
- UHT: успешно; runtime и editor modules linked.

## Direct Generic VNDF

Канонический artifact: V2 SingleBankPiecewise, two RGBA8 banks, SHA-256 `6251adaf858640aeb7c71b83f9271ab4fdc99288f7abec98b9607805d249a1f3`.

Независимый validator находится в `Tools/MLSMicroShadowValidation`. Он проверяет endpoints, bounds, deterministic QMC, manifest/payload hash, CPU/runtime interpolation, bank split, quantization, azimuth и monotonicity.

## CARD-09 cone EnvBRDF

UE editor bake:

```text
dimensions:       32 × 32 × 8
samples/cell:     8192
packing:          RG16_UNORM, A low16 / B high16
data SHA-256:     b5361e3ddfe51b7b9d80f2f893595a6075899be4c170d007969a21ed5867ce20
```

Независимые tests: 12/12 PASS. Проверены UE estimator goldens, c=0/c=1, finite/bounds, scalar/NumPy parity, cone adjustment, RG16 bound, manifest fail-closed, manual trilinear и golden linear indexing. Нативный `MultiLobeSpec.ConeEnvBRDF.CanonicalContract`: 1/1 Success.

Actual UE artifact, 256 held-out points × 4096 reference samples:

| coefficient | mean | p95 | p99 | max |
|---|---:|---:|---:|---:|
| A | 0.0134160 | 0.0456586 | 0.2805101 | 0.7278126 |
| B | 0.0007683 | 0.0015286 | 0.0227604 | 0.0495775 |

Monotonic violations: 0. UE float bake против independent double bake: 8/8192 packed texels отличаются, максимум на один RG16 step (`1/65535`).

Эти distributions — baseline, не PASS gate: v0.14 не задаёт допустимые численные thresholds, а coarse grid имеет длинный A-tail у high-gloss/grazing boundary. Поэтому numerical admission CARD-09 остаётся false.

## Shader overlay

Runtime exact-count receipt:

```text
Raw MaterialAO source/store:             1/1
Paired IBL AO/gather/response:            1/1/1
Non-Lumen indirect material visibility:   2/2
Lumen indirect diffuse:                   3/3
Skylight indirect visibility:             8/8
```

CARD-09 static-storage compile gate:

| attempt | HLSL layout | workers | result |
|---|---|---:|---|
| P29 | `uint[8192]` | 12 | около 4.46 GB private commit/worker, aggregate thrash, permutation не завершилась |
| P30 | `uint4[2048]` | 12 | около 2.97 GB/worker, aggregate thrash, permutation не завершилась |
| P31 | 8 × `uint4[256]` planes | 12 | около 2.9 GB/worker, aggregate thrash |
| P31 constrained | 8 planes | 1 | около 4.50 GB, более 10 минут CPU без первого output |

Все четыре запуска остановлены после достаточного evidence; shader error не был получен, но bounded compile-cost gate провален. Поэтому CARD-09 static storage не admitted, feature default-off и fail-closed. Pre-remap smoke compile архитектурно ещё отсутствует. Для production требуется renderer-bound Texture3D/SRV path.

Финальный P32 overlay с `MLS_CONE_ENVBRDF=0` и активным direct Generic VNDF прошёл `RecompileShaders All` за 19.50 s на прогретом DDC; shader compile errors отсутствуют. Capability подтвердил raw transport prerequisites `0/0/0`, transport patched, Generic Default Lit available, MegaLights false, CARD-09 artifacts present, runtime unavailable и `StaticStorageCompileAdmitted=false`. Ранее P26 зафиксировал полный cold/changed compile 1832/1832 за 229.57 s.

Итоговые независимые suites: direct Generic VNDF 20/20 PASS, CARD-09 12/12 PASS; `py_compile` обоих tools — PASS.

## Невыполненные acceptance gates

- ratified CARD-09 numerical thresholds;
- renderer-bound Texture3D/SRV transport и его shader compile/code-size gate;
- GPU instruction/static-read cost и capture/skylight timing;
- image regression/halo transition и W={0,0.5,1} scene sweep;
- Lumen/SSR per-lobe identity;
- cross-platform shader compile;
- cooked build support.
