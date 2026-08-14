# MultiLobeSpec v0.15.1

UE 5.7 editor plugin for legacy deferred shading. It implements dual-lobe GGX,
an Activision / Call of Duty: WWII direct material micro-shadow profile, raw
MaterialAO transport, and a Poisson/GTAO material-visibility baker through a
content-addressed shader overlay. **No engine fork is used.**

The recommended production profile is `Activision / CoD:WWII`. The Generic VNDF
LUT remains available as an experimental research mode, but is no longer the
default because its current 4D approximation can over-attenuate direct lighting
on production materials.

Required renderer contract:

```ini
[/Script/Engine.RendererSettings]
r.AllowStaticLighting=False
r.Substrate=False

[ConsoleVariables]
r.GBufferDiffuseSampleOcclusion=1
```

The BRDF preset, direct micro-shadow mode, and indirect material-visibility
profile are independent. `Preset=Off` plus an active micro-shadow mode keeps the
stock single-lobe UE BRDF and still activates direct micro-shadowing. DirectOnly
is the default indirect policy; Full RGB diffuse is an explicit opt-in. The
reflection-environment policy removes material AO from stock scalar capture/
skylight specular occlusion in DirectOnly/Full while preserving geometric AO.

`Direct Cavity Depth` (`MLS.CavityDepth <0..1> [power]`) multiplies the direct
micro-shadow term by `lerp(1, V^power, depth)`: lit-side cavities deepen toward
the Full RGB look while cast shadows and indirect lighting stay exactly
unchanged. Default 0.5; set 0 for the pure micro-shadow term.

`MLS.DebugView 0..5` switches through the existing SceneViewExtension/UserFlags
path on the next rendered frame, without rebuilding the overlay or recompiling
shaders.

The baker takes four physical parameters — Surface Size (cm), Relief Height
(cm), Occlusion Radius (cm), Quality (samples/texel) — and derives the rest
(relief multiplier from a unit Poisson solve and the robust P1..P99 height
span, pixel radius, slice/step split, fixed sampling kernel), so the same
settings give the same physical AO at 1K/2K/4K. Presets cover deep debris,
medium stone/brick, and shallow sand/earth; each bake writes an
`.mlsbake.json` calibration receipt next to the AO asset. Items can be removed
from the bake list, and successful outputs are assigned to the gathered
material instances during the same bake.

- [v0.15 Activision profile and domain audit](ACTIVISION_MICROSHADOW_V015_RU.md)
- [Russian documentation](README_RU.md)
- [Support matrix](SUPPORT_MATRIX_RU.md)
- [Generic VNDF execution spec](MultiLobeSpec_v0.14_Generic_VNDF_Execution_Spec_RU.md)
- [Validation receipt](VALIDATION_RU.md)
