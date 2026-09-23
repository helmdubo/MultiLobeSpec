# FogMS Box — руководство пользователя

Для технических художников: как поставить Box с многократным рассеянием в проект UE 5.8. Все цифры взяты из
`FogMS_Prod_Report.md` и измерены на одной тестовой сцене (RTX 3070, Box 32³, UE 5.8.2). В вашей сцене они будут другими.
Помечено «(не проверено)» то, что не подтверждается кодом или отчётом.

## 1. Что это и как работает

Штатный Volumetric Fog считает однократное рассеяние: свет пришёл от источника, один раз рассеялся в тумане и попал
в камеру. В плотном облаке этого мало: свет много раз переотражается внутри среды, поэтому теневая сторона
светлеет, а освещённая мягко «светится». FogMS Box считает это многократное рассеяние внутри своего объёма.

**Путь «инъекция» (основной, работает в редакторе и в игре):**

1. Box читает плотность из вашей Volume Texture и собирает свет: солнце и локальные источники с тенями через
   аппаратный ray tracing, небо (Sky View LUT, кубмапа SkyLight или SH, раздел 5), отражённый от поверхностей свет
   из Lumen surface cache или из публичного отката (`Lumen Bounce`, раздел 4).
2. Решатель переноса на сетке 32³ внутри Box решает задачу «сколько света приходит в каждую ячейку со всех сторон
   с учётом многократного рассеяния». Результат — поле J (падающая яркость на ячейку).
3. J записывается в объёмную текстуру Box (`FogMS_TransportField`, 32³, FloatRGBA). Volume-материал Box
   (`M_FogMS_Density`) отдаёт его в туман как Emissive = σs·J (σs — плотность × альбедо).
4. Штатный Volumetric Fog вокселизирует материал и интегрирует его к камере как обычно: свои фроксели,
   джиттер, временная история.

**Гибрид (`Hybrid Single Scattering`, v2):** прямое солнце в Box рендерит штатный туман: на разрешении фрокселей,
с тенями от геометрии и с фазой `Scattering Distribution` тумана (отсюда ореол вокруг солнца). Поле несёт остальное:
J минус нерассеянное солнце, то есть небо с его ореолом, локальные источники и многократное рассеяние. Штатное
однократное рассеяние всегда складывает солнце + небо + локальные источники, поэтому материал умножает его в каждой
ячейке 32³ на T_sun·k. T_sun — пропускание к солнцу (среда + геометрия), k — доля солнца в нерассеянном падающем
свете ячейки (по яркости). Ночью k → 0, и гибрид превращается в полную инъекцию.

**Контракт материала (для тех, кто делает свой материал).** Box задаёт в MID параметры плотности (`FogMS_Noise`,
`FogMS_ChannelMask`, `FogMS_TileScale`, `FogMS_WorldAligned`, `FogMS_WorldFrequencies`, `FogMS_WorldPhase0..2`,
`FogMS_Threshold`, `FogMS_Softness`, `FogMS_DetailStrength`, `FogMS_DetailScale`, `FogMS_DetailSecondOctave`,
`FogMS_Density`, `FogMS_Albedo`, `FogMS_WorldExtent`, `FogMS_DensityFeather`) и два параметра доставки:

- `FogMS_InjectionMode`: 0 — без инъекции, 1 — полное поле, 2 — гибрид;
- `FogMS_TransportField`: текстура поля, uvw = (Local + Extent) / (2·Extent) в осях Box. Alpha 0 — поля нет,
  материал возвращается к штатному освещению по альбедо; alpha ≥ 0,5 — поле валидно.
  Полное поле: RGB = J, A = 1. Гибрид: RGB = J − нерассеянное солнце, A = 0,5 + 0,5·T_sun·k.

Материал должен считать так: valid = (A ≥ 0,5), Emissive = valid·RGB·Albedo·σt. Режим 1: BaseColor = Albedo·(1 − valid).
Режим 2: BaseColor = Albedo·lerp(1, saturate(2A − 1), valid), где saturate(2A − 1) = T_sun·k. Как устроен сам
`M_FogMS_Density`, (не проверено): .uasset не читается, контракт взят из кода Box.

## 2. Требования и ограничения

| Требование | Где проверяется / что будет без него |
|---|---|
| Windows (Win64), D3D12, Shader Model 6, одна видеокарта | Иначе статус «Live FogMS Box requires single-GPU D3D12/SM6» |
| Аппаратный ray tracing с inline RT (`r.RayTracing=1`) | Иначе «FogMS Box transport requires inline hardware ray tracing…» |
| Lumen Global Illumination на виде | Иначе «World requires Lumen global illumination for this view.» |
| UE 5.8, проверено только на **5.8.2** | Lumen surface cache читается только на 5.8.2; иначе `Lumen Bounce` уходит в откат, решатель работает. Работа на других патчах (не проверено). В `.uplugin` стоит `EngineVersion 5.8.0` |
| `r.RayTracing.Culling 0` (по умолчанию в движке 3) и `r.Lumen.AsyncCompute 0` (по умолчанию 1) | Иначе «World lighting requires r.RayTracing.Culling=0 and r.Lumen.AsyncCompute=0…» |
| `r.LumenScene.GPUDrivenUpdate 0` (так по умолчанию в 5.8) | Проверяется |
| `r.RayTracing.Nanite.Mode 0` (так по умолчанию) | Нужен только для `bounce: Lumen`, иначе откат |
| Exponential Height Fog с Volumetric Fog | Scattering Distribution = 0 нужна только overlay-доставке («Overlay scattering requires fog Scattering Distribution=0…»). С Emissive Injection допустим прямой лепесток |
| Один вид реального времени: не Scene Capture, не стерео | Иначе «World lighting requires one real-time perspective view…» |
| SkyLight в режиме **Real Time Capture**, если есть смена дня и ночи | Статический SkyLight ночью остаётся «дневным» (раздел 7) |

**Кто ставит cvar.** В игре (упакованная сборка или `-game`) Box с включённым `Apply Required Render Settings`
(по умолчанию вкл.) в `BeginPlay` сам ставит `r.RayTracing.Culling 0` и `r.Lumen.AsyncCompute 0` с низким приоритетом
`SetByGameSetting`: значение из ini проекта, device profile или командной строки сохраняется, в лог пишется
«kept… Transport stays off». В PIE и в редакторе опция не работает: нажмите **Enable Indirect Preview** или пропишите
оба cvar в `DefaultEngine.ini`. Кнопка меняет и Lumen Translucency Volume до конца сессии; вернуть — **Restore Standard Lumen**.

**Что требует `-BindlessAll` (только редактор, «Advanced»).** В упакованной игре ключ не читается, поэтому единственный
путь в игре — Transport + Emissive Injection. Без `-BindlessAll` недоступны overlay-доставка B2/B3, Octaves,
`Spatial (Experimental)`, `World (Current Frame)`, A1d/A1e (`Authored Sun Shadow`, `Cast Sun Shadow`, фильтрованный
shadow cache), `r.FogMS.ViewIntegration`, SSFS sky disk, отладочные виды, `r.FogMS.BoxMode 1`. Статус тогда
«…requires -BindlessAll (injection-only: no BindlessAll; overlay features off)», туман освещается штатно.

**Источники света.** Решатель принимает directional, point и spot. Он **отказывает целиком** (Box переходит на штатное
освещение), если в зоне Box есть rect light, static-источник, источник с IES-профилем или light function, более 256
источников, а также солнце с `Cast Cloud Shadows` при включённых Volumetric Clouds. Причина видна в `Spatial Status`:
«B1 world source '<имя>': …».

## 3. Быстрый старт

1. Проверьте требования раздела 2. В логе при старте должна появиться строка
   `FogMS: bindless configuration …, inline RT yes; available modes: injection-only …`.
   Если режимы `all`, редактор запущен с `-BindlessAll`, и тогда после настройки нужна кнопка **Enable Live Box**.
2. Поставьте в уровень **FogMS Box Volume** и задайте размер через масштаб или Box Extent. Включённым и видимым
   в мире должен быть только один Box.
3. Плотность:
   - `Density Enabled` = вкл., в `Density Texture` назначьте Volume Texture;
   - текстура должна быть такой: исходник BGRA8, **Compression = VectorDisplacementmap**, **sRGB выкл.**, color/alpha
     adjustments сброшены, `Flip Green Channel` выкл., каждая ось 1…256, SizeY·SizeZ ≤ 16384, mip 0 не отброшен
     LOD bias или группой текстур;
   - `Density Channel` — канал с шумом. `Density` — пиковая экстинкция в 1/м (по умолчанию 0,1).
     `Density Albedo` — цвет рассеяния, каждый канал в пределах 0…1;
   - формат `T_FogMS_DefaultVolume` из комплекта и то, подходит ли он как стартовая текстура, (не проверено).
4. `Scattering Mode` = **Transport (B3 Angular)**, `Transport Preset` = **Production**.
5. `Emissive Injection` = вкл. Без `-BindlessAll` Box запускается сам: в редакторе на первом тике, в игре
   в `BeginPlay`. Нажимать ничего не нужно.
6. По желанию включите `Hybrid Single Scattering`. Для заметного ореола солнца поставьте у тумана
   `Scattering Distribution` ≈ 0,3–0,6 (предложение, не измерено). На полное поле без гибрида она не влияет.
7. Если статус показывает `bounce: fallback`, задайте `Fallback Ground Albedo` под землю локации (раздел 4).
8. Анимация: `World Aligned Texture` = вкл. (без него анимация не работает, статус «Static: animation requires World
   Aligned Texture»), затем `Animate Density` = вкл. Направление ветра задаётся поворотом стрелки `WindDirectionComponent`,
   скорость — `Wind Speed`, медленное изменение контуров — `Edge Flow Speed` (работает при `Detail Strength` > 0).
9. Посмотрите `Spatial Status`. Рабочее состояние выглядит так:
   `Active B3 isotropic transport (16 directions, tol 1.00e-06; emissive injection via material; hybrid: native single
   scattering; solve every 2 frames; bounce: Lumen; see convergence diagnostics) (injection-only: no BindlessAll;
   overlay features off) [sky: Sky View LUT, 5-tap sector average]`.
   На кадрах удержания вместо `solve every 2 frames` стоит `hold 1/2`; с async-решателем добавится `; one frame late`.

## 4. Настройки Box

**Основные (категории FogMS и FogMS|Scattering):**

| Свойство | Что делает | Цена / рекомендация |
|---|---|---|
| `Enabled` | Включает Box | В мире должен быть один включённый видимый Box |
| `Scattering Mode` | Для продукта — `Transport (B3 Angular)`. `Transport (B2)` — 6 осевых направлений | B2 ≤8 итераций ≈ 1,04 мс, но J ярче эталона на ~11 % (ошибка 16 %): кандидат в «Economy», в пресеты не входит |
| `Transport Preset` | Записывает три поля ниже. Если поправить любое из них вручную, пресет станет `Custom` | Production по умолчанию |
| `Angular Quality` | Число направлений переноса: 16/24/48/96 | Цена растёт линейно. Ошибка поля J к эталону 96/64: 16 → 2,2–2,5 %, 24 → 1,2 %, 48 → 0,8 %. Время решателя: 2,26 / 2,75 / 3,94 мс |
| `Transport Iterations` | До N итераций решателя за кадр (1…64). Благодаря warm start решение продолжается между кадрами | 2–4 уже рабочий бюджет. 96 направлений: 4 ит. — 11,1 мс, 16 ит. — 28,1 мс |
| `Transport Tolerance` (Advanced) | Порог сходимости: когда он достигнут, оставшиеся итерации кадра пропускаются. −1 — взять из `r.FogMS.Transport.Tolerance` | 1e-4 слишком грубо (тусклые ячейки до 3–8 %), 1e-6 рабочий, 1e-8 для High. 16 направлений: 2,3 / 3,4 / 5,2 мс |
| `Emissive Injection` | Доставляет J через Volume-материал Box (раздел 1) | На ~0,7 мс дешевле overlay в `LightScattering` (1,575 → 0,870 мс). Единственный путь без `-BindlessAll` |
| `Hybrid Single Scattering` | Прямое солнце рендерит штатный туман, остальное идёт через поле | +0,02 мс к полной инъекции. Внутри Box на 1,5–2 % ярче overlay |
| `Lumen Bounce` | Свет поверхностей, в которые упираются граничные лучи решателя. Auto — Lumen surface cache (сборка 5.8.2, кэш готов), иначе откат. Off — всегда откат: `Fallback Ground Albedo` × (солнце × тень × пропускание среды Box + SH-небо) | Откат против Lumen: в облаке на 3–5 % светлее (раунд 25, до ослабления средой) |
| `Fallback Ground Albedo` | Линейный цвет земли для отката, по умолчанию серый 0,3. Не действует при `bounce: Lumen` | По локации: снег ~0,8, трава ~0,15, почва/камень 0,2–0,3. Правка пересчитывает поле и один раз сбрасывает историю тумана |
| `Apply Required Render Settings` | В игре ставит два обязательных cvar (раздел 2) | Держать вкл., если проект сам не задаёт эти cvar |
| `Feather Distance`, `Density Edge Feather` | Мягкий край Box и край плотности, см | — |

**Пресеты:**

| Пресет | Направления / итерации / tolerance | Решатель на тестовой сцене |
|---|---|---|
| Production | 16 / ≤16 / 1e-6 | ~2,2–3,5 мс |
| High | 48 / ≤16 / 1e-8 | 5–8 мс (оценка тира в отчёте) |
| Cinematic | 96 / 64 / 1e-14 | 40–78 мс (в разных сессиях); это эталон |

**Плотность (FogMS|Density):** `Density Texture`, `Density Channel`, `World Aligned Texture` + `World Texture Size`
(размер повтора в см), `Texture Offset (World)`, `Tile Scale` (только без World Aligned), `Threshold`/`Softness`,
`Detail Strength`/`Detail Scale`/`Detail Second Octave`, `Density`, `Density Albedo`. Правки видны сразу.

**Анимация (FogMS|Density Animation):** `Animate Density`, `Wind Speed`, `Edge Flow Speed`; кнопки
`FreezeDensityAnimation`/`ResumeDensityAnimation`, `ResetMotionOrigin`; `Use Manual Animation Time` + `Manual Animation Time`
дают воспроизводимый кадр.

**FogMS|Sun** (`Authored Sun Shadow`, `Cast Sun Shadow`, `Filtered Sun Shadow`) и **FogMS|Indirect** работают только
с overlay. Transport не использует `Indirect Shadowing`, `Spatial Strength` и `Spatial Distance`.

## 5. Консольные переменные

**Решатель и источники:**

| Cvar | По умолчанию | Когда трогать |
|---|---|---|
| `r.FogMS.Transport.Tolerance` | 1e-14 | Действует, только если у Box `Transport Tolerance` = −1 |
| `r.FogMS.Transport.WarmStart` | 1 | Не трогать: 0 заставляет решатель начинать с нуля каждый кадр |
| `r.FogMS.Transport.SunAligned` | 1 | Поворачивает набор направлений на солнце. При 16 направлениях ошибка падает с 3,1 до 2,5 % |
| `r.FogMS.Transport.SkipConverged` | 1 | Не трогать |
| `r.FogMS.Transport.SweepThreads` | 1024 | 256/512/1024. Результат тот же, 1024 быстрее на ~6 % |
| `r.FogMS.Transport.DirectSamples` | 4 | Точек прямого света на ячейку: 4 — тетраэдр, чередуется между решениями; 8 — все углы |
| `r.FogMS.Transport.DirectSkipEmpty` | 0 | Только диагностика: 1 затемняет края тумана |
| `r.FogMS.World.SkySource` | 0 | 0 авто: Real Time Capture + SkyAtmosphere → Sky View LUT; статический захват → публичная кубмапа; иначе SH. 2/3/4 — принудительно LUT/кубмапа/SH (недоступный → SH). 1 = 0 с предупреждением в логе. Источник — в статусе `[sky: …]` |
| `r.FogMS.World.SkyLutSamples` | 5 | Выборок Sky View LUT на сектор (1…13) |
| `r.FogMS.World.SunExcludeDegrees` | 3 | Только кубмапа: вырезает солнечный диск, чтобы не считать солнце дважды |
| `r.FogMS.World.SkyMipBias` | 0 | Только кубмапа: больше — небо размытее, меньше — резче |
| `r.FogMS.World.FallbackMedium` | 1 | Откат `Lumen Bounce`: 1 — солнце на земле ослаблено средой Box (под плотным облаком темнее), 0 — только тень геометрии |
| `r.FogMS.World.Indirect` | 1 | Диагностика: 0 — только прямые источники, без неба и отражённого света |

**Доставка, async, удержание:**

| Cvar | По умолчанию | Когда трогать |
|---|---|---|
| `r.FogMS.Transport.AsyncCompute` | 0 | 1 вместе с Emissive Injection: решатель уходит в async-очередь, J приходит в туман на кадр позже. Нужны `r.RDG.AsyncCompute` > 0 и async в RHI, иначе проходы тихо остаются на графике |
| `r.FogMS.Transport.SolveInterval` | 2 | Решать раз в N кадров (1…8), между решениями держать прошлый результат. 4 — для дальних Box, 1 — каждый кадр. Изменение Box, настроек или солнца пересчитывает сразу |
| `r.FogMS.DensityAtlas.ForceGPUCopy` | 0 | A/B-проверка пути упакованной игры в редакторе |

**Диагностика:**

| Команда / cvar | Что делает |
|---|---|
| `FogMS.DumpSpatial <путь>` | Выгружает атлас поля. Без `-BindlessAll` поле **не** выгружается: резидентного атласа нет |
| `FogMS.Status` | Печатает состояние overlay и запрошенные настройки. Только редактор; что он показывает для инъекции, (не проверено) |
| `r.FogMS.Transport.Test*` | Синтетические тестовые входы. После проверки верните 0 |
| `r.FogMS.ViewIntegration`, `r.FogMS.SSFS*`, `r.FogMS.BoxMode`, `r.FogMS.ScreenScatteringSun` | Advanced-режимы, требуют `-BindlessAll`/overlay |

## 6. Стоимость и производительность

Все цифры сняты на тестовой сцене с RTX 3070.

- **Production-решатель:** ≈ 2,2 мс GPU (16 направлений, ≤16 итераций, 1e-6).
- **Инъекция вместо overlay:** `LightScattering` дешевле на ~0,7 мс (1,575 → 0,870 мс), кадр 11,55 → 10,76 мс.
- **Async + инъекция:** на графике остаётся ~0,35–0,46 мс (проходы с ray tracing). Кадр дешевле на ~1,07 мс
  из 2,2 мс, но `ComputeVolumetricFog` дорожает (3,46 → 3,81 мс): делит GPU с async-очередью. Временные буферы
  ~25 МБ при 48 направлениях и ~50 МБ при 96.
- **`SolveInterval 2` (по умолчанию):** на кадрах удержания решатель стоит 0,00 мс. Картинка против N = 1 — 46 дБ
  при N = 2 и 44 дБ при N = 4.
- **`DirectSamples 4` (по умолчанию):** проход 2 на графической очереди 0,313 → 0,107 мс, поле в пределах 0,5 % от 8 точек.
- **Гибрид:** стоит столько же, сколько полная инъекция (+0,02 мс).
- **Штатный туман** при 4 px / 208 слоях стоит ещё ~5–6 мс. Отчёт предполагает для продакшена 8–16 px.

**Что влияет на цену:** число направлений (линейно), итерации и tolerance (решатель останавливается, как только
сошёлся), интервал решения, async. Сетка фиксирована (32³). Зависимость от размера Box на экране и от числа
источников (не проверено).

## 7. Известные ограничения и типичные проблемы

- **Один активный Box на мир.** Если включённых и видимых Box два, эффект отключается («More than one enabled,
  visible FogMS Box…»).
- **Ночь при статическом SkyLight.** Кубмапа статического SkyLight после заката остаётся дневной и продолжает
  подсвечивать Box (как и штатный туман). Для смены дня и ночи включите у SkyLight Real Time Capture.
  С ним в сумерках Box отличался от штатного освещения на 5–7 % (замер до перехода на Sky View LUT).
- **Небо при Real Time Capture** берётся из Sky View LUT: в нём нет облаков и HDRI, поэтому под облачным небом
  Box светлее захвата. Со статическим захватом публичная кубмапа тождественна прежнему пути (раунд 25).
- **Задержка.** При `AsyncCompute 1` поле приходит на кадр позже. При `SolveInterval N` изменения, не запускающие
  пересчёт (локальные источники, небо, Lumen, фаза анимации плотности), приходят с задержкой до N−1 кадров
  (по умолчанию один). Движение солнца пересчитывается каждый кадр. При инъекции J ещё проходит через штатную
  временную историю тумана, поэтому отклик на смену света сглажен.
- **Лампа внутри облака при гибриде.** Штатная часть однократного рассеяния от лампы тоже умножается на T_sun·k;
  остальной её свет идёт через поле 32³, без теней на разрешении фрокселей. Как это выглядит, (не проверено).
- **Приближения гибрида:** k считается по яркости, а не по каналам. Небо из штатного пути (SH Lumen) частично
  перекрывается с секторным небом в поле (+1,5–2 %).
- **Движение Box:** по замыслу, пока Box перетаскивают, он откатывается к штатному освещению и сбрасывает историю
  тумана (не проверено).
- **Статус «Requires…» / «…requires…»** — не выполнено требование раздела 2.
- **«Transport needs Emissive Injection or -BindlessAll…»** — включите `Emissive Injection`.
- **«Waiting for current-frame isotropic transport»** — поле ещё не опубликовано. Если статус не меняется,
  проверьте, что Box виден, а вьюпорт в режиме realtime (не проверено).
- **«Waiting for density atlas GPU upload» / «…waiting for … mip 0 to become resident»** — текстура ещё стримится
  или компилируется. Если статус не уходит, проверьте формат (раздел 3).
- **«Transport unavailable; native lighting with authored density: …»** — решатель отказал, причина после
  двоеточия. Часто это неподдерживаемый источник света.
- **`bounce: fallback (<причина>)`** — Lumen-кэш не используется: `Lumen Bounce` Off, сборка не 5.8.2 или
  «Lumen source waiting…» в первые кадры. Box работает; проверьте `Fallback Ground Albedo`.
- **Упакованная сборка:** собирается (`UnrealGame` Development/Shipping), smoke-тест в `-game` пройден. Полный
  `BuildCookRun` и GPU-путь атласа плотности в настоящей упаковке (не проверено).

## 8. Что в комплекте для проверки

`Tools/FogMSEnergyValidation/ProdProbe/` — скрипты Python и shell для повтора замеров из отчёта. Они управляют
запущенным редактором через мост UE-MCP (`ws://127.0.0.1:9877`); нужны Python 3, numpy и Pillow, а редактор во время
замера должен быть окном на переднем плане.

Замер — `measure.py`, `gpuprofile.py`; A/B — `*ab.sh` и `abinject.sh` (тиры, доставка, гибрид, интервал, async, небо, `Lumen Bounce`,
проход 2); сравнение — `compare.py`, `fielddiff.py`, `resid_stats.py`; сценарии — `nightcmp.py`, `soak.py`,
`nobindless_test.sh`, `pie_test.py`; бесшовный 3D-шум для Volume Texture — `gen_perlin_worley.py`.

Для A/B полей зафиксируйте небо (`r.SkyLight.RealTimeReflectionCapture 0` на время прогона) и сравнивайте
с шумовым полом одинаковой конфигурации. При Real Time Capture одинаковые прогоны сами расходятся до ~1,4 %.
