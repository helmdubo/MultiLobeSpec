# FogMS — W/S reconstruction, screen scattering and solar disk

**FIELD REJECTED, 22 September 2026.** Package1 below made continuous W/S shimmer worse according to the owner, introduced dark silhouette outlines, and occulted the solar disk abruptly. The post-stop measurements below are retained as historical evidence, not acceptance. Native FSSS was disabled live (`r.Fog.ScreenSpaceScattering=0`, `r.Fog.SeparateComposition=0`). It is not the requested Karpukhin-style SSFS. A coherent per-ray L/T integration and a separate opt-in screen-space pass are under development; neither is accepted yet.

22 September 2026. Plugin-only UE 5.8.2 CL56702186. Delivered Package1 from
`E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_ViewIntegration_20260922`.
Engine remains read-only; the existing `/Game/FogMS_Test/FogMS_Box` is the only map.

## Current-frame integration

The valid B2/B3 medium no longer enters native `LightScatteringHistory`. Native
height fog, other media and their stochastic lighting retain their normal history.
The final ray integral adds current Box extinction and source **before** the common
analytic segment integral. A newly empty Box sample therefore cannot retain its old
density merely because the camera has moved through the logarithmic froxel grid.

Both quantities use the same eight fixed positive Gauss points (two mirrored
tetrahedra): `average(sigma_s * J)` and `average(sigma_t)`. This does not animate a
dither pattern or multiply independently averaged light and density. World transport,
its geometry connectivity, density animation and energy operator remain unchanged.

One shared validity predicate chooses ownership in MaterialSetup, LightScattering
and FinalIntegration. Globally unavailable transport uses the previous density/native
lighting fallback and the existing transition history reset. Debug modes keep their
previous diagnostic path. A non-finite/negative incident sample fails closed to zero
source with extinction retained; it is a numerical defect, not the representation of
a wall.

Native FinalIntegration has no SceneTextures binding. Its quadrature is geometric,
without the previous depth-buffer displacement. This removes an implicit screen-depth
dependency, but close opaque-surface interpolation remains an acceptance boundary.
Eight spatial samples and downstream TSR/TAA can still alias; this is not a claim of
zero shimmer at every distance. The native small-extinction denominator floor and
near fade also remain, so CPU transport guarantees do not prove continuum accuracy
of the complete camera image.

## Native screen scattering and the sun

The existing HeightFog now enables UE's **Fog Screen Space Scattering**. Native
controls: Scale1 / Power1 / Spread0.1 / Blur0.5. CVars: ScreenSpaceScattering1,
SeparateComposition-1, ScreenSpaceScattering.TAA0. The separate FSSS history uses no
velocity in this UE version; leaving it off avoids introducing a second temporal
trail. The ordinary fog history stays at 0.9. The project startup preset preserves
these settings; the original preset is backed up as `enable_box-before.py`.

This is the native UE approximation, not the proprietary SSFS plugin and not a new
anisotropic B3 solver. It filters the combined screen-space fog/background; it is
not Box-only, can soften geometry edges, and has native HDR clipping and inferred
optical-path limitations. See `FogMS_ScreenScattering_Research.md`.

For procedural SkyAtmosphere without a sky material, UE prepares the FSSS source
before drawing the solar disk. The plugin overlay supplies that missing disk to
the **source branch only**, using native View directions, disk luminance, angular
edge, transmittance LUT, planet occlusion and per-light exposure/clamp. Two atmosphere
lights are supported. The later sharp background branch remains untouched:

`T * a * Disk + T * (1-a) * Disk = T * Disk` before the native blur.

There is no second fog-transmittance multiplier, new bloom or forced zero-transmission
cutoff. Native cloud composition still attenuates both branches. Per-view resident
metadata gates the helper by Atmosphere/DeferredAtmospherePass and excludes captures,
orthographic views and multiple views. The existing allocation remains 24x2 texels;
the previously unused `(0,1).w` stores the flag. No hidden UAV/new descriptor lifetime
is introduced. `r.FogMS.ScreenScatteringSun 0/1` exposes this source correction for A/B.

The broader procedural sky color is still subject to the native FSSS ordering; this
patch supplies the solar disks, not a second full SkyAtmosphere evaluation. Visible
halo radius need not increase monotonically with density: at sufficiently high
optical depth the surviving background, including its halo, disappears. This is
an image-space approximation, not measured physical angular multiple scattering.

## Evidence

All files below are under the delivery evidence directory named above.

- `Build1.receipt.json`: StrictIncludes, NoPCH/NoSharedPCH/DisableUnity build passed.
  `install1-receipt.json` / `installed-parity.json`: 86 installed files hash-verified,
  plugin/map backup retained. `MainGPU1.log`: native D3D12/SM6 shader compilation and
  startup passed, including FinalIntegration and composition permutations.
- `view-integration-cpu-contract.json`: 12 independent mathematical CPU contracts.
  Solar split/kernel tests: 11; control transaction tests: 6. These are not GPU
  radiometric or visual proofs. Independent source review found no critical issue.
- `dolly-before`, `dolly-after`, `dolly-ssfs`: same frozen phase, light, bounds,
  exposure, endpoint/FOV55; 200m dolly and 200m pan over 90 frames. All completed
  with camera/authoring/preferences restoration. Earlier historical tests used
  unequal distances; these new runs use equal distances, not equal screen flow.
- `dolly-comparison.json`: in the stated sky/cloud ROI, approximate linearized-LDR
  RMS against each run's warmed reference fell from **1.912% to 0.984%** immediately
  after dolly, and **1.446% to 0.989%** after eight frames. Pan was **0.940% to 0.932%**.
  These measure post-stop settling, not continuous edge motion; captures are bracketed
  by request/file observation, not exact GPU-frame timestamps. Approximately 1%
  residual must not be presented as eliminated.
- `ssfs-motion-comparison.json`: SSFS-on dolly **1.026% / 0.978%** at the same two
  delays. No return to the old accumulation was observed in this test.
- `solar-01/receipt.json`: seven native viewport captures, fixed camera/phase,
  density restored exactly. Disk source on creates a soft halo; at 3x authored
  density the disk is hidden in this view. Source-off isolates the missing-source
  behavior. MaxExposedLuminance10/64000 were compared and restored to native10.
  These are LDR visual checks; no HDR integrated-energy claim is made.
- `profiles.json` / `profile-summary.json`: three stationary GPU profiles per state,
  held for 40 frames after each request. At 1061x813, native fog grid266x204x208,
  current package graphics-frame medians were **36.064ms off / 31.894ms SSFS on**;
  FinalIntegration **5.413 / 5.312ms**. Frame variance exceeds the SSFS cost: this is
  not evidence that SSFS accelerates rendering or a matched old/new package cost.
  Two earlier solar fixture profiles are excluded because state advanced too soon.
- `engine-anchors.json`: six previously recorded Engine shader hashes unchanged.
  The crash compatibility guard is unchanged. Final live observation is recorded
  separately in `field-live.json`; do not infer a long crash soak or Shipping test.
  The first fixed-camera observation, `field-ready.json`, stopped after 52 frames
  when the camera changed; its FAIL receipt is retained. It restored preferences,
  did not restore the camera and is not a crash. The following read-only observation
  permits camera/authoring changes and never writes them back.

User-authored B3 High96/8, Density0.2, texture size10000cm, enlarged bounds and lights
are preserved. The latest user Edge Flow Speed is **150cm/s**, not the historical20.
Wind Speed remains200. Test density multipliers were reverted. Animation resumes
from its frozen phase; the same map is saved. New publication/merge is not claimed.

## Still separate work

Anisotropic transport/physical angular broadening, longer temporal/close-wall tests,
full sky-color FSSS source ordering, and Shipping/cook validation remain separate.
This delivery addresses Box history ownership and reuses native screen scattering;
it does not turn the complete final image into a proven energy-conserving renderer.
