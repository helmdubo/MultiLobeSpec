# FogMS detail flow: verification and field protocol

21 September 2026. Package5 is READY_FOR_FIELD: build/install, CPU contracts, native controls, the five-capture morphology protocol and the final 1200-frame live run pass. Distant dolly/zoom shimmer remains open. See [FogMS_EdgeFlow_Report.md](FogMS_EdgeFlow_Report.md). Package4 reconstruction acceptance remains recorded separately in [FogMS_Motion_Verification.md](FogMS_Motion_Verification.md).

## Preserve the scene

Use the current MimirHead project and `/Game/FogMS_Test/FogMS_Box`. Keep the user's current Box bounds, camera, Perlin, exposure, lights and quality. Do not create a level or save temporary comparison states. Before mutation serialize plain JSON values for motion controls/reference, arrow, bounds, camera/FOV/Game View, authored light state, preferences and the map hash. Retained live Unreal struct wrappers are not immutable restoration snapshots.

Run probes with exclusive viewport control. Guard camera, authored scene and Game View through capture; any unexpected change invalidates the run. Restore controls before restoring the serialized reference with the actor's validating API. Check every restoration field and retain failed receipts/images. A bridge response is not a completed test.

## Automated checks

Evidence root is `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921`.

1. `edge_flow_density_contract.py`: require five passing CPU tests, the formula-source match and explicit synthetic-data provenance. Keep the strict empty-band inequality and filled-band threshold; do not describe the invariant as geometric edge-only behavior.
2. `directional_motion_contract.py`: require the 20 existing CPU motion/lifecycle contracts. These alone do not prove native editor reconstruction or GPU behavior.
3. Invoke `Tools/FogMSEnergyValidation/edge_flow_probe.py` through the existing editor bridge. Require all 21 native checks plus `restored=true` and all 13 restoration checks. Current evidence is `edge-flow-20260921-232540-887887.json` (PASS). This checks actor velocities/reference and MID phase only.
4. Verify built/installed manifests and `edge-source-parity.json`. Unchanged render sources reuse the earlier numerical operator evidence; this does not substitute for testing the new animated input visually.

## Native morphology captures

Use `edge_flow_visual.py` with a fresh immutable ID in `edge-flow-visual-config.json` and the existing `MotionProbe/capture_worker.mjs`. The script holds the current camera, forces Game View, temporarily uses zero common wind and zero advanced relative velocities, then captures:

| Capture | Edge speed, cm/s | Time after common phase origin |
|---|---:|---:|
| `off-t0` | 0 | 0 s |
| `off-t10` | 0 | 10 s |
| `on-t0` | 20 | 0 s |
| `on-t5` | 20 | 5 s |
| `on-t10` | 20 | 10 s |

Each static seek settles for 90 engine frames before a regular native editor screenshot request. Record request and file-observation frames; capture is not an exact GPU-frame measurement. Do not use SceneCapture or HighResShot. The script restores controls/reference, camera-independent authored state, Game View and preferences, and verifies the on-disk map hash unchanged.

Require five PNGs and receipt PASS with all restoration checks. Inspect images at a common scale/ROI. `off-t0` versus `off-t10`, and `off-t0` versus `on-t0`, establish the renderer's residual noise floor for equal density. Later on-images should show coherent detail changes above that floor while large-scale base phase remains fixed. Distinguish moving texture detail from overall brightening, history trails or changed lighting. Shadows and interior light should follow the evolving density. LDR image differences are not energy measurements, and warmed static samples do not demonstrate continuous-motion stability.

`edge-flow-visual-01` completes this protocol with all nine restoration checks true. Recorded request/file observation intervals are one engine frame each, without an exact GPU-frame claim. Whole-image RGB RMS is .264% for the static repeat, .265% for the equal-phase switch and 1.746% after ten seconds of detail flow. The close view shows changing contours and a stable overall outline, but its largely opaque interior limits sensitivity to other defects.

## Live field check

After restoring temporary capture state, use Directional motion, Animate Density, common wind 200 cm/s, Edge Flow Speed 20 cm/s and zero advanced relative vectors. Keep existing density, texture size, detail strength/scale, bounds and B3 quality. Preserve current phase when setting speed/resuming. This setup was saved to the existing map after the successful final run.

Run at least 1200 ordinary editor frames, recording phase/reference samples and active B3 status. Use an isolated callback closure, the same float32 frequencies as the producer, and write actual/expected phases plus frequencies before evaluating each assertion. The check requires wrapped phase error below `3e-6`, flow remaining 20 and production test overrides disabled. Restore background-throttle/autosave preferences and verify the camera stayed unchanged. Saving the deliberately prepared field state to the existing map is separate from comparison probes; record it explicitly.

During live inspection, compare flow 0/20, freeze/resume, speed changes and arrow rotation. No texture jump should occur at speed/direction edits. With wind zero, the base should stay fixed while detail evolves. With flow zero and advanced vectors zero, all octaves should move together. Check Box resizing from outside the volume for world locking. Observe near and far contours and lighting during camera motion; extra shimmer, banding, trails or a brightening buildup remains a failure even if phase math passes.

A 1200-frame run is a bounded smoke/phase test, not a long-duration crash guarantee. Edge-only erosion, anisotropy, fluid mass conservation and ground-following density are outside this slice; no additional GPU pass or runtime-cost improvement is claimed.

`edge-field-ready-03.json` is READY_FOR_FIELD: 1200 frames / 41.797 seconds, 12 samples with maximum wrapped phase error `2.95e-8`, initial phase preserved, preferences restored and camera unchanged. The existing map was saved with flow 20, wind 200, detail .1 / scale 8, density .2, texture size 10000 cm and B3 / High96 / 8 iterations.

Keep both earlier failed receipts. Run01 used double frequencies in the oracle instead of actual float32 frequencies. Run02 lacks its raw failing sample and has an unexplained transient mismatch; shared global callback state was a harness risk, not a proven explanation for that event. The final isolated callback records each sample before asserting. These corrections do not justify calling either earlier receipt PASS or claiming a production bug was fixed.

The owner reports distant dolly/zoom contour shimmer. The passing phase soak does not close this visual limitation; a separate controlled reconstruction diagnostic remains in progress. Edge Flow provides contour evolution, not an antialiasing fix.
