# FogMS — обзор для аудитора

## Передача новой сессии — W51b/W51c и стабильность Host, 2026-09-28

Для продолжения сначала читать это дополнение, [`FogMS_Weather_Coherence.md`](FogMS_Weather_Coherence.md),
[`FogMS_W51b_Architecture.md`](FogMS_W51b_Architecture.md) и [`FogMS_W51c_Verification.md`](FogMS_W51c_Verification.md).
Глобальная погода остаётся отдельным полем Weather, а локальный MS_FOG — близким детальным объёмом с решателем.
W51b выбрал общий нативный Cloud Host: измеренная высокая цена W51a связана прежде всего с солнечным маршем материала,
а не с непригодностью нативного облачного рендера. `Native Weather Preview` остаётся экспериментальным и выключен по умолчанию.
W51c ограничивает только солнечный марш этого Host при Preview, возвращает авторское значение при выходе и сохраняет ручной override;
глобальный лимит и остальные облачные компоненты не меняются. Ограничения W50 и открытые полевые сценарии сохраняются.

Последнее исправление удерживает editor-binding Box при пропуске actor/view ticks: фоновая пауза больше не обнуляет
материал и не сбрасывает Cloud Host с Mode 3 / минимум 32 на Mode 0 / минимум 2. Скрытие, выключение, неверная плотность
и удаление освобождают Host. Game/PIE сохраняют прежний heartbeat. Исходники и протокол —
[`FogMS_CloudHost_EditorBinding_Verification.md`](FogMS_CloudHost_EditorBinding_Verification.md).

Проверены свежие `BuildPlugin -StrictIncludes` для Editor, Game Development и Shipping, отдельные lifecycle-прогоны W51c
и editor-binding, затем установка в пользовательский проект и удержание настроек при поворотах камеры/солнца ±10°.
Пакет с W51c и последним исправлением установлен 28.09; предыдущий код/бинарники и квитанции сохранены в
`E:/GITHUB/MultiLobeSpec/.codex-build/CloudRotation_20260928/Install/`. Перезапуск без сохранения уровня разрешён владельцем;
хэш сохранённой карты не изменился. Пакет и журнал последней сборки — соседний каталог `Build/`.

Рабочая сцена — `/Game/FogMS_Test/FogMS_Box` в
`D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/MimirHead_portfolio.uproject`.
Engine `D:/PersonalProjects/UE5/UE_5.8` — только для чтения; изменения и fork запрещены. Новые уровни не создавать.
При запуске со стартовым скриптом использовать прямые слеши в `-ExecutePythonScript`.

Следующий шаг — полевое сравнение остаточной дрожи силуэта при поворотах камеры/солнца и W/S, плюс погодные состояния
и SkyLight capture по W51c. Стабильные настройки **не доказывают** полного устранения видимого мерцания. Отдельно сохраняется
нативное ограничение Cloud Host: плотность ниже нулевой высоты SkyAtmosphere приводит к Froxel Fog fallback.
После приёмки — W52 (пространственный свет неба) и последующие срезы из плана. Уже завершённые GPU-сравнения не повторять
без нового изменения или конкретного неразобранного дефекта.

## Дополнение W50 — ветка `codex/weather-coherence`, 2026-09-27

Текущее направление и границы работы — [`FogMS_Weather_Coherence.md`](FogMS_Weather_Coherence.md). Ориентир владельца —
согласованная погода RDR2 при сохранении существующего вида локального MS_FOG. W50 связывает погодное пропускание солнца
с прямым светом Cloud Host и входным светом MS, добавляет отдельное наследование погодного ветра и исправляет освобождение
погодного хоста при Clear/выключении. Авторские размеры, плотность, шумы, эрозия и настройки MS локального Box сохраняются;
`Use Weather Wind` по умолчанию выключен. Математика переноса и файлы Engine не менялись.

Общая плотность и оставшийся путь к солнцу находятся в `Shaders/Private/FogMS_WeatherLighting.ush`; захват неба и Lumen
повторно не затемняются. Адаптер локального солнечного света ограничен одним atmosphere sun index 0 над горизонтом;
при втором atmosphere light или солнце ниже горизонта он отключается с объяснением в статусе. Ограничения Thin по высоте
сохраняются. Управление описано в разделе 4а [`FogMS_UserGuide.md`](FogMS_UserGuide.md).

**Проверено для W50:** C++ `BuildPlugin -StrictIncludes` для Editor, UnrealGame Development и Shipping; компиляция шейдеров
и материалов; повторные запуски генераторов материалов без повторных изменений. В редакторе проверены переходы Clear/Overcast,
выключение/включение с сохранением host локального Box, солнечный A/B и общий ветер без скачка фаз. Замеры, ограничения и
непроверенные сценарии — [`FogMS_W50_Verification.md`](FogMS_W50_Verification.md). Визуальная приёмка владельцем остаётся открытой.
Закатный купол по текущему виду остаётся неудовлетворительным: W50 его не заменяет. Настоящая объёмная погода вокруг камеры
и рельефа — следующий отдельный срез, а не результат этой ветки.

## Исторический снимок аудита до W50

**Весь текст ниже — снимок до W50**, включая таблицы со словом «текущий», список открытых работ и ссылки `файл:строка`.
Эти номера строк относятся к старому снимку и не являются указателями на текущую ветку; при работе с W50 сверяйте символы
в исходниках и дополнение выше.

Состояние снимка на **2026-09-27**, ветка `main` (срез для аудита — ветка `fogms/audit-2026-09-27`); код — коммит `083341c` (после него до аудита менялась только
документация и два комментария с путём к журналу). Утверждения того аудита сверены с
исходниками (ссылки файл:строка — там, где это существенно); что не удалось проверить, помечено «(не проверено)». Хронология
решений и замеры по раундам — [`docs/history/FogMS_Prod_Report.md`](docs/history/FogMS_Prod_Report.md); прежний handover
(журнал этапов A–C до 23.09) и отчёты этапов A1–B3 — [`docs/archive/`](docs/archive/README.md).

Сокращения путей: `MLS/` = `Source/MultiLobeSpec/Private/`, `FR/` = `Source/FogMSRender/Private/`, `SH/` =
`Shaders/Private/`, `PP/` = `Tools/FogMSEnergyValidation/ProdProbe/`.

## 0. Коротко

- **FogMS** — многократное рассеяние света и самозатенение среды для локальных объёмов тумана и облаков в UE 5.8. Автор
  ставит актёр **FogMS Box Volume** (ориентированный бокс с плотностью из 3D-текстуры). Решатель переноса на сетке 32³ в осях
  Box считает падающий свет каждой ячейки со всех сторон (солнце, point/spot, небо, отражённый свет, все порядки рассеяния) и
  отдаёт это поле штатным рендерерам движка. Файлы движка не меняются.
- **Основной путь:** Box → решатель → поле `TransportField` (32³) → доставка: **облачный хост** (штатный Volumetric Cloud
  с материалом `M_FogMS_Cloud`; `Render Path` по умолчанию с раунда 46) или **Froxel Fog** (штатный Volumetric Fog через
  Volume-материал Box, `Emissive Injection`; он же откат хоста) → погода (актёр **FogMS Weather**: тени облаков погоды через
  проход теней хоста, купол-небо) → штатная карта теней облака и захват SkyLight.
- **Legacy-путь** (этапы A1–B1; только редактор с `-BindlessAll`): оверлей шейдеров движка — A1, Octaves, Spatial, World,
  авторская тень солнца, кэш теней, `Indirect Shadowing`, View Integration. В игре его нет. Экранный постфильтр SSFS остался
  опцией (по умолчанию выкл.).
- **Проверено** только на UE 5.8.2 (CL 56702186), Win64, D3D12 SM6, аппаратный ray tracing, RTX 3070, одна тестовая сцена
  заказчика. Автотестов UE Automation у FogMS нет (раздел 7). В `.uplugin` — `IsBetaVersion`.
- **Ассеты Unreal в репозитории не хранятся** (раздел 6); `M_FogMS_Density` и `T_FogMS_DefaultVolume` скриптами репозитория
  с нуля не создаются — только из истории git.

## 1. Документы

| Документ | Статус | Зачем читать |
|---|---|---|
| этот файл | текущий | архитектура, модули, пути рендера, глобальные изменения движка, сборка, техдолг, открытые задачи |
| [`FogMS_UserGuide.md`](FogMS_UserGuide.md) | текущий | каждое свойство, cvar, команда, статус, цена; политика ассетов (раздел 9) |
| [`FogMS_PerPixelClouds_Design.md`](FogMS_PerPixelClouds_Design.md) | дизайн, реализованы P1, P2, часть P4 | облачный хост; P3, P5, P6 открыты |
| [`FogMS_Weather_Design.md`](FogMS_Weather_Design.md) | дизайн, реализованы W47, W48, фаза 1 W49 | погода; W50–W53 открыты |
| [`FogMS_DensityAuthoring_Design.md`](FogMS_DensityAuthoring_Design.md) | частично устарел, S0–S2 реализованы | эрозия, профиль высоты, пакет Box |
| [`FogMS_ForwardLobe_Design.md`](FogMS_ForwardLobe_Design.md) | частично устарел, F1a/F2 реализованы | прямой лепесток, `Sun Softness` |
| [`FogMS_LOD_Research.md`](FogMS_LOD_Research.md), [`FogMS_Cloud_Lighting_Review.md`](FogMS_Cloud_Lighting_Review.md), [`FogMS_NativeCloudShadows_Research.md`](FogMS_NativeCloudShadows_Research.md), [`FogMS_References.md`](FogMS_References.md) | исследования и справка, частично устарели | обзоры игр и литературы, факты движка |
| [`FogMS_Fab_Readiness.md`](FogMS_Fab_Readiness.md) | частично устарел | план снятия приватных зависимостей (актуальный список — раздел 8 здесь) |
| [`docs/history/FogMS_Prod_Report.md`](docs/history/FogMS_Prod_Report.md) | журнал | раунды 1–49: что менялось, как проверяли, числа, решения владельца |
| [`docs/archive/README.md`](docs/archive/README.md) | архив | отчёты этапов A1–B3, ранние аудиты, прежний handover, устаревшие документы MLS |
| [`README_RU.md`](README_RU.md) и документы MLS | MLS | вторая часть плагина (BRDF-оверлей), к FogMS относится только общий оверлей |

У каждого дизайн-документа в шапке — статус реализации на 2026-09-27.

## 2. Архитектура и поток данных

### 2.1 Сущности

| Сущность | Где | Что хранит / делает |
|---|---|---|
| **Box** — `AFogMSBoxVolume` | `MLS/FogMS_BoxVolume.h/.cpp` | ориентированный бокс; авторская плотность (Volume Texture, порог, мягкость, две детальные октавы, мировая привязка, анимация «ветер + Edge Flow», эрозия, профиль высоты); режим рассеяния и пресет решателя; доставка (`Emissive Injection`, гибрид, `Render Path`); облик (`Multiple Scattering Look`, `Sun Softness`, `Sun Detail Shadow`). `UpdateDensity` пишет параметры плотности в MID своего `M_FogMS_Density` или в MID облачного хоста |
| **Runtime Box** — `FFogMSBoxRuntime`, `FBoxViewExtension` | `MLS/FogMS_BoxRuntime.cpp` | scene view extension: собирает пакет Box (32 × float4, `FogMSRender::BoxRowCount`), атлас плотности, снимок неба; планирует решения (`r.FogMS.MaxBoxesPerFrame`), вызывает решатель, публикует поле, собирает статус |
| **Атлас плотности** | `MLS/FogMS_DensityAtlas.h/.cpp`, `SH/FogMS_DensityAtlas.usf` | BGRA8-копия mip 0 Volume Texture (X × Y·Z) для решателя: в редакторе — загрузка с CPU из исходника текстуры, в игре — GPU-копия |
| **Решатель** | `FR/FogMS_Transport.*`, `FR/FogMS_WorldLighting.*`, `FR/FogMS_WorldSources.*`, `FR/FogMS_LumenSource.*`; `SH/FogMS_Transport.usf`, `FogMS_WorldLighting.usf`, `FogMS_WorldSources.ush`, `FogMS_Indirect.ush`, `FogMS_LumenSource.ush` | изотропное стационарное уравнение переноса на сетке 32³; результат — поле J |
| **Поле** — `TransportField` | свойство Box (`UTextureRenderTargetVolume` 32³ `PF_FloatRGBA`) | полное: RGB = J, A = 1; гибрид: RGB = max(J − нерассеянное солнце, 0), A = 0,5 + 0,5·T_sun·k (k — доля солнца в нерассеянном свете); A = 0 — поля нет, материал переходит к штатному освещению |
| **Sun Detail Shadow** | `FR/FogMS_SunDetail.cpp`, `SH/FogMS_SunDetail.usf` | карта пропускания солнца в пространстве света на Box (256² × 64) и среднее по ячейке 32³ — перераспределяет долю солнца внутри ячейки во фрокселях |
| **Облачный хост** — `UFogMSCloudHostSubsystem` | `MLS/FogMS_CloudHost.h/.cpp` | находит Volumetric Cloud с материалом на основе `M_FogMS_Cloud`, подгоняет его слой под Box, держит cvar облака и карты теней, принимает погоду |
| **Погода** — `AFogMSWeather`, `UFogMSWeatherState` | `MLS/FogMS_Weather.h/.cpp` | состояние погоды (пресеты Clear/Scattered/Broken/Overcast), карта погоды, подача хосту, купол-небо |
| **Legacy-оверлей** | `MLS/MultiLobeShaderPatcher.*`, `MLS/MultiLobeSpec.cpp`; `SH/FogMS_Common.ush`, `FogMS_Reconstruction.ush`, `FogMS_Spatial.usf`, `FogMS_ShadowCache.usf`, `FogMS_ScreenScattering.ush` | копия шейдеров движка с патчами по анкерам и ремап `/Engine` (только редактор) |

### 2.2 Поток данных за кадр

```text
GAME THREAD
  AFogMSBoxVolume::UpdateDensity (тик Box, правки, перемещение и BeginRenderViewFamily каждого view family)
    -> MID M_FogMS_Density (Froxel Fog)  или  MID хоста M_FogMS_Cloud (Cloud Host; в своём MID FogMS_FroxelWeight 0)
       параметры плотности FogMS_*, фазы анимации (один снимок времени мира на кадр), поле, лепесток, префильтры
  AFogMSWeather::Tick -> смесь состояния, RT_FogMS_WeatherMap (M_FogMS_WeatherCompose, только при смене погоды),
    RT_FogMS_WeatherSun (слой Thin), FeedWeather -> подсистема хоста; купол-небо (MID M_FogMS_WeatherSky)
  UFogMSCloudHostSubsystem::Tick: выбор хоста, слой, погода в MID хоста, cvar облака и карты теней
  FBoxViewExtension::BeginRenderViewFamily: направление на солнце, снимок SkyLight (+ bWeatherSky), пакет Box,
    порядок Box, render command FogMS_UpdateBox (атлас плотности, текстура поля, карты Sun Detail)
RENDER THREAD (движок без изменений; только хуки scene view extension)
  base pass -> PostRenderBasePassDeferred: кэш тени солнца (legacy), публикация полей Box пакета
  PostTLASBuild (только при ray tracing): на каждый Box — решение или удержание (до r.FogMS.MaxBoxesPerFrame решений на вид):
    источники (солнце после атмосферы, point/spot, небо, отражённый свет по граничным лучам: Lumen surface cache на 5.8.2
    или публичный откат) -> проход 2 (прямой свет ячейки с RT-тенями и пропусканием своей среды) -> свипы B2 или волновые
    фронты B3 по направлениям + PCG (warm start, решение раз в SolveInterval кадров) -> публикация в TransportField
    (сразу или на кадр позже при async compute) -> карта Sun Detail
  RenderLights -> ComputeVolumetricFog: вокселизация M_FogMS_Density, Emissive = σs·J, BaseColor = Albedo (гибрид: × S)
  RenderVolumetricCloud (хост): на каждом шаге луча плотность Box (тот же HLSL), свой марш к солнцу, Emissive = σs·J·лепесток;
    в проходе теней облака — ещё плотность погоды -> штатная карта теней облака
  SkyPass: купол-небо (Is Sky) -> захват SkyLight (Real Time Capture) рисует купол -> SH неба -> авто-источник неба решателя
  PrePostProcessPass: SSFS (если включён), копии поля «на кадр позже»
```

Хуки — только эти четыре (`MLS/FogMS_BoxRuntime.cpp:557–884`, флаги `SubscribesToPostTLASBuild |
RequiresHardwareInlineRayTracing`).

### 2.3 Функция плотности: одна формула в трёх местах

Решатель (HLSL `FogMS_IndirectLocalDensity`, `SH/FogMS_Indirect.ush`, по атласу плотности), Volume-материал `M_FogMS_Density`
(узел `FogMS_Extinction_v3`) и материал хоста `M_FogMS_Cloud` считают одну экстинкцию. HLSL материала хранится в
`PP/matedit_density.py` (`EXTINCTION_CODE_V3`) и дословной копией в комментарии `FogMS_Indirect.ush` между маркерами
`BEGIN/END EXTINCTION_CODE_V3`; `matedit_density.py` сверяет копию перед правкой (`ush_sync_check`), `matedit_cloud.py` берёт
тот же текст. Равенство функции решателя этой формуле при ширине префильтра 0 утверждается комментарием в `.ush`
(автоматической проверки нет, не проверено). Решатель всегда считает без префильтра, поэтому `Depth Prefilter` и
`Host Prefilter` меняют только картинку, не поле.

### 2.4 Решатель

- **Режимы:** `Transport (B2)` — шесть осевых направлений, точное интегрирование по ячейке; `Transport (B3 Angular)` —
  16/24/48/96 направлений (полярные узлы Гаусса × азимут), конечные объёмы против потока. Оба изотропные (g = 0), без
  художественных множителей; уравнение решается PCG с диагональным предобусловливанием. Квадратура по умолчанию повёрнута
  так, что одно направление смотрит на солнце (`r.FogMS.Transport.SunAligned 1`).
- **Бюджет:** пресеты Box — Production 16 / 16 итераций / 1e-6 (по умолчанию), High 48 / 16 / 1e-8, Cinematic 96 / 64 / 1e-14,
  Custom. Warm start продолжает решение между кадрами (сбрасывается при изменении границ Box); пропуск итераций по допуску —
  ранний выход в шейдере, только в B3. Решение раз в `r.FogMS.Transport.SolveInterval` кадров (2), между ними поле держится;
  `r.FogMS.Transport.AsyncCompute 1` (по умолчанию 0) уводит проходы решателя, кроме 0/1/2/14, в async-очередь с публикацией на
  кадр позже.
- **Источники** (`FR/FogMS_WorldSources.cpp`): солнце — освещённость у земли после атмосферы; point/spot — все, чья сфера
  влияния задевает Box, до 256; rect-, static-, IES- или light-function-источник в зоне Box **выключает решатель целиком**
  (Box уходит на штатное освещение, причина в статусе). Небо — `r.FogMS.World.SkySource` (авто: SH захвата при куполе
  погоды; иначе Sky View LUT при Real Time Capture и SkyAtmosphere; иначе обработанная статическая кубмапа; иначе SH).
  Отражённый свет по граничным лучам — Lumen surface cache (только сборка ровно 5.8.2, `Lumen Bounce` Auto), иначе публичный
  откат: `Fallback Ground Albedo` × (солнце × тень × пропускание среды Box + SH неба).
- **Видимость:** тени геометрии — inline ray tracing по TLAS сцены (публичный `FXRenderingUtils`), флаг CastShadow —
  из публичных привязок; своя среда ослабляет лучи и солнце (T_sun на ячейку). Другие Box и чужие облака решатель не видит;
  карту теней облака движка не читает (двойного счёта нет — комментарий W47 в `FogMS_WorldSources.cpp`).
- **Прямой свет ячейки** (проход 2): солнце по 4 чередующимся точкам (`r.FogMS.Transport.DirectSamples 4`) или по 8 при
  `Sun Softness` > 0 (конус Фогеля); point/spot — всегда по 8 точкам.
- **Несколько Box:** любое число Box с `Emissive Injection`, у каждого своё поле и состояние по паре (вид, Box); Box без
  инъекции (legacy-оверлей) — не больше одного. Приоритет решений: ждавший ≥ 8 кадров → камера внутри → крупнее на экране →
  ближе. Дополнительную точку стриминга сцены Lumen получает только один Box.

### 2.5 Доставка поля

| Путь | Условия | Как |
|---|---|---|
| **Cloud Host** (`Render Path` по умолчанию) | Transport + `Emissive Injection`, в уровне есть рендерящийся хост, полоса плотности Box выше земли SkyAtmosphere | Volumetric Cloud с материалом на основе `M_FogMS_Cloud` рисует Box попиксельно: плотность на каждом шаге луча, свой марш к солнцу, фаза `Phase G`; поле — через Emissive (всегда гибрид, кроме `Field Only`), небо — в поле (AO 0). Фроксельная копия выключена (`FogMS_FroxelWeight` 0), `Sun Detail Shadow` не строится. Один Box на хост. Хост создают кнопка Box **Create Cloud Host**, `FogMS.CloudHost.Create` и актёр погоды; Box сам хост не создаёт |
| **Froxel Fog** (и откат хоста) | Transport + `Emissive Injection` | штатный Volumetric Fog вокселизирует `M_FogMS_Density`: режим материала 1 — полное поле, 2 — гибрид (штатный туман считает прямое солнце с тенями движка и фазой тумана, поле — остальное), 3 — отладка `Field Only`; Emissive = valid·J·Albedo·σt |
| **Legacy-оверлей** | только редактор и только UE 5.8.2 CL 56702186; режимы Box — `-BindlessAll` и один Box (глобальный A1 без Box `-BindlessAll` не требует) | пакет Box и атлас читаются патченными шейдерами движка через bindless-дескрипторы: A1, Octaves, Spatial, World, доставка Transport без инъекции, `Authored/Cast Sun Shadow`, кэш теней, A1c `Indirect Shadowing`, View Integration, диск солнца для SSFS, debug-виды `FogMS.Debug`. В игре выключен всегда (`FogMS_IsBindlessAll`, `MLS/FogMS_BoxRuntime.cpp:207–216`) |

Причины отказа видны в статусе Box (`Spatial Status`): требования к виду и cvar, источники, атлас, хост
(`[cloud host: …, froxel fallback]`), небо (`[sky: …]`), отражённый свет (`bounce: …`). Перечень — `FogMS_UserGuide.md`.

### 2.6 Погода и небо

`AFogMSWeather` (один на мир; без актёра ничего не меняется) смешивает состояние во времени, рисует карту погоды
`RT_FogMS_WeatherMap` (512² RGBA16F: R покрытие нижнего слоя, G тип, B шторм — пока 0, A дека) только при смене состояния и
отдаёт её подсистеме хоста. Материал хоста добавляет плотность погоды **только в проходе теней облака** (Shadow Pass Switch):
штатная карта теней облака затеняет землю, туман, Lumen и атмосферу облаками погоды, а видимый проход рисует только Box. Слой
`Thin` (по умолчанию): столб погоды вдоль солнца (`RT_FogMS_WeatherSun`) размазан по слою Box; `Extended`: слой хоста растёт до
верха погоды (раунд 48: около +1 мс видимого прохода). Если хоста нет, актёр создаёт его сам (галка `Create Cloud Host`, по
умолчанию вкл.; и в состоянии Clear) — хост вытесняет облака неба уровня. Купол-небо (раунд 49): transient-сфера 1000 км с
материалом `M_FogMS_WeatherSky` (Unlit, Is Sky) рисует видимые облака погоды той же функцией плотности; захват SkyLight
(Real Time Capture) рисует купол, и авто-источник неба решателя берёт его SH. Солнце решателя и солнце облака хоста погоду
пока не видят (W50).

## 3. Модули и файлы

`MultiLobeSpec.uplugin`: `EngineVersion` 5.8.0, `VersionName` 0.15.3, `CanContainContent`, только Win64; зависимость от
плагина `EditorScriptingUtilities` (только цели Editor).

| Модуль (тип, фаза) | Файлы | Роль |
|---|---|---|
| **FogMSRender** (Runtime, `PostConfigInit`) | `FogMSRender.cpp` | старт модуля: защита D3D12, ремап `/Plugin/FogMS` → `Shaders/`; кэш тени солнца legacy (128×128×65); GPU-копия атласа плотности |
| | `FogMS_Transport.cpp/.h` | граф RDG решателя B2/B3, флаги теней из публичных привязок, проход 17 (публикация поля) |
| | `FogMS_WorldLighting.cpp`, `Public/FogMS_WorldLighting.h` | запуск решения (и legacy World), состояние по (вид, Box), warm start, `SolveInterval`, поздняя публикация, `FogMS.DumpSpatial`; публичные структуры запроса |
| | `FogMS_WorldSources.cpp/.h` | список источников из сцены, солнце атмосферы, выбор источника неба, пометка о чужих облаках |
| | `FogMS_LumenSource.cpp/.h` | чтение Lumen surface cache (только UE 5.8.2) и заглушки для остальных версий |
| | `FogMS_SunDetail.cpp` | карта `Sun Detail Shadow` (3 прохода) |
| | `FogMS_Spatial.cpp`, `Public/FogMS_Spatial.h` | legacy `Spatial (Experimental)` по истории тумана (только `-BindlessAll`) |
| | `FogMS_ScreenScattering.cpp`, `Public/FogMS_ScreenScattering.h` | SSFS-постфильтр (по умолчанию выкл.) |
| | `FogMS_RHICompatibility.cpp/.h` | защита D3D12 при `-BindlessAll` (раздел 4) |
| **MultiLobeSpec** (Runtime, `PostEngineInit`) | `FogMS_BoxVolume.cpp/.h` | актёр Box: свойства, `UpdateDensity`, MID, поле и карты, привязка к хосту, автозапуск и `Apply Required Render Settings`, пресеты, анимация, `FogMS.CloudHost.Create`, `r.FogMS.SunMap.*` |
| | `FogMS_BoxRuntime.cpp/.h` | runtime и view extension (раздел 2.2), пакет, планировщик, legacy-предпросмотр Lumen |
| | `FogMS_DensityAtlas.cpp/.h` | атлас плотности |
| | `FogMS_CloudHost.cpp/.h` | подсистема хоста, `FogMS.CloudHost.SetupShadows` |
| | `FogMS_Weather.cpp/.h` | погода, `FogMS.Weather.*`, `r.FogMS.Weather.SkyDome` |
| | `MultiLobeShaderPatcher.cpp/.h`, `FogMS_ShaderPatcher.h`, `MultiLobeSpec.cpp`, `MLSRawMaterialVisibilityOverlay.*`, `MultiLobeSpecSettings.*`, `MultiLobeSpecViewExtension.*`, `MLSShaderConfigValidation.cpp` | общий оверлей шейдеров движка (MLS и legacy FogMS), команды `MLS.*` и `FogMS.Apply/Status/Debug`, настройки MLS, автотесты MLS — всё под `WITH_EDITOR` и `GIsEditor`; в игре — заглушки |
| **MultiLobeSpecEditor** (Editor) | `MLSBaker*`, `MLSConeEnvBRDFGenerator.*`, `MLSMicroShadowLUT*`, `MultiLobeSpecEditorModule.cpp` | только MLS: бейкер AO, генераторы и проверки LUT (к FogMS не относится) |

Шейдеры (`SH/`, путь `/Plugin/FogMS/Private/…`): `FogMS_Transport.usf` (проходы решателя), `FogMS_WorldLighting.usf`
(legacy World), `FogMS_WorldSources.ush` (источники, небо), `FogMS_LumenSource.ush` (обёртка Lumen или заглушка),
`FogMS_Indirect.ush` (функция плотности и помощники), `FogMS_SunDetail.usf`, `FogMS_DensityAtlas.usf`,
`FogMS_ScreenScatteringPost.usf` (SSFS); только для оверлея — `FogMS_Common.ush`, `FogMS_Reconstruction.ush`,
`FogMS_Spatial.usf`, `FogMS_ShadowCache.usf`, `FogMS_ScreenScattering.ush`. Материалы (`.uasset`) строят скрипты `PP/matedit_*.py`
(раздел 6). `Config/DefaultMultiLobeSpec.ini` — `[CoreRedirects]` переименований W38; `Config/FilterPlugin.ini` включает его в
пакет плагина.

## 4. Что плагин меняет в настройках движка

Всё — через консольные переменные и свойства актёров; файлы движка не правятся, ini не пишутся.

| Кто | Что ставит | Приоритет, возврат |
|---|---|---|
| Box, `Apply Required Render Settings` (по умолчанию вкл.) | `r.RayTracing.Culling 0`, `r.Lumen.AsyncCompute 0` (требования решателя; в движке 3 и 1) — в игре в `BeginPlay` включённого Box Transport/World; в редакторе, PIE и Simulate при самозапуске Box Transport с инъекцией (в редакторском мире только без `-BindlessAll`) | `SetByGameSetting`, раз на экземпляр актёра; значение из ini, профиля, командной строки или консоли сильнее — тогда Transport не запускается и статус это называет; не возвращаются (`MLS/FogMS_BoxVolume.cpp:206–241`) |
| Облачный хост, пока через него рисуется Box | `r.VolumetricCloud.DistanceToSampleMaxCount`, `.ViewRaySampleMaxCount`, `.SampleMinCount`, `r.VolumetricRenderTarget.Mode`, `.UpsamplingMode`, `.ReprojectionBoxConstraint`, `.MinimumDistanceKmToEnableReprojection`; при тенях облаков солнца (и при погоде без Box) — `r.VolumetricCloud.ShadowMap.SpatialFiltering`, `.SnapLength`, `.SnapToPixelGrid`; при слое погоды Extended — `r.VolumetricCloud.StepSizeOnZeroConservativeDensity` (11 cvar, `MLS/FogMS_CloudHost.cpp:133–146`; значения — `r.FogMS.CloudHost.*`, `r.FogMS.Weather.SkipSteps`) | `SetByGameSetting`, явное значение сильнее («kept» в статусе); прежние значения возвращаются, когда хост больше не нужен |
| Облачный хост, свойства компонента | слой (`Layer Bottom Altitude` / `Layer Height`: полоса плотности Box ±10 м, гистерезис 5 м; `r.FogMS.CloudHost.FitLayer 0` — только проверка), `View Sample Count Scale`; у хоста погоды без Box — `Tracing Start Distance` = `Tracing Max Distance`; хост поднимается над другими облаками сцены | прямо в уровне, без транзакции; хост погоды без Box получает прежние слой и дистанцию, когда погода уходит; помечается ли уровень изменённым — не проверено |
| Кнопки и команды по запросу | `FogMS.CloudHost.SetupShadows`, `FogMS.Weather.SetupShadows`, кнопка погоды `Setup Sun Shadows`: солнцу `Cast Cloud Shadows`, `Cloud Shadow Extent`, масштабы разрешения и выборок | одна строка лога с прежними значениями, один шаг Undo; ничего не сохраняют; сам плагин солнце и SkyLight не трогает |
| Защита D3D12 (`FR/FogMS_RHICompatibility.cpp`) | `r.RHICmd.ParallelTranslate.Enable 0` и однократная подготовка bindless-куч контекста — только UE 5.8.2 + D3D12 SM6 + один GPU + bindless `All` (`-BindlessAll`); применяется снова при любом изменении консольных переменных | до конца процесса (редактор и игра), одна строка Warning; при отсутствии cvar — `checkf` (причина защиты — `docs/archive/FogMS_Crash_Report.md`) |
| MLS-оверлей (редактор) | при каждом старте редактора копирует `Engine/Shaders` в `<Project>/Saved/MultiLobeSpec/`, патчит, ремапит `/Engine` и перекомпилирует изменённые шейдеры, если настройки MLS его включают — **по умолчанию включают** (`Micro Shadow Mode` = Activision WWII, `MLS/MultiLobeSpecSettings.h:102`, `MLS/MultiLobeSpec.cpp:202–203`); нужны `r.Substrate 0` и `r.AllowStaticLighting 0`, иначе уведомление «Apply FAILED»; тот же оверлей несёт legacy FogMS | только редактор; `MLS.Disable` возвращает штатные шейдеры |
| Legacy: `Enable Live Box`, `Use Global A1`, `Enable Indirect Preview`, `FogMS.Debug` | `r.FogMS.Enable`, `r.FogMS.BoxMode`; набор из 12 cvar Lumen/TLV/RT предпросмотра (`MLS/FogMS_BoxRuntime.cpp:157–177`); `r.GeneralPurposeTweak` и `r.VolumetricFog.TemporalReprojection` для debug-видов | `SetByConsole`; `Restore Standard Lumen` возвращает набор предпросмотра, `FogMS.Debug 0` — debug; только редактор |
| Атлас плотности в игре | `SetForceMipLevelsToBeResident` на Volume Texture автора (продлевается каждые 5 с), пока идёт GPU-копия | пока нужен атлас |

## 5. Ключевые консольные переменные и команды

Полный список с описаниями и диапазонами — `FogMS_UserGuide.md`, раздел 5 (сверен с кодом).

| Группа | Cvar (умолчание) |
|---|---|
| Решатель | `r.FogMS.Transport.SolveInterval` (2), `.AsyncCompute` (0), `.DirectSamples` (4), `.WarmStart` (1), `.Tolerance` (1e-14), `.SweepThreads` (1024), `.SunAligned` (1), `.SkipConverged` (1), `.DirectSkipEmpty` (0), `.Test*` (0; `TestTau` 4, `TestAlbedo` 1); `r.FogMS.MaxBoxesPerFrame` (4); `r.FogMS.SunMap.Resolution` (256), `.Steps` (64); `r.FogMS.DensityAtlas.ForceGPUCopy` (0) |
| Источники | `r.FogMS.World.SkySource` (0), `.SkyLutSamples` (5), `.SunExcludeDegrees` (3), `.SkyMipBias` (0), `.FallbackMedium` (1), `.Indirect` (1) |
| Хост | `r.FogMS.CloudHost.StepSettings` (1), `.FitLayer` (1), `.ViewSampleScale` (8), `.RTMode` (3), `.FarRTMode` (1), `.SampleMinCount` (32), `.FarSampleMinCount` (8), `.NearDistanceKm` (1), `.UpsamplingMode` (2), `.ReprojectionBoxConstraint` (1), `.ReprojectionMinKm` (4), `.ShadowSpatialFiltering` (2), `.ShadowSnapFraction` (0.25) |
| Погода | `r.FogMS.Weather.SkipSteps` (8), `r.FogMS.Weather.SkyDome` (1) |
| SSFS | `r.FogMS.SSFS` (0), `.Amount` (0.5), `.Radius` (24) — постфильтр, работает с любой доставкой Transport |
| Legacy-оверлей | только редактор (`MLS/MultiLobeShaderPatcher.cpp:486–491`): `r.FogMS.Enable` (0), `.Steps` (16), `.MarchDistance` (0), `.MaxDistance` (2000000), `.ExcludeGlobalLayer` (0), `.DebugViews` (1); только с оверлеем: `r.FogMS.BoxMode` (0), `r.FogMS.ViewIntegration` (0), `r.FogMS.ScreenScatteringSun` (1) |

Команды: `FogMS.CloudHost.Create [Box]`, `FogMS.CloudHost.SetupShadows [ExtentKm] [ResolutionScale]`,
`FogMS.Weather.Set <пресет|путь> [сек]`, `FogMS.Weather.SetupShadows [ExtentKm] [ResolutionScale] [RaySampleScale]`,
`FogMS.Weather.Status`, `FogMS.DumpSpatial <префикс>`; только редактор — `FogMS.Apply`, `FogMS.Status`, `FogMS.Debug 0…4`.

## 6. Ассеты Unreal

С коммита `083341c` в репозитории нет `.uasset`/`.umap` (`.gitignore`: `Content/**/*.uasset`, `Content/**/*.umap`). Плагин
ожидает в `/MultiLobeSpec/FogMS/`: `M_FogMS_Density` (Volume-материал Box; жёсткая ссылка из конструктора Box),
`T_FogMS_DefaultVolume` (заглушка текстурных параметров материалов), `M_FogMS_Cloud` + `MI_FogMS_Cloud` (хост; ищутся по пути) и в
`/MultiLobeSpec/FogMS/Weather/`: `T_FogMS_WeatherPattern`, `T_FogMS_Curl2D`, `T_FogMS_CloudTypeLUT`, `M_FogMS_WeatherCompose`,
`M_FogMS_WeatherSun`, `M_FogMS_WeatherSky`, `DA_FogMS_Weather_Clear/_Scattered/_Broken/_Overcast` (ищутся по пути). Скрипты
`PP/matedit_weather.py` и `PP/matedit_cloud.py` создают материалы хоста и погоды, текстуры (из `PP/texgen/gen_weather_textures.py`)
и пресеты с нуля; `PP/matedit_injection.py` и `PP/matedit_density.py` только **правят существующий** `M_FogMS_Density`.
**`M_FogMS_Density` (базовый граф и промежуточные версии контракта поля) и `T_FogMS_DefaultVolume` ни один скрипт репозитория
не создаёт** (исходный генератор материала лежал вне репозитория, в `.codex-build`). Для чистого клона их нужно восстановить из
истории git: `git restore --source=201f33f --worktree -- Content/FogMS` (13 ассетов раунда 48, `M_FogMS_Density` — в состоянии
W41; `M_FogMS_WeatherSky` в истории нет — его строит `matedit_weather.py`). Без ассетов плагин не падает, а пишет статус
(плотность выключена, Box во фрокселях, погода `Inactive`, купол скрыт). Таблица, порядок, упаковка —
`FogMS_UserGuide.md`, раздел 9. Процедура чистого клона целиком не прогонялась (не проверено).

## 7. Сборка, установка, проверка

**Конвейера сборки в репозитории нет.** Рабочие скрипты лежат вне репозитория, в `E:/GITHUB/MultiLobeSpec/.codex-build/
FogMS_Prod_20260922/` на машине разработчика; в репозитории — только копия `PP/build_plugin.sh` (путь к рабочей копии в ней
захардкожен на старый worktree).

| Шаг | Скрипт (вне репозитория) | Что делает |
|---|---|---|
| Сборка раунда N | `build.sh <N>` (`FOGMS_W=<рабочая копия>`) | копирует `Source`, `Shaders`, `Content` (локальная, игнорируемая git), `Config`, `Resources`, `*.uplugin`, `*.md` в `Source<N>/MultiLobeSpec` и запускает `RunUAT.bat BuildPlugin -Plugin=… -Package=Package<N> -TargetPlatforms=Win64 -StrictIncludes`; проверяет наличие трёх editor-DLL |
| Установка + запуск | `cycle.sh <N> <LogName> [launch.ps1\|launch_nobindless.ps1]` | при закрытом редакторе: резервная копия установленного плагина и карты в `<Project>/Saved/FogMS_Backups/`, замена `Binaries`, `Content`, `Config`, `Resources`, `Shaders`, `Source` и `.uplugin` в `<Project>/Plugins/MultiLobeSpec`, запуск редактора |
| Запуск редактора | `launch.ps1` / `launch_nobindless.ps1` | `UnrealEditor.exe <Project>.uproject /Game/FogMS_Test/FogMS_Box -d3d12 -sm6 [-BindlessAll] -ExecutePythonScript=<Project>/Saved/FogMS/start_box.py -abslog=<LogName>.log`; стартовые скрипты `Saved/FogMS/*.py` — в проекте заказчика |
| Выход без сохранения | `quit.sh` | через мост: список изменённых пакетов, `QUIT_EDITOR` |
| (не используется) | `install.ps1` | установка по квитанциям сборки, которые `build.sh` не пишет |

Последняя сборка (раунд 49): BUILD PASS, цели UnrealEditor + UnrealGame Development/Shipping, 9 известных предупреждений C4701
(журнал, «Раунд 49»). Установка в проект заменяет папку `Content` плагина содержимым пакета, поэтому ассеты, изменённые скриптами
в проекте, перед следующей сборкой копируются обратно в локальную `Content/` рабочей копии (подкоманды `copyback`, не для git).

**Проверка.** Автотестов UE Automation у FogMS нет (они есть только у MLS: `MLS/MLSShaderConfigValidation.cpp`,
`Source/MultiLobeSpecEditor/Private/MLS*Validation.cpp`, `MLSBakerSelectionValidation.cpp`). FogMS проверялся: (1) в редакторе
скриптами `PP/` через мост UE-MCP (плагин `UE_MCP_Bridge` проекта, `ws://127.0.0.1:9877`, в репозиторий не входит): снимок сцены,
изменение, статусы и лог, `ProfileGPU`, дамп поля `FogMS.DumpSpatial`, кадры; с раунда 45 — облегчённый протокол «статусы и лог
+ одна цифра», облик оценивает владелец; (2) CPU-проверками без редактора (`PP/fwd_lobe_check.py`, эталоны и контракты
`Tools/FogMSEnergyValidation/*.py` этапов B1–B3); (3) smoke-тестами `-game` с редакторными бинарниками (раунды 20, 45).
Правила прогонов и группы скриптов — `PP/README.md`.

## 8. Приватные зависимости от движка (техдолг)

`FogMSRender.Build.cs:13` добавляет `Engine/Source/Runtime/Renderer/Private` в пути include; модуль `MultiLobeSpec` заголовков
Renderer не включает. Используются:

| Файл | Приватный заголовок | Что берётся |
|---|---|---|
| `FR/FogMS_WorldSources.cpp` | `LightSceneInfo.h`, `ScenePrivate.h`, `VolumetricCloudRendering.h` (с W47) | `FScene::Lights`, `AtmosphereLights[0]`, `VolumetricCloud`, `FLightSceneInfo::Proxy`/`bVisible`, `FVolumetricCloudRenderSceneInfo::GetVolumetricCloudSceneProxy()` — список источников, солнце атмосферы, «чужое» облако |
| `FR/FogMS_LumenSource.cpp` | `Lumen/LumenSceneData.h` (только под гейтом 5.8.2), `SceneRendering.h` | `FViewInfo::ViewLumenSceneData`, атласы и буферы карточек Lumen, **захардкоженные страйды буферов** — отражённый свет по граничным лучам; шейдерная обёртка включает приватный `LumenSurfaceCacheSampling.ush` |
| `FR/FogMS_Spatial.cpp` (legacy) | `SceneRendering.h`, `SceneViewState.h`, `RayTracing/RayTracingScene.h` | `FViewInfo`, история Volumetric Fog, TLAS — режим `Spatial (Experimental)` |
| `FR/FogMS_ScreenScattering.cpp` | `SceneRendering.h`; `PostProcess/PostProcessInputs.h` (Renderer/Internal) | `FViewInfo`, `VolumetricFogResources.IntegratedLightScatteringTexture` — SSFS |

Решатель (`FogMS_Transport.cpp`, `FogMS_WorldLighting.cpp`) приватных заголовков не включает (публичные `FXRenderingUtils`,
`SceneRendererInterface`, `RayTracingMeshDrawCommands`). Другие связи с внутренностями движка:
- **Точные версии:** источник Lumen и защита D3D12 компилируются только для 5.8.2 (`ENGINE_*_VERSION`); legacy-оверлей FogMS
  принимает только 5.8.2 CL 56702186 (`MLS/MultiLobeShaderPatcher.cpp:658–664`); `.uplugin` при этом объявляет 5.8.0.
- **Анкеры оверлея** (только редактор): побайтовые вставки в `VolumetricFog.usf` (15), `HeightFogPixelShader.usf` (2),
  `DeferredLightPixelShaders.usf` (2), `LumenTranslucencyVolumeLighting.usf` (4); каждый анкер должен встретиться ровно один раз,
  иначе применение отменяется и остаётся прежнее отображение. MLS-патчи сверены с UE 5.7 и гейта версии не имеют.
- **Шейдеры движка:** решатель включает `RayTracingCommon`, `TraceRayInline`, `SkyAtmosphereCommon`; упаковка SH неба
  (`SkyIrradianceEnvironmentMap`) переписана вручную (`SH/FogMS_WorldSources.ush`); бит CastShadow в пользовательских данных
  hit group Lumen (бит 29) повторён в плагине.
- **D3D12RHI** (`ID3D12DynamicRHI`, собственные committed-ресурсы) — только под `-BindlessAll`: пакет Box, резидентные атласы,
  кэш тени; проверки имени RHI «D3D12».
- **Ассеты по пути** (не жёсткие ссылки): материал хоста и все ассеты погоды — для упаковки нужны ссылки из уровня или настройки
  кука (не проверено).

## 9. Известные ограничения и открытые задачи

**Ограничения текущего кода** (подробности — `FogMS_UserGuide.md`, разделы 4, 4а, 4б, 7):
- Только UE 5.8.2, Win64, D3D12 SM6, inline hardware ray tracing, deferred; Lumen GI на виде обязателен даже при
  `Lumen Bounce` Off; один вид реального времени (Scene Capture, ортогональные и многовидовые семьи Transport не решает).
- Решатель изотропный, сетка 32³ на Box: свет мельче ячейки не разрешается (для солнца это частично компенсируют `Sun Detail
  Shadow` во фрокселях и собственный марш к солнцу облачного хоста); анизотропия — только поверх поля (лепесток в материале,
  фаза тумана или облака для прямого солнца).
- Box не видят плотность друг друга, перекрытые Box складывают свет; legacy-оверлей обслуживает один Box.
- Облачный хост: один Volumetric Cloud на сцену (хост вытесняет облака неба); один Box на хост; Box виден только в пределах
  трассы хоста (2 км); полоса плотности ниже земли SkyAtmosphere — откат во фроксели (обрезка вместо отката не сделана);
  RT-тени солнца не затеняют собственное солнце облака (вывод дизайна, не проверено); фроксельная копия при весе 0 всё ещё
  вокселизируется.
- Погода: решатель и солнце облака хоста погоду не видят (W50), тумана по погоде нет (W51); фаза 2 раунда 49 (проверки купола,
  оценка владельцем) не выполнена — `M_FogMS_WeatherSky` собран, проверки `d49_sky.py check/cost` не запускались.
- Упаковка: собираются UnrealGame Development/Shipping, пройдены smoke-тесты `-game` с редакторными бинарниками; настоящая
  упаковка (`BuildCookRun`, кук ассетов по пути, GPU-копия атласа в куке) не проверена.
- В тонкой среде многократного рассеяния мало (физика, не дефект); толщину оценивает статус `[tau core ~X, upper bound]`.

**Замечания по коду** (выведены из чтения кода, в работе не проверены):
- Три правила «солнца атмосферы»: направление решателя — первый `ADirectionalLight` с индексом 0 (`MLS/FogMS_BoxVolume.cpp:279–290`),
  хост и команды теней — самый яркий (`MLS/FogMS_CloudHost.cpp:413–437`), render thread — `Scene.AtmosphereLights[0]`; при нескольких
  таких светах они могут разойтись.
- Облачный хост читает поле через MID без зависимости RDG: при `r.VolumetricRenderTarget.PreferAsyncCompute 1` облако движка
  рисуется до `PostTLASBuild` и видит поле прошлого кадра (как и при `r.FogMS.Transport.AsyncCompute 1` по замыслу).
- Scene Capture, ортогональные и многовидовые семьи в режиме «без задержки» очищают общее поле Box и перезаписывают его статус,
  что сбивает удержание `SolveInterval`.
- Хост всегда включает гибридное поле, даже если галка `Hybrid Single Scattering` снята.
- Проверка записи RGBA карты погоды один раз читает пиксель с GPU на game thread.
- В упакованной игре с bindless `All` путь GPU-копии атласа отклоняется (`MLS/FogMS_DensityAtlas.cpp:27–31, 374–378`) и Transport
  не стартует — «density atlas GPU upload failed» (не проверено в упаковке).
- Тултип `Render Path` в коде говорит «the plugin never creates it itself», но актёр погоды хост создаёт (с раунда 48).

**Открытые срезы и решения** (статусы — в шапках дизайн-документов):
- Погода (`FogMS_Weather_Design.md`, раздел 6): **W49 фаза 2** (проверка купола); **W50** погода внутри героических облаков
  (солнце решателя и хоста × пропускание погоды); **W51** туман и атмосфера по погоде, `MPC_FogMS_Weather`; **W52** грозы и
  молнии; **W53** переходы, остальные пресеты, `GetWeatherAt`, `Weather Influence`. Решения владельца 3–7 (кроме принятых 1 —
  купол и 2 — слой Thin по воротам раунда 48) открыты, в том числе 4 — масштаб погоды.
- Облачный хост (`FogMS_PerPixelClouds_Design.md`, раздел 4): **P3** несколько Box на хост; **P4** остаток — видимость геометрии
  для RT-солнца в облаке; **P5** уровни детализации и дальние Box; **P6** облегчение фроксельной сетки; обрезка Box у земли.
- Плотность и LOD: S3–S5 (`FogMS_DensityAuthoring_Design.md`) и тиры/планировщик (`FogMS_LOD_Research.md`) не начаты.
- Готовность к Fab (`FogMS_Fab_Readiness.md`): сделаны Runtime-модули и allow-list, публичные замены P2–P7/P10, удаление приватного
  неба и флагов теней, необязательный источник Lumen; остаются приватные заголовки (раздел 8), сбор источников на game thread,
  описание в `.uplugin`, проверка упаковки.
- MLS (BRDF-оверлей) на UE 5.8.2 по коду и журналам проекта применяется, но корректность затенения не проверена; режим Generic
  VNDF LUT в установке из git не проходит проверку квитанции (см. `README_RU.md`).

## 10. Где хронология и доказательства

- `docs/history/FogMS_Prod_Report.md` — журнал этапа C по раундам 1–49: что поменяли, как проверяли, числа, решения владельца.
  Числа сняты на одной тестовой сцене и относятся к сборке своего раунда.
- `PP/results/diagNN/` — JSON-итоги и листы кадров, на которые ссылается журнал (часть файлов, например `results/diag49/`, в
  репозиторий не добавлена). Кадры (`PP/measure/`), логи редактора и сборок (`Main*.log`, `Build*.log`) и пакеты раундов лежат
  вне репозитория (`E:/GITHUB/MultiLobeSpec/.codex-build/…`, `D:/FogMS_ProbeFrames`).
- `docs/archive/` — отчёты этапов A1–B3 с протоколами, доказательства среза B1 (`evidence_B1_20260921/`);
  `Tools/FogMSEnergyValidation/Results/*.summary.json` — сводки GPU-проб B2/B3.
- История git: коммиты с префиксом `FogMS` описывают каждый срез и раунд; `083341c` — вывод ассетов из репозитория.
