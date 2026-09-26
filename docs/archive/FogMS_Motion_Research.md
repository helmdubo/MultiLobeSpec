# FogMS: coherent motion and reconstruction

2026-09-21. This slice addresses camera-motion blur and density animation after
B3. It does not add an anisotropic scattering operator or a fluid simulation.

The subsequent Package5 detail-flow control and its exact limits are documented in [FogMS_EdgeFlow_Report.md](FogMS_EdgeFlow_Report.md); the motion/reconstruction research below remains the Package4 checkpoint.

## Source findings

- [Rockstar, SIGGRAPH 2019](https://advances.realtimerendering.com/s2019/slides_public_release.pptx),
  slides 17–18, 45–48: coarse shape and erosion are distinct; coefficient history
  follows wind. Scattered lighting in that volumetric-fog path is not temporally
  blended, avoiding moving-light and camera trails. Slides 52–56 describe
  depth-aware history constraints and reconstruction. Their hybrid renderer is
  not the same as UE's froxel implementation.
- [Guerrilla, Horizon 2015](https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf),
  PDF pages 32–38 and 49: base shape, finer edge erosion, curl distortion and wind
  are separate parts of cloud authoring.
- [Guerrilla, Nubis Cubed](https://d3d3g8mu99pzk9.cloudfront.net/AndrewSchneider/Nubis%20Cubed.pdf),
  PDF pages 100, 125, 172–174, 198: wind offsets detail coordinates; jitter and
  filtering vary with distance. Voxel-cloud motion is described as pseudomotion;
  actual voxel evolution remains future work. This is not evidence of a cheap
  general fluid simulation that can simply replace FogMS.
- Installed UE 5.8.2, read only: `VolumetricCloudMaterialPixelCommon.ush:45–75`
  gives materials world-space coordinates, leaving density animation to them.
  `VolumetricCloud.usf:797–805` uses blue-noise ray-start jitter.
  `VolumetricRenderTarget.usf:285–301` reconstructs from cloud depth and camera
  motion; its velocity TODO does not establish compensation for animated
  material wind. Native cloud reconstruction cannot be copied as an isolated
  noise function into the FogMS grid.

The full RDR2 PPTX transfer timed out. All 155 slide and notes XML entries were
extracted from the downloaded prefix and verified against the ZIP central
directory CRC/length. No claim of a full presentation download is made. Raw
source receipts are under `.codex-build/FogMS_Motion_20260921/research` in the
parent workspace.

## Applied engineering decisions

1. Keep B3 illumination as the current-frame world-space solution. Its PCG state
   does not retain accumulated light between frames.
2. Replace the one jittered Box receiver with a fixed shared footprint in density,
   lighting and previous-density temporal support. Package2/3 tested eight
   tensor-product Gauss samples. Package4 instead uses four tetrahedral samples,
   each weighted `.25`, at `.5 + (1/sqrt(12)) * sign` with signs `+++`, `+--`,
   `-+-`, `--+`. This integrates normalized-froxel polynomial moments through
   degree two exactly, but not the mixed cubic `xyz` moment. It is a deliberate
   sample-cost/accuracy trade, not an equivalent rewrite of eight-point Gauss.
   Estimate `average(sigma_s * J)`, not the product of separate averages. Native
   medium and emissive remain separate; there is no eight-sample quality switch.
3. Shorten local native-image history according to camera displacement in froxel
   units and disagreement in both extinction and exposure-corrected RGB. The
   global native HistoryWeight remains unchanged. Previous density phases track
   cleared animated cells; empty Box support does not modify native history.
4. Use one coherent wind for the existing three octaves by default. Expose a
   rotatable independent arrow, speed and world-space texture offset. Preserve
   the current authored density function and texture. Slow independent detail
   motion remains an advanced option; it can still change interior density,
   because the current detail function is additive before the density threshold.
5. Preserve the current phase when editing wind. A saved displacement reference
   replaces the discontinuous `new_velocity * total_elapsed_time` mapping.
6. Hoist identical field-metadata reads outside the quadrature loop and include
   the native depth-offset allowance in the conservative local froxel bound.
   Package3 also tried retaining interpolation values/masks and same-anchor
   connectivity across samples. Although its CPU parity tests passed, its GPU
   test did not establish a consistent gain and moving LightScattering reached
   median 12.835 ms versus 7.856 ms for the prior uncached run. Other GPU scopes
   varied too, so this is not an isolated cache-penalty estimate. The cache is
   rejected and removed from the Package4 candidate.

The four-point choice halves the logical density/source sample count. For three
manually trilinear density octaves, one estimate requires 96 atlas texel requests
instead of 192 before compiler/cache effects. This excludes other passes and
previous-phase evaluation, and does not predict proportional frame-time savings.
The controlled near03 settling improvement belongs to the interim eight-point
Package2 implementation. Package4's nine CPU reconstruction contracts pass and
its native control probe repeats 13/13 with exact restoration. Near04/05 image
A/B is invalid because Box X/Z scale increased while the owner was editing
parameters, moving the near camera inside the bounds; adjacent GPU profiles lack
transform guards. Matched image/cost acceptance therefore remains inconclusive.
The field checkpoint completed 1200 frames in 43.219 seconds with sampled wrapped
phase error below 3.45e-7, and the owner reports visual improvement. These are
separate observations, not a replacement for controlled A/B. The unchanged seven-file
producer/operator/runtime source retains Package2 numerical evidence by hash
parity; that does not validate the changed reconstruction shader.

The native control test exposed a separate editor integration issue:
`ActorConstruction.cpp::ResetPropertiesForConstruction` resets non-editable,
writable Blueprint properties to their defaults. The saved motion reference
therefore uses `BlueprintReadOnly` and an explicit validated restore function.
The reference remains hidden from artist Details. Source-backed CPU lifecycle
checks pass 20/20, and the Package3 native probe passes 13/13 with exact
restoration. The retained C++ implementation does not establish acceptance of
the subsequently changed receiver shader.

## Limits and next separation

The four fixed samples estimate the footprint; they do not create detail below the native
froxel footprint or the 32-cubed lighting grid. History reactivity is an image
reconstruction heuristic, not additional physical energy transport. Native and
Box contributions share the final RGBA history where they overlap. Structural
authoring edits still use the existing whole-fog history reset.

The animated support test samples the preceding density phase at the current
quadrature positions, not a separate previous-camera Box history volume. It is
not exact motion-compensated reprojection of each density octave. True edge-only
erosion and domain warping should be added as a separate opt-in material model,
with identical density evaluation in the material, solver and shadow paths.
Moving edge detail resembling native clouds is the next requested research
slice. It is not implemented by the Package4 motion checkpoint.
