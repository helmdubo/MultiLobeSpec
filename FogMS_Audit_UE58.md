# FogMS — Аудит Volumetric Fog в UE 5.8 (контракт данных)

**Движок:** UE 5.8.2, ветка `++UE5+Release-5.8`, CL 56702186 (`Engine/Build/Build.version`).
**Корень исходников:** `D:\PersonalProjects\UE5\UE_5.8\Engine` — все пути ниже относительно него.
**Проект заказчика:** `D:\PersonalProjects\UE5\MimirHead_portfolio 5.7 5.8 - 3`.
**Дата аудита:** 2026-09-19. Режим: только чтение, код не писался.
**Актуализация:** 2026-09-20, по `FogMS_Research_Note_01.md` v1.3, решениям заказчика и сверке контракта A1. Утверждение модели не означает приёмку реализации: компиляция и визуальная проверка заказчиком впереди.

Маркировка утверждений: **VERIFIED (файл:строка)** — прочитано в исходнике; **NOT FOUND** — искали, не нашли; **ASSUMED** — вывод/оценка без прямой строки; **DERIVED** — математический вывод; **DECISION** — решение заказчика; **HYPOTHESIS** — непроверенная ветка исследования.

**DECISION — граница проекта:** файлы установленного Engine не изменять; Engine FORK/PATCH запрещены. Читать исходники для исследования разрешено. Все изменения шейдеров FogMS доставляются оверлеем из плагина MultiLobeSpec.

Сокращения: VF — Volumetric Fog; LFV — Local Fog Volume; VSM — Virtual Shadow Maps; RT — ray tracing; HWRT — hardware ray tracing; MS — multiple scattering; RGS — ray generation shader; CS — compute shader; PS — pixel shader.

---

## 0. Резюме для заказчика (что важно знать до чтения)

1. **Свет с RT-тенями в туман уже попадает и уже затенён трассировкой.** В 5.8 есть отдельные RGS-проходы `InjectShadowedLocalLightRGS` и `InjectShadowedDirectionalLightRGS`, привязывающие TLAS. Они включаются cvar `r.VolumetricFog.InjectRaytracedLights` (по умолчанию 0), а **в проекте заказчика он уже = 1** (`Config/DefaultEngine.ini:22`).
2. **TLAS уже привязан к RGS-проходам тумана** движком. Существующая инфраструктура (`View.GetRayTracingSceneLayerViewChecked`, `RayTracing::BindStaticUniformBufferBindings`, `View.MaterialRayTracingData`) используется внутри `VolumetricFog.cpp`. Плагин может получить TLAS публично для своего прохода (§8.3), но это не добавляет TLAS или новые ресурсы в штатный `LightScatteringCS`.
3. **Самозатенения среды нет.** Ни один проход не маршит extinction к источнику. Единственные «объёмные» тени на туман — тень облаков (`CloudShadowmap`) и DF-конус вверх для sky occlusion.
4. **Фаза применяется в сторону камеры прямо при инжекции** каждого источника (`PhaseFunction(PhaseG, dot(L, -CameraVector))`). В `LightScattering` и в истории лежит уже «фазированная» величина. **DERIVED:** её пространственный перенос как изотропного source term размерно корректен при g = 0, без деления на σs; ограничения — `FogMS_Research_Note_01.md` §2.1–2.3 и §12 п.3.
5. **Emissive не умножается на scattering** — прибавляется отдельно (`VolumetricFog.usf:1169`), как самостоятельный физический источник. Инвариант: **при σs = 0 вклад MS равен нулю** (`FogMS_HANDOVER.md` §9); штатный emissive этим не запрещён, в тестовой сцене он = 0.
6. **История хранит pre-exposed `LightScattering`** (до интегрирования), blend = `lerp(new, history, 0.9)`.
7. Sky/Lumen GI приходят как **SH2 (two-band)** из Lumen Translucency GI Volume, свёртка с зональной гармоникой HG — то есть Lumen уже даёт «мягкий заполняющий свет», но без учёта среды между точками.
8. **Граница оверлея подтверждена.** TLAS плагину доступен публично (`FXRenderingUtils.h:89`), inline RT в compute на D3D12 SM6 доступен при bindless. Текущие `VBufferA` и `LightScattering` RDG-локальны внутри `ComputeVolumetricFog`; между ближайшими хуками (`PostTLASBuild` до `RenderLights`, `PostOpaque` после тумана) точки вставки нет. A1 проектируется как оверлей `VolumetricFog.usf`, но передача масштаба аналитической плотности требует отдельного решения (§13.1). **B без изменения Engine — HYPOTHESIS до мини-аудита №2** (`FogMS_Research_Note_01.md` §5); Engine FORK/PATCH запрещены.
9. **DECISION 2026-09-20: MegaLights в проекте не используется.** Поддержка отложена, правила совместимости — `FogMS_Research_Note_01.md` §3.8. Тени в тумане сейчас решает per-light enum `CastRaytracedShadow`: RT → жёсткий 1 луч/froxel; Disabled → VSM.

---

## 1. Граф проходов Volumetric Fog (критерий 1)

### 1.1 Место в кадре (Deferred)

**VERIFIED** `Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp`:

| Порядок | Проход | Строка |
|---|---|---|
| … | `RenderDiffuseIndirectAndAmbientOcclusion` (Lumen; внутри — `ComputeLumenTranslucencyGIVolume`, см. `Lumen/LumenScreenProbeGather.cpp:2164`) | 3429 |
| … | `RenderLights` (deferred свет, тени уже отрендерены) | 3467 |
| … | `RenderMegaLights` (в т.ч. MegaLights Volume, если включён) | 3471 |
| … | `RenderTranslucencyLightingVolume` | 3478 |
| … | Front-layer translucency, reflections, sky lighting | 3495–3566 |
| **→** | **`ComputeVolumetricFog(GraphBuilder, SceneTextures)`** — комментарий движка: «Volumetric fog after Lumen GI and shadow depths» | **3660–3664** |
| … | `RenderHeterogeneousVolumes` | 3668 |
| … | Volumetric Cloud | 3673+ |
| … | FSSS: `RenderFogSeparateCompositionTextures` → `RenderFogScreenSpaceScatteringMipChain` → `UpsampleFogSeparateCompositionTextureForView`; иначе `RenderFog` | 3745–3822 |
| … | `RenderLocalFogVolume` (аналитический LFV поверх) | 3837 |
| … | `RendererModule.RenderPostOpaqueExtensions` (делегат для плагинов) | 3924 |
| … | `RenderTranslucency` (сэмплит `IntegratedLightScattering`) | 4011 |

Forward shading: `ComputeVolumetricFog` вызывается раньше, до base pass (2943). Проект заказчика — deferred.

### 1.2 Проходы внутри `ComputeVolumetricFog`

**VERIFIED** `Source/Runtime/Renderer/Private/VolumetricFog.cpp` (функция `FSceneRenderer::ComputeVolumetricFog`, 1497–2067) и `Shaders/Private/VolumetricFog.usf`.

```
[сбор списка источников]  1527–1589
        │
        ├─ ConservativeDepth (2D R16F, размер ResourceGrid.xy)          1635–1648
        ├─ DirLightFunction (2D, только главный directional)             1682–1694
        │
        ├─ VolumetricFog::LocalLights   RenderLocalLightsForVolumetricFog 1718
        │     ├─ "ShadowedLights"  PS-растр по слайсам, 1 проход/источник  988
        │     │      VS WriteToBoundingSphereVS + GS WriteToSliceGS + PS InjectShadowedLocalLightPS
        │     └─ "RayTracedShadowedLights"  RGS InjectShadowedLocalLightRGS, 1 dispatch/источник  1093
        │            → LocalShadowedLightScattering (RGBA16F, PRE-EXPOSED)
        │
        ├─ VolumetricFog::RaytraceDirLightShadow  RGS InjectShadowedDirectionalLightRGS  611–692
        │            → RaytracedShadowVolume (R16F, 0/1 видимость)
        │
        ├─ VolumetricFog::InitialiseVolume
        │     ├─ "InitializeVolumeAttributes"  CS MaterialSetupCS       1755
        │     │      → VBufferA (scattering.rgb, extinction.a)
        │     │      → VBufferB (emissive.rgb, 0)  [если r.VolumetricFog.Emissive]
        │     └─ VoxelizeFogVolumePrimitives (volume-материалы, additive)  1770
        │
        ├─ VolumetricFog::LightScattering  CS LightScatteringCS           1979
        │      входы: VBufferA, VBufferB, LocalShadowedLightScattering, MegaLightsVolume,
        │             RaytracedShadowsVolume, LightScatteringHistory, ConservativeDepth(prev),
        │             ForwardLightStruct (light grid), LumenGIVolumeStruct, VSM, DirLightFunction,
        │             CloudShadowmap, LightFunctionAtlas, GlobalDistanceField
        │      → LightScattering (RGBA16F): rgb = PreExposure·(Σ L·σs + emissive), a = σt
        │
        ├─ VolumetricFog::FinalIntegration  CS FinalIntegrationCS          2015
        │      → IntegratedLightScattering (RGBA16F): rgb = ∫ (pre-exposed), a = transmittance
        │
        └─ QueueTextureExtraction(LightScattering → ViewState.LightScatteringHistory)  2038
```

### 1.3 Ресурсы и форматы

| Ресурс (RDG-имя) | Формат / размер | Создан | Содержимое |
|---|---|---|---|
| `VolumetricFog.VBufferA` | `PF_FloatRGBA` (RGBA16F), 3D ResourceGrid | 1707 | rgb = σs (scattering), a = σt (extinction) |
| `VolumetricFog.VBufferB` | RGBA16F, 3D | 1713 | rgb = emissive (плотность излучения), a = 0 |
| `VolumetricFog.LocalShadowedLightScattering` | RGBA16F, 3D (RenderTargetable) | 955 / 1043 | Σ по затенённым локальным источникам, **pre-exposed**, фаза применена |
| `VolumetricFog.RaytracedShadowVolume` | `PF_R16F`, 3D | 651–657 | RT-видимость directional (0/1, один луч, джиттер) |
| `VolumetricFog.ConservativeDepthTexture` | `PF_R16F`, 2D ResourceGrid.xy | 1641 | консервативная глубина для отсечения освещения froxel'ов за геометрией; плотность `VBufferA` этим не обнуляется (§2.4) |
| `VolumetricFog.LightScattering` | RGBA16F, 3D (без RT-флага) | 1780 | см. §2.3 |
| `VolumetricFog.IntegratedLightScattering` | RGBA16F, 3D | 2002 | см. §2.4 |
| `ViewState.LightScatteringHistory` | extracted `LightScattering` | 2038 | + `LightScatteringHistoryPreExposure` (2039) |
| `ViewState.PrevLightScatteringConservativeDepthTexture` | extracted ConservativeDepth | 2060 | для FixupHistoryUV |

Общий дескриптор: `GetVolumetricFogRDGTextureDesc` — `Create3D(ResourceGridSize, PF_FloatRGBA, Black, ShaderResource|RenderTargetable|UAV|ReduceMemoryWithTilingMode|3DTiling)` (1401–1408).

### 1.4 Сетка (froxel grid)

**VERIFIED**:
- XY: `GetFroxelGridSize` = `DivideAndRoundUp(Resolution, GridPixelSize)` (`Froxel/FroxelGridUtils.cpp:20–31`). Есть **две** сетки: *ResourceGrid* по размеру scene textures (чтобы не переаллоцировать при dynamic resolution) и *ViewGrid* по `View.ViewRect` (1378–1386, 1598–1602). Диспатчи идут по ViewGrid, текстуры — ResourceGrid.
- Z: `CalculateGridZParams(Near, Far, DepthDistributionScale, GridSizeZ)` (`RenderCore/Public/RenderUtils.h:721–738`): `slice = log2(z·B + O)·S`, `S = r.VolumetricFog.DepthDistributionScale`, `Near = max(NearClip, VolumetricFogStartDistance) + 9.5 см`, `Far = VolumetricFogDistance` (StartDistance + View Distance, `SceneCore.cpp:430`). Обратная формула в шейдере: `ComputeDepthFromZSlice` (`Common.ush:2437–2441`). Слои сгущаются у камеры логарифмически.
- Cvars сетки (`VolumetricFog.cpp`): `r.VolumetricFog.GridPixelSize` = **16** (118), `r.VolumetricFog.GridSizeZ` = **64** (126), `r.VolumetricFog.DepthDistributionScale` = **32** (110).

**Оценка качества:** GridPixelSize 4 и 128 слоёв значительно дороже штатных 16 px × 64 слоя; это не дефолт движка. Полный список cvars — §11.

---

## 2. Контракт буферов (критерий 2)

### 2.1 Единицы и источники плотности

**VERIFIED** `VolumetricFog.usf:153–298` (`MaterialSetupCS`):

- Глобальная плотность: `GlobalDensity = P3.x·exp2(−P.y·(z − P3.y)) + P2.z·exp2(−P2.y·(z − P2.w))` (169–171), где `FogData[i].Density = FogDensity/1000`, `HeightFalloff/1000` (`SceneCore.cpp:404–407`). Мир в **сантиметрах**, значит плотность — в **1/см** после `/1000`.
- `GlobalDensity *= 0.5` («match height fog») (176–177).
- `Extinction = max(GlobalDensity · GlobalExtinctionScale, 0)` (178); `Scattering = Albedo · Extinction` (180). **Альбедо ≤ 1 гарантирует σs ≤ σt.**
- LFV в froxel-сетку: extinction считается в **1/м**, затем `*= 1/METER_TO_CENTIMETER` (269–270), ограничивается `LFVMaxDensityIntoVolumetricFog` (272). Radial и height складываются через −log(A − AB + B) (264–266) — это не сумма σt, а «покрытие», документированный компромисс Epic.
- Emissive: `VBufferB = GlobalEmissive (·GlobalDensity при PROJECT_EXPFOG_MATCHES_VFOG) + Σ LFVExtinction·LFV.Emissive` (291–295). Масштаб `EmissiveUnitScale = 1/10000` если проект **не** в режиме ExpFogMatchesVFog (`SceneCore.cpp:425`). **В проекте заказчика режим включён** (`r.SupportExpFogMatchesVolumetricFog=True`) → масштаб 1, emissive следует плотности.

**VERIFIED — контракт масштаба для A1:** `GlobalExtinctionScale` объявлен в `VolumetricFog.usf:135`, применяется в `MaterialSetupCS:178`, привязан как параметр этого прохода (`VolumetricFog.cpp:353, 1737`). Его **нет** ни в `LightScatteringCS::FParameters` (`VolumetricFog.cpp:1145–1201`), ни в `FFogUniformParameters` (`FogRendering.h:16–44`). Наличие объявления в общем `.usf` и привязки `Fog` UB не означает доставки этого значения в `LightScatteringCS`. В A1 написан захват того же свойства компонента при Apply с передачей отдельным define (§13.1); UE-проверка ещё впереди.

**VERIFIED** `FinalIntegrationCS` (1221–1249): `Transmittance = exp(−σt · StepLength)`, StepLength в см. Следовательно **σt в `VBufferA.a` — в 1/см**, интегрирование энергосохраняющее по Frostbite (`(S − S·T)/σt`).

### 2.2 Где применяется фаза

**VERIFIED** — везде в сторону камеры, до записи в буфер:
- Directional: `PhaseFunction(PhaseG, dot(LightDir, −CameraVector))` (988).
- Локальные без тени (light grid): 1139.
- Локальные с тенью (PS/RGS): 554.
- Sky/Lumen/VLM: свёртка SH2 с `RotatedHGZonalHarmonic = (1, V.y, V.z, V.x)·(1, g, g, g)` (991–992) — приближение HG первой полосой. Комментарий Epic: знак g, вероятно, надо инвертировать («I believe PhaseG here should be negated»).
- `PhaseFunction` = `HenyeyGreensteinPhase` (304–307). **Один глобальный g** = `VolumetricFogScatteringDistribution` (1888), clamp ±0.99 (`SceneCore.cpp:418`). Per-froxel g — **NOT FOUND** (см. §5).

### 2.3 `LightScattering` (вход интегрирования и содержимое истории)

**VERIFIED** `VolumetricFog.usf:1147–1199`:

```
L_total = Σ_direct(без тени, light grid) + directional·shadow + SH(Lumen|Sky) + VLM
L_total /= NumSuperSamples                                                  (1147)
L_total += OneOverPreExposure · LocalShadowedLightScattering                (1151)
L_total += OneOverPreExposure · MegaLightsVolume            [USE_MEGA_LIGHTS] (1157)
out.rgb = PreExposure · (L_total · VBufferA.rgb + VBufferB.rgb)             (1169)
out.a   = VBufferA.a   (σt, без pre-exposure)
```

- **Единицы rgb:** яркость рассеянного источника (люминанс/освещённость, как у света на поверхности) × σs [1/см] × PreExposure. Т.е. это **source term j(x) = σs·L_in·p**, не яркость, — за это отвечает деление на σt в интегрировании.
- Emissive прибавляется **без умножения на σs** (1169) — «плотность излучения» в тех же единицах, что и j.
- `MakePositiveFinite` перед записью (1197).

### 2.4 История и temporal blend

**VERIFIED**:
- В историю уходит **`LightScattering`** (до интегрирования), `QueueTextureExtraction` (2038); сохраняется `PreExposure` кадра (2039).
- Формула: `out = lerp(new, history·(PrevInvPreExposure·PreExposure), HistoryAlpha)` (1177–1179). Extinction в `.a` тоже блендится (lerp по всему float4) — «Leave extinction untouched» в комментарии относится лишь к масштабу экспозиции.
- `HistoryAlpha = HistoryWeight` = `r.VolumetricFog.HistoryWeight` = **0.9** (150–156; 327), т.е. новый кадр входит с весом **0.1**. **DERIVED:** для режима ошибки с собственным значением k и весом нового кадра β это даёт множитель `1 − β + β·k` (`FogMS_HANDOVER.md` §4).
- `HistoryWeight = 0`, если `!bTemporalHistoryIsValid` (327): требуется `TemporalReprojection && ViewState && !bCameraCut && !bPrevTransformsReset && bRealtimeUpdate && LightScatteringHistory` (1617–1626).
- Per-voxel сброс: `HistoryAlpha = 0`, если HistoryUV вне [0, PrevUVMax) или `FixupHistoryUV` не нашёл валидных соседей по prev conservative depth (894–903, 777–842). При сбросе — суперсэмплинг `HistoryMissSupersampleCount` (4 → округляется до 1/4/8/16, 289–305).
- Джиттер: Halton(2,3,5) по номеру кадра (270–281) + опционально `LightScatteringSampleJitterMultiplier` (по умолчанию 0).
- Репроекция: `ComputeHistoryVolumeUVFromTranslatedPos(..., UnjitteredPrevTranslatedWorldToClip)` — по мировой позиции центра ячейки, трилинейный сэмпл (856, 1177).
- **VERIFIED — уточнение ConservativeDepth:** `MaterialSetupCS` вычисляет плотность и пишет `VBufferA` по проверке границ ресурса (`VolumetricFog.usf:153–180, 287–297`), в том числе за conservative depth; `ApplyDepthConstraintsToOffset` корректирует позицию выборки, а не удаляет все такие ячейки. Именно `LightScatteringCS` при тесте за глубиной пишет `float4(0,0,0,0)` и выходит (`VolumetricFog.usf:860–878`). Поэтому в извлекаемой истории такие ячейки имеют нулевые rgb **и α**, хотя текущий `VBufferA` содержит плотность. Плотность текущего кадра и `LightScatteringHistory.a` здесь не взаимозаменяемы; полный разбор вокселизации — Q5 мини-аудита №2.

### 2.5 `IntegratedLightScattering` и его применение

**VERIFIED**:
- rgb = накопленный pre-exposed inscatter от камеры до слоя; a = накопленная transmittance (1248). Fade-in от `VolumetricFogNearFadeInDistance` (1235, 1246).
- Применение в `HeightFogCommon.ush:535–585` (`CombineVolumetricFog`): `result.rgb = VF.rgb·OneOverPreExposure + HeightFog.rgb·VF.a; result.a = VF.a·HeightFog.a` (584). Опционально трикубическая фильтрация `SampleVolumetricFogFiltered` (473+).
- Ссылка для translucency и FSSS — через `FogStruct.IntegratedLightScattering` (465, 552).

---

## 3. Тип источника × способ теней (критерий 3)

**VERIFIED** логика отбора `VolumetricFog.cpp`:

- Локальный источник идёт в **отдельную инжекцию** (`LightNeedsSeparateInjectionIntoVolumetricFogForOpaqueShadow`, 748–772), если: `r.VolumetricFog.InjectShadowedLightsSeparately` (по умолчанию 1), тип Point/Spot/Rect, не static, `CastsDynamicShadow && CastsVolumetricShadow`, и есть хоть одна тень: whole-scene shadow map (не DF), static shadow depth map, VSM id, или RT-тени (`LightHasRayTracedShadows` = `GetLightOcclusionType == Raytraced && GVolumetricFogInjectRaytracedLights`, 743–746).
- Среди отдельных: RT-источники → список `RayTracedLights` → **RGS** (1561–1571; 1039–1111); остальные → **PS-растр** (919–1036).
- Всё, что не отобрано, считается **в `LightScatteringCS` через light grid без теней** (1073–1144). При MegaLights из счётчика вычитаются «мега»-источники (1065–1071), их вклад берётся из `MegaLightsVolume` (1153–1159).
- Directional: только `SelectedForwardDirectionalLightProxy` (1576–1587); считается в `LightScatteringCS` (943–989). Тень = static shadowing × CSM (`ComputeDirectionalLightDynamicShadowing`) × VSM × `RaytracedShadowsVolume` × cloud shadow (946–976). При MegaLights, если `DirectionalLightHandledByMegaLights`, ветка directional пропускается (941).

| Тип | Тени | Проход | Источник тени | Статус |
|---|---|---|---|---|
| Directional | CSM (shadow map) | `LightScatteringCS` | `ForwardDirLightShadowStruct` → `ComputeDirectionalLightDynamicShadowing` | VERIFIED 952 |
| Directional | VSM | `LightScatteringCS` | `SampleVirtualShadowMapTranslatedWorld(ForwardLightStruct.DirectionalLightVSM)` | VERIFIED 954–960 |
| Directional | RT shadows | `InjectShadowedDirectionalLightRGS` → `RaytracedShadowsVolume`; потребление в `LightScatteringCS` | 1 луч/froxel (`TraceVisibilityRay`, TMax 1e27), джиттер по кадру | VERIFIED 599–612, 962–967. Требует `InjectRaytracedLights=1` (618–622) |
| Directional | MegaLights | `MegaLightsVolume` (см. отчёт A) | — | см. §3.1 |
| Directional | Cloud shadow | `LightScatteringCS` | `CloudShadowmapTexture` × `CloudShadowmapStrength` | VERIFIED 969–975 |
| Point/Spot/Rect | без тени | `LightScatteringCS`, light grid | нет | VERIFIED 1073–1144 |
| Point/Spot/Rect | shadow map (cube/spot) | `InjectShadowedLocalLightPS` | `ComputeVolumeShadowing` (FVolumeShadowingShaderParameters) | VERIFIED 532, 967 |
| Point/Spot/Rect | VSM | `InjectShadowedLocalLightPS` (+`VIRTUAL_SHADOW_MAP`) | `SampleVirtualShadowMapTranslatedWorld(VirtualShadowMapId)` | VERIFIED 534–540, 933–934 |
| Point/Spot/Rect | RT shadows | `InjectShadowedLocalLightRGS` | `ComputeRayTracedShadowFactor` (луч к источнику, TMax = дистанция) | VERIFIED 528–529, 586–595 |
| Point/Spot/Rect | MegaLights | `MegaLightsVolume` | — | см. §3.1 |
| Любой | static shadow depth map | отдельная инжекция / `ComputeDirectionalLightStaticShadowing` | precomputed | VERIFIED 764, 950 |
| Любой | Light function | атлас (`GetLocalLightFunctionCommon`) или 2D для directional | `LightFunctionAtlas`, `DirectionalLightLightFunctionTexture` | VERIFIED 546–552, 743–760 |

**`r.VolumetricFog.InjectRaytracedLights`:** существует, default **0** (199–205), «Whether lights with ray traced shadows are injected into volumetric fog». При 0 источник с RT-тенями **не** проходит `LightHasRayTracedShadows` → если у него нет shadow map/VSM, он попадает в light grid и инжектится **без тени** (VERIFIED: для occlusion = Raytraced shadow map/VSM не создаются, `ShadowSetup.cpp:6404–6409`; см. §3.1). **В проекте заказчика = 1** (`DefaultEngine.ini:22`), так что RT-пути активны.

RT-проходы дополнительно требуют `View.bHasRayTracingShadows && IsRayTracingAllowedForView && GRHISupportsRayTracingShaders` (618–622, 696, 751) — это **pipeline RGS**, не inline; используют `View.MaterialRayTracingData.PipelineState` и SBT (688, 1106).

### 3.1 Приоритет способа теней и защита от двойного учёта

**VERIFIED** (отчёт A, сверено мной `LightRendering.cpp:757–771`, `LightGridInjection.cpp:1380–1385`):

- `GetLightOcclusionType`: **MegaLights побеждает RT-тени**: если `GetMegaLightsMode != Disabled` → `MegaLights` / `MegaLightsVSM`; иначе `bUseRaytracing ? Raytraced : Shadowmap` (`LightRendering.cpp:757–771`). `bUseRaytracing` = `ShouldRenderRayTracingShadowsForLight` — per-light enum `CastRaytracedShadow` (Enabled / UseProjectSetting → `r.RayTracing.Shadows` / Disabled) (`LightRendering.cpp:253–272`; enum `EngineTypes.h:503–514`).
- `r.RayTracing.Shadows` default **0** (`LightRendering.cpp:83–88`); **в проекте = True**. RT-тени в 5.8 **не deprecated** — deprecирован лишь старый bool `bCastRaytracedShadow_DEPRECATED` (`LightComponentBase.h:92`), enum и `SetCastRaytracedShadows` актуальны.
- Shadow map / VSM строятся **только** для `Shadowmap` и `MegaLightsVSM` (`ShadowSetup.cpp:6404–6409`). Следствие: у источника с occlusion = `Raytraced` нет ни shadow map, ни VSM. Поэтому при `InjectRaytracedLights = 0` он попадает в light grid **без тени** (и для directional — `NumDirectionalLightCascades = 0`, `DirectionalLightVSM = INDEX_NONE` → без тени, кроме cloud/static). ASSUMED в §3 подтверждено логикой.
- **Защита от двойного учёта:** источник, отобранный в отдельную инжекцию, получает `VolumetricScatteringIntensity = 0` в light grid (`LightGridInjection.cpp:1380–1385`), так что цикл grid в `LightScatteringCS` и MegaLights-объём дают для него 0. MegaLights-источники лежат в конце списка ячейки (`LightGridCommon.ush:126`), вычитание `NumMegaLights` их отрезает.
- DF-тени (`bRayTracedDistanceField`) в туман **не попадают** (`VolumetricFog.cpp:735`). Capsule shadows — NOT FOUND.

### 3.2 MegaLights volume path

**VERIFIED** (отчёт A; сверено `MegaLightsResolve.cpp:845–849`, `MegaLights.cpp:26–31, 531–556`):

| Вопрос | Ответ |
|---|---|
| Включён ли в проекте | **DECISION 2026-09-20: заказчик MegaLights не использует**, поддержка отложена. Механизм включения VERIFIED: `r.MegaLights.EnableForProject` default **0** (`MegaLights.cpp:26–31`), `IsRequested` читает `FinalPostProcessSettings.bMegaLights` (531–538); это нужно учитывать при будущем подключении |
| Выход | `MegaLights.Volume.ResolvedLighting`, `Texture3D`, **`PF_FloatRGB`** (reference mode: `PF_A32B32G32R32F`), размер **= ResourceGrid тумана** (`MegaLightsResolve.cpp:845–849`, `MegaLights.cpp:1810`); создаётся только если `UseVolume() && bShouldRenderVolumetricFog` (823) |
| Своя сетка трассировки | при `r.MegaLights.Volume.Unified = 1`: `GridPixelSize` 8 × `GridSizeZ` 128, `DownsampleMode` 2 → трассировка на **½ разрешения по каждой оси**, стохастическая трилинейная реконструкция (`MegaLightsVolumeShading.usf:175–260`); `NumSamplesPerVoxel` 2 |
| HWRT для froxel'ов | **да**: `VolumeHardwareRayTraceLightSamples` (`MegaLightsRayTracing.cpp:1684–1733`, `MegaLightsVolumeHardwareRayTracing.usf:39–60`), fallback — Global SDF |
| Directional | только при `r.MegaLights.DirectionalLights = 1` (default 0); тогда ветка directional в `LightScatteringCS` пропускается (`VolumetricFog.usf:941`) |
| VSM-метод MegaLights | volume-трассировщик **без VSM-пути**; такие источники идут в классический `InjectShadowedLocalLightPS` + VSM (у них есть VSM id), а в MegaLights-объёме их вклад 0 |
| Что применяет | HG-фаза (`VolumePhaseG`), `VolumetricScatteringIntensity`, soft fading, light function atlas, IES, cloud shadow (`MegaLightsVolume.ush:202–235`) |
| Потребление в VF | `LightScattering += MegaLightsVolume[GridCoordinate] * OneOverPreExposure` (`VolumetricFog.usf:1157`), pre-exposed при записи (`MegaLightsVolumeShading.usf:404`) |
| Ловушка | MegaLights включён, но `r.MegaLights.Volume = 0` → текстуры нет → `bUseMegaLights = false` → цикл grid берёт **все** источники, включая MegaLights-овые, **без тени** |

### 3.3 Light functions

- Directional: отдельная текстура `VolumetricFog.LightFunction` **`PF_G8`** (`VolumetricFogLightFunction.cpp:204–207`), material shader `FVolumetricFogLightFunctionPS` (108), `r.VolumetricFog.LightFunction.DirectionalLightSupersampleScale` = 2.0; либо атлас, если у источника валидный слот (`VolumetricFog.cpp:1915–1924`).
- Локальные: атлас в обеих инжекциях и в grid-цикле (`USE_LIGHT_FUNCTION_ATLAS`); `r.LightFunctionAtlas` = 1, `r.VolumetricFog.UsesLightFunctionAtlas` = 1 (alias `r.VolumetricFog.LightFunction`, `LightFunctionAtlas.cpp:89–101`).
- Мёртвый код: `extern int GVolumetricFogLightFunction` в `VolumetricFogLightFunction.cpp:27–31` — переменной в движке нет.

### 3.4 Итог для проекта заказчика (HWRT-тени, MegaLights не используется)

1. Локальные Point/Spot/Rect с `Cast Ray Traced Shadows` (или UseProjectSetting при `r.RayTracing.Shadows=1`) → **`InjectShadowedLocalLightRGS`**: один жёсткий луч видимости на froxel к источнику, без мягкости/площади. Итог в `LocalShadowedLightScattering`.
2. Directional с RT-тенями → `InjectShadowedDirectionalLightRGS` → `RaytracedShadowVolume` (R16F, 1 луч, джиттер по кадру) × cloud shadow × static.
3. Если появится MegaLights → таблица меняется: локальные уходят в `MegaLightsVolume` (½ разрешения трассировки, свой шумодав), directional — по-прежнему в `LightScatteringCS`, если `r.MegaLights.DirectionalLights = 0`.
4. Источники с `CastVolumetricShadow = false` — без тени в любом пути.

---

## 4. Lumen / Sky / Emissive → туман (критерий 4)

Собственное чтение `VolumetricFog.usf` + отчёт исследователя C (ключевые строки сверены: `LumenTranslucencyVolumeLighting.usf:278–293`).

### 4.1 Что именно сэмплится

- **Только Lumen Translucency GI Volume.** Классический Translucency Lighting Volume (`TranslucentLighting.cpp`) туманом **не читается** — NOT FOUND в `VolumetricFog.cpp/.usf`. Привязка: `LumenGIVolumeStruct` = `GetLumenTranslucencyLightingParameters(View.GetLumenTranslucencyGIVolume())` (`VolumetricFog.cpp:1814–1816`); условие `Texture0 != nullptr && GetSupportsLumenGI` (1926); пермутация `LUMEN_GI` (1124).
- Сэмпл: `GetTranslucencyGIVolumeLighting(WorldPosition, WorldToClip, bTemporalFiltered = false)` → `FTwoBandSHVectorRGB`; `DotSH(SH, RotatedHGZonalHarmonic)` (`VolumetricFog.usf:1003–1011`). `false` → читаются `TranslucencyGIVolume0/1`, не History (`Lumen/LumenTranslucencyVolumeShared.ush:45–54`).
- **Масштаб не применяется:** ни intensity-cvar Lumen для тумана (NOT FOUND в `Lumen/*.cpp`), ни `SkyLightVolumetricScatteringIntensity` (ветка sky отключена при `bLumenGI`, 1013–1015). Множитель ×4 `TranslucencyVolumeIntensityScale` для translucency применяется **после** записи fog-выхода (`LumenTranslucencyVolumeLighting.usf:278–293`) — туман его не получает.

### 4.2 Представление и построение объёма Lumen

- **SH2 (L0+L1), RGB ambient + 3 монохромных направленных коэффициента**, две 3D-текстуры: `Lumen.TranslucencyVolume.SHLighting0` (`r.Lumen.LightingDataFormat` → `PF_FloatR11G11B10`) и `SHLighting1` (`PF_FloatRGBA`) (`LumenTranslucencyVolumeLighting.cpp:914–918`; запись `.usf:280–287` — направленные коэффициенты проецируются на luminance-веса ambient, реколоризация при чтении `LumenTranslucencyVolumeShared.ush:56–67`).
- **View-aligned froxel-сетка** (не world, без clipmap): XY = ViewRect / `r.Lumen.TranslucencyVolume.GridPixelSize` (**32**), Z логарифмическая до `EndDistanceFromCamera` (**8000** см × `LumenSceneViewDistance/20000`) (`.cpp:592–608, 263–269`). **Грубее сетки тумана (16 px)** — GI в тумане билинейно интерполируется из более редкой сетки.
- Источник радианса: cone-трассировка из froxel (`TraceFromVolume` = 1) по Global SDF **или HWRT** (`r.Lumen.TranslucencyVolume.HardwareRayTracing` = 1, требует `Lumen::UseHardwareRayTracing`; `.cpp:818–830`, `LumenTranslucencyVolumeHardwareRayTracing.cpp:23–44`) + Lumen Radiance Cache для дальнего сегмента (`RadianceCache` = 1). 9 лучей/froxel (`TracingOctahedronResolution` = 3), пространственный фильтр 3 прохода, **свой temporal blend с весом 0.9** (`Temporal.HistoryWeight`, `.cpp:115–120`), Halton-джиттер.
- **Sky light внутри:** `ApplySkylightToTraceResult` для промахов (`.usf:149`, `LumenTracingCommon.ush:50–61`) + skylight leaking (`.usf:153`). Sky приходит **затенённым геометрией** через трассировку — это и есть «Lumen Dynamic GI + shadowed Skylight».
- **Emissive поверхностей:** через Lumen surface cache FinalLighting = `CombineFinalLighting(Albedo, Emissive, Direct, Indirect)` (`LumenSceneLighting.usf:501–505, 699–724`). **Иного пути emissive поверхностей в туман нет** (VERIFIED по перечню входов `LightScatteringCS`).
- **HZB-отсечение:** froxel'ы за HZB получают **ноль** GI (`LumenTranslucencyVolumeLightingShared.ush:120–138`) — в этих ячейках indirect отсутствует. В тестовой сцене с перегородкой и обходом (`FogMS_HANDOVER.md` §9) это источник артефакта, не связанный с MS.

### 4.3 Sky без Lumen и static lighting

- Sky: `View.SkyLightColor · GetSkySHDiffuseSimple(CameraVector·−g)` × `SkyLightVolumetricScatteringIntensity` × `ComputeSkyVisibility` (1017–1040). SH — из view-uniform `SkyIrradianceEnvironmentMap` (`ReflectionEnvironmentShared.ush:110–121`). Видимость: DF-конус 45° вверх (только при `!bUseLumenGI`, `SkyLight.bCastShadows && bCastVolumetricShadow`, 1930–1940) или длина bent normal VLM.
- VLM: `GetVolumetricLightmapSH2 · StaticLightingScatteringIntensity/π` (1044–1052) — **не отключается при Lumen** (возможен двойной учёт, если оба настроены).

### 4.4 Замечания Epic в коде (влияют на A2/B)

- Знак g для SH-свёртки: `VolumetricFog.usf:992` («PhaseG here should be negated»), `:1034` («should be CameraVector * PhaseG») — две разные конвенции в одном шейдере.

## 5. Local Fog Volumes и volume-материалы (вопрос E)

Собственное чтение + отчёт C (сверено: `VolumetricFogVoxelization.usf:326–345`, `.cpp:427–430`).

### 5.1 Два пути LFV

| Путь | Где | Что считает | Выбор |
|---|---|---|---|
| **Вокселизация в сетку VF** | внутри `MaterialSetupCS`, `VolumetricFog.usf:184–285` | только плотность в центре froxel: radial + height extinction, комбинация −log(A−AB+B) (264–266), `/METER_TO_CENTIMETER` (270), clamp `r.LocalFogVolume.MaxDensityIntoVolumetricFog` = **0.01** (272; `LocalFogVolumeRendering.cpp:35–38`), soft-fade по StartDistance (211–226). Пишет σs, σt, emissive | `r.LocalFogVolume.RenderIntoVolumetricFog` = 1 → `ShouldRenderLocalFogVolumeInVolumetricFog` (1751) |
| **Аналитический** | тайловый splat `LocalFogVolumes/LocalFogVolumeSplat.usf` (или внутри height-fog PS: `r.LocalFogVolume.RenderDuringHeightFogPass` = 0 по умолчанию; при FSSS принудительно в height-fog pass, `DeferredShadingRenderer.cpp:2277`) | замкнутая форма оптической толщины (`LocalFogVolumeCommon.ush:168–293`), освещение: directional с **per-volume** `HenyeyGreensteinPhase(−FogInstance.PhaseG, …)` (307), sky SH без окклюзии (310–316), × Albedo + Emissive | начинается **за** `VolumetricFogMaxDistance` (367–375), чтобы не дублировать VF |

**Per-volume PhaseG теряется при вокселизации:** авторится (`LocalFogVolumeComponent.h:35–37`, default 0.2), пакуется в UNORM8 (`LocalFogVolumeRendering.cpp:487`), читается только в аналитическом пути (`LocalFogVolumeCommon.ush:307, 312`); в `VolumetricFog.usf` `FogInstance.PhaseG` **не читается** (VERIFIED по 204–283). Внутри VF-дистанции LFV освещается глобальным g.

Clamp 0.01 1/см (= 1 1/м) на плотность LFV в сетке — жёсткий потолок «материальности» для стадии Max; причина в комментарии cvar — утечки temporal reprojection. Для FogMS это **параметр, который придётся поднимать**, с пониманием, что он защищает от лика истории.

### 5.2 Volume-domain материалы

- Проход `VoxelizeVolumePrimitives` (`VolumetricFogVoxelization.cpp:708, 755`), только `MD_Volume` (650); GS-размножение треугольника по слайсам (`VoxelizeGS`, `.usf:105–107, 165–216`), `r.VolumetricFog.VoxelizationSlicesPerGSPass` = 8; режимы sphere/box (93–94).
- Выходы материала: Extinction (`.r` subsurface / Substrate), Albedo (BaseColor), Emissive (`.usf:29–67`). **Extinction авторится в 1/м**, `UnitScale = 1/100` (333), fade к `MaxDistance` от 0.6 (330–331); `OutVBufferA = (Albedo·Ext·Scale, Ext·Scale)`, `OutVBufferB = (Emissive·Scale, 0)` (343–344).
- **Blend: additive** на оба RT, depth off (`.cpp:427–430`), load `ELoad` поверх `MaterialSetupCS`.
- Шейдер — **material shader** (mesh pass processor, 425) → правка `VolumetricFogVoxelization.usf` тянет перекомпиляцию volume-материалов (см. §9).

### 5.3 Чего нет (NOT FOUND)

- Per-froxel g или третий V-буфер: нет — `VBufferA = (σs, σt)`, `VBufferB = (emissive, 0)`.
- World-space кэш плотности вне фрустума: нет; все объёмы — froxel-сетка `ResourceGridSize`. Единственные world-aligned структуры в цепочке — Lumen Radiance Cache (радианс) и Global Distance Field (геометрия).

## 6. Самозатенение среды (вопрос F)

**VERIFIED — отсутствует.** В `LightScatteringCS` и в обеих инжекциях тень берётся только от геометрии (shadow map / VSM / RT) и от облаков; ни одна ветка не читает `VBufferA` до применения к источнику. `VBufferA` читается единожды на выходе (1161). Единственный марш по объёму — `HemisphereConeTraceAgainstGlobalDistanceField` для sky (651–707), и он маршит SDF геометрии, не extinction.

Следствие для A1: марш τ к источнику придётся писать поверх `VBufferA` (доступен в `LightScatteringCS` как SRV, 1795) — в оверлее это возможно для directional (данные и направление источника есть в `ForwardLightStruct`), но **не для локальных затенённых источников**: их инжекция идёт в отдельных PS/RGS-проходах, куда `VBufferA` **не привязан** (структура параметров 454–462; 1058–1063). Масштабирование готового `LocalShadowedLightScattering` не даёт per-light самозатенение: сумма по источникам уже сложена. Изменение C++-привязок штатных проходов движка запрещено; локальное самозатенение остаётся вне A1.

Граничное условие: `VBufferA` существует только в froxel-сетке до `VolumetricFogDistance`. Штатный композит использует аналитический height fog (`CombineVolumetricFog`, `HeightFogCommon.ush:584`); LFV также имеет отдельный аналитический путь за VF-дистанцией (§5.1). **DERIVED — модель A1:** продолжение луча учитывает только аналитический height fog; плотность LFV и volume-материалов вне фрустума для этого марша недоступна. Обрывать весь вклад τ на границе сетки нельзя: это создаёт зависимость от поворота камеры (`FogMS_Research_Note_01.md` §3.2–3.4).

---

## 7. FSSS — Fog Screen Space Scattering (критерий 5)

Отчёт исследователя B, ключевые строки сверены мной (`HeightFogPixelShader.usf:222–241`, `FogSeparateComposition.ush:28–52`, `FogScreenSpaceScattering.ush:22–60`).

**Название в коде:** «Fog Separate Composition» + «FSSS» (`FogSeparateComposition.h:6–7`). Cvars: `r.Fog.SeparateComposition` = −1 (auto: включается, если первый Exponential Height Fog имеет `bEnableFSSS`), `r.Fog.ScreenSpaceScattering` = 1, `.MaxExposedLuminance` = 10, `.TAA` = 1 (`FogSeparateComposition.cpp:14–36`, `FogRendering.cpp:65–69`). `r.FSSS.*` — NOT FOUND. Scalability: принудительно 0 на Effects 0–2 (`BaseScalability.ini:823, 852, 881`).

**Точка вставки (`DeferredShadingRenderer.cpp`):** после `ComputeVolumetricFog` (3663) и облаков, внутри лямбды `RenderLightShaftSkyFogAndCloud` (3723; вызов 3912): `RenderFogSeparateCompositionTextures` (3764) → `ComposeVolumetricRenderTargetOverScene` в ту же текстуру (3769) → `RenderFogScreenSpaceScatteringMipChain` (3776) → `UpsampleFogSeparateCompositionTextureForView` (3811). До `RenderTranslucency` (4011). Заменяет `RenderFog` (3822) в той же позиции кадра.

**Буферы** (`FogSeparateComposition.cpp:438–457`), full-res по ViewRect:

| Текстура | Формат | Каналы |
|---|---|---|
| `FogSeparateComposition` (Texture0) | `PF_FloatRGBA`, mip chain до `CeilLogTwo(min(W,H))` | rgb = **fog inscatter (height + VF + LFV [+ aerial perspective]) + доля scene color** (`HeightFogPixelShader.usf:235–237`); a = cloud transmittance |
| `FogSeparateComposition2` (Texture1) | `PF_G16R16F` | x = W (ширина PSF по Premoze/Elek, `FSSS_W`, `FogScreenSpaceScattering.ush:42–60`); y = `FinalFogTransmittanceToScene` (без облаков) |

**Ответ на вопрос «есть ли буфер только яркости тумана»:** **отдельного нет.** Texture0.rgb = `OutColor.rgb + SceneColorToScatter` (236). При `FSSSSceneColorScatteringAmountScale = 0` вклад scene color обнуляется (227, 230) и Texture0.rgb становится «только туман» — но это mip0 **экранного** 2D-буфера после интегрирования, а не объёмная величина. Для MS оно непригодно как вход; пригодно как эталон/визуализация «что FSSS размывает».

**Что делает scene color scattering** (`HeightFogPixelShader.usf:222–232`): `Amount = pow(saturate(Coverage·Albedo·Scale), Power)`, `Albedo` захардкожен = 1 (`FogScreenSpaceScattering.ush:28`), `Coverage = 1 − T_fog`. Доля `Amount·T_fog` scene color **переносится** в буфер тумана, а прозрачность для резкой сцены уменьшается на `1 − Amount` (232) — энергосохраняющий перенос. Далее размывается **весь** буфер (туман + перенесённая сцена): mip-цепочка (4-tap box, `FogSeparateComposition.usf:29–54`) + 5×5 Gaussian на каждом mip (9 билинейных тапов, 122–128, 198–226) + межуровневый lerp по `FSSSBlurControl` (169–175). Выбор mip при композите: `Mip = Coverage_mip0 · SpreadScale · clamp(log2(W/2), 1, MaxMip)` (`.ush:39`). Transmittance берётся всегда из mip0 (30).

Свойства компонента (`ExponentialHeightFogComponent.h:197–229`, defaults `.cpp:115–119`): `bEnableFSSS` = false, `FSSSSceneColorScatteringAmountScale` = 1.0, `FSSSSceneColorScatteringAmountPower` = 1.0, `FSSSSpreadScale` = 0.1, `FSSSBlurControl` = 0.5. Setter-функций Blueprint нет.

**Связь с VF:** потребляет `IntegratedLightScattering` через тот же `CombineVolumetricFog` (`HeightFogPixelShader.usf:184`) — всё, что FogMS добавит в source term VF, автоматически попадёт в FSSS-буфер. На translucency FSSS не влияет (translucency сэмплит `IntegratedLightScattering` напрямую). Ограничения: только deferred (`FogRendering.cpp:960–966`), SM5+, требуется typed UAV load для RGBA16F (`FogSeparateComposition.cpp:44–51`); мобильной реализации нет; half-res режим не реализован (`:79–83`).

**Мёртвый код / замечания:** `GetDefaultFSSSGlobalParameters` без вызовов (`:372–388`); `FogStruct.EnableFSSS / FSSSSpreadScale / FSSSBlurControl` в шейдерах не читаются; в оптимизированном пути alpha фильтруется вместе с rgb (безвредно, alpha читается только с mip0).

Оценка заказчика («bloom для тумана с ореолом») объясняется данными: FSSS — 2D-размытие экранного результата с W, посчитанным из **средней** extinction вдоль луча при albedo = 1 и g = 0 (`FogScreenSpaceScattering.ush:28–32, 45`); объёмная структура среды и направление света в ядре отсутствуют.

## 8. Привязки и расширяемость (критерий 6)

Собственное чтение + отчёт исследователя D (сверено: `SceneViewExtension.h:108–115, 205`, `FXRenderingUtils.h:86–91`, `DeferredShadingRenderer.cpp:3310–3326`, `Config/Windows/DataDrivenPlatformInfo.ini:86–89`, `SceneRendering.h:837, 853–866`).

### 8.1 Что привязано в `LightScatteringCS`

**VERIFIED** `VolumetricFog.cpp:1145–1201`: View, ForwardLightStruct (light grid + directional), ForwardDirLightShadowStruct (CSM), Fog, LightFunctionAtlas, SceneTextures, VolumetricFogParameters (UB `VolumetricFog` + матрицы + джиттер + HistoryWeight), MegaLightsVolume, VBufferA/B, LocalShadowedLightScattering, DirectionalLightLightFunctionTexture, CloudShadowmapTexture, ConservativeDepthTexture (+Prev), LightScatteringHistory, RaytracedShadowsVolume, LumenGIVolumeStruct, VirtualShadowMapSamplingParameters, AOParameters, GlobalDistanceFieldParameters, RWLightScattering. **TLAS не привязан** к `LightScatteringCS`; TLAS есть только у RGS-проходов (661, 1060) — это **pipeline RGS** (`SF_RayGen`, `View.MaterialRayTracingData.PipelineState` + SBT), не inline.

**VERIFIED:** `GlobalExtinctionScale` в этом списке отсутствует; его нет и внутри `Fog` UB (`FogRendering.h:16–44`). Он привязан к `MaterialSetupCS` (`VolumetricFog.cpp:353, 1737`), а не ко всем entry point общего `.usf`. Риск точного аналитического продолжения A1 — §2.1 и §13.1.

Пермутации: TemporalReprojection, DistanceFieldSkyOcclusion, LumenGI, VirtualShadowMap, RaytracedShadowsVolume, SampleLightFunctionAtlas, MegaLights, LightSoftFading, SuperSampling, Ubershader (1122–1143). Ubershader-режим переводит ветки в динамические `IF_UBERSHADER(b…)` (1944–1976).

### 8.2 Хуки вокруг тумана

**VERIFIED**:
- Внутри `ComputeVolumetricFog` (1497–2067) делегатов и ViewExtension-вызовов **нет** (grep `Delegate|Broadcast|ViewExtension` по файлу — 0 совпадений).
- Ближайший хук **до** тумана: `ISceneViewExtension::PostTLASBuild_RenderThread` (`SceneViewExtension.h:205`), вызывается в `DeferredShadingRenderer.cpp:3312–3324` — **до `RenderLights` (3467)**, т.е. до готовности теней и Lumen-объёма; между 3324 и 3663 хуков нет. Требует флагов `GetFlags()` = `SubscribesToPostTLASBuild | RequiresHardwareInlineRayTracing` (`SceneViewExtension.h:108–115`; второй флаг включает `bAnyInlineHardwareRayTracingPassEnabled`, 1156–1164).
- Ближайший хук **после** тумана: `IRendererModule::RegisterPostOpaqueRenderDelegate` (`RenderCore/Public/RendererInterface.h:329`) → `RenderPostOpaqueExtensions` (3924), после `RenderFog`/FSSS, до translucency. `FPostOpaqueRenderParameters` (`RendererInterface.h:85–102`) даёт scene textures, View UB, RDG builder, `const FViewInfo*` (непрозрачный для плагина) — **ничего о тумане**.
- `PrePostProcessPass_RenderThread` (4268) — слишком поздно.
- Полный список callbacks `ISceneViewExtension` (`SceneViewExtension.h:145–261`): SetupViewFamily, SetupView, SetupViewPoint, SetupViewProjectionMatrix, BeginRenderViewFamily, PostCreateSceneRenderer, PreRenderViewFamily/PreRenderView, PreInitViews, PreRenderBasePass, PostRenderBasePassDeferred/Mobile, **PostTLASBuild**, PrePostProcessPass(+Mobile), SubscribeToPostProcessingPass, PostRenderViewFamily/PostRenderView, GetPriority, IsActiveThisFrame, GetFlags. Ничего с «Fog/Volumetric/Lighting» — NOT FOUND.

### 8.3 Доступ к TLAS и inline RT из плагина

**VERIFIED — возможно без приватных заголовков:**
- `UE::FXRenderingUtils::RayTracing::GetRayTracingSceneViewRDG(const FSceneInterface&, const FSceneView&)` → `FRDGBufferSRVRef` TLAS слоя **Base** (`Renderer/Public/FXRenderingUtils.h:89`, `RENDERER_API`; реализация `FXRenderingUtils.cpp:325–334`). Слои FarField/Decals публично — NOT FOUND.
- `GetInlineRayTracingBindingDataBuffer(const FSceneInterface&)` (`:91`) — буфер биндингов для inline-трассировки.
- В шейдере: `SHADER_PARAMETER_RDG_BUFFER_SRV(RaytracingAccelerationStructure, TLAS)`; `TraceRayInline` из `RayTracing/TraceRayInline.ush` (55, 74, 201); минимальный compute-пример `RayTracing/RayTracingBarycentrics.cpp:98–145` (`CFLAG_InlineRayTracing`, `CFLAG_Wave32`, 1:1 группа↔wave — комментарий 133; `ShouldCompilePermutation`: `IsRayTracingEnabledForProject && RHISupportsRayTracing && RHISupportsInlineRayTracing`, 130). Позиции переводить через `TranslatedWorldToTLASWorld` (`RayTracingCommon.ush:434–444`).
- Платформа PCD3D_SM6: `bSupportsInlineRayTracing=true`, **`bInlineRayTracingRequiresBindless=true`** (`Config/Windows/DataDrivenPlatformInfo.ini:88–89`) — inline RT на D3D12 требует bindless-ресурсов (проверка `ShaderCore.cpp:816`).
- Общего cvar `r.RayTracing.Inline` нет (NOT FOUND); есть per-feature `r.Lumen.HardwareRayTracing.Inline`, `r.MegaLights.HardwareRayTracing.Inline`.
- Uniform buffer сцены RT (`FRayTracingSceneUniformParameters`) — NOT FOUND в 5.8.

### 8.4 Экспорт символов (что плагин может включить)

| Тип / функция | Экспорт | Заголовок | Видимость |
|---|---|---|---|
| `FViewInfo` | нет макроса на классе; RT-геттеры `RENDERER_API` (1671–1677) | `Renderer/Private/SceneRendering.h:1254` | **PRIVATE** |
| `FSceneRenderer`, `FDeferredShadingSceneRenderer`, `FScene`, `FSceneViewState` | нет | `Renderer/Private/*` | **PRIVATE** |
| `FRayTracingScene` | класс без макроса, методы `RENDERER_API` | `Renderer/Private/RayTracing/RayTracingScene.h:46` | **PRIVATE** |
| `FVolumetricFogViewResources` (`IntegratedLightScatteringTexture`) | нет | `Renderer/Private/SceneRendering.h:853–866` | **PRIVATE** |
| `FVolumetricFogGlobalData`, `SetupVolumetricFogGlobalData` | нет (`extern` без API, 837) | `SceneRendering.h:823–837` | **PRIVATE** |
| `FVolumetricFogIntegrationParameters`, `FFogUniformParameters` | нет | `Renderer/Private/VolumetricFogShared.h`, `FogRendering.h:42` | **PRIVATE** |
| `FForwardLightUniformParameters` / light grid | нет | `SceneRendering.h:728` | **PRIVATE** |
| `GetSceneTextureShaderParameters(const FSceneView&)` | `RENDERER_API` | `Renderer/Public/SceneRenderTargetParameters.h:134` | PUBLIC |
| `GetSceneUniformBufferRef(FRDGBuilder&, const FSceneView&)` | `RENDERER_API` | `Renderer/Public/SceneRendererInterface.h:70` | PUBLIC |
| `FViewUniformShaderParameters` (в т.ч. `VolumetricFogGridSize/InvGridSize/GridZParams/SVPosToVolumeUV/ScreenToResourceUV/UVMax/MaxDistance`, `SceneView.h:1073–1084`) | `ENGINE_API` | `Engine/Public/SceneView.h` | PUBLIC |
| `FGlobalShader`, `GetGlobalShaderMap`, `FRDGBuilder`, `FComputeShaderUtils::AddPass` | RenderCore public | `GlobalShader.h`, `RenderGraphBuilder.h`, `RenderGraphUtils.h:333` | PUBLIC |
| TLAS: `FXRenderingUtils.h:86–91` | `RENDERER_API` | `Renderer/Public` | PUBLIC |

Список `Renderer/Public` (37 файлов) — в отчёте D; RT-релевантные: `FXRenderingUtils.h`, `RayTracing*.h` (геометрия/инстансы/SBT layout). `FRayTracingScene` — приватный.

**Ключевое:** `VBufferA/B`, `LocalShadowedLightScattering`, `RaytracedShadowsVolume`, `LightScattering` — RDG-transient, локальные для `ComputeVolumetricFog`, **нигде не публикуются**. `IntegratedLightScattering` публикуется только в приватный `FVolumetricFogViewResources` и приватный `FFogUniformParameters`.

### 8.5 Оверлей MultiLobeSpec (механизм доставки)

**VERIFIED** в этом репозитории: `Source/MultiLobeSpec/Private/MultiLobeShaderPatcher.h:67–98` — плагин копирует весь `Engine/Shaders` в `Saved/MultiLobeSpec/<buildid>`, патчит текст, ремапит `/Engine` (`MultiLobeSpec.cpp:173`), затем `RecompileShaders Changed`. Editor-only (`README_RU.md:59`). Исходные файлы установленного Engine не меняются.

**DECISION 2026-09-20:** FogMS размещается внутри MultiLobeSpec отдельной изолированной группой с файлами `FogMS_*`, defines `FOGMS_*` и собственным выключателем, независимым от BRDF-пресетов. Причина — единый remap `/Engine`. Изменение Engine и его fork запрещены; вынос общего патчера за рамками A1.

### 8.6 Вывод: что куда умещается

| Требование | Вердикт |
|---|---|
| Марш τ_in по `VBufferA` к directional-источнику внутри `LightScatteringCS` (часть A1) | **Оверлей** — ресурс уже привязан (1795), шейдер глобальный. Обязательное продолжение τ_out требует решения контракта `GlobalExtinctionScale` (§13.1) |
| Октавы по конвенции Epic (A1b) | **Оверлей**, поверх τ к источнику; только после разбора A1, без изменения материалов |
| Интегрирование source term из истории (A2) | **DERIVED:** размерно корректный эксперимент в оверлее при g = 0, без деления на σs; фазовая ошибка, лаг, нули истории за depth и отсутствие occlusion остаются ограничениями (§12 п.3) |
| Самозатенение для локальных затенённых источников | В текущем оверлее **недоступно**: `VBufferA` не привязан к `InjectShadowedLocalLightPS/RGS` (454–462, 1058–1063). Изменять C++-контракт штатных проходов Engine запрещено |
| Debug-виды через `View.GeneralPurposeTweak` | **Оверлей**; cvar есть в non-shipping (`SceneRendering.cpp:437–454`, присвоение 2026–2039), в Shipping = 1.0 |
| Новый низкоразрешённый буфер MS с inline HWRT (этап B) | TLAS доступен публично; отдельный plugin pass возможен на `PostTLASBuild` или `PostOpaque`. Текущий `VBufferA` и вход нового ресурса в штатный `LightScatteringCS` через эти хуки не опубликованы. **B без изменения Engine — HYPOTHESIS**, пути получения данных и доставки результата проверяет мини-аудит №2 (`FogMS_Research_Note_01.md` §5, §7) |
| Вставка прохода между `LightScatteringCS` и `FinalIntegrationCS`, новый вход в `LightScatteringCS`, публикация текущего `VBufferA` | В проверенном штатном контракте нет точки расширения. Engine FORK/PATCH **запрещены**, это не разрешённая будущая задача |
| Правка `HeightFogCommon.ush` / `LocalFogVolumeCommon.ush` в оверлее | **Не входит в A1**: вызывает полную перекомпиляцию материалов (§9); эти файлы не менять |

**DERIVED — оценка сложности, не план изменения Engine:** нефазированное поле потребовало бы второй цели накопления также в `InjectShadowedLocalLightPS/RGS`, где фаза уже применяется до суммы, плюс создания ресурса и его привязок. Порядок — **десятки строк в трёх шейдерах и C++**, а не несколько строк и один include (`FogMS_Research_Note_01.md` §2.4). Такая оценка описывает недоступный в разрешённой границе renderer contract; реализация через Engine FORK/PATCH запрещена. Для B исследуется только гипотеза без изменения движка, до результатов мини-аудита №2 её осуществимость не подтверждена.

## 9. Область перекомпиляции (критерий 7)

**VERIFIED** (отчёт D, сверено по объявлениям шейдеров):

| Файл | Класс стоимости | Почему |
|---|---|---|
| `Shaders/Private/VolumetricFog.usf` | **GLOBAL-ONLY** (секунды) | 7 типов `FGlobalShader`/RGS (`VolumetricFog.cpp:375, 401, 510, 565, 607, 1327, 1351`); никем не включается |
| `Shaders/Private/Froxel/Froxel.ush`, `FroxelBuild.ush` | GLOBAL-ONLY | HZB, SLW composite, VSM page marking, MegaLights VSM marking — все глобальные |
| `Shaders/Private/VolumetricFogVoxelization.usf` | **MATERIAL (ограничено)** | `FMeshMaterialShader` + `IMPLEMENT_MATERIAL_SHADER_TYPE` (`.cpp:177, 260, 335`), но `ShouldCompilePermutation` требует `MaterialDomain == MD_Volume` (189–193, 272–278, 345–349) → перекомпилируются только volume-материалы |
| `Shaders/Private/HeightFogCommon.ush` | **MATERIAL (полная)** | включается `BasePassPixelShader.usf`, `BasePassVertexCommon.ush`, `MobileBasePassVertexShader.usf` (+ `HeightFogPixelShader.usf`, `Lumen/LumenTracingCommon.ush`, `VolumetricCloud.usf`, `ReflectionEnvironmentShaders.usf`, `RayTracing/RayTracingPrimaryRays.usf`, `HeterogeneousVolumes/…RayMarchingUtils.ush`, `HairStrands/HairStrandsComposition.usf`, `VolumetricRenderTarget.usf`, `MobileFog.usf`, `VolumetricFog.usf`) |
| `Shaders/Private/LocalFogVolumes/LocalFogVolumeCommon.ush` | **MATERIAL (полная)** | `BasePassPixelShader.usf`, `BasePassVertexShader.usf`, `MobileBasePassVertexShader.usf` и др. |
| `FogSeparateComposition.usf/.ush`, `FogScreenSpaceScattering.ush`, `HeightFogPixelShader.usf` | GLOBAL-ONLY | все FSSS-шейдеры глобальные (`FogRendering.cpp:296–330`, `FogSeparateComposition.cpp:98–191, 391–424`) |
| `Engine/Public/SceneView.h` (новый член View UB) | полная + пересборка движка | ASSUMED: смена layout View UB инвалидирует все шейдеры |

Полный список include `VolumetricFog.usf` — §8.1/§1 (строки 7–53, 372, 380). Практическая граница A1: **математика живёт в `VolumetricFog.usf` и отдельном `FogMS_*` include оверлея; материалы не менять.** Возможность формулы в шейдере не доказывает наличие всех её параметров в проходе: контракт масштаба — §13.1.

### 9.1 Эталон октав — Volumetric Cloud (вопрос J)

**VERIFIED** `Shaders/Private/VolumetricCloud.usf` (отчёт D):
- `MSCOUNT = 1 + MATERIAL_VOLUMETRIC_ADVANCED_MULTISCATTERING_OCTAVE_COUNT` (339–343; default define 0 → 282–283). Ссылка на Wrenninge в комментарии (339).
- Коэффициенты по октавам (`SetupParticipatingMediaContext`, 374–401): `σs[ms] = σs[ms−1]·MsSFactor`, `σt[ms] = σt[ms−1]·MsEFactor`, затем `MsSFactor *= MsSFactor`, `MsEFactor *= MsEFactor` (390–393) — факторы **самоквадрируются** на каждой октаве, поэтому σs[i] = σs[0]·a^(2^i − 1) (a, a³, a⁷, …). Эту конвенцию использует `FogMS_Research_Note_01.md` §4.
- Фаза по октавам (`SetupParticipatingMediaPhaseContext`, 413–430): `Phase[ms] = lerp(IsotropicPhase(), Phase[0], MsPhaseFactor)`, `MsPhaseFactor *= MsPhaseFactor` (424–428) — не `p(cⁱ·g)`, а lerp к изотропии.
- Входы из материала (746–755): `MsScattFactor = GetVolumetricAdvancedMaterialOutput3` (saturate при `bClampMultiScatteringContribution`), `MsExtinFactor = Output4`, `MsPhaseFactor = Output5`.
- Параметры на **узле материала** `UMaterialExpressionVolumetricAdvancedMaterialOutput` (`Engine/Public/Materials/MaterialExpressionVolumetricAdvancedMaterialOutput.h`): `MultiScatteringApproximationOctaveCount` = 0 (clamp 0–2; 64), `ConstMultiScatteringContribution` = 0.5 (68), `ConstMultiScatteringOcclusion` = 0.5 (72), `ConstMultiScatteringEccentricity` = 0.5 (76), `bClampMultiScatteringContribution` = true (92). На `VolumetricCloudComponent` MS-свойств нет (NOT FOUND).
- Интегрирование back-to-front по октавам: 1359 (`for (ms = MSCOUNT − 1; ms >= 0; --ms)`), transmittance к свету на октаву — `TransmittanceToLight0[ms]` (368, 383, 395).

**Для A1b:** конвенция Epic — три фактора (contribution, occlusion, eccentricity) с самоквадрированием на каждой октаве; октавы применяются к **transmittance к источнику** (τ), которого в VF нет → A1b зависит от A1 (марш τ).

## 10. Сводка проверенных фактов и границ (критерий 7)

| Вопрос | Статус | Факт |
|---|---|---|
| RT-источники в тумане | VERIFIED | `InjectRaytracedLights` default 0, **в проекте = 1**; при 1 есть RGS-проходы с TLAS для локальных и directional (§3) |
| Инжекция затенённых локальных | VERIFIED | `InjectShadowedLocalLightPS` и `InjectShadowedLocalLightRGS` для RT (§3) |
| HistoryWeight | VERIFIED | 0.9 (§2.4) |
| Фаза в сторону камеры | VERIFIED | в `LightScatteringCS` и в инжекциях, до суммирования (§2.2) |
| Sky через SH | VERIFIED | SH2 (two-band), при Lumen — внутри Lumen-объёма (§4) |
| Самозатенение среды | VERIFIED | в штатном VF отсутствует (§6) |
| Класс `VolumetricFog.usf` | VERIFIED | глобальные шейдеры (§9) |
| Хук между light scattering и интегрированием | NOT FOUND | внутри `ComputeVolumetricFog` нет делегата или ViewExtension-вызова (§8.2) |
| Доступ плагина к TLAS | VERIFIED | публичный `FXRenderingUtils`, без изменения Engine; это не даёт доступ к текущему `VBufferA` и не меняет входы штатного `LightScatteringCS` (§8.3–8.4) |
| Emissive в source term | VERIFIED / DERIVED | прибавляется без σs (`VolumetricFog.usf:1169`); инвариант ограничивает вклад **MS** при σs = 0, а не эмиссию |
| Размер сетки | VERIFIED | дефолт движка 16 px × 64 слоя; ResourceGrid ≥ ViewGrid (§1.4) |
| FSSS-свойство `FSSSSceneColorScatteringAmountScale` | VERIFIED | имя точное |
| Тени MegaLights в froxel'ах | VERIFIED / DECISION | `MegaLightsVolume` на сетке тумана, HWRT на ½ разрешения (§3.2); заказчик MegaLights не использует (2026-09-20) |
| Путь GI Lumen | VERIFIED | **Lumen Translucency GI Volume** (SH2, 32 px, HWRT-трассировка), не классический Translucency Lighting Volume (§4) |
| Статус RT Shadows | VERIFIED | RT-тени поддерживаются; MegaLights имеет приоритет в `GetLightOcclusionType` (§3.1) |
| Единицы плотности | VERIFIED | 1/см внутри сетки; UI-параметры: height fog `/1000`, LFV и volume-материалы `/100` (§2.1) |
| Отдельный FSSS-буфер только тумана | NOT FOUND | Texture0 = туман + перенесённый scene color; при Scale = 0 — только туман, но 2D после интегрирования (§7) |
| Октавы Epic | VERIFIED | факторы самоквадрируются по октавам, фаза — lerp к изотропии (`VolumetricCloud.usf:390–393, 424–428`) |
| `View.GeneralPurposeTweak` доступен в non-shipping | VERIFIED | `SceneRendering.cpp:437–454`; в Shipping/Test = 1.0 |
| Хуки вокруг fog pipeline | VERIFIED | ближайший `PostTLASBuild` — до `RenderLights`; `PostOpaque` — после тумана (§8.2) |
| Реализуемость B без изменения Engine | HYPOTHESIS | проверяется мини-аудитом №2; недоступность текущего `VBufferA` через проверенные хуки не доказывает невозможность всех альтернативных путей. Engine FORK/PATCH запрещены (§8.6) |
| Масштаб аналитической плотности в `LightScatteringCS` | VERIFIED / реализация требует UE-проверки | `GlobalExtinctionScale` привязан только к `MaterialSetupCS`, отсутствует в параметрах `LightScatteringCS` и `Fog` UB. A1 читает scale активного компонента при Apply и передаёт собственным define (§13.1) |

---

## 11. Cvars `r.VolumetricFog.*` (VERIFIED `VolumetricFog.cpp`)

| Cvar | Default | Строка | Смысл |
|---|---|---|---|
| `r.VolumetricFog` | 1 | 94 | вкл. |
| `.InjectShadowedLightsSeparately` | 1 | 102 | отдельные PS/RGS для затенённых локальных |
| `.DepthDistributionScale` | 32 | 110 | S в log-распределении слоёв |
| `.GridPixelSize` | 16 | 118 | px на froxel по XY |
| `.GridSizeZ` | 64 | 126 | слоёв |
| `.TemporalReprojection` | 1 | 134 | |
| `.Jitter` | 1 | 142 | Halton-джиттер |
| `.HistoryWeight` | 0.9 | 150 | вес истории |
| `.HistoryMissSupersampleCount` | 4 | 158 | 1/4/8/16 |
| `.InverseSquaredLightDistanceBiasScale` | 1 | 167 | антиалиасинг 1/d² |
| `.Emissive` | 1 | 175 | VBufferB |
| `.RectLightTexture` | 0 | 183 | |
| `.ConservativeDepth` | 1 | 191 | |
| `.InjectRaytracedLights` | **0** (проект: 1) | 199 | RT-инжекция |
| `.LightScatteringSampleJitterMultiplier` | 0 | 207 | доп. джиттер позиции |
| `.LightSoftFading` | 0 | 215 | мягкие края spot/rect |
| `.GridCenterOffsetFromDepthBuffer` | 0.5 | 221 | сдвиг сэмпла от depth |
| `.OffsetThresholdToAcceptDepthBufferOffset` | 1.0 | 228 | |

---

## 12. Пробелы и риски, найденные по ходу (сигнал заказчику)

1. **Инвариант MS и штатная эмиссия — разные условия.** Emissive физически корректно прибавляется без σs (`VolumetricFog.usf:1169`). **DERIVED:** при σs = 0 вклад **MS** обязан быть нулевым; самостоятельный emissive допустим. В тестовой сцене emissive = 0. В пространственном режиме эмиссия, уже содержащаяся в source term истории, должна рассеиваться наравне с другими источниками (`FogMS_Research_Note_01.md` §2.2).
2. **Потолок плотности LFV в сетке = 0.01 1/см** (`r.LocalFogVolume.MaxDensityIntoVolumetricFog`). «Плотный, материальный» локальный объём упрётся в него. Поднимать — можно, но cvar защищает от лика истории при репроекции; при поднятии нужно измерять лаг.
3. **История хранит source term q, который можно интегрировать без деления на σs. DERIVED** (`FogMS_Research_Note_01.md` §2.1): `L(x, ω) = ∫ T(x, s)·V(x, s)·q(x + s·ω) ds`, затем `q_MS(x) = σs(x)/(4π) · ∫_Ω L(x, ω) dω`. Здесь q — `LightScatteringHistory.rgb` с поправкой экспозиции как в `VolumetricFog.usf:1177–1179`, а σs(x) — **текущий** `VBufferA.rgb`. Размерности: q [яркость/см] → интеграл по ds [яркость] → интеграл по углам [яркость·ср] → σs/(4π) [1/см/ср] → q_MS [яркость/см]. A2 в оверлее при **g = 0** размерно корректен. Реальные ограничения: история уже фазирована при g ≠ 0 (§2.2), лаг `1 − β + β·k`, нули за `ConservativeDepth` (§2.4), отсутствие подтверждённого пути геометрической видимости V для gather. Устойчивость требует `ρ(K) < 1` всего дискретного оператора, а не только ограничения одного коэффициента; отдельный буфер сам по себе эти проблемы не решает.
4. **Две сетки и два temporal-фильтра с весом 0.9 в цепочке:** Lumen Translucency Volume (32 px, свой blend 0.9) → VF (16 px, blend 0.9). Любая новая итерация MS поверх этого получает лаг ≥ двух каскадов истории. Измерять временную реакцию (инвариант 6) нужно с учётом Lumen-объёма отдельно.
5. **Lumen HZB-отсечение обнуляет GI в ячейках за HZB** (`LumenTranslucencyVolumeLightingShared.ush:120–138`). В тестовой сцене с перегородкой и обходом (`FogMS_HANDOVER.md` §9) нужно зафиксировать эту тёмную зону как baseline A0 и отдельно оценивать вклад MS; улучшение не должно маскировать потерю входных данных.
6. **Локальные затенённые источники инжектятся в проходах без доступа к `VBufferA`** (структуры параметров `VolumetricFog.cpp:454–462`, RGS аналогично). Самозатенение среды для них в оверлее недостижимо; для directional — достижимо в `LightScatteringCS`.
7. **Один глобальный g** на весь froxel-объём; per-volume g LFV теряется. Если сцена заказчика использует LFV с разными g, режим «Пространственный» (g = 0) изменит и LFV-вид.
8. **Directional light в тумане только один** (`SelectedForwardDirectionalLightProxy`, 1576–1587). Вторая directional-лампа в туман не попадёт вовсе.
9. **DECISION 2026-09-20: MegaLights не используется**, поддержка отложена. При будущем включении локальные источники могут уйти в `MegaLightsVolume` со своим разрешением и шумоподавлением, а при `r.MegaLights.DirectionalLights = 1` ветка directional A1/A1b пропускается (`VolumetricFog.usf:941`). Сейчас требуется компиляционная совместимость всех пермутаций и сохранение готового вклада `MegaLightsVolume`; отдельную поддержку не реализовывать (`FogMS_Research_Note_01.md` §3.8).
10. **`r.RayTracing.Shadows=True` + `r.Shadow.Virtual.Enable=1` одновременно** в конфиге: для источника с occlusion = `Raytraced` (enum `CastRaytracedShadow` = Enabled или UseProjectSetting) VSM **не строится** (`ShadowSetup.cpp:6404–6409`) — в тумане у такого источника только RT-луч (жёсткая тень, 1 sample/froxel). Источники с `CastRaytracedShadow = Disabled` идут через VSM. Это значит, что в проекте **вид тени в тумане зависит от per-light enum**, а не от глобального cvar — учесть при A0.
11. **Inline RT на D3D12 SM6 требует bindless** (`bInlineRayTracingRequiresBindless=true`). Для этапа B это проектное требование (`r.D3D12.Bindless.*`), которое надо проверить в конфиге заказчика до проектирования C++.
12. **VERIFIED — `GlobalExtinctionScale` не доставлен в `LightScatteringCS`.** Он объявлен в `VolumetricFog.usf:135`, применяется в `MaterialSetupCS:169–178` и привязан в `VolumetricFog.cpp:353, 1737`, но отсутствует в `LightScatteringCS::FParameters:1145–1201` и `FogRendering.h:16–44`. A1 считывает scale из активного компонента при Apply и фиксирует отдельным define; изменение scale/карты требует нового Apply, разные scale активных компонентов/миров отвергаются. Код написан; его сборка/UE-приёмка и шовный тест ещё не выполнены.
13. **VERIFIED — `VBufferA.a` и `LightScatteringHistory.a` различаются за conservative depth.** `MaterialSetupCS` продолжает писать плотность (`VolumetricFog.usf:153–180, 287–297`), а `LightScatteringCS` за depth обнуляет весь float4 и выходит (`860–878`). Для A1 доступна текущая плотность; для A2/B использование history α теряет её в этих ячейках. Это ограничение данных, а не отсутствие среды в сцене; Q5 мини-аудита №2 уточняет весь путь вокселизации.

## 13. Утверждённая спецификация A1 и критерии приёмки

**DECISION 2026-09-20:** A1 принят с поправками `FogMS_Research_Note_01.md` §3, включая аналитическое продолжение за сеткой. Размещение внутри MultiLobeSpec подтверждено заказчиком. Здесь зафиксированы модель и критерии; результат реализации ещё требует компиляции и визуальной приёмки заказчиком.

**Цель A1:** самозатенение среды для **одного directional light** до штатного интегрирования VF. Extinction, локальные источники и MS не меняются. Все изменения — в файлах плагина и его shader overlay; Engine FORK/PATCH запрещены.

### 13.1 Модель и контракт данных

**DERIVED** (`FogMS_Research_Note_01.md` §3.1–3.5):

`ShadowFactor *= exp(−τ_eff)`

`τ = τ_in + τ_out`

`τ_eff = τ`, если `FOGMS_EXCLUDE_GLOBAL_LAYER = 0` (**утверждённый дефолт**).

`τ_eff = max(τ − τ_ref, 0)`, если `FOGMS_EXCLUDE_GLOBAL_LAYER = 1`.

- **τ_in:** марш в мировых сантиметрах от центра froxel к источнику по трилинейному `VBufferA.a`; источник выборки скрыт за `FOGMS_SAMPLE_EXTINCTION(uvw)`. Неравномерное квадратичное распределение шагов, выборка в интервале, вес — его мировая длина Δs. Джиттер берётся из существующего покадрового источника; N и S_march задаются defines. Ориентиры заметки: N = 8/16, 32–64 для Max; S_march порядка VF-дистанции.
- **Переход за сетку:** при выходе world → froxel UVW за границы марш передаёт точку выхода в аналитику. Если достигнут S_march внутри сетки, аналитика продолжается от последней точки. Обнуление дальнейшей τ на границе фрустума не соответствует утверждённой модели.
- **τ_out:** аналитический интеграл двух height-fog слоёв от точки продолжения до **общего конца луча S_max от исходного froxel** (define; ориентир 10–20 км), то есть длиной `S=max(S_max−S_in,0)`. Для `ρ_i(z) = D_i·2^(−F_i·(z − H_i))` вдоль `z(s) = p_z + s·L_z` интеграл длиной S равен `ρ_i(p_z)·(1 − 2^(−F_i·L_z·S))/(F_i·L_z·ln2)`; при малом показателе используется устойчивое симметричное разложение с правильным горизонтальным пределом.
- **τ_ref:** полностью аналитическая τ height fog из исходной мировой точки. Сумма τ_in + τ_out и τ_ref должны описывать один и тот же отрезок до одной конечной точки, иначе шовный тест измеряет также несовпадение длин.
- **Единицы и множители:** τ_out и τ_ref используют те же два слоя, множитель 0.5 и эффективный `GlobalExtinctionScale`, что `MaterialSetupCS` (`VolumetricFog.usf:169–178`). Поведение `PROJECT_EXPFOG_MATCHES_VFOG` нельзя подменять конвенцией экранного fog; вызов `CalculateLineIntegralShared` без проверки не доказывает совпадение. `HeightFogCommon.ush` и `LocalFogVolumeCommon.ush` не менять.

**VERIFIED — место вставки:** ветка directional в `LightScatteringCS`, `VolumetricFog.usf:943–989`; штатные геометрические и cloud-тени вычисляются в 946–976, фаза — в 988. `VBufferA` уже привязан как SRV (`VolumetricFog.cpp:1795`), итоговый source term и extinction пишутся в `VolumetricFog.usf:1169`, история смешивается в 1177–1179. Новый множитель применяется к directional до его суммирования; готовые `LocalShadowedLightScattering` и `MegaLightsVolume` не масштабируются.

**VERIFIED — пробел штатной привязки:** `GlobalExtinctionScale` есть в `MaterialSetupCS` (`VolumetricFog.usf:135, 178`; `VolumetricFog.cpp:353, 1737`), но **нет** в параметрах `LightScatteringCS` (`VolumetricFog.cpp:1145–1201`) и `Fog` UB (`FogRendering.h:16–44`). Само объявление scalar в общем `.usf` не делает его доступным другому проходу. **Реализация A1:** `FFogMSShaderPatcher::ReadConfig` читает `UExponentialHeightFogComponent::VolumetricFogExtinctionScale`, затем передаёт `FOGMS_GLOBAL_EXTINCTION_SCALE`. После изменения свойства/карты необходим Apply; неоднозначность между активными компонентами/мирами — отказ. Этот путь не изменяет Engine; сборка UE и визуальная приёмка остаются PENDING.

**Организация кода (≤6 файлов для A1):** отдельный `FogMS_*` include оверлея с чистыми функциями марша и аналитики; входы задаются явно, без зависимости от локальных переменных directional-ветки. Патчер плагина доставляет defines `FOGMS_*` и вставки по проверенным анкерам. Собственный выключатель независим от BRDF-пресетов. Debug-переключение — `View.GeneralPurposeTweak` (`SceneRendering.cpp:437–454`); конкретные команды и ожидаемый результат обязательны в протоколе для заказчика.

### 13.2 Семь критериев приёмки A1

Источник: `FogMS_Research_Note_01.md` §3.7. **Приёмка ещё не выполнена.**

1. Все новые эффекты выключены (defines включения = 0) → **рендер побитно совпадает с vanilla**. Патченный `VolumetricFog.usf` компилируется во всех пермутациях, включая **MegaLights и Ubershader**; тождество результата не подменять совпадением текста шейдера.
2. **Шовный тест:** сцена только с height fog. Debug-вид `|τ_in + τ_out − τ_ref|` ≈ 0 по всему экрану; допуск фиксируется в протоколе, ориентир — **≤5% от τ_ref**, с явной обработкой τ_ref около нуля. Это общая проверка единиц, длин шагов, репроекции и множителей.
3. Debug-виды: **Extinction** (`VBufferA.a`), **Transmittance** (`exp(−τ)`), **Seam error**. Переключение через `View.GeneralPurposeTweak`.
4. Поворот камеры на **360°** в статичной сцене с height fog: яркость тумана в фиксированной мировой точке не меняется заметно. Оценка заказчика плюс debug Transmittance; условия сцены и экспозиции фиксируются в протоколе.
5. `FOGMS_EXCLUDE_GLOBAL_LAYER = 1` в чистом height fog → **визуально vanilla**.
6. Заказчик снимает **`ProfileGPU` → `LightScattering` при N = 8/16/32**; значения и конфигурация сетки заносятся в отчёт.
7. **Локальные источники не затронуты** — это явно записано как ограничение A1.

### 13.3 Ограничения и остановка этапа

- LFV и volume-материалы **вне фрустума** не затеняют луч A1: их плотности там нет в `VBufferA`. Аналитика продолжает только height fog.
- За `ConservativeDepth` текущий `VBufferA` сохраняет плотность, но `LightScatteringCS` обнуляет освещение и α истории (§2.4); влияние вокселизации уточняет Q5 мини-аудита №2.
- Локальные источники и sky/Lumen-ambient остаются без нового самозатенения. Физичный дефолт может затемнить туман относительно поверхностей, где это ослабление солнца не применяется (`FogMS_Research_Note_01.md` §3.5).
- MegaLights сейчас не используется. При `r.MegaLights.DirectionalLights = 1` штатная directional-ветка пропускается (`VolumetricFog.usf:941`) и A1/A1b не действует на такой источник. Все пермутации должны компилироваться; вклад `MegaLightsVolume` остаётся штатным.
- MS, октавы A1b и дальнейшие режимы не входят в A1. После реализации A1, протокола проверки и мини-аудита №2 — **стоп до разбора заказчиком и исследователем** (`FogMS_HANDOVER.md` §6).
