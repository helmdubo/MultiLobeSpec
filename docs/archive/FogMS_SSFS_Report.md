# FogMS custom screen scattering and Box-anchored view integration

**Superseded SSFS algorithm:** the owner correctly reported that this version mainly blurred the sun because it intentionally excluded the fog's own light. The current full-HDR correction and its validation are recorded in `FogMS_SSFS_FogLight_Fix.md`. The project preset now uses ViewIntegration0; expensive3 remains experimental. The previous algorithm/results below are retained as history, not the current specification.

22 September 2026. Package6 is installed; candidate for owner field comparison, not final W/S or performance acceptance. Evidence: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_ViewIntegration_20260922`. Engine source remains read-only; only the existing FogMS_Box map is used. Controls and comparison procedure: `FogMS_SSFS_Verification.md`.

## Requested effect and physical scope

The owner requested SSFS similar to [Dmitry Karpukhin's screen-space fog scattering](https://dmkarpukhin.com/docs/ssfs/), not UE's native FSSS. The native FSSS experiment was rejected and disabled. This independent plugin pass runs before post processing/DOF, with a positive HDR pyramid and no temporal history. It preserves current volumetric lighting and redistributes the transmitted scene remainder:

`Q = max(SceneColor - IntegratedFog.rgb, 0)`; `target = T * filtered(Q) / filtered(T)`; `output = SceneColor + Amount * (1-T) * (target-Q)`.

Numerator and transmission use identical weights. A constant underlying background therefore does not acquire a dark coverage outline. Depth-range rejection limits transfer across opaque silhouettes. Package5 reconstructs coarse levels with positive cubic B-spline taps; first-level Karis weighting suppresses isolated HDR outliers in scene-linear units. These are independent implementation choices, not a claim to reproduce proprietary SSFS code.

This is an image-space approximation. It does not change B3's isotropic `g=0` transport, add a solved scattering order, or establish global energy conservation. Its mask is the shared integrated native+FogMS fog; it is not isolated Box-only coverage. Consequently rays through native fog can also change while an eligible transport Box enables the optional pass. Analytic fog, atmosphere, native clouds and translucency complicate the `SceneColor-L` separation. FP16 range and depth rejection remain limitations. Disable SSFS for transport radiometry and native-parity checks.

## Controls

- `r.FogMS.SSFS 0/1` (default 0); `r.FogMS.SSFS.Amount` (default .5, range 0..1); `r.FogMS.SSFS.Radius` (default24 render pixels).
- Native `r.Fog.ScreenSpaceScattering=0` and `r.Fog.SeparateComposition=0` in the project startup preset. Own SSFS bypasses simultaneous native FSSS, FogMS debug101..104, captures, multiview, invalid transport publication and missing native volumetric resources.
- `r.FogMS.ViewIntegration 0`: previous temporal coefficient path, default. `1`: rejected mean-coefficient experiment. `2`: coherent four rays but sampling follows camera Z layers. `3`: four coherent rays, 256 fixed intervals over the complete Box chord; negative entry is retained for cameras inside. Froxel boundaries clip those intervals without moving their density/J midpoint. Native and Box coefficients are integrated together, native Fade once per complete layer. Modes2/3 are diagnostics until accepted.

## Evidence and failures retained

Package1 field rejection: worse W/S, dark rim, abrupt solar occlusion. Post-stop RMS improvement was insufficient. Package2 hit UE root-parameter relocation on grouped global HLSL declarations; Package3 hit incorrect float3-array initialization. Both corrected; Package4 D3D12 shaders/run succeeded. No engine files were edited.

Package4 `own-ssfs-01` and `own-ssfs-sun-04`: protocol/restoration PASS, not visual acceptance. Sun04 exposed a rectangular bright footprint from coarse reconstruction; Package5 addresses it. First SSFS GPU sample was .154ms at1061x813; this is one sample of the old filter, not Package5 performance. `own-ssfs-sun-02` aborted on camera movement. Sun03 suffered partial JSON request publication; after changing the test camera it aborted/restored settings. Future requests use atomic publication. Failed records remain.

Package4 continuous01:64 images, matched0/2 trajectories, but pitched backtracking went below the ground; unsuitable as primary field acceptance. Ground02 stayed above ground and restored authoring: viewed0/2 edges were similar, without gross dark rim, but no W/S improvement was established. Pair29/61 has unequal capture intervals and must be excluded from strict pose comparison. Sparse asynchronous screenshots do not prove frame-by-frame absence of shimmer.

CPU: coherent rays11/11, Box-anchored integration11/11, smooth SSFS8/8 with10000 randomized cases, continuous harness11/11. Anchored CPU invariance under shifted/logarithmic layer partitions is an equation check, not GPU fp32/visual proof. Package5 StrictIncludes build passed and89 installed files were hash verified. Runtime findings are to be appended after the pending checks.

### Package5 measured results

`MainGPU5.log` completed D3D12/SM6 startup and both GPU probes without shader/Python/fatal markers. `continuous-anchored5-03` completed four180-frame legs over200m (64 captures,32 paired poses, all request/observation intervals one engine frame). The endpoint was moved120m into the Box from the owner's current camera. Authoring, exposure, map file and owned controls were restored. Reviewed far/porous/inside pairs did not show a new dark outline or gross color change; LDR ROI mean deltas3-minus0 were -.157%, -.053%, -.022%. This is neither a continuous-video flicker verdict nor an energy-conservation measurement.

`own-ssfs-sun-05` completed all seven settings and restored its controls. Full Amount1/Radius24 produces a smooth local solar patch; the Package4 rectangular hotspot is absent in reviewed images. Radius48 nearly dissolves the disk, while Amount.5 retains a saturated core from the unfiltered HDR fraction. These sky-heavy captures do not validate all opaque silhouettes. Custom SSFS measured .183ms at1061x813 in one ProfileGPU sample. Mode3 FinalIntegration measured16.314/22.865ms versus mode0 .500ms; these sparse samples are not a stable benchmark and the integration cost is substantial. Package6 will test caching repeated anchored-cell coefficients without changing sample positions or the transport equations. Package5 is retained as the uncached comparison.

Package6 changes only the last-cell cache for each of the four view rays: anchored index and pre-exposed, unscaled Box source/extinction. Native source, path-length ratio, segment transfer and fade remain layer-dependent. Its CPU reference passed16/16 checks, including cached/uncached equality, empty/partial cells, camera inside, four independent rays and fresh dispatch data. The127-layer example reduces382 to256 density evaluations per ray. This does not predict GPU speedup, because the persistent cache consumes registers. Receipt: `.codex-build/FogMS_Coherent_20260922/anchored-cache-cpu-agent-01.json` outside the plugin repository.

### Package6 installed verification

StrictIncludes/NoPCH/NoSharedPCH/DisableUnity build passed; all89 installed package files were rechecked by SHA-256. `MainGPU6.log` reached D3D12/SM6 startup. Animation phase at restart matched exactly before live animation resumed. `continuous-cached6-04` completed64 images/four180-frame legs/200m with all32 request+observed pose pairs matching, no intervention, and authoring/exposure/map-file/owned-state checks passing. Reviewed pairs01/33,09/41,17/49,25/57 did not show a new dark halo or gross color/coverage regression. This remains sparse visual evidence, not proof that continuous W/S shimmer is eliminated.

`own-ssfs-sun-06` passed seven settings with restoration. The smooth sun redistribution remains free of the Package4 rectangular hotspot in the reviewed full-strength image. ProfileGPU samples at1061x813: native temporal FinalIntegration .779ms; anchored FinalIntegration18.467/17.925ms; SSFS .184ms. Medium phase differed between Package5 and6: no controlled GPU-speedup percentage is claimed. Anchored integration is still substantially more expensive and needs further optimization; the cache's CPU sample-count reduction must not be presented as a measured equivalent frame-time gain.

`own-ssfs-objects-06` passed the off/full comparison from the restored owner camera. The reviewed image pair shows no new dark halo; most pillars are heavily obscured by the current density, so this is limited silhouette coverage. `field6-ready.json` records360 observed engine frames over34.687s with live animation, no camera changes and no runtime error markers. This observation is not a GPU-only benchmark or performance acceptance. Background throttling was restored, autosave remains enabled, the same map was saved and the final field settings are integration3/SSFS1/Amount1/Radius24, native FSSS0/SeparateComposition0. Density.5, wind100, edge150, authored scale and owner camera remain intact. RHI crash guard remains0; six engine anchor hashes match the initial receipt. Package6 backup and89-file installed parity are recorded in `install6-receipt.json`.

The project startup preset selects the same field settings; its prior version is retained as `enable_box-before_field6.py`. Plugin CVar defaults remain0. Owner W/S quality and acceptable runtime cost remain open. No new commit, push or merge is claimed by this slice.

**Performance correction after reviewing the full log:** the18ms figure above covers only the solar near-Box view. The later restored owner-camera `own-ssfs-objects-06` profiles in the same `MainGPU6.log` measured FinalIntegration77.298ms (SSFS off) and76.851ms (SSFS on); SSFS itself .211ms. These figures were omitted from the initial delivery summary. Mode3 has a severe, view-dependent performance regression and must not be described as costing only18ms or as an optimized production path. `r.FogMS.ViewIntegration 0` restores the previous integration while leaving SSFS independently available. No automatic runtime setting was changed during this read-only diagnosis.

## Current authored scene

Owner snapshot `scene-inspect-20260921T212216_072687Z-c952cf64.json`: Density=.5, WindSpeed100, EdgeFlow150, world texture size10000cm, extents11110.2298/4888.2433/5029.0851cm; B3High96/8. Do not restore older .2/200 or larger Box snapshots. Preserve the latest owner camera; the restart5 snapshot records the current pose and phase.
