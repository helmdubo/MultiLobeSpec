# FogMS: W/S contour instability, 21 September 2026

The owner's report concerns **camera translation along the view direction with fixed FOV**, not an optical zoom. Thin noise-hole contours fluctuate more during W/S than during A/D and less when flying close inside the volume. Edge Flow is a separate feature and does not resolve this report.

## Controlled checks

Used the same existing FogMS_Box level and Package5. Density was frozen once at time 2222.1315462067723; all runs used the same phases, authored bounds, lighting, exposure and target camera. B3 remained High96 / 8 iterations. FOV was 55 degrees. The dolly path was 200 m over 90 frames; the lateral path was 50 m over 90 frames. **These are not equal-speed or equal-screen-flow paths**, so their numerical difference is not a normalized ranking of movement types.

Each capture repeated its path, then sampled after 1, 8 or 80 settled frames. Native editor screenshots were used, not SceneCapture or HighResShot. The analyzed ROI `(0.08, 0.04, 0.60, 0.48)` contains cloud holes against the sky. The values below are display-referred RGB relative RMS against each run's own 80-frame reference; they measure post-stop settling, **not continuous-motion shimmer or physical energy error**. Request-to-observation intervals were [1,2] and [8,9] engine frames, not exact GPU timestamps.

| Diagnostic | Dolly, early | Dolly, after 8 frames | Pan, early |
|---|---:|---:|---:|
| Z208, history .9, miss samples 4 | 3.378% | 2.100% | 1.287% |
| Z416, history .9, miss samples 4 | 3.117% | 2.021% | 1.118% |
| Z208, history .9, miss samples 1 | 3.272% | 2.154% | 1.161% |
| Z208, history 0, miss samples 1 | 1.060% | .978% | 1.064% |

The matched history comparison holds miss samples at 1 in both cases; it is not the earlier confounded history/miss-count comparison. Doubling Z only slightly changed this settling measure. Removing fog history reduced the dolly residual substantially; stable-reference drift is already approximately .9%, so differences near that floor should not be overinterpreted. This supports a significant history/reconstruction contribution, but does not identify one faulty expression or rule out spatial undersampling.

All four measured runs completed with exact authored-state restoration; comparison status is MEASURED, not a new visual-quality PASS. An earlier attempt aborted before capture because its camera was still moving; its receipt is retained. No permanent quality-setting change was made. The final restore verified Z208, history .9, miss samples 4 and the original live animation controls.

## Source-backed follow-up

`FogMS_Indirect.ush` samples mip zero, combines detail, then applies a nonlinear density threshold. Four fixed quadrature points in `FogMS_Reconstruction.ush` cannot integrate every thin opening as a froxel's world footprint changes with distance. Native frame jitter does not jitter those four points.

There is also a concrete history-support limitation: when the current samples are empty, `FogMS_BoxTemporalSupport` tests the previous animation phase at the **current** sample positions. For frozen noise those positions remain empty, even if the reprojected previous camera footprint contained Box density. Native history can therefore retain a contribution while a hole opens. This is a source-supported candidate, not an isolated proven cause from the screenshots.

Prioritize explicit previous Box-density support for the reprojected footprint, with correct invalidation on grid/view/bounds changes. It must distinguish Box from native fog and preserve empty-medium parity. Applying history rejection throughout the whole bounding box or globally disabling history is not an acceptable permanent fix. Then evaluate footprint filtering of the **thresholded density** together with `sigma_s * J`; simply selecting a mip of raw noise before the threshold changes coverage and is not equivalent. Keep the world B3 operator independent of camera and preserve positive weights and obstacle barriers.

Implementation of that reconstruction change remains open. No jitter, solver normalization, shader or density equation was changed by this diagnostic.

## Evidence

Under `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Motion_20260921`:

- `motion-distance-z208-02`, `motion-distance-z416`, `motion-distance-historymatched`, `motion-distance-nohistory`: immutable native captures and receipts.
- `distance-z-comparison.json`, `distance-history-matched-comparison.json`: valid comparisons, no scene/phase mismatch.
- `distance-ab-original.json`, `distance-ab-start.json`, `distance-ab-restored-final.json`: original settings, frozen phase and final restoration.
- `scene-inspect-20260921T194816_169796Z-f372be6e.json`: final read-only runtime inspection after restoration.

For the delivered motion feature and controls, see [FogMS_EdgeFlow_Report.md](FogMS_EdgeFlow_Report.md).
