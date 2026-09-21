# FogMS — filtered shadows and spatial transport slice

2026-09-21. Status: implemented and installed; StrictIncludes Build5 passed. GPU evidence and limitations are in FogMS_Fields_Report.md. Owner visual acceptance remains pending; the final runtime/install receipt is in the task artifact directory.

The owner approved both cloud-style shadow preparation/filtering and spatial scattering, then authorized autonomous project restarts. Keep the existing FogMS_Box level, current Captured Scene Sky Light, authored density, lights, transforms and exposure. Engine source is read-only; no fork. This is not permission to implement screen-space darkening or multiply all Lumen GI.

## Bounded tasks

1. Early FogMSRender module and resident filtered shadow field (new module files and shader; existing modules keep their lifecycle).
2. Box runtime/overlay integration, opt-in filtered shadow controls, reference march fallback. Prefix integration in a light-space atlas stores T at 65 depth boundaries; XY filtering averages T, not optical depth. Sigma is an explicit artistic world-space softness control, not a claim to trace the solar disk. Geometry shadows remain native.
3. Experimental isotropic SpatialPreview alternative to Octaves. A plugin compute pass traces world-space segments through the native TLAS and integrates the previous fog source analytically per segment. The receiving native sigma_s is applied once. Previous-frustum source availability, temporal delay and opaque treatment of masked triangles are limitations to measure; this is A2, not completion of the future camera-independent B solver.
4. StrictIncludes build, source/Engine hash checks, D3D12 GPU checks in the same existing level, package installation and restored user setup.

Each implementation task has at most six owned product files. Shared integration is sequenced by the main agent; other agents own separate module/shader files or verification artifacts.

## Acceptance for this slice (not full project completion)

1. Disabled new modes preserve the prior A1e rendering path, native geometry shadows, density, and light/emissive inputs.
2. The filtered shadow respects receiver depth before/inside/after the OBB, softens in a world-space scale and does not stretch with camera movement.
3. SpatialPreview actually receives light from neighbouring fog, respects opaque blockers, is mutually exclusive with Octaves, and gates unsupported settings visibly.
4. Zero scattering produces no added MS; the de-exposed field responds linearly to source scaling in controlled GPU checks, within measured numerical tolerance.
5. Camera changes, history resets and illumination changes are measured; do not describe the history-fed preview as camera-independent or a converged physical solver.
6. Build and current D3D12 shaders pass; no Engine modifications; installed files match the built package.
7. The original level and latest authored settings are preserved with backups; the editor is left open with comparison controls and a short field protocol.

## Current evidence

- Read and saved the live scene after the user switched Sky Light to Captured Scene. Detailed snapshot and backup receipt: `.codex-build/FogMS_Fields_20260921/` in the parent workspace.
- Backup: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/FogMS_Backups/FogMS_Fields_20260921_021821`.
- Prior A1c research used a specified sunset cubemap with SkyAtmosphere hidden; that did not prove dynamic day/night balance. The recent lighting diagnosis is in `.codex-build/FogMS_LightingBalance_20260921/findings.md`.
- Final receipt: `delivery-receipt.json`, Build5/Package5, 70 package hashes equal, all staged Source/Shaders equal current source, 6 Engine shader hashes unchanged, same 15 actors, no unexpected authored changes or recorded CVar changes. Final5 saved and active; main editor remains open.
- GPU checks and remaining acceptance gaps are recorded in `FogMS_Fields_Report.md`. Forced RDG async=2 stress test failed with an RHI GraphicsContext assertion; default1 restored. Final normal launch has only the independently identified native Dataflow Date startup diagnostic, not a FogMS shader/RHI error. Full B, continuous-motion acceptance and an isolated opaque-wall source test remain unfinished.
