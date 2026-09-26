# FogMS — A1c: исследование ослабления входящего непрямого света

2026-09-20. Заказчик разрешил исследовательский шаг перед следующим кодовым срезом. Движок UE 5.8.2 CL 56702186 читался, не изменялся. Установленный A1b и его шейдеры не менялись; новых уровней нет.

## Решение

Следующий эксперимент — **ослабление вдоль входящих лучей Lumen до их пересечения с поверхностью**, с отдельным накоплением результата для Volumetric Fog. Умножение всей готовой Lumen SH-смеси на вертикальное `exp(-tau_up)` не принимается как продуктовый A1c: оно неправильно обращается с близкими поверхностями и эмиссивами внутри среды.

По исходникам найден ограниченный plugin-only путь без нового Renderer pass: второй SH accumulator внутри существующего `TranslucencyVolumeIntegrateCS`, использующий уже привязанный atlas расстояний. Эта заметка фиксирует исходный research и контракт. **Позднее 2026-09-20 прототип скомпилирован, проверен на GPU и установлен**; актуальные результаты, ограничения и управление — `FogMS_A1c_Report.md`. Не смешивать исходные 11 source-isolation captures ниже с последующими измерениями A1c.

Точное разложение готового SH на sky/surfaces/emissive не нужно, если известны направление и длина пути от точки тумана до входящей радиации. Поверхностный Lumen radiance становится граничным условием для этого отрезка. Это не полный сопряжённый перенос «туман ↔ поверхности» и не многократное рассеяние внутри тумана.

## Почему одного верхнего луча недостаточно

В точке может приходить одинаковый RGB как от неба через 20 м среды, так и от эмиссива в 1 м. При sigma_t=.1 1/м правильные пропускания для этих отрезков — .135335 и .904837. Общий вертикальный множитель .135335 оставил бы только 14.96% должного вклада близкого эмиссива. После суммирования источников восстановить это различие нельзя.

Проверяемая формула для каждого входящего луча:

`L_fog(ray) = L_native(ray) * exp(-integral(sigma_t(x + s*direction), s=0..endpoint))`

Для surface hit endpoint ограничивается расстоянием до принятого Lumen пересечения; для sky miss интеграл локальной среды заканчивается на выходе из Box. Интегрировать нужно только пересечение отрезка с Box. Затем выполнить ту же SH-проекцию и нормировку, что использует UE. Геометрическую видимость повторно не умножать; sigma_s применяется позже в штатном LightScatteringCS один раз.

Нулевые длина и плотность дают T=1. Численный пример и проверки: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_A1c_Research_20260920\segment_reference.py`, `segment-counterexample.json`.

## Что проверено на GPU в открытом проекте

Работа через установленный UE MCP Bridge, в `/Game/FogMS_Test/FogMS_Box`. Созданы только временные transient capture, чёрная подложка и эмиссивный прямоугольник. Подложка имеет нулевой unlit Emissive; в измеряемом центральном ROI сам эмиттер не виден. Использован существующий Engine material через временные MID, исходный asset не изменялся.

Capture: 256×144 RGBA16F SceneColor HDR, persistent rendering state, ray tracing разрешён, Lumen GI явно выбран, PreExposure=1. По 120 кадров на состояние; fog temporal/jitter отключены только на время измерения. Измеряемый ROI 16×16. Солнце и Sky Light отключались физической интенсивностью, а не только fog scattering intensity. Sky — существующий specified SunsetAmbientCubemap, intensity=4; SkyAtmosphere оставался скрыт. Поэтому sky-тест **не является отдельной проверкой динамического SkyAtmosphere**. В начале серии Sun intensity=140; предыдущие snapshots с 250/sky20 уже не описывают эту серию.

| Контроль | Средний линейный RGB в ROI |
|---|---|
| Все источники выключены, четыре измерения | (0, 0, 0) |
| Только солнце (включая возможный surface bounce) | (6.672455, 6.672455, 6.672333) |
| Только Sky Light, Lumen volume включён | (.033292, .012624, .010832) |
| Sky Light, native fallback без Lumen volume | (.072541, .035473, .026891) |
| Только surface emissive, Lumen volume включён | (.927599, .927599, .918592) |
| Тот же emissive, Lumen volume выключен, fog включён | (0, 0, 0) |
| Тот же emissive, Lumen включён, Volumetric Fog выключен | (0, 0, 0) |
| Повтор emissive с Lumen и fog | (.927986, .927986, .918337) |

Все значения конечны. Emissive repeat MAE=.0006078, relative L1=.06574%. Два нулевых контроля подтверждают, что положительный центральный сигнал emissive пришёл через Lumen и volumetric fog, а не от прямого изображения светящейся панели. Это демонстрация существующих входов, не калибровка будущего A1c, не сравнение энергетической эффективности солнца/неба/эмиссии и не гарантия работы произвольного маленького эмиссива.

Артефакты: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_A1c_Research_20260920`: `isolate_sources.py`, `source-receipt.json`, одиннадцать raw `.rgba32f`, `measurements.json`, `source-controls.png`. Иллюстрация использует один общий Reinhard display transform; таблица выше — сырые линейные значения.

Все изменённые скриптом свойства и CVars восстановлены, временные акторы удалены, cleanup errors отсутствуют. Дополнительное MCP-чтение `restoration.json` подтверждает тот же уровень, отсутствие временных акторов, Sun=140, Sky=4, SkyAtmosphere hidden=true. Скрипт не вызывал сохранение карты/активов. `camera_unchanged=false`: пользовательская камера изменилась за время серии; скрипт не вызывал её setter и не откатывал перемещение. Измерительная камера была отдельной и неподвижной. Не утверждается неизменность всей сцены при параллельной работе пользователя.

## Анкеры UE 5.8.2

Пути ниже относительно `D:\PersonalProjects\UE5\UE_5.8\Engine`. Проверка независимым read-only subagent согласуется с локальным чтением.

1. `Shaders/Private/VolumetricFog.usf:1003–1009`: единый Lumen GI + shadowed sky SH; consumer вызывает `GetTranslucencyGIVolumeLighting(..., false)`. Native non-Lumen sky отдельно в 1014–1040. Own volume emissive отдельно в 1163–1169.
2. `Shaders/Private/Lumen/LumenTranslucencyVolumeHardwareRayTracing.usf:117–157`: hit radiance из surface cache; miss — radiance cache или sky; затем sky leaking и pre-exposure, clamp, запись RGB и hit distance. `LumenHardwareRayTracingCommon.ush:295` задаёт `HitT`, при miss — `Ray.TMax`. Это первый принятый Lumen hit с его правилами masked geometry/self-intersection, не гарантия первой любой геометрической поверхности. Distance atlas R16F ограничен MaxHalfFloat.
3. `Shaders/Private/Lumen/SurfaceCache/LumenSurfaceCache.ush:50–55`: surface radiance содержит direct, indirect и Emissive. Source identity уже смешана.
4. `Shaders/Private/Lumen/LumenTranslucencyVolumeLighting.usf:251–287`: ray loop, SH projection и отдельный current output для Volumetric Fog. History для translucent surfaces начинается после этого. Native SH сворачивает цветную направленность в ambient RGB плюс монохромные directional coefficients.
5. `Source/Runtime/Renderer/Private/Lumen/LumenTranslucencyVolumeLighting.cpp:549–557,928–937`: Integrate уже объявляет и получает `VolumeTraceHitDistance`, View и VolumeParameters. Добавлять параметр в Engine C++ для чтения расстояния не нужно.
6. Полный поиск `GetTranslucencyGIVolumeLighting` по Engine/Shaders: только VolumetricFog выбирает `false`; BasePass, Substrate, PrimaryRays, EyeAdaptation и **обе HV ветки** выбирают `true`. HV anchors: `Shaders/Private/HeterogeneousVolumes/HeterogeneousVolumesRayMarchingUtils.ush:260,635`. Не менять history accumulator/output.
7. `Shaders/Private/Lumen/LumenTranslucencyVolumeLightingShared.ush:103–116`: actual ray direction использует BlueNoise через `GetProbeTexelCenter`/`GetProbeTracingUV`, даже при Temporal.Jitter=0. Центр `.5` из native SH integration — не фактический trace direction. Для T восстанавливать фактический луч; native basis оставить прежним ради T=1 parity.
8. `Shaders/Private/Lumen/LumenRadianceCacheInterpolation.ush:156,429`: RC ограничивает реальный trace и затем даёт интерполированную глубину probes с parallax correction. Это не достоверный endpoint данного луча.

## Контракт ограниченного прототипа

Исследовательский режим, по умолчанию Off. Два накопителя: native `Lighting` полностью сохраняется для history; `FogLighting` учитывает medium T по каждому лучу и заменяет только current `RWTranslucencyGI0/1`. Strength=0/Off должны проходить прежним порядком операций, без лишних выборок. Смешивание с legacy — по BoxWeight, без нового вклада снаружи. Будущий shader compile должен подтвердить все реально затронутые permutations.

Обязательные начальные ограничения:

- Lumen HWRT и `r.Lumen.TranslucencyVolume.TraceFromVolume=1`.
- `r.Lumen.TranslucencyVolume.RadianceCache=0` **и** `r.Lumen.TranslucencyVolume.ShareRadianceCacheWithOpaque=0`; shared branch иначе передаёт opaque RC независимо от первого переключателя (`LumenScreenProbeGather.cpp:2601–2603`).
- `r.Lumen.TranslucencyVolume.SpatialFilter=0`: иначе RGB уже перемешан с соседними ячейками, а distance остаётся исходным.
- `r.Lumen.TranslucencyVolume.GridCenterOffsetFromDepthBuffer=-1`: Integrate не привязывает SceneTextures, поэтому не может повторить depth-dependent сдвиг ray origin. В контроле начало восстанавливается из FrameJitterOffset.
- PostProcess `LumenSkylightLeaking=0` для измерений: добавка leaking не имеет отдельного endpoint.
- Полный возможный путь через поддерживаемый Box должен укладываться в MaxTraceDistance и R16F; усечённый miss нельзя объявлять полным пропусканием до неба.

Эти CVar меняют и базовый Lumen GI. Поэтому сравнение неизменности translucent surfaces выполняется при **одинаковой конфигурации в обеих половинах A/B**. Прототип не должен молча принуждать пользовательский проект к этим настройкам. Перенос на штатный RC/filter/depth-offset — отдельный вопрос после доказательства алгоритма, а не автоматически обещанная совместимость.

Плотность: к моменту Lumen integration `VBufferA` ещё не создан. Нужна выборка авторского поля **нашего Box+Perlin** в мировом пространстве, через plugin-owned runtime payload/SRV с доказанными lifetime/residency. Это не новый кэш всей среды и не новая сетка плотности. Начальный scope не включает native LFV, чужие Volume materials и автоматическое восстановление их плотности. История освещения не заменяет полную текущую плотность.

Для первой калибровки — только добавленная Box-плотность, область ближе 60% Fog View Distance; height-fog фон сохраняется штатным. UVW/channel/threshold/softness/tile/edge/negative scale должны совпасть с материалом. Поле для освещения трактуется как авторская локальная среда, в том числе вне фрустума; camera fade в native voxelization остаётся отдельным ограничением приёмника света. Texture binding нельзя объявлять безопасным только потому, что descriptor index существует: нынешний runtime специально удерживает resident allocation, так как bindless доступ невидим штатному сбору ресурсов прохода.

Стоимость пропорциональна `число активных ячеек TLV × число лучей × число шагов среды`. Default TracingOctahedronResolution=3 означает 9 лучей на ячейку, не один дополнительный марш. Box ограничивает выборки плотности, но не убирает штатный full-view Lumen dispatch. GPU бюджет A1b нельзя переносить на этот алгоритм.

## Приёмка следующего кодового среза

1. StrictIncludes, D3D12/SM6 shader compile, unchanged Engine; минимальные binding/permutation guards. Off/Strength=0 дают прежний результат при одинаковых CVar.
2. White/black/Perlin density и UVW соответствуют volume material в поддерживаемой области; нули, transform, negative/nonuniform scale, live texture swap и residency проверены.
3. Sky-only: контроль известной толщины, уменьшение incoming light при росте оптической толщины, T=1 при sigma=0; sky miss не обрезан раньше BoxExit.
4. Internal emissive на двух расстояниях: ослабление учитывает путь до поверхности. Сравнение с upward multiplier только как отрицательный контроль, не как эталон.
5. Translucent/HV history output сохраняется при одинаковой renderer-конфигурации; directional A1/A1b, local lights, собственная volume emission и extinction не меняются этим патчем.
6. Движение камеры/Box, редкий persistent SceneCapture и переключения сбрасывают только нужную fog history; unsupported config отключает эксперимент с конкретной причиной.
7. GPU-время по проходам, память и изображения в том же уровне; installation с backup только после проверок. Полный spatial MS, feedback в Lumen и Shipping не заявляются.

## Открытые вопросы перед распространением режима

Как совместить fog-only attenuation с radiance cache, spatial filtering и depth-corrected origins, сохранив достоверную длину каждого пути? Достаточна ли грубая сетка TLV для границ маленького Box? Как устранить SH ringing/color compression и световые утечки при интерполяции возле Box без дублирования GI? Это вопросы качества и инфраструктуры, которые shader-only возможность сама по себе не решает.
