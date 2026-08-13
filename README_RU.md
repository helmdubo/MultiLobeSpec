# MultiLobeSpec v0.14.3 — Generic VNDF micro-visibility для UE 5.7

Экспериментальный editor-only плагин для legacy deferred shading (`r.Substrate=0`). Он реализует горизонтальную смесь двух GGX-лобов, Generic VNDF-based direct micro-shadowing и RGB indirect material visibility. CARD-09 cone-aware Environment BRDF для reflection captures/skylight реализован как staging path, но выключен: overlay-only static LUT не прошёл SM6 compile-cost gate. Файлы движка не изменяются: плагин создаёт content-addressed shader overlay в `Saved/MultiLobeSpec` и remap-ит `/Engine` только после успешного transactional patch.

## Обязательный renderer contract

Для непрерывного исходного Material AO нужны все три настройки до запуска редактора:

```ini
[/Script/Engine.RendererSettings]
r.AllowStaticLighting=False
r.Substrate=False

[ConsoleVariables]
r.GBufferDiffuseSampleOcclusion=0
```

Это не опциональная рекомендация. В stock UE 5.7 `GBufferAO` может содержать sample mask, единицу или уже преобразованную specular occlusion. При нарушении контракта Generic VNDF и cone-aware IBL fail closed; плагин не меняет restart-required CVars автоматически.

Целевой production case v0.14: static meshes, legacy deferred, Default Lit, isotropic BRDF.

## Что реализовано

| Подсистема | Реализация | Статус |
|---|---|---|
| Direct dual-lobe GGX | два аналитических GGX-лоба с парной energy compensation | реализовано |
| Direct micro-shadow | Off, Reference Step, CoD:WWII, PZ Analytical, Generic VNDF LUT, experimental cone overlap | реализовано |
| Generic VNDF LUT | offline 8192-sample QMC bake; V2 two-bank RGBA8; runtime без Monte Carlo | canonical receipt и SHA-256 binding проверяются fail-closed |
| Raw MaterialAO transport | BasePass сохраняет исходный AO до `AOMultiBounce` | только при обязательном renderer contract |
| Indirect diffuse | Direct Only, Raw Scalar AO, RGB Interreflection | реализовано для Lumen/non-Lumen/skylight diffuse |
| Captures/skylight IBL | отдельные direction, radiance и response для R1/R2 | реализовано для legacy Default Lit isotropic |
| Cone-aware EnvBRDF | 32×32×8 `NoV × Roughness × AdjustedCosCone`, RG16_UNORM | staging implementation; выключено и fail-closed из-за static-storage compile gate |
| Lumen/SSR indirect specular | lobe identity не переносится через pipeline | per-lobe cone pairing не поддержан |
| Rect LTC | специализированная dual-lobe аппроксимация | Generic VNDF LUT не применяется |
| Anisotropy/Clear Coat | stock fallback | Generic VNDF/cone IBL не заявлены |
| Cook/package | overlay активен только в editor | не поддержан |

## Offline и runtime

Direct Generic VNDF и cone EnvBRDF генерируются только явными editor-командами. Runtime не выполняет Monte Carlo/VNDF sampling: он делает early-outs, unpack и ручную трилинейную интерполяцию маленьких packed LUT.

```text
MLS.RegenerateVNDFLUT
MLS.ValidateVNDFLUT
MLS.ExportVNDFValidation
MLS.RegenerateConeEnvBRDF
```

Артефакты находятся в `Resources/Generated` и входят своими digest в build ID overlay. Готовая таблица не пересоздаётся при каждом запуске.

## Установка и применение

1. Скопировать плагин в `<Project>/Plugins/MultiLobeSpec`.
2. Добавить renderer contract выше и перезапустить редактор.
3. Собрать `UnrealEditor Win64 Development` для проекта.
4. В `Project Settings → Plugins → MultiLobe Specular` выбрать preset и режимы.
5. Применить `MLS.Apply` или кнопкой `MLS Apply` на toolbar.

Полезные команды:

```text
MLS.Apply
MLS.Disable
MLS.Preset 0|1|2|3
MLS.Tonemap 0|1|2|3|4
MLS.MicroShadow 0|1|2|3|4|5
MLS.IndirectVisibility 0|1|2
MLS.DebugView 0|1|2|3|4|5
MLS.Capabilities
```

Каждая конфигурация получает отдельный immutable overlay. Stamp записывается последним; exact-count mismatch оставляет предыдущий overlay активным. Первая компиляция дорогая, дальнейшие сборки переиспользуют DDC.

### Быстрый Debug View без engine patch

Плагин резервирует биты 28..30 стандартного `View.PostVolumeUserFlags` и публикует
selector через собственный scene view extension. Остальные user flags сохраняются;
layout View uniform и Engine binaries не меняются. Обычные
scene/reflection/planar/VT/custom captures получают mode 0; realtime skylight capture
дополнительно закрыт через `RenderingReflectionCaptureMask` в HLSL.

После однократного `MLS.Apply` для версии P35 команда `MLS.DebugView 0..5` меняет
только атомарный runtime selector, перерисовывает viewport и не вызывает новый overlay,
remap, shader-cache flush или `RecompileShaders Changed`. То же поведение действует для
поля Debug View в Project Settings.

## Семантика material visibility

Material AO трактуется как cosine-weighted micro-visibility, а не как screen-space geometry AO. Для direct lighting значение применяется к полным diffuse/specular contributions каждого лоба. Для indirect diffuse default `RGB Interreflection` использует:

```hlsl
V_rgb = saturate(V / max(1 - Albedo * (1 - V), 1e-4));
```

Screen AO, DFAO и geometric occlusion остаются в своих stock путях. Для capture/skylight specular material visibility сначала переводится в cone, затем корректируется отдельно по roughness каждого лоба.

Raw transport запрашивается для любого активного MLS BRDF overlay: это позволяет
включить Debug View 3 позднее без перестройки overlay. Capability не может объявить
material-visibility пути доступными, если исходный `MaterialAO` не сохранён в BasePass.

## Диагностика Generic VNDF на реальном материале

Сначала отделите direct VNDF от RGB indirect — иначе albedo-aware непрямой свет может
визуально маскировать подавленный direct term:

```text
MLS.MicroShadow 4
MLS.IndirectVisibility 0
MLS.DebugView 3   # lighting-weighted raw Material Visibility
MLS.DebugView 4   # lighting-weighted итоговый direct diffuse micro-shadow multiplier
MLS.DebugView 5   # lighting-weighted per-light N dot L
MLS.DebugView 0   # обычный шейдинг
```

Режимы 1..5 внедрены в per-light `DefaultLitBxDF`, поэтому это быстрые
lighting-weighted diagnostic masks, а не абсолютный полноэкранный grayscale buffer:
результат зависит от цвета, интенсивности и shadow источников и затем проходит
exposure/tonemapping. В одном кадре складываются вклады всех direct lights; mode 5 не
выбирает один light. Для абсолютных scalar views нужен отдельный debug/postprocess pass.
Внутри target Default Lit hook неподдерживаемые Rect/anisotropic/back-facing случаи
дают чёрный; остальные shading models не входят в заявленный diagnostic scope.

Для чистого direct-сравнения временно выключите GI/skylight/reflections viewport show
flags либо используйте `ShowFlag.GlobalIllumination 0`,
`ShowFlag.ReflectionEnvironment 0`, `ShowFlag.SkyLighting 0`. После диагностики верните
их в `1` и только затем сравните `MLS.IndirectVisibility 0` с `2`.

Generic VNDF ожидает именно cosine-weighted P2 surface visibility, а не произвольную
conventional AO map. На гладкой границе direct проходит только при
`NoL > sqrt(1 - Visibility)`: например, при `NoL=0.2` требуется
`Visibility > 0.96`. Поэтому рисунок «видны только самые белые texels» при скользящем
свете сначала проверяется через debug views 3/4/5, а не исправляется инверсией LUT.

Текущая 4D таблица канонизирована при relative azimuth `phi=0`. Полный azimuth sweep
измеряется, но пока не admission-gated; capability честно называет путь
`Isotropic4D Phi0 approximation`.

## Честные ограничения CARD-09

- Single-scatter response совпадает с реализованным UE 5.7-style estimator в узлах таблицы; coarse 32×32×8 interpolation имеет длинный error tail у high-gloss/grazing boundary.
- Спека v0.14 не задаёт численные admission thresholds для CARD-09, поэтому capability оставляет `IndirectSpecular_ConeEnvBRDF_NumericallyAdmitted=false` до отдельной ратификации.
- При `USE_ENERGY_CONSERVATION` применяется документированная аппроксимация: cone/open single-scatter ratio масштабирует `EnergyTerms.E`. Это не exact cone-integrated multi-scatter.
- Lumen/SSR сохраняют stock authored-roughness response, потому что их radiance не имеет надёжной per-lobe identity.
- Static LUT проверен в scalar, `uint4` и eight-plane layout: все варианты провалили bounded UE 5.7 SM6 compile-cost gate (многогигабайтный compiler footprint и отсутствие первой завершённой permutation более 10 минут даже с одним worker). Опция выключена по умолчанию и явное включение fail-closed.
- Production-активация требует Texture3D/SRV и renderer binding. Это изменение C++ renderer contract/engine fork, а не допустимый overlay-only fallback.

Подробности: `MICROVISIBILITY_ARCHITECTURE_RU.md`, `SUPPORT_MATRIX_RU.md`, `VALIDATION_RU.md`.
