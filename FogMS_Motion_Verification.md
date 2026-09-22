# FogMS motion: verification and field protocol

21 September 2026. Package4 is READY_FOR_FIELD: build, CPU contracts, native controls and the bounded field run pass. Quantitative final A/B remains inconclusive; see [FogMS_Motion_Report.md](FogMS_Motion_Report.md). This is a project checkpoint, not final task completion.

Subsequent Package5 detail-flow checks have their own [FogMS_EdgeFlow_Verification.md](FogMS_EdgeFlow_Verification.md); they do not replace this reconstruction protocol.

## Preserve the existing scene

Use the current MimirHead project, `/Game/FogMS_Test/FogMS_Box` and `FogMS - Live Box`. Do not create another level, replace Perlin or save temporary test settings into the map.

Before mutation capture the current actor transforms/hidden flags, Box controls/reference and arrow, native height-fog settings, relevant CVars, lights, exposure, viewport camera/FOV/Game View, editor preferences and map hash. Fresh user state is the restoration authority. The comparison baseline uses B3 / High96 / 8 iterations, existing Perlin G, world size 10000 cm, density .2, white albedo, detail .1 / scale 8 / second octave .5.

The user may change Box bounds between tests and confirmed editing parameters during near04/05. For a controlled baseline repeat, first retain the latest bounds, temporarily apply baseline scale `(5.22643157,5.231491,2.96504416)`, verify it after update, and guard actual scale/location/rotation through profiling and capture. Restore the latest user bounds afterward. An initial setter assertion alone is insufficient: near05's later snapshot again showed enlarged bounds. Coordinate exclusive viewport control for future quantitative captures.

Keep native cloud visibility, Sky/SkyAtmosphere, exposure, lights and Game View identical. Restart can change transient hidden/show state; inspect both images and metadata. Verify that `FogMS - Fixed Exposure` is demonstrably fixed. Do not interpret a nearly uniform reference or reduced contrast as improved settling.

## Controlled native camera test

Use `Tools/FogMSEnergyValidation/MotionProbe` with a fresh immutable run ID and evidence root `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921`. Set `reference_camera_file` to its `near-camera.json`.

| Parameter | Value |
|---|---|
| Destination position | `(-8000,2400,2500)` cm |
| Destination rotation | pitch -5, yaw 4, roll 0 degrees |
| Destination FOV | 55 degrees |
| Cases | yaw, dolly, FOV |
| Warm-up / motion | 32 / 60 engine frames |
| Yaw excursion | 60 degrees |
| Dolly excursion | 10000 cm along the destination view direction |
| FOV excursion | 100 to 55 degrees |
| Independent capture delays | 1, 2, 4, 8, 32 engine frames |
| Warmed reference | 80 engine frames after motion |
| Deadline | 300 seconds |
| Game View | `force_game_view=true`; record, guard and restore exactly |
| Density | Frozen at the same phase; no lighting/exposure/quality changes |

Each delay repeats the movement independently. Start the hidden capture worker and invoke `motion_probe.py` through the existing bridge as described in [MotionProbe/README.md](Tools/FogMSEnergyValidation/MotionProbe/README.md). Native regular editor screenshots are used, not HighResShot or SceneCapture. Camera/Game View deviation aborts with an explicit error; it does not by itself identify whether the editor or a user caused the change.

Completion requires `status=COMPLETED`, all 18 PNGs with matching hashes and `restoration.ok=true`. Bridge dispatch success alone is insufficient. Then run:

```powershell
python -P Tools/FogMSEnergyValidation/MotionProbe/analyze_motion.py <run>/receipt.json
python -P Tools/FogMSEnergyValidation/MotionProbe/compare_motion.py <before>/receipt.json <after>/receipt.json --output <comparison.json>
```

Preserve both fixed ROIs: main `[.15,.15,.85,.90]` and boundary `[.05,.22,.40,.90]`. Compare early, delay32 and warmed PNGs at the same scale. Inspect contrast, structure, trails, bands and brightening after stopping. Also compare warmed images directly across versions: low error against each version's own reference can conceal a wrong steady image.

Reject mismatched camera, resolution, frozen phase, scene, exposure, known Game View or bounds. Record warmed-reference drift; differences near that floor do not prove improvement. Capture intervals are CPU request-to-file-observation engine frames, not exact GPU-frame numbers. LDR metrics are not an energy test. Without declared budgets, `MEASURED` is not visual PASS.

Evidence so far: interim eight-point near03 supports limited settling improvement; final four-point near04/05 are invalid A/B because Box scale differs. Retain those receipts and images. The original baseline lacks Game View metadata, so a later otherwise-matching comparison retains that explicit limitation. A successful final run must not inherit the interim eight-point result.

## Artist controls and native continuity

Package4 `directional-225339-607885.json` passes 13 native checks and exact restoration; CPU lifecycle/reference contracts pass 20/20. These control checks do not replace rendered motion acceptance.

1. Existing saved actors can remain in Legacy Velocity Vectors. Invoke `Use Directional Motion` explicitly. Conversion must preserve phase without enabling animation or altering texture, threshold or bounds.
2. Rotate `WindDirectionComponent`, set `Wind Speed` in cm/s and enable `Animate Density`. With both relative detail velocities zero, the three octaves share one wind. The arrow direction is independent of Box rotation/scale.
3. Change speed/direction after a long elapsed time: phase stays continuous at the edit; only subsequent velocity changes. Test native actor reconstruction as well as direct API assignment.
4. `Texture Offset (World)` explicitly translates the pattern in cm. Positive X moves the pattern toward world +X. A phase change/history reset here is intentional.
5. Freeze must hold phase; Resume must continue without a jump. Manual time and Time Offset are seeks. Changing Box size must not stretch world-aligned noise; `World Texture Size` deliberately changes pattern scale.
6. Check density, self-shadow, surface shadow and transport move coherently. Test relative detail separately; it may alter interior density and does not promise edge-only erosion or mass conservation.

Native entry: `Tools/FogMSEnergyValidation/directional_motion_probe.py`. Snapshot plain JSON values, not retained live FVector/struct wrappers. Restore controls first, then the validated `restore_density_motion_reference` API; do not write the hidden property directly. Read every completed check and restoration difference. Prior failed receipts and exact recovery evidence remain preserved.

Legacy animation evidence is separate: `animation/motion-animation-001` passes 11 cases / 57 checks with restoration. It covers frozen MID/producer parity, world-lock, relative detail, live/freeze/resume and history revision. It does not prove exact live snapshot alignment, rendered trails or performance; readbacks stall rendering.

## Checkpoint field state and remaining checks

`field-ready.json` records 1200 frames over 43.219 seconds, no probe error and sampled wrapped phase error at most 3.45e-7. Field state is B3 / High96 / 8 iterations, Directional motion, animation enabled, speed 200 cm/s and both relative detail velocities zero. Initial phase was preserved, camera unchanged and preferences restored. `controlled-bounds-restored.json` verifies restoration of the latest user bounds. Field preparation deliberately saved the existing map in that usable state; ordinary comparison probes must continue to avoid saving temporary states. Six monitored native shader hashes are unchanged in `engine-after.json`.

The owner reports visual improvement. The 43-second run does not establish long-duration stability, and authored-state changes invalidate near04/05 A/B. Moving edge detail is the next requested investigation, not an already delivered Package4 feature.

- Four-point CPU reconstruction contracts pass 9/9 in `reconstruction4-contract.json`. Keep shared density/source/previous-phase positions, exposure/fallback/native-medium invariants and explicit non-exact mixed `xyz` coverage. CPU integration checks are not GPU visual acceptance.
- The nine-case Package2 GPU numerical suite passes. `regression-source-parity.json` verifies seven unchanged producer/operator/runtime files in Package4, so this evidence is reused without a redundant rerun. Changed reconstruction HLSL still needs its own image tests. Re-run numerical fixtures if those producer/operator sources change.
- Check disabled/zero-density Box with nonzero native fog, camera motion and exposure changes for native parity. Inspect a dense animated region vacating cells for trails. The prior-phase support is not exact previous-camera/wind reprojection.
- Profile with no screenshots/readbacks in flight and guarded matching bounds/scene. Report frame, own solver passes and native LightScattering separately, with all samples/ranges. Use at least the established three samples per condition; do not turn a missing MaterialSetup event or rounded-zero timing into a cost claim. Latest unguarded Package4 timings are run-local evidence, not final matched acceptance.
- Extend field testing to B2/B3 switching, animation/freeze, visibility and fallback. Inspect shader, Python, RHI and crash logs. The completed 1200-frame directional run is a bounded smoke/phase check, not exhaustive mode coverage or long-duration stability.
- Compare installed files to the immutable final package; verify Engine/map hashes and the latest user controls, bounds, camera, visibility and preferences after restoration. Record final open scene and quality/motion state in a delivery receipt. Never save probe settings into the map.

Further owner field feedback remains separate from automated contracts and measurements.
