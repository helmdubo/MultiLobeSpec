# FogMS без -BindlessAll: маршрут через emissive volume-материала (исследование)

Дата: 2026-09-23. Исследователь: субагент (read-only), сверка выборочная Architect'ом. Ветка `claude/fogms-production`. Все ссылки file:line относятся к этому worktree и к `D:\PersonalProjects\UE5\UE_5.8\Engine`.

## Резюме по-русски

**Что нашли.** Bindless в FogMS нужен только для одного: протащить данные (пакет Box, атлас плотности, атлас транспорта, кэш тени солнца) внутрь шейдеров движка, у которых нет слотов под плагинные текстуры. Все собственные RDG-проходы плагина (транспорт B2/B3, world lighting, shadow cache, SSFS) уже используют обычные привязки (`FogMS_Transport.usf:4`, `FogMS_WorldLighting.usf:6`, `FogMS_ShadowCache.usf:4`, `FogMS_ScreenScattering.cpp:32–51`).

**Точный bindless-free маршрут для MS-света.** Штатная вокселизация volume-материалов (`VolumetricFogVoxelization.usf:328–344`) пишет `Scattering = Albedo·Extinction` и `Emissive` аддитивно в VBufferA/VBufferB, а `LightScatteringCS` (`VolumetricFog.usf:1161–1169`) прибавляет emissive к source term **без умножения на σs** и сам применяет pre-exposure. Значит, если материал Box выдаёт `BaseColor = 0`, `Extinction = σt` (как сейчас в raster MID), `Emissive = σs[1/м]·J` (сэмпл из `UTextureRenderTargetVolume`, который заполняет наш проход в `PostTLASBuild`), то штатный путь даёт ровно ту величину, которую сегодня оверлей подставляет вручную (`MultiLobeShaderPatcher.cpp:808–810`). Двойного учёта нет, единицы сходятся (`UnitScale 1/100` движка компенсируется выдачей σs в 1/м).

**Что теряется без оверлея** (честная цена): управление историей и джиттером штатного тумана для Box (native history будет смешивать J, как для любого volume-материала; с warm start J стабилен между кадрами, поэтому это менее болезненно, чем в прежнем дизайне); затухание вокселизации после 60 % дистанции тумана (частично компенсируется в материале через `View.VolumetricFogMaxDistance`); связная реконструкция через маску граней (нужен Custom-узел или предзапечённое поле); A1-самозатенение глобального слоя, A1e-тень на поверхностях, диск солнца в SSFS, режимы ViewIntegration 1–3, debug-виды.

**Второй блокер, который bindless не решает:** модуль FogMSRender включает `Runtime/Renderer/Private` (`FogMSRender.Build.cs:13`) и приводит `FSceneView` к `FViewInfo` (`FogMS_WorldLighting.cpp:273`) ради TLAS, inline-RT binding data и данных Lumen. Для TLAS и binding data есть публичные `UE::FXRenderingUtils::RayTracing::GetRayTracingSceneViewRDG` / `GetInlineRayTracingBindingDataBuffer` (см. `FogMS_Audit_UE58.md` §8.3); доступ к surface cache Lumen и `LumenHardwareRayTracingHitDataBuffer` публичного пути не имеет — отдельная задача.

**Предложение.** (a) Production-ядро: emissive-инъекция, без bindless и патчей. (b) «Advanced (требует -BindlessAll)»: оверлейные функции как опция. (c) Невозможное без патча движка: любой per-froxel член в `LightScatteringCS`, зависящий от плотности; интеграция в `FinalIntegrationCS`; per-primitive управление историей; отключение затухания 0.6 и джиттера вокселизации.

Полный отчёт исследователя (английский, с file:line) ниже.

---

## 1. Inventory of every bindless read

### 1.1 The hard gate

`Shaders/Private/FogMS_Common.ush:25-27`: `#if !UE_BINDLESS_ENABLED || !PLATFORM_SUPPORTS_BINDLESS  #error Live FogMS Box requires the D3D12 SM6 BindlessAll shader configuration.`
Runtime gate: `Source/MultiLobeSpec/Private/FogMS_BoxRuntime.cpp:918-926` — `Prepare()` refuses unless `GetBindlessConfiguration == All`, D3D12, SM6, single GPU.

### 1.2 How the descriptor index reaches the shader

| Level | Mechanism | Where |
|---|---|---|
| Packet texture descriptor (root) | `#define FOGMS_BOX_DATA_SRV %uu` baked into generated `/Engine/Private/FogMS_Config.ush` at `FogMS.Apply` | `MultiLobeShaderPatcher.cpp:855-862`, written at `:875` |
| All other descriptors (density atlas, transport/world atlas, sun-shadow cache) | floats inside the packet rows, read at runtime | `FogMS_BoxRuntime.cpp:729` (row 7.z), `:813` (row 22.x), `:863` (row 20.x) |

Packet = `24 x 2` RGBA32F committed D3D12 resource outside RDG (`FogMS_BoxRuntime.cpp:940-963`), `RHICreateTexture2DFromResource(External)` + `SRV->GetBindlessHandle()`. Rows re-uploaded per frame by `FBoxGPUState::Upload` (`:320-336`) inside a `NeverCull` pass with manual transitions because bindless readers are invisible to RDG (`:886-897`). The descriptor index is part of the overlay identity (`MultiLobeShaderPatcher.cpp:640-642`), so a different index forces a full overlay rebuild.

### 1.3 The nine `GetSRVFromHeap` call sites

| # | Site | Reads | Engine stage(s) |
|---|---|---|---|
| 1 | `FogMS_Common.ush:33` (`FogMS_BoxRow`) | packet rows 0..23 | MaterialSetupCS, LightScatteringCS, FinalIntegrationCS (include at patcher `:761-763`); TranslucencyVolumeIntegrateCS (`:564-565`); DeferredLightPixelShaders (`:717-721`) |
| 2 | `FogMS_Common.ush:43` (`FogMS_BoxPreviousPhase`) | packet row y=1, rows 13..15 | LightScatteringCS (`FogMS_Reconstruction.ush:330-332`) |
| 3 | `FogMS_Indirect.ush:106` (`FogMS_IndirectSampleNoise`) | density atlas (row 7.z), 8 manual loads | every stage evaluating authored density |
| 4 | `FogMS_Indirect.ush:213` (`FogMS_FieldSample`) | sun-shadow cache (row 20.x), spatial/world field (row 22.x) | LightScatteringCS, DeferredLightPixelShaders |
| 5 | `FogMS_Indirect.ush:295` (`FogMS_TransportIncident`) | transport atlas | **dead code**, no callers |
| 6 | `FogMS_Reconstruction.ush:13` (`FogMS_BoxViewIntegrationMode`) | packet (0,1).x | LightScatteringCS + FinalIntegrationCS |
| 7 | `FogMS_Reconstruction.ush:136` | transport atlas → `FogMS_TransportFieldIncident` | LightScatteringCS (patcher `:805`), FinalIntegrationCS (`:833`) |
| 8 | `FogMS_Reconstruction.ush:269` | transport atlas | FinalIntegrationCS (`:845`) |
| 9 | `FogMS_ScreenScattering.ush:35` | packet gate rows | HeightFogPixelShader `RENDER_FOG_COMP_TEXTURE_CS` (`:691-694`) |

Transport atlas layout (`FogMS_Transport.usf:372-377`): slab0 = (Total J, error), slab1 = Primary (+6-bit face mask in .a, read at `FogMS_Indirect.ush:248`), slab2 = (σs.rgb, σt.a), slab3 = flux diagnostics. Height `4·N·N` (`FogMS_WorldLighting.cpp:138`).

### 1.4 What is not bindless

All producer passes bind normally with `float4 BoxRows[24]` uniform. Bindless exists solely to smuggle data into engine shader stages without parameter slots.

## 2. The Volume-material route — verified against engine source

### 2.1 The plugin already drives a Volume-domain MID
`FogMS_BoxVolume.cpp:170-191`: `DensityComponent` = `UStaticMeshComponent` with `/Engine/BasicShapes/Cube.Cube` (`:185`) and `/MultiLobeSpec/FogMS/M_FogMS_Density` (`:186`). `:454-456` validates `MD_Volume && BLEND_Additive`. `:554-570` sets 17 MID parameters incl. `SetTextureParameterValue("FogMS_Noise", DensityTexture)` (a `UVolumeTexture`, `FogMS_BoxVolume.h:191`) — a Texture3D parameter on this material is proven. Visibility toggles the primitive (`:579-582`) → `View.VolumetricMeshBatches` via `bHasVolumeMaterialDomain` (`SceneVisibility.cpp:1952-1966, 2615-2631`). Not stolen by Heterogeneous Volumes (`HeterogeneousVolumes.cpp:556-558` requires `IsHeterogeneousVolume()`).

### 2.2 How Volume-material outputs reach the fog grid
`VolumetricFogVoxelization.usf`: `:29-41` Extinction = `GetMaterialSubsurfaceDataRaw().r` clamp [0, 65000]; `:43-53` Emissive = `GetMaterialEmissiveRaw()` clamp [0, 65000]; `:55-67` Albedo = saturate(BaseColor); `:328` `Scattering = Albedo * Extinction`; `:330-334` `SliceFadeAlpha` from 60 % of `VolumetricFog.MaxDistance`, `Scale = (1/100) * SliceFadeAlpha^3`; `:337` OBB shape masking; `:343-344` `OutVBufferA = (Scattering*Scale, Extinction*Scale)`, `OutVBufferB = (Emissive*Scale, 0)`. Blend additive on both RTs (`VolumetricFogVoxelization.cpp:427-429`), targets bound `ELoad` (`:717-721`). World position from `SvPosition` (`MaterialTemplate.ush:4754-4757`). Jitter via `ViewToVolumeClip` (`.cpp:61-62`) and `FrameJitterOffset0.z` (`.usf:167`).

### 2.3 How emissive is consumed
`VolumetricFog.usf:1161-1169`: emissive added with no σs multiply, `View.PreExposure` applied by the engine. `r.VolumetricFog.Emissive` default 1 (`VolumetricFog.cpp:1650-1656`). FinalIntegration treats the source as radiance per unit length (`VolumetricFog.usf:1240`). Unit chain: material Emissive → ×1/100 → froxel source [radiance·cm⁻¹] ⇒ **the material must output `σs[1/cm]·J·100 = σs[1/m]·J`**.

### 2.4 Exactness
With `BaseColor = 0`: `VBufferA.rgb += 0` (no native single scatter on Box σ), `VBufferA.a += σt·Scale` (occlusion kept), `VBufferB.rgb += σs·J·100·Scale` (full transport source). This reproduces the overlay's manual substitution at `MultiLobeShaderPatcher.cpp:808-810` without touching the engine shader; the global height-fog layer keeps being lit natively.

### 2.5 Every mismatch

| # | Mismatch | Evidence | Severity / mitigation |
|---|---|---|---|
| M1 | `SliceFadeAlpha^3` after 60 % of fog distance applies to extinction and emissive | `VolumetricFogVoxelization.usf:330-334` | Real; compensable in-material via `View.VolumetricFogMaxDistance` (`SceneView.h:1084`) with a clamp; far boundary stays lossy |
| M2 | Pre-exposure | `VolumetricFog.usf:1169` | Not a mismatch: engine applies it; delete the manual multiplies (`FogMS_Reconstruction.ush:247, :307`; patcher `:810, :835`) |
| M3 | Temporal history blends injected emissive | `VolumetricFog.usf:1173-1181` | Structural: the bespoke rejection (`FogMS_Reconstruction.ush:320-370`, patcher `:811`) becomes unreachable; only global `r.VolumetricFog.TemporalReprojection` |
| M4 | Per-froxel voxelization jitter | `.usf:167`, `.cpp:61-63` | Coupled with M3; fixed tetrahedral quadrature (`FogMS_Reconstruction.ush:26-46`) is lost |
| M5 | Emissive bypasses phase | `VolumetricFog.usf:1169` | Benign: transport is isotropic by contract (`FogMS_WorldLighting.cpp:236`) |
| M6 | Connectivity-aware reconstruction lost with trilinear sample | `FogMS_Indirect.ush:234-282` | Real (leaks through thin walls); recover via Custom-HLSL node with 8 loads + mask, or pre-baked connectivity-resolved field (precedent `FogMS_Transport.usf:355-364`) — ASSUMED that a Custom node can `.Load()` a TextureObject input |
| M7 | VBuffer half precision | `VolumetricFog.cpp:1401-1408` | Already the case today |
| M8 | Clamps ≤ 65000 before Scale | `.usf:40, :52` | Emissive in per-metre units; practically safe, warn |
| M9 | Voxelization needs GS or VS-layer | `.cpp:36-43` | Fine on D3D12 SM6 desktop |
| M10 | Ordering | `DeferredShadingRenderer.cpp:3663`, hooks at `:3324` / `BasePassRendering.cpp:1162` | OK, both hooks precede fog |

### 2.6 Can a material sample a Texture3D the plugin writes from the render thread?
Yes. `UTextureRenderTargetVolume::GetMaterialType()` = `MCT_VolumeTexture` (`TextureRenderTargetVolume.cpp:135-138`); `bSupportsUAV` → `bCanCreateUAV` (`:127-130`), `TexCreate_UAV` (`:246-250`), `GetUnorderedAccessViewRHI()` (`TextureRenderTargetVolumeResource.h:62`), class in `Engine/Public`. Engine example: `NiagaraDataInterfaceRenderTargetVolume.cpp` (`:1054-1078`, `:1107`, `:458-460`, `:799-800`). Hand-off to samplers RDG cannot see: `FRDGBuilder::UseExternalAccessMode(Resource, SRVMask, Graphics)` (`RenderGraphBuilder.h:384`).

## 3. What still needs the overlay, and which of that needs bindless

| Feature | Overlay? | Bindless data? | Notes |
|---|---|---|---|
| A1 medium self-shadow of the directional light (`FogMS_Common.ush:122, :192-224`; patcher `:773-785`) | Yes, unavoidable (`VBufferA` bound only in LightScatteringCS, `VolumetricFog.cpp:1155`) | Only Box gate rows in Box mode; **global-layer A1 uses defines only, zero bindless** | Inside a Transport box A1 is redundant: `FogMS_WorldLighting.usf:151-158` already applies medium transmittance to direct light |
| Octaves (`FogMS_Common.ush:104-118`; patcher `:786-794`) | Yes | rows 5, 6 | artistic constants could be define-baked; rows 0-4 cannot |
| Sun-shadow cache on opaque surfaces (A1e, patcher `:717-721`) | Yes | Yes | no hook in deferred lighting |
| SSFS sky disk (patcher `:691-694`) | Yes | gate only (`FogMS_ScreenScattering.ush:36-45`) | bakeable as defines |
| ViewIntegration modes 1/2/3 (`FogMS_Reconstruction.ush:153-316`; patcher `:829-853`) | Yes | Yes, hard (FinalIntegrationCS binds only `LightScattering`, `VolumetricFog.cpp:1338-1339`) | impossible without bindless or patch |
| History rejection (patcher `:811-816`) | Yes | rows 5, 22, 23 | see M3 |
| Lumen TLV indirect shadowing (`:549-617`) | Yes | Yes | optional preview |

Packet rows via defines: no (rows 0-4 change on every move; identity includes them → recompile). MPC: material-only, useless for global shaders. `View.GeneralPurposeTweak`: one float, already used for debug (`FogMS_Common.ush:14`). **Overlay and bindless stand or fall together.**

## 4. The density split today
`FogMS_BoxVolume.cpp:516` `bUseNativeDensity = !bEnabled || !IsTransportMode`; `:567` MID `FogMS_Density` forced to 0 in Transport modes. Non-transport modes: raster MID → native `VoxelizePS` → VBufferA (no bindless, no patch). Transport modes: packet + density atlas via overlay (`MultiLobeShaderPatcher.cpp:765-768`, `:799-810`). The HLSL density function (`FogMS_Indirect.ush:98-158`) is a hand-port of the material graph — a maintenance liability that disappears in the material route.

## 5. Architecture proposal, ranked

### (a) Fully bindless-free, patch-free — "Emissive Injection" (recommended product core)
1. Keep the transport solver as is (bound parameters).
2. Add a `UTextureRenderTargetVolume` (N³, RGBA32F or FloatRGBA) owned by the Box actor, `bSupportsUAV`, `UpdateResourceImmediate(true)` once.
3. In `PostTLASBuild_RenderThread` write `σs·J·100` into it (`RegisterExternalTexture(CreateRenderTarget(...))` + `CreateUAV`, then `UseExternalAccessMode(SRVMask, Graphics)`).
4. Extend `M_FogMS_Density`: Extinction = authored density (re-enable raster MID in Transport mode, drop the gate at `:516`); `BaseColor = 0`; `Emissive = Texture3DSample(FogMS_TransportField, uvw)` with uvw from `AbsoluteWorldPosition` and `FogMS_WorldExtent`.
5. `SetTextureParameterValue("FogMS_TransportField", RT)` once.
6. Delete `FOGMS_BOX_DATA_SRV`, the packet texture, the density atlas and the HLSL density re-implementation for this path.
Accepted regressions: M1, M3, M4, M6; no debug views, no SSFS sky disk, no A1e, no ViewIntegration 2/3.
Legality: `Engine/Classes/Engine/TextureRenderTargetVolume.h`, `Engine/Public/TextureRenderTargetVolumeResource.h:62`, `RenderCore/Public/RenderGraphUtils.h` (`CreateRenderTarget`), `RenderGraphBuilder.h:359,384,398`, `MaterialInstanceDynamic.h`, `FSceneViewExtensionBase`. Caveat: the producer still needs `Runtime/Renderer/Private` (`FogMSRender.Build.cs:13`) and `FViewInfo` (`FogMS_WorldLighting.cpp:273`) for TLAS / inline-RT data / Lumen cards — a separate marketplace blocker.

### (b) Bindless only for optional features
Ship (a) as default; keep overlay + `-BindlessAll` behind an "Advanced" switch (global-layer A1, A1e, SSFS sky disk, ViewIntegration 1-3 with history control, debug views). Cheap wins inside (b): delete dead `FogMS_TransportIncident` (`FogMS_Indirect.ush:284-297`) and `FogMS_TransportCoefficients` (`:227-232`); bake the SSFS gate as defines; bake rows 5/6/16/21/23 as defines.

### (c) Impossible without an engine patch
Per-froxel density-dependent terms in LightScatteringCS (A1 march); FinalIntegrationCS anchored integration; per-primitive temporal-history control; removing the `SliceFadeAlpha^3` fade (`VolumetricFogVoxelization.usf:330-334`, no cvar); suppressing voxelization jitter per primitive; a deferred-lighting hook for A1e.

## Bottom line
The multiple-scattering injection has an exact, fully supported bindless-free home: Volume material with `BaseColor = 0`, `Emissive = σs[1/m]·J`, `SubsurfaceColor.r = σt`, J delivered through a `UTextureRenderTargetVolume` written by the existing RDG pass. What is lost is control (temporal history, per-froxel jitter), not the quantity. Two extra flags: `FogMS_TransportIncident` / `FogMS_TransportCoefficients` are dead code; removing bindless does not make the plugin marketplace-clean by itself (private renderer headers remain).
