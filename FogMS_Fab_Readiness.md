# FogMS — готовность к FAB: зависимости от приватного рендерера и структура плагина

Дата: 2026-09-23. Источник: read-only аудит (worker Opus 5.5) кода плагина и UE 5.8.2; ссылки VERIFIED файл:строка, кроме
помеченных ASSUMED. Контекст: с раунда 12 Box в режиме Emissive Injection работает без `-BindlessAll`
(`FogMS_Prod_Report.md`), поэтому продуктовый путь — «решатель → инъекция → Volume-материал → штатный туман».
Overlay-функции остаются «Advanced, editor + -BindlessAll» и здесь не рассматриваются.

> **Статус 2026-09-23 (вечер), раунды 18–20.** §0 п. 1–2 закрыты: модули `Runtime` + `PlatformAllowList: Win64`, editor-only
> код под `WITH_EDITOR`, `EngineVersion`, новое описание; `BuildPlugin` собирает `UnrealEditor` и `UnrealGame` (Development,
> Shipping); smoke-тест `-game` пройден (`FogMS_Prod_Report.md`, «Раунды 19–20»). §0 п. 3 (гейт 5.8.2 источника Lumen) —
> открыт. §3 п. 1 (P2/P3/P4/P6/P7/P10) — сделано; п. 2 (P5) — сделано: `r.FogMS.Transport.PublicHitFlags` = 1 по умолчанию, диагностика раунда 21
> показала тождественность публичного и приватного буферов (расхождение 1,3 % было шумом замера при Real Time Capture); п. 4 (атлас без нативного
> D3D12-ресурса) — сделано для конфигурации без `-BindlessAll`; п. 3 (сбор источников на game thread, P11) — не начат.
> Блокеры упакованной игры сверх §0: атлас из GPU-текстуры (сделано, не проверено в куке), обязательные cvar
> (`Apply Required Render Settings`, проверено в `-game`). Остаются приватными: P8 Lumen, P9 небо, P11 источники, SSFS, Spatial.

## 0. Три структурных блокера (важнее приватных заголовков)

1. **Все модули плагина — `"Type": "Editor"`** (`MultiLobeSpec.uplugin:13-26`). В упакованной игре ничего не загрузится:
   ни runtime Box, ни материал-хуки. Нужны `Runtime`-модули (MultiLobeSpec, FogMSRender) с вынесенным editor-only кодом.
2. **Нет `PlatformAllowList`.** Для FAB достаточно объявить `"PlatformAllowList": ["Win64"]` на модуль (UE5-имя бывшего
   `WhitelistPlatforms`). Код уже обёрнут в `#if PLATFORM_WINDOWS` с `#else`-ошибками (`DensityAtlas.cpp:6,253,300`;
   `WorldLighting.cpp:125,155,191`; `BoxRuntime.cpp:22,1189`), Vulkan исключён проверками `GetName()=="D3D12"`; сборка
   под другие платформы «с выключенными функциями» — ASSUMED, не собиралась. `EngineVersion` в .uplugin отсутствует,
   FriendlyName/Description (`:5-6`) всё ещё говорят «UE 5.7 legacy overlay».
3. **Жёсткая привязка к 5.8.2**: `ENGINE_PATCH_VERSION == 2` в `LumenSource.cpp:22` и `RHICompatibility.cpp:20,35,105`.
   Отказ источника Lumen валит весь продюсер (`WorldLighting.cpp:370-371`) — хотфикс 5.8.3 выключит инъекцию целиком.
   Источник Lumen должен стать необязательным (откат — см. §3, P8).

## 1. Приватные зависимости на пути инъекции

| # | Где | Приватный элемент | Что даёт |
|---|---|---|---|
| P1 | `FogMS_Transport.cpp:10,16`; `FogMS_WorldLighting.cpp:22-23,29`; `FogMS_WorldSources.cpp:4,12,14`; `FogMS_LumenSource.cpp:5,10` | include `SceneRendering.h`, `SceneViewState.h`, `RayTracing/RayTracingScene.h`, `LightSceneInfo.h`, `ScenePrivate.h`, `Lumen/LumenSceneData.h` | типы ниже |
| P2 | `WorldLighting.cpp:350`; сигнатуры с `FViewInfo` в `Transport.h:14`, `WorldSources.h:46`, `LumenSource.h:52` | `static_cast<const FViewInfo&>` | доступ к членам вида |
| P3 | `Transport.cpp:218`, `WorldLighting.cpp:385` | `GetRayTracingSceneLayerViewChecked(Base)` | TLAS сцены |
| P4 | `Transport.cpp:219`, `WorldLighting.cpp:357,386` | `GetInlineRayTracingBindingDataBuffer()` | метаданные треугольников (`Transport.usf:130`) |
| P5 | `Transport.cpp:203,220`, `WorldLighting.cpp:351` | `LumenHardwareRayTracingHitDataBuffer` | бит 29 CastShadow на сегмент (`Transport.usf:111-112`) |
| P6 | `WorldLighting.cpp:356` | `HasRayTracingScene()`, `ViewState` | проверки готовности |
| P7 | `WorldLighting.cpp:372,518` | `ViewState->GetViewKey()`, `GetFrameIndex()` | ключ кэша вида, индекс кадра |
| P8 | `LumenSource.cpp:39-161` | `View.ViewLumenSceneData` (атласы, card/page буферы) | радианс surface cache в точках попадания |
| P9 | `WorldSources.cpp:52-119,141` | `FScene::SkyLight` proxy, `ConvolvedSkyRenderTarget[ReadyIndex]` | кубмапа неба для секторной префильтрации |
| P10 | `WorldSources.cpp:54,135` | `CachedViewUniformShaderParameters->SkyLightColor` | масштаб цвета неба |
| P11 | `WorldSources.cpp:178-183,224` | `Scene.AtmosphereLights[0]`, `Scene.Lights`, `FLightSceneInfo`, `Scene.VolumetricCloud` | список источников, «кто солнце», отсев облачной тени |
| P12 | `Shaders/Private/FogMS_LumenSource.ush:36`; страйды `LumenSource.cpp:101-106` | приватный шейдер `LumenSurfaceCacheSampling.ush`, раскладка card-буферов | привязка к версии движка на стороне шейдера |

`GetSceneUniformBufferRef` уже публичный (`Renderer/Public/SceneRendererInterface.h:70`). SSFS-постфильтр
(`FogMS_ScreenScattering.cpp:92,120,122,148`, заголовок `PostProcess/PostProcessInputs.h`) публичной замены не имеет
(`FPostProcessingInputs` не содержит текстуру тумана, `PostProcessInputs.h:8-27`) — оставить как Advanced или вынести из ядра.

## 2. Публичные замены в UE 5.8

| # | Публичная замена | Точное соответствие? |
|---|---|---|
| P2/P6/P7 | `FSceneView::State` (`SceneView.h:1486`), `GetViewKey()` (`:2066`), `ViewUniformBuffer` (`:1489`); индекс кадра — `FSceneViewFamily::FrameNumber` (`:2439`) | да; `GetFrameIndex` приватен, но нужен только как признак смены кадра |
| P3 | `UE::FXRenderingUtils::RayTracing::GetRayTracingSceneViewRDG(Scene, View)` (`FXRenderingUtils.h:89`; реализация `FXRenderingUtils.cpp:325-331` возвращает слой Base) | да |
| P6 | `FXRenderingUtils::RayTracing::HasRayTracingScene(Scene)` (`:86`) + проверка TLAS на null | эквивалентно с проверкой (ASSUMED) |
| P4 | `virtual FSceneView::GetInlineRayTracingBindingDataBuffer()` (`SceneView.h:2203`, переопределён в `SceneRendering.h:1685`) | да, без cast |
| P5 | собрать самим из `GetVisibleRayTracingShaderBindings(View)` (`FXRenderingUtils.h:112`), `GetRayTracingMeshCommands(Scene)` (`:113`), `FRayTracingShaderBindingData::SBTRecordIndex` (`RayTracingMeshDrawCommands.h:228-234`), `FRayTracingMeshCommand::bCastRayTracedShadows` (`:79`); движок строит так же (`LumenHardwareRayTracingMaterials.cpp:135,178`) | да; число сегментов приватно (`:145`) — брать max index + 1 и bounds-check в шейдере |
| P5 (откат) | трассировать с `RAY_TRACING_MASK_OPAQUE_SHADOW` + `FORCE_OPAQUE`, игнорируя hit data | маска инстанса ставится, если **любой** сегмент отбрасывает тень (`RayTracingInstanceMask.cpp:226-253,122`) → лишняя тень только у инстансов со смешанными слотами |
| P8 | **нет**: в `Renderer/Public` нет Lumen-заголовков; `FLumenCardScene` — неэкспортируемый глобальный UB (`LumenSceneData.h:45`, `LumenSceneRendering.cpp:2282`), не входит в scene UB; `ViewLumenSceneData` приватен (`SceneRendering.h:1664`) | нет |
| P9 | `FSceneInterface` не отдаёт sky light (`SceneInterface.h:233-234` — только Set/Disable); `FSkyLightSceneProxy` публичен (`SkyLightSceneProxy.h:16-60`), но достижим только через `FScene`; `USkyLightComponent::GetProcessedSkyTexture()` публичен (`SkyLightComponent.h:308`), `SceneProxy` и blend-destination защищены (`:338,350`); real-time capture — приватно (`ScenePrivate.h:1840-1843`) | только статическая захваченная кубмапа (mip 0 + префильтр); без blend и real-time capture |
| P9 (альт.) | View UB `SkyIrradianceEnvironmentMap` (`SceneView.h:1214`), `SkyViewLutTexture` (`:1218`) | SH L2 с уже свёрнутым косинусом (`ReflectionEnvironmentShared.ush:105-108`) — не радианс секторного разрешения; SkyView LUT — только атмосфера (без HDRI/облаков) |
| P10 | `View.SkyLightColor` в View UB (`SceneView.h:1041`), уже привязан (`Transport.cpp:60`) | да, умножение перенести в шейдер |
| P11 | `ULightComponent::SceneProxy` публичен (`LightComponent.h:379`), API `FLightSceneProxy` публичен (`LightSceneProxy.h:87,204,229,301`); направление на солнце уже едет в `Request.DirectionToSun`; облака — `FSceneInterface::GetVolumetricCloudSceneInfo()` (`SceneInterface.h:513`) | да, через сбор на game thread (значения, не указатели — lifetime, ASSUMED) |

## 3. Ранжирование

**Ограниченные задачи (≤6 файлов каждая):**
1. P2/P3/P4/P6/P7/P10 — сигнатуры `FViewInfo` → `FSceneView`, FXRenderingUtils. Файлы: `Transport.cpp/.h`, `WorldLighting.cpp`, `WorldSources.cpp/.h`, `FogMS_WorldSources.ush`.
2. P5 — собственный буфер флагов CastShadow из публичных binding’ов + bounds-check в `Transport.usf` (CPU-стоимость обхода — не измерена; движок сам планирует перенести это в `FRayTracingScene`, `LumenHardwareRayTracingMaterials.cpp:140`).
3. P11 — сбор источников на game thread (`BoxRuntime.cpp`, `WorldSources.cpp/.h`, `FogMS_WorldLighting.h`).
4. Атлас плотности без нативного D3D12-ресурса для injection-only (`FogMS_DensityAtlas.cpp:253-302`; нативная аллокация нужна только скрытым bindless-читателям, `:265-266,284-285`) — один файл (ASSUMED).
5. `.uplugin`: модули `Runtime` + вынос editor-only кода, `PlatformAllowList`, `EngineVersion`, описание.
6. Снять гейт 5.8.2: источник Lumen — необязательный, с откатом из п. P8.

**Без публичной замены в 5.8 (изменение движка или урезание функции):**
- **P8 радианс Lumen в точках попадания.** Минимальный откат: при попадании в `BoundaryRadiance` (`Transport.usf:133-134`)
  вернуть `albedo_param × (солнце × DirectShadow(hit→sun) + SH-иррадианс неба(N))` или чёрное (попадание = окклюдер).
  Цена: поверхностный GI теряет текстурное альбедо, стены, освещённые локальными источниками, эмиссив и многократный отскок;
  Box у освещённой геометрии теряет цветной отскок; для открытого воздуха под небом/солнцем разница мала (ASSUMED, не измерено).
  Снимает и P12, и гейт 5.8.2.
- **P9 кубмапа неба.** Минимальный откат: в `FogMS_WorldSky` заменить секторный mip-lookup (`WorldSources.ush:125-150`) на SH
  по ординате. Цена: SH L2 гораздо размытее секторного футпринта (полуугол сектора ≈29° при 16 ординатах, ≈12° при 96);
  ореол солнца и структура горизонта размажутся; 3°-исключение солнечного диска (`:103-120`) к SH неприменимо — энергию
  диска придётся вычитать аналитически, иначе двойной счёт с прямым солнцем (ASSUMED). Гибрид: публичный
  `GetProcessedSkyTexture` для статического неба (полное качество) и SH только для real-time capture / blend.
  Ошибка относительно эталона 96/64 НЕ ИЗМЕРЕНА — нужен тот же A/B, что в отчёте (`tierab.sh`).
- **SSFS** — вне ядра или Advanced.

## 4. Предлагаемый порядок (решение за владельцем)

1. §3 п. 1, 2, 4 (публичный API там, где он есть) — не меняет картинку, снимает большую часть `Renderer/Private`.
2. §0 п. 1–2 (`Runtime`-модули, allow-list) — обязательное условие упакованной игры; проверить PIE-процесс и Package.
3. §0 п. 3 + P8-откат как опция `Lumen Bounce: Auto/Off` — плагин переживает хотфиксы движка.
4. Решить по P9: гибрид (статическая кубмапа публично, SH для динамики) с A/B против эталона.

Что остаётся приватным осознанно (если владелец выберет качество): один файл источника Lumen и один файл динамического неба,
оба помечены как «Advanced, точная версия движка».
