# FogMS and native screen-space scattering

**Historical, superseded:** the owner rejected native FSSS and explicitly requested a Karpukhin-like SSFS. See `FogMS_SSFS_Report.md` for the independent implementation and its limits. The native research below remains evidence; its integration recommendation is no longer current.

22 September 2026. Source research and an unexecuted native control harness. **No runtime, visual or performance acceptance is claimed by this document.** Installed UE source root: `D:/PersonalProjects/UE5/UE_5.8/Engine`, read only. Work remains in the current plugin and existing map.

## Integration choice

Reuse native UE Fog Screen Space Scattering (FSSS) as an explicit scene-level option on the existing ExponentialHeightFog. FogMS B3 already writes the standard volumetric source; `HeightFogPixelShader.usf:184` consumes its integrated result through `CombineVolumetricFog`. No new lighting solver, renderer pass or Engine edit is necessary. Native FSSS is distinct from Dmitry Karpukhin's commercial SSFS plugin; equivalent implementation or quality is not asserted.

The native effect is an image-space approximation after B3 integration. It has no access to the B3 directional/spatial field and does not add a measured transport order. It also affects native height fog and other volumes sharing the image. Do not expose a Box-local switch that silently promises isolated behavior or interpret the result as extending B3 energy validation.

## Verified controls and source anchors

| Anchor relative to Engine | Contract |
|---|---|
| `Source/Runtime/Engine/Classes/Components/ExponentialHeightFogComponent.h:194–229` | Experimental component controls: Enable FSSS, Scene Color Scattering Amount Scale/Power, Spread Scale, Blur Control |
| `Source/Runtime/Engine/Private/Components/ExponentialHeightFogComponent.cpp:115–119` | Defaults: disabled, Scale1, Power1, Spread.1, Blur.5 |
| `Source/Runtime/Renderer/Private/FogSeparateComposition.cpp:53–76` | First `ExponentialFogs[0]` must enable FSSS; global ScreenSpaceScattering positive; SeparateComposition -1 auto, 0 off, 1 forced |
| `Source/Runtime/Renderer/Private/FogSeparateComposition.cpp:14–35` | CVars default SeparateComposition -1, ScreenSpaceScattering 1, MaxExposedLuminance 10 |
| `Source/Runtime/Renderer/Private/FogRendering.cpp:65–69,847–874` | Separate FSSS TAA/history, default 1, black velocity buffer, Low TAA quality, mip1 output |
| `Source/Runtime/Renderer/Private/FogSeparateComposition.cpp:438–456` | Full-resolution RGBA16F mip-chain and RG16F side data; half-resolution is unimplemented at lines79–82 |
| `Source/Runtime/Renderer/Private/FogRendering.cpp:188–199,782–786` | Integrated volumetric scattering, depth, current SceneColor, fog/view uniforms are bound |
| `Source/Runtime/Renderer/Private/FogRendering.cpp:960–965` | Deferred path only; SM5+ and typed RGBA16F UAV support are also required by FogSeparateComposition.cpp:44–50,118,181 |
| `Config/BaseScalability.ini:823,852,881` | EffectsQuality0–2 force SeparateComposition0; Epic/Cine permit auto at lines913/948 |

For the next controlled integration, the harness enables the native actor flag and ScreenSpaceScattering1 / SeparateComposition-1 / TAA0. TAA0 is a deliberate diagnostic/initial-integration choice to avoid another motion-history layer; it does not fix FogMS aliasing. Native FSSSHistory cannot receive the current Box history-confidence response. Compare TAA0/1 separately before selecting a shipping policy.

## What the pass computes

`Shaders/Private/HeightFogPixelShader.usf:222–240` transfers part of background SceneColor into a fog source image:

```text
A = pow(saturate((1 − T) * Scale), Power)
source = Lfog + T * A * SceneColor
remaining sharp background = T * (1 − A) * SceneColor
```

Before filtering, this split preserves the original `T*SceneColor` algebraically. FSSS then filters the entire source, including the already integrated B3 luminance. `FogScreenSpaceScattering.ush:22–47` assumes albedo1, g0 and average extinction inferred from transmittance and depth to the visible surface. It does not use the true Box segment length or colored absorption. `FogSeparateComposition.usf:198–225` uses a 5×5 Gaussian through nine bilinear samples; HDR sample values are capped by MaxExposedLuminance. `FogSeparateComposition.ush:29–50` chooses a mip from PSF width/coverage/spread while retaining fog and cloud transmittance from mip0.

This is not a rigorously energy-normalized extension to B3. There is no geometric scattering path behind the camera or outside the frame; spatially varying kernels, scene-color heuristics and clamping affect the result. Native warnings identify halos around depth discontinuities at high spread. Scale0 isolates fog-image filtering; it does not make that filtering a volumetric transport solve.

## Sun, sky and ordering

`DeferredShadingRenderer.cpp:3734–3737` places SkyAtmosphere aerial perspective in separate composition only with an existing sky material. Fog source preparation runs at3764, cloud composition at3769, mip filtering at3776 and final fog composition at3811. Without a sky material, direct SkyAtmosphere renders at3781–3784, after FSSS source preparation. Its sun disk is then absent from the SceneColor being blurred, although final fog transmittance still attenuates it. Native FSSS therefore cannot be promised as universal sun-disk softening. Adding a second FogMS transmittance multiplication would double attenuation rather than solve this ordering limitation.

## Control harness and acceptance

[ScreenScattering/README.md](Tools/FogMSEnergyValidation/ScreenScattering/README.md) documents explicit enable/disable/restore and metadata probing. The original scalar state is stored once; source files, density, camera and maps are not changed by the script. It restores CVar numeric values, not original console SetBy priority. Multiple fog components are rejected because actor enumeration cannot establish the renderer's first fog.

Validation must keep B3, density phase, lights, exposure, camera and resolution fixed. Compare baseline, separate composition without scattering, fog-only FSSS, and fog+scene FSSS. Then compare TAA0/1 independently. Inspect W/S movement and stopping, lateral movement, near/far contours, a thin foreground pillar, emissive behind fog, zero density, absorbing albedo, and the sun with/without a sky material. No zero-density/global fog regression or foreground halo should be hidden by a broad image average. Use native viewport capture; a controls-only receipt cannot establish visible pass execution or quality.

Profile `Fog::RenderSeparateCompositionTextures`, `FSSS::GenerateMipChain`, `Fog::UpsampleSeparateCompositionTextures` and total frame separately from B3. Numerical B3 fixtures remain FSSS-off. Distant dolly/zoom shimmer, coherent spatial multiple scattering and native FSSS appearance are separate acceptance questions. No timings or rendered PASS are available for this new harness yet.
