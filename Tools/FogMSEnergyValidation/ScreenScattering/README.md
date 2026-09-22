# Native UE 5.8 FSSS control probe

`native_fsss_control.py` imports without executing. Call it explicitly through the existing editor bridge; it performs synchronous control changes/readback only. It does not capture images, run UE, change B3, create actors, touch camera/density, save maps, or claim rendered acceptance.

```python
import runpy
control = runpy.run_path('E:/GITHUB/MultiLobeSpec/MultiLobeSpec/Tools/FogMSEnergyValidation/ScreenScattering/native_fsss_control.py')['run']
session = 'E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_ScreenScattering_20260922/native-01'
control('inspect', session)
control('enable', session, mode='separate_only')
control('enable', session, mode='fog_only')
# Separate invocation after a controlled visual comparison:
control('enable', session, mode='fog_and_scene', settings={'scale': 1., 'power': 1., 'spread': .1, 'blur': .5})
control('disable', session)
control('restore', session)
```

Do not execute the entire example as a visual test: each call changes controls immediately; image warm-up/capture and GPU profiling belong to the caller. `probe` aliases `inspect` and has no mutations. Inspection returns `OBSERVED`; a mutation `PASS` means scalar readback and same-call scene/map guards passed, not that a render-thread frame or SSFS pass ran.

Default map is `/Game/FogMS_Test/FogMS_Box.FogMS_Box`; a different saved `/Game/` map requires explicit `expected_world`. PIE must be stopped. The harness requires exactly one ExponentialHeightFog component: native FSSS uses the first renderer fog, whose ordering is not exposed by Python actor enumeration. Multiple candidates fail before mutation. Native property aliases resolve from verified C++ names and Python snake-case; the receipt records actual names.

Enable sets `bEnableFSSS=true`, `r.Fog.ScreenSpaceScattering=1`, `r.Fog.SeparateComposition=-1`, and `r.Fog.ScreenSpaceScattering.TAA=0`. This last setting avoids introducing a second motion history during current FogMS reconstruction diagnostics. Native scalar defaults are Scale1 / Power1 / Spread.1 / Blur.5; fog_only uses Scale0. The separate_only comparison instead disables the component flag and forces SeparateComposition1, isolating composition without scattering. `settings` accepts only these four controls. Power must be positive; Blur is 0–1. Disable only clears the component enable flag, retaining the session's other controls. Restore returns all five component fields and three CVar numeric values to the original snapshot.

The first mutation creates `original.json` with exclusive creation. It is never overwritten. Each action produces a new timestamped receipt; mutations also write a unique intent before changes. Use one session directory serially. The next mutation rejects control drift from the last successful receipt, preserving later manual changes. After inspecting a crash/rollback/manual-edit state, explicit `control('restore', session, force_restore=True)` permits recovery from the original snapshot. Never use this automatically to override an unexpected edit. Partial setter failures attempt rollback to the immediate pre-call controls and retain the failure.

Unreal's exposed console command API restores CVar **values**, not original `SetBy` priority flags. Receipts state this limitation. Restarting/reapplying the owning project or scalability configuration may be required to restore original priority behavior. No exact CVar-priority restoration is claimed. The script neither modifies project config files nor lowers unrelated scalability settings.

The map's on-disk hash, actor transforms/hidden flags, camera/FOV and Game View are checked across each call. These are not a full scene-asset audit; no continuous guard is installed between calls. The caller owns exclusive viewport control, later restoration and any intentional field-state save.

Native algorithm, source order and acceptance matrix: [FogMS_ScreenScattering_Research.md](../../../FogMS_ScreenScattering_Research.md).

Offline harness-safety checks: `python -P Tools/FogMSEnergyValidation/ScreenScattering/test_native_fsss_control.py`. These use a fake Unreal adapter to check immutable snapshots, partial-write rollback and drift refusal; they are not native or visual acceptance.
