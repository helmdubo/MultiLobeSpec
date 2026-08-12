# v0.14.0 — Generic VNDF execution spec

- Добавлен production-shaped direct Generic VNDF LUT path: deterministic offline QMC bake, V2 two-bank RGBA8 representation, fail-closed manifest и независимый validator.
- Direct Default Lit интеграция переведена на per-lobe micro-shadow multiplication; Rect и anisotropy имеют честные fallback semantics.
- Введён lossless raw MaterialAO transport для legacy deferred. Он требуется независимо для direct Generic VNDF и cone-aware capture/skylight IBL; prerequisites: `r.Substrate=0`, `r.AllowStaticLighting=0`, `r.GBufferDiffuseSampleOcclusion=0`.
- Разделены material micro-visibility и screen/DFAO geometry visibility; добавлены Direct Only, Raw Scalar AO и RGB Interreflection modes для indirect diffuse.
- Реализован CARD-09 cone-aware EnvBRDF: 32×32×8 `NoV × Roughness × AdjustedCosCone`, буквальный UE 5.7 PreIntegratedGF estimator со strict cone reject, RG16_UNORM и manual trilinear runtime.
- Reflection captures/skylight теперь вычисляют direction, radiance и response отдельно для R1/R2; устранена mixture-of-mixtures композиция.
- Lumen/SSR per-lobe cone pairing остаётся capability-false. Energy-conservation ветка явно маркирована как cone/open single-scatter ratio approximation над UE multi-scatter `EnergyTerms.E`.
- Energy-aware F90 приведён к `F0RGBToMicroOcclusion`: `saturate(50*max(F0))`; legacy single-scatter EnvBRDF сохраняет stock green-channel F90.
- Overlay стал content-addressed по source artifacts/config; requested patches используют exact counts и transactional stamp-last commit.
- Добавлены независимые stdlib/NumPy validation tools для direct Generic VNDF и CARD-09, golden indexing/packing/hash/endpoints/monotonicity tests и capability manifest.
- CARD-09 cone-aware indirect выключен по умолчанию и fail-closed: static HLSL LUT в scalar, `uint4` и eight-plane layouts провалил UE 5.7 SM6 compile-cost gate. Для production требуется отдельно авторизованный renderer-bound Texture3D/SRV path.

# v0.12.1-P2-Assign

- Integrated P2 baker: RG normal -> Poisson FFT/multigrid height -> slope-aware orthographic GTAO.
- Added one-click **Find + Assign AO From Current Selection** for selected actors, Static Mesh assets and material instances from the viewport or Content Browser.
- AO asset lookup is exact and deterministic from `<normal base> + AO suffix` in the same folder.
- Normal/AO parameter pairing prefers matching numeric suffixes and preserves Material Layer association/index.
- Existing assignment path no longer invents missing material parameter infos.
- Added `MICROVISIBILITY_REVIEW_RU.md` with semantic and calibration guidance.

# MultiLobeSpec — dual-lobe cinematic specular для UE 5.7 (legacy path)

MVP-плагин: добавляет второй, широкий GGX-лоб («hazy gloss» — яркое ядро блика + ореол вокруг)
в легаси-шейдинг UE без Substrate и без кастомного билда движка. Работает по схеме ShaderShift:
недеструктивный оверлей движковых шейдеров + перекомпиляция на лету + переключаемые пресеты.

---

## 1. Требования

- Unreal Engine 5.7 (Launcher-версия или source build — без разницы, движок не модифицируется).
- **C++ проект.** Если проект Blueprint-only: `Tools → New C++ Class → None → Create`,
  редактор сам превратит проект в C++ и сгенерирует solution.
- Windows: Visual Studio 2022 (workload «Game development with C++», MSVC v143, Windows SDK 10.0.22+).
  Либо Rider. На Mac/Linux — соответствующий тулчейн UE.

## 2. Установка

1. Скопировать папку `MultiLobeSpec` в `<Проект>/Plugins/` (создать `Plugins`, если нет).
   Должно получиться: `<Проект>/Plugins/MultiLobeSpec/MultiLobeSpec.uplugin`.
2. ПКМ по `<Проект>.uproject` → **Generate Visual Studio project files**.
3. Открыть `.sln`, конфигурация **Development Editor | Win64**, собрать (Build).
   Альтернатива без VS-интерфейса:
   ```
   <UE>/Engine/Build/BatchFiles/Build.bat <ПроектName>Editor Win64 Development -project="<путь>/<Проект>.uproject"
   ```
4. Запустить редактор. `Edit → Plugins → Rendering → MultiLobe Specular BRDF` — включён
   (плагин в папке проекта включается автоматически; если нет — включить, рестарт).

## 3. Первый запуск и настройка

Рекомендуется **до** первого запуска добавить в `<Проект>/Config/DefaultEngine.ini`:

```ini
[ConsoleVariables]
r.ShaderDevelopmentMode=1
```

Без этого ошибка в HLSL уронит редактор вместо диалога Retry (важно на этапе отладки патчей).

При старте плагин:
1. Копирует `<Engine>/Shaders` → `<Проект>/Saved/MultiLobeSpec/Shaders` (~единицы секунд, один раз;
   копия переиспользуется, пересоздаётся при смене версии движка или версии патчера).
2. Патчит в копии `SpecularGGX`, `EnvBRDF`, `EnvBRDFApprox` (клонирует оригинал + ставит dual-lobe обёртку).
3. Перемапливает виртуальную директорию `/Engine` на оверлей.
4. Запускает `RecompileShaders Changed`.

**Первая компиляция активного пресета — минуты** (BasePass — самый жирный пермутационный куст).
Дальше каждый пресет лежит в DDC отдельным ключом, и переключение между уже собранными
пресетами — секунды.

Настройки: **Project Settings → Plugins → MultiLobe Specular**:

| Параметр | Смысл |
|---|---|
| Preset | Off / Subtle / Cinematic / Custom |
| Lobe2Weight | вес второго лоба для аналитических источников (0..0.6) |
| EnvLobe2Weight | вес второго лоба в EnvBRDF — отклик отражений/IBL |
| Lobe2RoughnessScale/Offset | roughness второго лоба = `saturate(R * Scale + Offset)` |

Любое изменение → перезапись конфиг-`.ush` → авто-`RecompileShaders Changed`.

Консольные команды: `MLS.Apply`, `MLS.Disable`, `MLS.Preset 0..3` (Off/Subtle/Cinematic/Custom).

## 4. Как быстро увидеть эффект

Тестовая сцена: линейка сфер с Metallic=1, Roughness 0.05 → 0.5 (шаг 0.05) + вторая линейка
диэлектриков (Specular 0.5). Освещение: один яркий Point/Spot + HDRI-скайлайт с контрастным
окружением (окна/солнце).

Что должно измениться при `Cinematic` vs `Off`:
- у бликов аналитических источников появляется широкий ореол вокруг яркого ядра
  (вместо «стерильного» одиночного пятна GGX);
- отклик отражений (EnvBRDF) становится «жирнее» на средних roughness — металл читается
  как реальный металл с двумя сосуществующими roughness, а не как плоская хром-заглушка;
- полированные поверхности (R < 0.1) получают лёгкую дымку поверх зеркального отражения.

Для A/B: `MLS.Preset 2` ↔ `MLS.Preset 0` (после первой сборки обоих состояний переключение быстрое).

## 5. Ограничения MVP (осознанные)

- **Blur-профиль лучей Lumen/SSR — single-lobe**: трассировка идёт по авторскому roughness,
  двойное размытие самих отражений не делается. Dual-lobe применяется в *отклике* (EnvBRDF/
  EnvBRDFApprox) и в прямом свете. Если после теста захочется «двойного размытия» именно в
  зеркальных отражениях Lumen — это итерация 2 (патч LumenReflections resolve, дороже и по
  перфу, и по сложности).
- Path Tracer не тронут — референсы в PT будут single-lobe.
- Анизотропный оверлоад `SpecularGGX(ax, ay, ...)` не патчится (материалы с Anisotropy
  останутся single-lobe).
- ClearCoat использует собственный `DualSpecularGGX` — не затрагивается (и не должен).
- **Cooked-билды не поддержаны**: плагин активен только в редакторе. Для упаковки нужен
  ранний LoadingPhase + активный маппинг в cook-коммандлете — вне скоупа MVP.
- Mobile/forward пути частично зацепятся через `EnvBRDFApprox`, но не валидировались.

## 6. Точки риска при первой компиляции (код написан против 5.7 «вслепую»)

Проверить при ошибках сборки C++ (все — в `Engine/Source/Runtime/RenderCore/Public/ShaderCore.h`):

1. `AllShaderSourceDirectoryMappings()` — должна существовать и возвращать
   `const TMap<FString,FString>&`. Если имя/сигнатура в 5.7 отличается — поправить
   `RemapEngineShaders()` в `MultiLobeSpec.cpp`.
2. `ResetAllShaderSourceDirectoryMappings()`, `AddShaderSourceDirectoryMapping()`,
   `FlushShaderFileCache()` — аналогично.
3. `FPlatformProcess::ShaderDir()` — если отсутствует, fallback на `<Engine>/Shaders` уже в коде.

При ошибках в рантайме смотреть лог `LogMultiLobeSpec`:

- **«SpecularGGX … not found»** — движок переложил функцию в другой файл. Найти определение
  (`grep -r "float3 SpecularGGX" <Engine>/Shaders/Private`) и добавить файл в
  `MLS_CandidateFiles` в `MultiLobeShaderPatcher.cpp`. После правки удалить
  `Saved/MultiLobeSpec/` (или поднять `PatchVersion`) и перезапустить.
- **Шейдерная ошибка компиляции после патча** — открыть патченный файл в
  `Saved/MultiLobeSpec/Shaders/Private/`, найти `_MLS_Orig` и обёртку, сверить руками.
  Патченный файл — обычный текст, править можно прямо там + `Ctrl+Shift+.` (recompile changed);
  но правки перетрутся при пересоздании оверлея.
- **Откат в ноль**: Preset = Off (или `MLS.Disable`), либо просто отключить плагин —
  движковые файлы не менялись, ломаться нечему.

## 7. Как устроен патч (для Architect/Worker-декомпозиции)

```
BuildOverlay
 ├─ EnsureOverlayCopy      — копия <Engine>/Shaders, штамп = EngineVersion|PatchVersion
 ├─ для каждого кандидата (ShadingModels.ush, BRDF.ush, …):
 │   ├─ FindNextFunctionDef — поиск определения по имени: identifier boundary,
 │   │                        скобочный матчинг параметров, `)` → `{`, brace-матчинг тела
 │   ├─ фильтр оверлоадов   — патчим только те, где есть параметр `Roughness`
 │   ├─ клон verbatim       — `<Name>_MLS_Orig` (тело движка НЕ переписывается → версионная устойчивость)
 │   └─ обёртка             — lerp(Orig(MLS_R1(R),…), Orig(MLS_R2(R),…), weight); convex blend → энергия сохранена
 ├─ EnsureConfigInclude    — `#include "/Engine/Private/MultiLobeSpecConfig.ush"` после `#pragma once`
 └─ WriteConfigFile        — MLS_ENABLED / веса / MLS_R2(r) = saturate(r*Scale+Offset)

Remap: снапшот таблицы виртуальных маппингов → Reset → перерегистрация с подменой "/Engine".
Recompile: FlushShaderFileCache() + Exec("RecompileShaders Changed").
Fail-safe: любой сбой патча → маппинг НЕ применяется, проект живёт на ванильных шейдерах.
```

---

# v0.2 — что нового

1. **AgX / AgX Punchy тонмаппер** (переключатель `Tonemapper` в настройках, default: AgX Punchy).
   Реализация: свой `MLS_AgX.ush` в оверлее + перенаправление всех вызовов `FilmToneMap` в
   `PostProcessCombineLUTs.usf` на обёртку `MLS_FilmToneMap`. Замена сделана в точке бейка LUT,
   поэтому color grading из Post Process Volume, композиция блума и film-контролы сохраняются.
2. **Фикс «упавшего» ядра блика**: веса пресетов снижены (Cinematic 0.35→0.22), добавлен
   `CoreFade` — вес второго лоба = `W * saturate(R * CoreFade)`: зеркальные поверхности
   сохраняют острое ядро, дымка нарастает с roughness.
3. **Diffuse model switch**: Lambert (default) ↔ `Diffuse_Chan` (уже реализован в движке,
   GGX-консистентный, retro-reflection на шероховатых под скользящим светом). Патч call-site
   в `DefaultLitBxDF`.
4. Все патчи теперь ставятся всегда, поведение управляется только define'ами в конфиге —
   переключение любых опций не требует пересоздания оверлея, только recompile changed.

При обновлении с v0.1: просто пересобрать плагин — `PatchVersion` поднят до 4, оверлей
пересоздастся сам при первом запуске.

## Калибровка AgX (важно при первом запуске)

Точное цветовое пространство на входе/выходе `FilmToneMap` немного гуляет между версиями
движка. В `MultiLobeSpecConfig.ush` (генерируется в `Saved/MultiLobeSpec/Shaders/Private/`)
есть три переключателя, дефолты выставлены под типовой вызов `FilmToneMap(ColorAP1)`:

| Define | Симптом, если выставлен неправильно |
|---|---|
| `MLS_AGX_INPUT_AP1` | общий сдвиг оттенков/перенасыщение → попробовать 0 |
| `MLS_AGX_OUTPUT_AP1` | ненасыщенная/зеленоватая картинка → попробовать 0 |
| `MLS_AGX_OUTPUT_LINEARIZE` | картинка сильно темнее или «молочная» (двойная гамма) → инвертировать |

Править можно прямо в сгенерированном файле + `Ctrl+Shift+.` для быстрой итерации; после
подбора — скажи мне значения, я зашью их в генератор. Валидация: красный/синий эмиссив с
интенсивностью 50+ — под ACES уходят в оранжевый/фиолетовый, под корректным AgX держат оттенок
с мягким уходом в белый.

## Быстрый A/B чеклист v0.2

1. `MLS.Preset 2` + Tonemapper=EngineACES → скриншот.
2. Tonemapper=AgXPunchy → скриншот. Ореолы бликов должны «проявиться».
3. DiffuseModel=Chan → смотреть матовые диэлектрики под скользящим светом.
4. Дополнительно: `r.Material.EnergyConservation=1` в ConsoleVariables.ini —
   встроенная в движок multi-scatter компенсация (шероховатые металлы светлее/корректнее).

---

# v0.2.1 — фиксы тонмаппера (сверка с реальным 5.7 и drop-in'ом TonemapOverride)

1. **ASC CDL luma-фикс** в AgX Punchy: luma теперь считается после pow (как требует CDL и
   как исправил автор TonemapOverride в комментариях; его скачиваемый файл всё ещё со старым
   порядком).
2. **Отключено движковое `/ 1.05`** на выходе LUT при активном MLS-тонмаппере — это
   ACES-специфичная компенсация, из-за неё кастомные тонмапперы выходили ~5% темнее
   («серый слой»).
3. Калибровочные дефолты (`INPUT_AP1 / OUTPUT_AP1 / LINEARIZE = 1`) подтверждены по
   оригинальному `PostProcessCombineLUTs.usf` из 5.7 — трогать их не должно понадобиться.
4. Осознанное отличие от TonemapOverride: мы делаем AP1↔sRGB с хроматической адаптацией
   (D60→D65, как в референсном Blender-пайплайне), они — без. Разница — доли процента в
   оттенке ярких участков.

`PatchVersion` поднят до 5 — оверлей пересоздастся автоматически.

---

# v0.3.0

1. **Кнопка Apply Changes** прямо на странице Project Settings → Plugins → MultiLobe Specular.
   Изменения настроек больше НЕ запускают перекомпиляцию сами — они копятся и применяются
   кнопкой либо консолью (`MLS.Apply` / `MLS.Preset` / `MLS.Tonemap` применяют сразу).
2. **Micro-Shadowing** (Chan, CoD:WWII): AO-канал материала трактуется как конус видимости
   и гасит ПРЯМОЙ свет вне конуса. Включается в секции "Micro Shadowing"; `MicroShadowSpecular`
   регулирует силу для спекуляра (diffuse гасится полностью). Требование к контенту: в AO-пин
   материала должен идти cavity/short-ray AO (не крупноформенный бейк), AO=1 → эффекта нет.
3. `PatchVersion` = 7, оверлей пересоздастся автоматически.

---

# v0.3.1 — BRDF-aware micro-shadowing

Новый переключатель `Specular Model` в секции Micro Shadowing:
- **Simple (NoL cone)** — Activision-терм, тень по геометрической нормали.
- **BRDF-aware (GGX, analytic)** — аналитическая свёртка VNDF-подхода (irradiance.ca, part 2):
  спекуляр гасится по активным микрофасетам вокруг H с шириной перехода ~GGX alpha.
  Детерминировано, без шума. Diffuse в обоих режимах — NoL-конус.

Ожидаемая разница: на гладких поверхностях блик режется краем каверны резко, на шероховатых —
плавно; блики и diffuse больше не гасятся одной маской (правильная асимметрия).
`PatchVersion` = 8.

---

# v0.4.0 — Lumen Dual Blur (EXPERIMENTAL)

Двойное размытие отражённой картинки в Lumen: часть лучей отражений (Lumen Blur2 Weight)
стохастически трассируется широким лобом на этапе генерации лучей; темпоральная аккумуляция
сводит смесь в «резкое отражение + мягкое гало». Секция настроек "Lumen Dual Blur",
по умолчанию ВЫКЛЮЧЕНО.

Требования и ожидания:
- Lumen temporal accumulation должен быть включён (дефолт). При выключенном — шум.
- Эффект сходится за несколько кадров: для скриншотов дай камере постоять ~секунду.
- Перф не меняется (число лучей то же), CoreFade защищает зеркала.
- Смотреть на: лужи/полировка/глазурь R 0.05–0.25 с ярким контрастным отражением.

Точки риска (код против 5.7 вслепую): анкер — вызовы `ImportanceSampleVisibleGGX` в
`Lumen/LumenReflections.usf` (+ Common/Tracing). Если в логе предупреждение
"call sites not found" или шейдерная ошибка в этих файлах — прислать оригинальный
`Engine/Shaders/Private/Lumen/LumenReflections.usf` для подгонки анкеров.
`PatchVersion` = 9.

---

# v0.4.1 — Lumen dual-blur сверен с реальным 5.7

Враппер переписан по фактическому `LumenReflections.usf`: стратификация по сырому E.x
(BlueNoiseVec2, темпорально меняется — смесь сходится; GGXSamplingBias трогает только E.y),
основной оверлоад — float2 Alpha (= roughness²). Один call-site, строка ~384.
Известное ограничение движка: при Roughness < 0.001 Lumen минует сэмплирование (чистый
reflect) — идеальные зеркала гало не получают by design. `PatchVersion` = 10.

---

# v0.4.2 — hotfix

`NoL` в патчах micro-shadowing и Diffuse Chan больше не берётся из параметра функции
(в 5.7 перегрузка `DefaultLitBxDF` его не объявляет) — вычисляется на месте как
`saturate(dot(N, L))`. Латентный баг существовал с v0.3.0 и вскрылся при инвалидации
DDC-кэша FDeferredLightPS. `PatchVersion` = 11.

---

# v0.4.3 — сверка с реальным ShadingModels.ush 5.7

В 5.7 изменилась сигнатура BxDF (старая помечена UE_DEPRECATED): NoL/Falloff/направление
света живут в FAreaLight. Исправлено по фактическому коду:
- micro-shadow: NoL = AreaLight.NoL, блок обёрнут в `if (AreaLight.NoL > 0)` (NaN-safety),
  спекуляр-терм читает Context.NoH/VoH.
- diffuse-переключатель пересажен с удалённого Diffuse_Chan на Diffuse_GGX_Rough (5.7).
- бонус от новой сигнатуры: анизотропный SpecularGGX теперь тоже dual-lobe (оба
  определения имеют параметр Roughness).
`PatchVersion` = 12.

---

# v0.5.0 — корректность (по внешнему ревью) + P1

Терминология: модель официально называется **Horizontal Dual-Roughness GGX** — горизонтальная
смесь двух roughness-распределений одной поверхности (дымка/неоднородная полировка). Это НЕ
coating/лак/мокрость — для них нужен вертикальный слой с отдельным интерфейсом. Convex blend
не создаёт энергию (passivity), но НЕ компенсирует multiscatter-потери single-scattering GGX.

Исправления патчера:
- штамп оверлея пишется ПОСЛЕДНИМ, только после успеха всех патчей (устранён сценарий
  «валидный штамп у полусобранного оверлея»);
- RemapEngineShaders возвращает статус; bOverlayActive только после подтверждённого ремапа;
  лог ошибки Apply показывает фактическое состояние;
- при Disable восстанавливается прежнее значение маппинга /Engine (не физическая директория).

Модель/интеграция:
- Rough Diffuse получает движковый GetAreaLightDiffuseMicroReflWeight(AreaLight) вместо 1.0;
- один физический вес лоба для direct/EnvBRDF/Lumen; отдельная Sampling Probability — только
  явным override;
- **Two-sample IBL** (вкл. по умолчанию): radiance captures/skylight сэмплится на лоб
  (+1 выборка кубмапы). Слепой анкер: CompositeReflectionCapturesAndSkylight — при warning
  в логе прислать ReflectionEnvironmentComposite.ush;
- **Rect Light LTC** — dual-lobe через клон RectGGXApproxLTC (слепой анкер, при warning
  прислать RectLight.ush);
- Debug Views: Effective Lobe Weight / Second-Lobe Roughness;
- дефолты для честного A/B: Preset=Off, Tonemapper=Engine; micro-shadow spec-модель
  переименована в "GGX-aware heuristic (experimental)".

Актуализация ограничений: анизотропный SpecularGGX ПОКРЫТ (оба определения в 5.7 имеют
параметр Roughness — старое утверждение README устарело). Известные не-dual пути: Path
Tracer, forward/mobile.

Протокол честного A/B (fixed exposure): PPV → Exposure → Metering Mode = Manual,
фикс. Exposure Compensation; тонмаппер одинаковый в обеих сторонах сравнения; SDR.
Temporal-тесты dual-blur: статика / медленный pan / disocclusion / малый яркий эмиссив.
`PatchVersion` = 13.

---

# v0.5.1 — анкеры IBL/Rect сверены с реальным 5.7

- Two-sample IBL: цель — `CompositeReflectionCapturesAndSkylightTWS` (базового имени в 5.7
  нет); обёртка под препроцессорным гейтом `MLS_TWO_SAMPLE_IBL` — при выключенной фиче цикл
  по капчам НЕ дублируется (ноль стоимости).
- Rect LTC: патчатся только внутренние перегрузки `RectGGXApproxLTC` (с
  `OutMeanLightWorldDirection`); тонкие форвардеры не трогаются — исключено двойное
  применение смеси.
Ожидаемые строки лога: `Patched Private/ReflectionEnvironmentComposite.ush:
CompositeReflectionCapturesAndSkylightTWS x1` и `Patched Private/RectLight.ush:
RectGGXApproxLTC x2`. `PatchVersion` = 14.

---

# v0.6.0

1. **AgX независим от BRDF**: тонмаппер работает и при Preset=Off — «Off + AgX» = ванильный
   BRDF + AgX (Off теперь жёсткая гарантия ванильного шейдинга: все BRDF-опции, включая
   micro-shadowing/diffuse/dual-blur, принудительно выключаются). `MLS.Tonemap` больше не
   требует активного пресета.
2. **Пер-лоб energy conservation** (§7 ревью): EnergyPreservation/Conservation смешиваются
   по обоим лобам (анкеры из реального ShadingModels.ush; лог: `EnergyMix x3/3`).
3. **Mixture-aware Lumen resolve**: радиус кернела реконструкции и effective roughness
   спатиального фильтра считаются от roughness смеси при включённом dual-blur (лог:
   `Lumen resolve: 2 ... site(s)`). Это resolve-слой; temporal-тесты (pan/disocclusion/яркий
   эмиссив) остаются рекомендованной валидацией, но не блокером.
`PatchVersion` = 15.

---

# v0.6.1 — P0 второго ревью

Признанные несоответствия README↔код исправлены: GetAreaLightDiffuseMicroReflWeight теперь
РЕАЛЬНО в коде (дословный движковый вызов с локальными до-морфными NoV/VoH/NoH); конфиг
пишется до штампа. MLS.Apply → ApplyFromSettings (Off+Engine не мапит оверлей). DebugView/
TwoSampleIBL/diffuse жёстко за Preset≠Off (C++ + MLS_ENABLED в HLSL). PatchEnergyMix за
MLS_ENERGY_MIX=0 (cross-terms; до пер-лоб применения). Lumen: mixture-PDF (самодостаточная
VNDF-математика), sampling override удалён (q=w), ReconR — alpha-space от фактического
веса сэмплинга. PreviousEngineMapping сбрасывается после restore. CoreFade → «Haze Onset».
README разделён: текущее состояние / CHANGELOG. PatchVersion=16.

---

# v0.7.0 — Paired IBL (принцип «пары, а не произведение смесей»)

Captures/skylight: парная per-lobe композиция Σ w·Lᵢ·Bᵢ прямо в call-site
ReflectionEnvironmentPixelShader.usf, обе ветки отклика (EnergyTerms.E при
USE_ENERGY_CONSERVATION — важно: до этого на включённой energy conservation отклик был
целиком однолобым; EnvBRDF через однолобые клоны). Lumen/SSR-часть буфера восстанавливается
вычитанием L1 и получает mixture-отклик (lobe-ID pairing — следующий шаг). Clear coat
дегенерирует к оригинальной математике (вес 0). Mixed-radiance TWS-режим v0.5 выведен из
эксплуатации (MLS_TWO_SAMPLE_IBL=0 навсегда). MegaLights: вне матрицы (не используется
студией). PatchVersion=17.

---

# v0.7.1

Новый editor-модуль MultiLobeSpecEditor (начало Editor/Runtime-разделения): кнопка
**MLS Apply** в главном тулбаре вьюпорта — обход несрабатывающей CallInEditor-кнопки в
Project Settings 5.7. Консоль: `MLS.DebugView 0|1|2`. Шейдерная логика не менялась
(PatchVersion остаётся 17 — оверлей не пересоздаётся).

---

# v0.8.0 — хвост v0.7: пер-лоб энергия, Rect-адаптер, mixture-числитель resolve

1. Пер-лоб energy conservation ВКЛЮЧЕНА по-настоящему: компенсация умножается на каждый лоб
   ВНУТРИ SpecularGGX-обёртки (и Rect-адаптера); глобальная спекуляр-компенсация в
   DefaultLitBxDF для MLS пропускается (двойного применения нет). Diffuse preservation —
   смешанная (первое приближение по ревью).
2. Rect LTC: специализированный адаптер вместо generic-обёртки — отдельные направления на
   лоб, luminance-взвешенное среднее в OutMeanLightWorldDirection, пер-лоб энергия;
   в RectLight.ush инжектится ShadingEnergyConservation.ush (pragma once — безопасно).
3. LumenReflectionResolve: SampleNDF = mixture D (согласован с mixture PDF из v0.6.1) —
   закрыт числитель §5; лог ожидает 3 сайта.
Остаток по Lumen: mixture-отклик композита (истинный lobe-ID/двухлучевой reference =
хирургия лейаутов буферов — вынесено в отдельное решение). PatchVersion=18.

---

# v0.8.1 — hotfix линковки

`FMultiLobeSpecModule` экспортирован (`MULTILOBESPEC_API`) — editor-модуль ссылается на него
кросс-модульно с v0.7.1, но ошибка проявилась только сейчас: Rebuild пересобрал editor-модуль
против свежего runtime. Шейдерная логика не менялась (PatchVersion 18).

---

# v0.9.0 — GT Uchimura

Тонмапперы 3/4: GT (Uchimura, CEDEC 2017) и GT High Contrast (a=1.2, m=0.18, c=1.45).
Референсная трёхсекционная кривая (toe/linear/shoulder), поканально, тот же цветовой тракт,
что AgX (AP1↔sRGB c CAT + gamut compress), но выход display-linear без 2.2-шага.
Дефолтный тонмаппер при активном плагине — GT Uchimura (внимание: сохранённый в
DefaultEngine.ini выбор переопределяет дефолт — для перехода один раз MLS.Tonemap 3).
PatchVersion=19 (MLS_AgX.ush регенерируется).

---

# v0.10.0 — нативный узел MLS Height Blend Cavity

Материальный узел в палитре (категория MultiLobeSpec), runtime-модуль: сабкласс
UMaterialExpressionCustom с предзаполненным HLSL. Пины: Tex1..3 (TextureObject),
UV (общий), Weights (float3 сырых b1..b3 из графа бленда — нормализация внутри).
Параметры (mip-радиус/сила/гамма/стык) — свойства узла, заходят в код define'ами
(правка свойства пересобирает материал). Выход Float1 -> AO-пин. Бленд остаётся
в нодах мастера — узел потребляет его веса, дублирования логики нет.
PatchVersion не менялся (оверлей не пересоздаётся).

---

# v0.10.1 — baked-режим узла cavity

MLS Height Blend Cavity двухрежимный: Use Baked AO (дефолт ON) переключает пины на
AO1..AO3 (предзапечённые _CavityAO из ue_normal_to_height.py, horizon-scan) — смесь по
весам бленда, +1 tap на слой, Activision-класс качества; OFF — прежний рантайм
мип-режим (Tex1..3, фоллбек). BakedIntensity, стыки слоёв работают в обоих режимах.

---

# v0.10.2 — узел cavity: смена механизма

Нативный сабкласс UMaterialExpressionCustom УДАЛЁН: в 5.7 класс не экспортирован из
Engine-модуля (виртуальные методы без ENGINE_API, в т.ч. новый Build(MIR::FEmitter)) —
наследование из плагина не линкуется принципиально. Замена: генератор Material Function
ассетов (mls_make_cavity_mf.py, MaterialEditingLibrary) — MF_MLS_CavityBaked и
MF_MLS_CavityMip в /Game/MLS/, та же математика, палитровый UX без единой строчки линковки.

---

# v0.11.0 — окно MLS Baker

Editor-таб (кнопка MLS Baker в тулбаре рядом с MLS Apply), два раздела:
1) Offline bake: Normal -> Height (coarse-to-fine Jacobi-релаксация по WWII-статье,
   wrap/тайлинг) -> Horizon AO -> ассет <base>_tex_ao (G16, TC_Grayscale, sRGB off) рядом
   с нормалью. Find From Selection: текстуры/статик-меши в Content Browser + акторы сцены,
   фильтры parent-материала (список) и суффикса нормали (_tex_n). Настройки AO в окне.
2) Assign: по инстансам выбранного — BaseColor-параметры (суффикс _tex_bc) -> поиск
   <base>_tex_ao в той же папке -> назначение в TextureObject-параметры AO1..AO3.
Предпосылка: в мастере AO-текстуры MF заведены как TextureObjectParameter AO1..AO3.

---

# v0.11.1 — hotfix сборки Baker

InputCore в зависимости editor-модуля (EKeys для SListView); GetUsedTextures переведён на
актуальную 5.7-перегрузку; EditorScriptingUtilities добавлен в Plugins-зависимости
дескриптора.


---

# v0.11.3 — deterministic Baker editor link fix

- `InputCore` made an explicit public dependency of `MultiLobeSpecEditor`; this is the
  module that owns exported `EKeys`/`FKey` symbols.
- The normal-map result list was display-only, so `SListView`/`STableRow` were replaced
  by a passive scrollable `STextBlock`. This removes all inline key-navigation references
  that produced the eight `EKeys::*` unresolved externals in UE 5.7.
- No shader or bake mathematics changed in this hotfix.

---

# v0.12.2 — VNDF Cone micro-shadowing + interreflection для indirect

1. Спекулярный micro-shadow: рамп-эвристика заменена на generic VNDF cone —
   аналитическое пересечение сферических кэпов (конус видимости из AO вокруг N ×
   конус лоба вокруг L, апертура k*alpha); GTAO-класс specular occlusion. Display:
   "VNDF Cone (generic)". Табличный LUT-путь (static const массив в конфиге) — резерв.
2. Indirect diffuse (не-Lumen ветка DiffuseIndirectComposite): Material.MaterialAO
   получает albedo-aware interreflection Чана (V/(1-a(1-V)), a=0.35 в конфиге) вместо
   raw — снят double-occlusion с направленным micro-shadowing; экранный AO не трогается.
   Lumen SPG применяет材 material AO внутри себя — отдельный анкер (нужны файлы
   LumenScreenProbeGather) — задокументировано.
3. Микротени остаются свойством направленного света (подтверждено наблюдением по статье):
   в indirect добавляется только корректный подъём, не микротень. PatchVersion=20.

---

# v0.12.2-addendum — аудит Lumen SPG (без изменений кода)

По файлам LumenScreenProbe* (5.7): материальный AO применяется к indirect diffuse
ЕДИНОЖДЫ и уже через AOMultiBounce(DiffuseAlbedo, MaterialAO) — albedo-aware
интеррефлексия с настоящим пер-пиксельным альбедо (LumenScreenProbeGather.usf:1421,
1327, 1010). Double-occlusion в Lumen-ветке отсутствует by engine design; патч
MLS_INDIRECT_VIS актуален только для не-Lumen конфигураций (страховка). Visibility-
цепочка закрыта по всем веткам: direct = VNDF Cone (raw), indirect Lumen = движковый
AOMultiBounce, indirect non-Lumen = Chan-патч. Пересборка не требуется.

---

# v0.12.3 — AO = Micro-Visibility Only

Исправление подмены задачи из v0.12.2: требование было "AO-карта НЕ затемняет indirect
вообще" (WWII-семантика), а не "затемняет однократно с multibounce". Новый режим
(дефолт ON, гейт Preset≠Off): материальный AO полностью исключён из indirect —
Lumen SPG: три анкера (per-ray occlusion bit -> false; оба AOMultiBounce-умножения
пропущены), байт-точно по присланному LumenScreenProbeGather.usf; не-Lumen composite:
MaterialAO убран из FinalAmbientOcclusion (экранный AO остаётся). AO-пин теперь читает
только direct micro-shadowing (VNDF Cone). Мгновенная проверка диагноза без сборки:
r.Lumen.ScreenProbeGather.MaterialAO 0. Interreflection-режим сохранён как фоллбек при
выключенной галке. PatchVersion=21.
