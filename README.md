# MultiLobeSpec v0.14

Experimental UE 5.7 editor plugin for legacy deferred shading. It implements
dual-lobe GGX, offline Generic VNDF direct micro-visibility LUTs, raw MaterialAO
transport, and RGB indirect material visibility through a transactional shader
overlay.

The production target is static meshes using isotropic Default Lit materials.
The required renderer contract is:

```ini
[/Script/Engine.RendererSettings]
r.AllowStaticLighting=False
r.Substrate=False

[ConsoleVariables]
r.GBufferDiffuseSampleOcclusion=0
```

CARD-09 cone-aware capture/skylight EnvBRDF is included as a validated staging
implementation, but remains disabled and fail-closed because the overlay-only
static LUT did not pass the UE 5.7 SM6 compile-cost gate. Production activation
requires a renderer-bound Texture3D/SRV path.

- [Документация и установка](README_RU.md)
- [Execution spec v0.14](MultiLobeSpec_v0.14_Generic_VNDF_Execution_Spec_RU.md)
- [Support matrix](SUPPORT_MATRIX_RU.md)
- [Validation receipt](VALIDATION_RU.md)
