# FogMS SSFS: include the illuminated fog

22 September2026. Evidence: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_SSFS_FogLight_20260922`. Same existing map; plugin-only change. GPU/scene validation in progress.

## Defect

The previous custom pass removed the integrated in-scattered fog radiance before filtering, then returned that radiance unchanged. It also bypassed pixels with near-zero transmission. Thus the sun/background could blur while the illuminated or opaque fog did not. The owner's report exposed a mismatch with the requested SSFS effect; the previous solar-only acceptance was insufficient.

## Change

The pyramid now filters the full current pre-exposed HDR scene, including the light of the fog. Full-resolution volumetric coverage controls the blend:

`WeightedColor, Mass = positive depth-guided HDR filter(SceneColor, 1)`

`Result = SceneColor + Amount * (1 - Transmission) * (WeightedColor - Mass * SceneColor)`

For positive accepted mass this is a blend toward the normalized filtered image with effective amount multiplied by that mass. Writing it without division makes the transition to zero accepted support continuous.

Source alpha is a unit normalization weight, never transmission. A separate full-resolution guide stores coverage; coarse guides retain depth ranges. There is no division by transmission, fog-light subtraction or fully opaque fog exclusion. Constant images are preserved, no-fog and Amount0 bypass the effect, and fully opaque fog is processed. First-level scene-linear Karis weighting and positive cubic reconstruction remain.

This is an independent image-space approximation inspired by the requested effect, not proprietary SSFS code. It is not an angular B3 solver or a proof of global energy conservation. Spatially varying coverage, nonlinear Karis weights, opaque-depth guidance and FP16 clipping remain limitations. Opaque-background depth can restrict filtering of fog in front of geometry. The mask remains shared native+FogMS fog, not isolated Box coverage.

## Performance separation

SSFS does not require experimental ViewIntegration3. The project preset has been returned to `r.FogMS.ViewIntegration 0`; SSFS remains enabled independently. The previous77ms integration regression is not repaired by this SSFS change and must not be hidden by reporting SSFS-only timing.

## Verification

Independent CPU contract `Tools/FogMSEnergyValidation/ScreenScattering/test_full_hdr_scattering.py`:12/12 PASS. It includes a localized light signal entirely from opaque fog on a black background, constant HDR, no-fog/Amount0/off identity, positive bounded blending, depth rejection/fallback, pre-exposure invariance and a counterexample to global energy conservation. Evidence: `.codex-build/FogMS_SSFS_20260922/ssfs-full-hdr-contract.json` outside the plugin repository.

Fresh StrictIncludes/NoPCH/NoSharedPCH/DisableUnity Package1 build passed;89 installed files were SHA-256 verified and the existing map was unchanged by installation. The first scene test (`fog-light-01`) revealed a hard depth-support boundary, so it is a retained visual failure despite protocol PASS. Normalizing tiny accepted support back to full-strength blur caused the discontinuity. A shader-only correction now attenuates effect strength by that support mass. C++ resource layout is unchanged; the installed USF was backed up and hash-verified (`shader-confidence-install.json`). UE's native `RecompileShaders Changed` successfully compiled all9 FFogMSScreenScatteringCS permutations. Final scene results will be appended below.
