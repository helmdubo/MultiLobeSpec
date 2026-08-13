# MultiLobeSpec v0.15.0

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

With micro-shadowing Off, the plugin leaves the stock legacy AO/sample-occlusion
transport intact. With any micro-shadow mode active, the overlay transports raw
MaterialAO as continuous micro-visibility and forces it to **direct-only** use;
it does not darken Lumen GI, skylight diffuse, or other indirect diffuse paths.

`MLS.DebugView 0..5` switches through the existing SceneViewExtension/UserFlags
path on the next rendered frame, without rebuilding the overlay or recompiling
shaders.

The baker includes calibrated presets for deep debris, medium stone/brick, and
shallow sand/earth. Items can be removed from the bake list, and successful
outputs are assigned to the gathered material instances during the same bake.

- [v0.15 Activision profile and domain audit](ACTIVISION_MICROSHADOW_V015_RU.md)
- [Russian documentation](README_RU.md)
- [Support matrix](SUPPORT_MATRIX_RU.md)
- [Generic VNDF execution spec](MultiLobeSpec_v0.14_Generic_VNDF_Execution_Spec_RU.md)
- [Validation receipt](VALIDATION_RU.md)
