# FogMS: moving detail contours

Subsequent W/S versus A/D diagnosis and restored runtime settings: [FogMS_Dolly_Research.md](FogMS_Dolly_Research.md). This diagnostic did not change the delivered Edge Flow implementation.

21 September 2026. **Package5 installed and READY_FOR_FIELD: CPU, native controls, five morphology captures and the 1200-frame field run pass. Distant dolly/zoom shimmer remains open.** This is an incremental checkpoint after [Package4 motion reconstruction](FogMS_Motion_Report.md), not completion of the lighting project.

`Edge Flow Speed` adds a simple control for detail moving relative to the large-scale fog pattern. It uses the existing two detail octaves, world coordinates and continuous motion reference. The existing MimirHead level `/Game/FogMS_Test/FogMS_Box` is retained. No Engine edit, additional level, shader pass or density equation is introduced.

## Artist controls

Select `FogMS - Live Box`, use Directional motion and enable `Animate Density`. `Wind Speed` moves the whole pattern along the independent wind arrow. `Edge Flow Speed`, in cm/s, moves the detail relative to that common wind; `Detail Strength` controls how much the density contours can change. The saved field setting is wind 200 cm/s, flow 20 cm/s and existing detail strength .1, with both advanced relative vectors zero.

The new control defaults to zero. Zero stops its automatic relative flow while common wind and any advanced relative velocities continue. It preserves the current pattern rather than rewinding it. Freeze/Resume holds and resumes all motion. Legacy Velocity Vectors ignores Edge Flow Speed. Box scaling does not stretch the world-aligned texture; `World Texture Size` deliberately changes its scale.

## Motion contract

For the world-space arrow basis `F`, `R`, `U`, wind speed `W`, edge speed `E`, and existing advanced vectors `D1`, `D2`:

```text
V0 = F * W
V1 = V0 + D1      + R * E
V2 = V0 + D1 + D2 - U * (E / 2)
```

The automatic right-axis offset is not inherited by V2. Its half world speed compensates for the second detail octave's doubled spatial frequency, giving the two automatic components equal UV-speed magnitude on different axes. Base velocity V0 is unchanged. Speed and arrow edits preserve current displacement through the existing piecewise motion reference; no new per-frame history reset is introduced.

Implementation is confined to the actor control and velocity calculation in `FogMS_BoxVolume.h/.cpp`. `edge-source-parity.json` verifies 14 shader, material, producer and runtime files unchanged from Package4. Existing raster density, injected density, shadow cache and B2/B3 paths receive the same established phase packet. That source evidence is not a new rendered coherence or performance measurement.

## What the detail changes

The existing density equation adds bounded detail `delta` to the base sample before the threshold: `abs(delta) <= DetailStrength`. For threshold `T`, softness `S` and strength `D`, a fixed base sample stays empty below `T-S/2-D` and fully filled at or above `T+S/2+D`. At the current `.5/.1/.1` settings those boundaries are `.35` and `.65`. Box feather may still reduce extinction in the filled region.

This is a **base-noise value band**, which can include internal pockets. It is not a geometric outer-edge mask, subtractive erosion, domain warping or fluid simulation. Large detail strength can remove the guaranteed filled/empty plateaus. Common wind also changes the base sample at a fixed world point. Density is not mass conserving, and this control does not add a scattering order or change B3 energy normalization.

## Evidence and remaining acceptance

Evidence root: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921`.

| Evidence | Result and scope |
|---|---|
| `Build5.receipt.json`, `install5-receipt.json` | Strict build and installation PASS; 85 installed files |
| `edge-flow-directional-contract.json` | 20/20 CPU motion/lifecycle reference checks PASS |
| `edge-flow-density-contract.json` | 5/5 CPU density-bound and world-mapping checks PASS; synthetic periodic 32-cubed data, not the project Perlin |
| `edge-flow-20260921-232540-887887.json` | Native actor/MID checks 21/21 PASS; all 13 restoration checks true |
| `edge-source-parity.json` | 14 renderer/material/runtime files unchanged from Package4 |
| `edge-flow-visual-01/receipt.json` | Five native viewport captures PASS; controls/reference, authored scene, camera, map hash and preferences restored |
| `edge-flow-visual-01/image-difference.json` | LDR RMS: static repeat .264%, same-phase flow switch .265%, 10-second relative flow 1.746% |
| `edge-field-ready-03.json` | READY_FOR_FIELD: 1200 frames / 41.797 seconds, 12 phase samples, maximum error 2.95e-8; preferences restored, camera unchanged, existing map saved |
| `edge-field-ready.json`, `edge-field-ready-02.json` | Two prior harness failures retained; scope and diagnosis below |

The native controls cover default/zero/Legacy behavior, nonzero advanced vectors, speed/arrow continuity, invalid-speed rejection and recovery, detail phase advancement, freeze/resume and exact restoration. They do not read back the GPU lighting field or establish rendered temporal quality.

The synthetic density check found no changes outside the predicted band over 8192 points and eight sampled times. It cannot establish the appearance of the authored Perlin, and changes inside the band can reach the full zero-to-one mask range.

Native PNG inspection shows contours changing around a stable large-scale outline. The equal-phase flow switch is at the static renderer noise floor; the 10-second change is visibly and numerically larger. These are warmed static samples from the authored Perlin, not continuous-animation flicker or radiometric acceptance. Much of this close view is opaque/low-contrast, so it does not establish distant edge quality.

The final isolated field run completed 1200 frames in 41.797 seconds, with all 12 recorded MID phase samples within `2.95e-8` of the float-frequency oracle. It kept B3 / High96 / 8 iterations active, preserved initial phase, restored preferences and left the camera unchanged. It deliberately saved the usable state to the existing map; temporary comparison probes did not. This is a bounded smoke/phase check, not a long-duration crash guarantee or GPU lighting-field readback.

Two earlier harness failures remain in evidence. Run01's `6.4881e-6` difference came from the oracle using double-precision frequencies instead of the producer's float32 frequencies; its camera also changed. Run02 reported a transient `.34978` difference but omitted the failing raw sample, so its exact cause remains unknown. Shared global callback state was identified as a harness risk; the final run used an isolated `run()` closure and recorded actual/expected phases and frequencies for every sample. Neither failed run establishes a production defect, and the passing repeat does not retroactively explain Run02.

The owner reports contour shimmer during distant dolly/zoom, with panning and close/interior views looking better. That remains a separate unresolved reconstruction issue: Edge Flow animates detail and is not an antialiasing fix. No GPU cost or image-quality improvement beyond demonstrated contour evolution is claimed for Package5. Reproduction and failure criteria: [FogMS_EdgeFlow_Verification.md](FogMS_EdgeFlow_Verification.md).
