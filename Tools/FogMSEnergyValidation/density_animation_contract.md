# FogMS world-space density animation

Animation is opt-in (`Animate Density = false` by default). No authored asset, texture or default density changes. It requires an enabled, visible density source, world-aligned texture coordinates and World or Transport scattering. Unsupported scattering modes, local coordinates or invalid animation values retain the original static mapping and report the reason in `Density Animation Status`. Animation is not fluid simulation or a mass-conserving density evolution.

## Coordinates and time

All three octaves use world axes. For octave `k`, frequency `f_k` is the exact float already shared by the material and FogMS packet:

```
f0 = float(1 / WorldTextureSize)
f1 = float(f0 * DetailScale)
f2 = float(f1 * 2)
v0 = DensityWindVelocity
v1 = DensityWindVelocity + DensityDetailVelocity
v2 = DensityWindVelocity + DensityDetailVelocity + DensityEvolutionVelocity
t  = (UseManualAnimationTime ? ManualAnimationTime : SnapshotWorldTime) + AnimationTimeOffset
phase_k = frac(frac(center * f_k) - frac(v_k * t * f_k))
uvw_k(P) = frac((P - center) * f_k + phase_k)
```

Velocity units are world centimetres per second. Positive velocity moves a feature in the positive direction: `density(P + v*t, t) = density(P, 0)` for a single advected octave. Detail motion is relative, so different octave velocities evolve shape. Rotation, scaling and translation of the box change its clipping bounds without carrying or stretching the world-space pattern.

The game thread snapshots `UWorld::GetTimeSeconds()` once per `GFrameCounter` and world. The actor tick, construction/property updates and view-family packet construction use that snapshot. World time respects game pause and dilation; each PIE/editor world has its own snapshot. There is no GPU clock or per-view time. Manual time plus offset reproduces a frozen density phase regardless of editor run time.

`Freeze Density Animation` captures the current phase by setting manual time. `Resume Density Animation` compensates Time Offset so motion resumes from the same phase. Directly changing the Manual Time or Time Offset properties is a deliberate seek. World time and phase arithmetic use doubles; center and travel are reduced separately before the final bounded float phase conversion. This avoids subtracting a large travelled world position from the world center. As with any double arithmetic, astronomically large coordinates/times are not exact.

## Consumer and history contract

- `LastDensityState` refreshes after every successful update, even if only phase changed. MID parameters and packet rows 13–15 XYZ are populated from that same state.
- Packet row 5 W is `1` when validated animation is active, `0` otherwise. The existing 24-row ABI does not grow.
- Material equality includes phases. Authored/discontinuity equality excludes only ordinary animation phase progression and still covers velocities, mode, manual seeks, time offset, bounds and backwards world time. Inactive animation retains the original static equality.
- The global history comparison uses a copy of the packet with only animated phase XYZ values zeroed. Their W components retain the surface-shadow controls. The actual packet keeps current phases, so the sun-cache signature and current-frame lighting rebuild against the current density.
- Density texture atlas upload/revision remains driven by texture data/resource changes, not time.
- This patch supplies the row 5 W marker for the parent renderer's local density-reactive native history rejection. Animation must not be presented as accepted until that renderer change and its moving-density test pass. Camera reprojection alone can retain stale fog after density moves.

## Verification

Run the independent numerical contracts with the configured Python runtime:

```
python -P Tools/FogMSEnergyValidation/density_animation_contract.py --output density-animation-contract.json
```

Ten CPU contracts cover positive advection, relative octaves, center/scale invariance, negative time, large coordinates against a Decimal reference, modulo wrapping, static time-zero parity, history discontinuities, freeze/resume continuity and per-world frame time. They do not compile C++ or prove native runtime behavior.

Native acceptance still required, in the existing test map with all authored settings backed up/restored:

1. Build the plugin with StrictIncludes/no-unity/no-PCH and enable the existing supported World or Transport mode.
2. Keep Animate Density off: confirm the density material/packet match the previous static result.
3. Enable world alignment and animation, e.g. wind `(80,0,0)`, relative detail `(0,12,0)`, evolution `(0,0,5)` cm/s. Verify positive-X base motion, distinct detail evolution and matching fog/surface sun shadow motion.
4. Move/scale/rotate the box: the world pattern remains fixed apart from authored motion; only clipping bounds move.
5. Inspect live DensityRevision/reset metadata: normal time progression must not raise the global reset each frame. Change Time Offset once: global history must reject the discontinuity, then settle. The sun cache must continue following phases without a density texture re-upload every frame.
6. Freeze with the button; record Manual Time and Time Offset. Move the camera and confirm the frozen phase remains stable. Resume without a phase jump. Seek a negative manual time and reload that same value to reproduce it.
7. Toggle animation off, Box Enabled off, local coordinates, and Spatial mode: verify static mapping and accurate status, with no silent claim of Spatial animation support.
8. Check the final moving-density image for local history trails and pumping. Compare current density against historical extinction through the parent renderer's reactive rejection; do not use a global reset every frame to hide failures.

No Unreal process, build, installation or scene mutation is performed by the CPU contract script.
