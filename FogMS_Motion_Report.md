# FogMS: coherent density motion and froxel reconstruction

21 September 2026. **Package4 checkpoint: installed and READY_FOR_FIELD. Controlled final image/cost A/B remains inconclusive.**

Subsequent Package5 moving-detail work is tracked separately in [FogMS_EdgeFlow_Report.md](FogMS_EdgeFlow_Report.md); the Package4 evidence below remains its original checkpoint.

This slice adds continuous world-space wind controls and reduces reliance on accumulated image history when the camera or density moves. It preserves the current-frame B2/B3 transport equation and native global HistoryWeight. The Engine remains read only. Work uses the existing MimirHead project and `/Game/FogMS_Test/FogMS_Box`; no additional level is required.

## Current implementation

- Density injection, the Box lighting receiver and previous-density temporal support share four fixed tetrahedral positions and the native depth-offset constraint. Positions are `.5 + (1/sqrt(12)) * sign`, with signs `+++`, `+--`, `-+-`, `--+` and weight `.25`. The source is `average(sigma_s * J)`. Constants, linear terms and degree-two moments are exact in normalized froxel coordinates; mixed cubic `xyz` is not. There is no eight-sample quality switch.
- Field metadata is read once outside the footprint loop. Native medium, emissive, missing-field fallback and VBuffer quantization remain present; only valid Box coefficients are replaced. Exposure scales the final source once, not extinction. Reciprocal wall connectivity remains. The experimental cross-sample stencil cache is absent from Package4.
- Local image-history confidence responds to camera displacement in froxel units and changes in extinction and exposure-corrected RGB. It applies to current Box density or valid previous animated density; empty geometric bounds alone do not shorten native history. Structural edits retain the existing reset path.
- A resident `24 x 2` texture retains the 24-float4 producer packet in row zero and previous actual octave phases in row one. Current/previous values travel together. Repeated publications within one `GFrameCounter` do not advance history; worlds keep separate phase state. First use/skipped frames reset active-animation history. No previous density atlas is retained.
- `Use Directional Motion` enables an independent rotatable wind arrow, speed in cm/s and `Texture Offset (World)` in cm. Editing speed/direction preserves current displacement; only subsequent motion changes. With both relative detail velocities zero, all three octaves follow one wind. Box scale/rotation does not stretch or rotate world-aligned noise.
- Existing serialized actors retain Legacy Velocity Vectors until explicitly converted. Freeze/resume preserve phase; manual time and Time Offset are deliberate seeks. The hidden serialized motion reference is `BlueprintReadOnly` and restored through a finite-validating API. This avoids UE construction resetting a non-editable writable Blueprint property during wind edits.

Four points halve the logical sample count compared with the interim eight-point implementation. Three manually trilinear density octaves require 96 rather than 192 atlas texel requests per estimate before compiler/cache effects. That excludes other passes and previous-phase work and does not imply proportional GPU-time savings.

## Verification evidence

Evidence root: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921`.

| Evidence | Result and exact scope |
|---|---|
| `Build4.receipt.json`, `install4-receipt.json` | Build/install PASS; 85 installed files |
| `reconstruction4-contract.json` | 9/9 CPU quadrature/history/source contracts PASS; explicitly records non-exact `xyz` |
| `directional-motion-lifecycle-contract.json` | 20/20 CPU reference/lifecycle contracts PASS |
| `directional-225339-607885.json` | Package4 native controls 13/13 PASS; all restoration checks true |
| `phase_contract/receipt.json` | Source-extracted C++ packing/state contracts PASS, MSVC `/W4 /WX` |
| `gpu_b3-motion-b3-regression-001-analysis.json` | Package2 GPU numerical suite 9/9 PASS: GL48/96, furnace, absorption, vacuum/gap, RGB and wall fixtures |
| `regression-source-parity.json` | Seven producer/operator/runtime files byte-identical Package2 to Package4; unchanged numerical paths reuse the preceding GPU evidence, without claiming a new rerun |
| `animation/motion-animation-001/analysis.json` | Package2 legacy animation: 11 cases / 57 checks PASS, restored; frozen parity/world-lock/live/freeze/resume/history revision |
| `field-ready.json` | READY_FOR_FIELD: 1200 frames / 43.219 seconds, directional wind 200 cm/s, sampled wrapped phase error at most 3.45e-7, preferences restored |
| `controlled-bounds-restored.json` | Latest user Box bounds restored before field preparation |
| `engine-after.json` | Six monitored native shader files unchanged |
| Matched Package4 image/cost A/B | **INCONCLUSIVE**; near04/05 bounds differ from baseline |

The legacy animation suite does not prove rendered-trail quality or exact live MID/producer time alignment; live snapshots are asynchronous. Its GPU readbacks invalidate timing conclusions. Changed reconstruction HLSL requires its own image tests even when producer/operator files are unchanged.

## Camera response and cost

The controlled protocol uses one destination camera, static density, fixed exposure, 60-degree yaw, 100 m dolly and FOV 100 to 55 degrees. Each motion repeats for delays 1/2/4/8/32 and an 80-frame reference: 18 native viewport captures. Timing is a request-to-file-observation interval, not an exact GPU-frame claim.

The interim eight-point Package2 run `motion-after-near-03` completed/restored under matching recorded scene conditions. In boundary ROI `[.05,.22,.40,.90]`, early RGB RMS against each run's own reference decreased by about 20% / 34% / 32% for yaw/dolly/FOV; reference drift stayed ~0.72%. Baseline lacks Game View metadata, so this is `LIMITED` evidence supported by PNG inspection. It belongs to that interim implementation and is not inherited by four-point Package4.

Package4 `motion-after-near-04` and `-05` completed capture/restoration but are **invalid cross-run comparisons**: their recorded Box X/Z scale is larger than baseline. The owner subsequently confirmed editing scene parameters during this interval. Their nearly uniform reference loses substantial sky/surface contrast; low error against that altered reference is not a quality gain or proof of a shader defect. The analyzer rejects changed bounds. A future quantitative repeat requires a persistent transform guard and exclusive viewport control. The owner reports that the current result looks substantially better; that qualitative feedback is separate from numerical A/B.

The latest six-sample Package4 profile (`after-four-matched-profile-summary.json`) measured:

| Median, ms | Standing | Moving | Baseline standing / moving |
|---|---:|---:|---:|
| Frame | 38.29 | 38.65 | 28.48 / 28.67 |
| Native LightScattering | 8.072 | 8.014 | 3.073 / 3.046 |
| Own B3 passes | 14.500 | 14.358 | 13.675 / 13.838 |

Frame ranges were 37.81-38.51 / 37.60-38.75 ms. The profile does not record/guard Box bounds, and subsequent near05 disproves persistent matching state. These are run-local costs, not an isolated causal shader-cost estimate or confirmed matched A/B. No performance gain is claimed. Named MaterialSetup timing is unavailable; rounded-zero InitializeVolumeAttributes is not proof of no cost.

Invalid/confounded captures and the rejected cache experiment remain in evidence. The latter passed CPU parity but provided no consistent GPU gain and is absent from Package4. Earlier harness recovery was verified against immutable plain JSON snapshots; failed receipts were retained.

## Limits and remaining acceptance

The medium remains isotropic (`g=0`), with the B3 discretization/native-source limits in [FogMS_B3_Report.md](FogMS_B3_Report.md). Eight authored solver iterations do not prove convergence. Quadrature cannot recover detail below the native froxel footprint or 32-cubed lighting grid, and polynomial accuracy does not establish exact integration of arbitrary textured density.

History reactivity is image reconstruction, not another scattering order. Native and Box contributions share final RGBA history where they overlap. Previous phases are evaluated at current footprint positions, not through exact wind-motion reprojection or a previous-camera Box history volume. Animated density does not conserve fluid mass; additive detail can alter the interior. Anisotropy, edge-only erosion/domain warping and ground-following density remain separate work.

The bounded field run completed 1200 frames in 43.219 seconds with no recorded probe error. It preserved the initial density phase, left B3 / High96 / 8 iterations and directional animation active at 200 cm/s, and kept both relative detail velocities zero. It restored editor preferences, left the camera unchanged and deliberately saved that field-ready state to the existing map; this is distinct from temporary probes, which did not save test states. Latest user bounds were restored separately. The short run is not a long-duration crash guarantee.

This is a checkpoint, not completion of the overall project. Quantitative final A/B, native parity/vacated-cell visual acceptance and longer field stability remain open. The next requested slice investigates moving edge detail resembling native volumetric clouds; edge-only erosion is not part of Package4. Procedure: [FogMS_Motion_Verification.md](FogMS_Motion_Verification.md).
