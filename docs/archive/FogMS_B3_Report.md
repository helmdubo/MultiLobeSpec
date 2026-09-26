# FogMS B3: angular transport and density animation

21 September 2026. Implemented and installed in the existing MimirHead project, Package5. The editor is open on the existing FogMS_Box map, with B3 / 48 directions / 24 iterations selected. B2 remains the accepted comparison. Owner field acceptance of B3 is pending. The changes remain local in `codex/fogms-b2`; this report does not claim a new commit, push or merge.

## Scope

- Preserve enum 4 / Transport (B2) and its six exponential full-line sweeps.
- Add enum 5 / Transport (B3 Angular): 48 paired positive half-range Gauss ordinates; optional 96. Solve the isotropic stationary equation with reciprocal finite-volume upwind sweeps and PCG. No artistic strength or brightness renormalization.
- Keep the existing 32 cubed world grid, shared geometry faces, density, native light adapters and connected receiver reconstruction.
- Add opt-in world-space density wind and relative octave velocities. A shared CPU time and phase snapshot feeds density, self-shadow, surface shadow and the transport solver. No Engine edits and no extra level.
- This remains isotropic (g=0). Directional phase/anisotropy needs a separate transport unknown and acceptance step. Animated noise is authored density evolution, not fluid simulation or conservation of fog mass.

## CPU evidence

`Tools/FogMSEnergyValidation/angular_transport_reference.py` contains the independent implementation. GL48: 139/139 contracts passed; GL96: 31/31 3D contracts passed. Across 102 slab normals at tau=4/albedo=.9, worst angular RMS is .868% / .394%; combined error with 32-cell first-order spatial discretization is 2.240% / 1.819%. These figures concern those fixtures, not arbitrary scene error. Independent shader-index float32 emulation agreed with CPU double to RMS <=1.55e-7.

The scheme is positive and reciprocal for paired directions and shared face masks. Face fluxes cancel discretely. Upwind spatial diffusion is first order and remains a limitation. GL48 integrates the z hemisphere moment exactly, with x/y first-moment bias about +2.531%. Boundary radiance in scene mode is sampled from the boundary cell center; grazing directions and geometry near an outer face retain that approximation. Missing Lumen surface coverage is exposed rather than silently called valid.

`density_animation_contract.py`: 10/10 CPU contracts passed. Ordinary phase progression does not reset all native fog history. A local extinction-change test shortens native history inside the animated box (10% dead band, continuous exp2 response). That reconstruction heuristic does not change the stationary transport or its energy accounting. It is not a motion-vector reconstruction or proof of zero temporal trails.

## Runtime gate

Evidence root: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_B3_20260921`.
The original B2 source snapshot and current authored scene were preserved before edits. Package5 passed BuildPlugin StrictIncludes with no PCH/shared PCH/unity. D3D12/SM6 global and overlay shaders compiled; installed hashes match all 84 package files, and current Source/Shaders/Config/Content match the immutable build input. Six Engine anchors and the existing parallel-translate guard remained unchanged (`delivery-verification.json`). The final log has no Python error, shader failure, assertion or crash. Native optional-profiler DLL messages, built-in UnifiedErrorTest messages and Epic telemetry warnings are unrelated.

| Verification | Result |
|---|---|
| Independent CPU/GPU, GL48/96, furnace, absorption, vacuum, RGB albedo and thin wall | 9/9 PASS on final Package5 |
| Worst GPU/CPU radiance RMS in those fixtures | 0.0006361% |
| Worst absolute relative flux defect in those fixtures | 0.0002687% |
| Scene Sun/Point/Spot doubled and zeroed independently | Exact 2x / zero in captured fields |
| Scene FOV30/FOV90/dolly/return/repeat | Worst world-lighting RMS change 0.02752% |
| 24 versus 64 iterations in the live scene | RMS 0.02690%, including live Lumen variation |
| Animation: static/time0, advection, resize, relative detail, live clock, Freeze/Resume | 11/11 scenarios PASS; all restoration checks PASS |
| Static/time0, wind-following Box and frozen coefficient comparisons | Bit-identical coefficients |
| CPU phase to MID / frozen MID to GPU packet | 0 / <=4.97e-10 wrapped error |
| Normal animated frames | Revision stable; no continuous global history reset |
| Soak with B2/B3/96, visibility, disabled/native and unsupported-g recovery | 1800 frames / 93.42 seconds, no errors; guard stays 0 |

Numerical evidence: `gpu_b3-b3-numeric-002-analysis.json`; scene/source measurements: `scene-01/analysis.json`; animation: `animation/animation-002/analysis.json`; soak: `soak-02/receipt.json`. The scene/source test used Package3; the final numerical suite repeated after the Package5 optimization. Package5 also repeated the 1800-frame runtime test. Portable harnesses are in `Tools/FogMSEnergyValidation/B3Probe` and `AnimationProbe`.

## Cost and optimization

Median of three native ProfileGPU captures per mode, same authored scene and global preview settings, without readbacks:

| Mode | Graphics frame | Own solver passes |
|---|---:|---:|
| B2 / 24 iterations | 16.64 ms | 2.206 ms |
| B3 / 48 / 24 iterations | 31.85 ms | 18.186 ms |
| B3 / 96 / 24 iterations | 43.82 ms | 30.028 ms |

B3 is substantially more expensive. The current implementation is a quality mode, not a demonstrated cheap replacement for B2. It adds no frame-history energy reservoir. `r.FogMS.Transport.SkipConverged=1` skips terminal matrix sweeps only after **all** RGB channels satisfy the existing PCG stopping threshold; final transport/flux still run. Before that optimization GL48 cost 22.093 ms and GL96 36.395 ms for the solver. Across the nine fixed fixtures, eight complete atlases stayed bit-identical; GL96 thick furnace radiance differed by RMS 1.51e-7. `SkipConverged=0` retains a complete-pass comparison. These timings do not establish performance on other hardware/scenes or sum asynchronous queues.

## Delivery and limits

- Same 16 actors/map; temporary wall, test source, diagnostics and preferences restored. Final animation is **off** to retain the static comparison, with prepared wind `(80,0,0)`, detail `(0,12,0)`, evolution `(0,0,5)` cm/s. Freeze/Resume are exposed to both Details and Blueprint/Python. Density, albedo, texture size and lights were not retuned for B3.
- `field-ready.json` and the final read-only `scene-inspect-20260921T164412_764069Z-e71730fe.json` record the delivered state. The current camera moved during the later visual pass; its latest pose was preserved, not replaced with the older baseline. Background throttling/autosave are restored to true. An idle background editor can report the one-realtime-view fallback until focus returns.
- Native screenshots `b3-static.png`, `b3-motion-a.png`, `b3-frozen.png` were inspected. They show changing shape and the rendered volume/shadow; differing camera/time means these are **not** a controlled temporal artifact metric. Owner motion/trail/flicker acceptance remains open. Exact live CPU-time-to-GPU-phase parity is also not claimed because readback metadata lacks producer time; frozen parity is measured.
- In the scene test 3028 diffuse-face and 3264 angular-boundary samples lacked surface-cache coverage. They remain explicit unavailable/black sources. Small equation/flux residuals prove the discretized operator for its supplied sources, not complete Lumen coverage or total native-scene energy conservation.
- B3 is isotropic, first-order spatial upwind, with 32 cubed resolution, face-snapped obstacles and boundary-cell source approximation. Froxel pixel detail, strong anisotropy and fluid mass conservation are not solved by this change.
- Package2 first-start exposed a retained null boundary SRV in the shared B2 gather. It was corrected by a valid unused B2 binding; Package3/5 startup and B2/B3 switching passed. The first animation probe stopped because CallInEditor alone did not export Freeze/Resume to Python; BlueprintCallable fixed this and the full final probe passed. Failed receipts remain as evidence.

Next rendering slice: direction-resolved scattering/receiver with a positive, energy-normalized phase operator and a solver suitable for its nonsymmetric system. Multiplying scalar B3 radiance by HG would not implement it. Its angular accuracy, g=0 parity and GPU budget require separate acceptance. Lowland/ground-following density shaping remains a later authoring feature. Field instructions: `FogMS_B3_Verification.md`.
