# FogMS B3 GPU numerical probe

Explicit, bounded acceptance harness for the existing
`/Game/FogMS_Test/FogMS_Box` editor world. It neither starts UE nor creates,
deletes or saves actors, levels or assets. Run it only after installing B3.

## Required runtime contract

- `unreal.FogMSScatteringMode.ANGULAR_TRANSPORT` (C++ value 5).
- Actor property `angular_quality`, with `unreal.FogMSAngularQuality.BALANCED48`
  and `HIGH96` (C++ values 0/1); both enum attributes are resolved before mutation.
- `r.FogMS.Transport.TestAlbedo -1` retains `BoxRows[23].rgb` under synthetic
  Test1/Test2 density; nonnegative values retain scalar diagnostic albedo.
- Existing Test/Tau/Albedo/Geometry/Boundary/Reconstruction CVars.
- Normal four-slab `FogMS.DumpSpatial`, `domain=transport`, directions 48 or 96,
  and **`transport_scheme=upwind_half_gauss`**. Metadata must describe the actual
  iteration count and test controls; old B2 output is rejected.
- Atlas alpha in the primary slab: bits 0..5 reciprocal topology, bits 6..11
  missing diffuse face samples, bits 12+ missing directional exterior samples.

The capture forces `TestReconstruction=0` and restores its previous value. This
suite does not validate the native trilinear receiver diagnostic.

## Cases

The default nine cases use 24 GPU iterations:

1. GL48 white furnace, tau 4.
2. GL96 white furnace, tau 16.
3. GL48 homogeneous finite box, tau 4, albedo 0.9.
4. GL48 vacuum.
5. GL48 two slabs separated by a vacuum gap, tau parameter 4, albedo 0.9.
6. GL48 RGB albedo `(0.9, 0.65, 0.25)`, tau 4.
7. GL96 pure absorption, tau 4.
8. GL48 one-sided illumination, no wall.
9. Matching GL48 case with one 5 cm full-domain black wall.

The first seven cases do not move or hide geometry. Only the final pair
temporarily repurposes the **existing** `FogMS - Pillar 1` native cube, hides
other StaticMeshActors and positions the wall through the Box center. This is
the established B2 fixture with a known independent topology and a lit control.
The full wall spans the Box's Y/Z domain; the ordinary scene's isolated pillars
cannot establish an exact zero-leakage control.

The capture preserves fresh mode, quality, iteration count, albedo, all changed
CVars, throttle/autosave preferences, StaticMeshActor transforms and temporary
visibility. It writes its recovery snapshot **before the first mutation**.
Completion, stop and callback errors share the same restoration path. The map
file hash and ten restoration checks must pass. It never saves the map.

## Run

Review `gpu_config.json` and choose a new `id` for every run. Default output is
`E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_B3_20260921`. Existing receipts/dump
prefixes are never overwritten. With the existing map open outside PIE:

```text
py "E:/GITHUB/MultiLobeSpec/MultiLobeSpec/Tools/FogMSEnergyValidation/B3Probe/gpu_probe.py"
```

This only registers a callback. Wait for
`FOGMS_B3_GPU_FINISHED COMPLETED` and inspect
`gpu_b3-<id>.json`: it must say `restoration_ok=true`. Each case has a barrier
dump followed by a measurement at least two fresh producer/render frames later.
Keep the viewport unchanged during capture; movement/property changes detected
by its guard abort the run and restore the temporary changes.

Offline analysis with a Python installation containing NumPy:

```powershell
python -P analyze_gpu.py "E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_B3_20260921/gpu_b3-b3-numeric-001.json"
```

The analyzer compares the full GPU radiance, primary field, coefficients,
reciprocal geometry, equation residual and flux against
`../angular_transport_reference.py`. Default acceptance thresholds are 0.2%
for radiance/flux/residual and relative `1e-6` for wall leakage. These thresholds
are implementation acceptance tolerances, not a claim of 0.2% continuous
physical accuracy.

White furnaces use analytic `J=1`, verified through the CPU formal operator,
to avoid expensive redundant CPU PCG solves. Other cases use converged CPU
PCG with a 64-iteration budget and `1e-8` relative tolerance; a nonconverged CPU
reference fails instead of silently serving as ground truth. Measured CPU cost
on this host for a 32^3 finite box: GL48/albedo 0.9 converged in 13 iterations
and 10.8 seconds; GL96/pure absorption required no PCG iterations and 4.5
seconds. These are CPU reference timings, not GPU feature cost.

Scalar RGB flux compares using UE's default sRGB luminance factors
`(0.2126390059, 0.7151686788, 0.0721923154)` from `Common.ush`. If shader
compilation uses legacy luminance or a different working color space, supply
the **verified** factors using `--luminance-factors R G B`. The report explicitly
records this assumption. RGB radiance and density checks do not depend on it.

Optional `case_names` in the capture config selects a subset of the fixed case
names for diagnosis. Such a capture is marked incomplete; the analyzer requires
`--allow-partial` and does not present it as full-suite acceptance.

## Stop and recovery

For an active callback, set config `action` to `stop` and invoke the same entry.
After an editor process restart, set `action` to `restore` and
`restore_receipt` to the filename of **that run's** `gpu_b3-*.json` in the same
output root, then invoke the entry on the original map. Recovery reuses the
durable original snapshot. Restore the config to `action=run` and use a fresh id
before a new capture. If restoration reports incomplete, retain the receipt
and investigate its named checks rather than saving the temporary fixture.

## Offline checks already available

```powershell
python -P verify_offline.py
python -P analyze_gpu.py --help
```

`verify_offline.py` does not import or operate UE. It compiles all Python,
checks import safety, generates small **synthetic CPU** payloads for the nine
cases, runs the analyzer, and confirms rejection of wrong direction counts,
wrong transport scheme, stale sources and wrong albedo. It statically checks
the restoration of added controls and absence of scene-save/spawn/delete calls.
It does **not** mock a UE callback or validate UE's property export/runtime.
Generated artifacts are under ignored `_verification/`.

Remaining acceptance requires the real D3D12/SM6 capture and visible field
inspection. Camera stability, performance, animation, anisotropy and continuous
angular accuracy are outside this numerical suite.
