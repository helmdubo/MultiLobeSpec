# FogMS Box — руководство пользователя

Для технических художников: как поставить Box с многократным рассеянием в проект UE 5.8. Состояние на 2026-09-27: свойства,
умолчания, диапазоны, cvar и команды сверены с кодом коммита `083341c`. Все цифры взяты из журнала
`docs/history/FogMS_Prod_Report.md` и измерены на одной тестовой сцене (RTX 3070, Box 32³, UE 5.8.2). В вашей сцене они будут
другими. Помечено «(не проверено)» то, что не подтверждается кодом или журналом. Архитектура и техдолг — `FogMS_HANDOVER.md`;
ассеты Unreal в репозиторий не входят — раздел 9.

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
`FogMS_Density`, `FogMS_Albedo`, `FogMS_WorldExtent`, `FogMS_DensityFeather`; эрозия и профиль высоты `FogMS_ErosionStrength`,
`FogMS_ErosionDepth`, `FogMS_ErosionMask`, `FogMS_HeightProfile`, `FogMS_HeightBottom`, `FogMS_HeightTop`,
`FogMS_HeightBottomSoftness`, `FogMS_HeightTopSoftness`, `FogMS_HeightAnvilStrength`; с раунда 36 ещё `FogMS_DepthPrefilter` и
`FogMS_PrefilterWavelengths`, префильтр по глубине, раздел 4; с раунда 37 — `FogMS_ForwardStrength`, `FogMS_ForwardG`,
`FogMS_ForwardDepth`, `FogMS_ForwardFloor`, с раунда 38 ещё `FogMS_ForwardEcc`, прямой лепесток, раздел 4
«Multiple Scattering Look»; с раунда 41 — `FogMS_SunDetail`, `FogMS_SunMap`, `FogMS_SunMapCell`, `FogMS_SunMapU/V/W`
(`Sun Detail Shadow`); с раунда 45 — `FogMS_FroxelWeight` (1 — Box во фрокселях, 0 — его рисует облачный хост)) и два
параметра доставки (запись — `FogMS_BoxVolume.cpp`, `WriteDensityParameters`). В MID облачного хоста Box пишет те же
параметры плотности, но вместо префильтра по глубине — `FogMS_CloudPrefilter`, без `FogMS_SunDetail`/`FogMS_SunMap*`/
`FogMS_FroxelWeight`, плюс положение куба `FogMS_CloudBoxCenter`, `FogMS_CloudWorldToLocal0..2`:

- `FogMS_InjectionMode`: 0 — без инъекции, 1 — полное поле, 2 — гибрид, 3 — отладка «только поле» (`Field Only (Debug)`);
- `FogMS_TransportField`: текстура поля, uvw = (Local + Extent) / (2·Extent) в осях Box. Alpha 0 — поля нет,
  материал возвращается к штатному освещению по альбедо; alpha ≥ 0,5 — поле валидно.
  Полное поле: RGB = J, A = 1. Гибрид: RGB = J − нерассеянное солнце, A = 0,5 + 0,5·T_sun·k.

Материал должен считать так: valid = (A ≥ 0,5), Emissive = valid·RGB·Albedo·σt. Режим 1: BaseColor = Albedo·(1 − valid).
Режим 2: BaseColor = Albedo·lerp(1, saturate(2A − 1), valid), где saturate(2A − 1) = T_sun·k. Режим 3 (контракт v4):
поле полное, как в режиме 1, BaseColor = 0 всегда (и без валидного поля), Emissive как в режиме 1. С раунда 41 при `Sun Detail
Shadow` в режиме 2 вместо S = saturate(2A − 1) берётся S_hi узла `FogMS_SunDetail` (перераспределение внутри ячейки). В
`M_FogMS_Density` плагина (ассет в репозиторий не входит, раздел 9) это узлы `FogMS_InjectionAlbedo` (BaseColor) и
`FogMS_EmissiveInjection` (Emissive); режим 3 в нём появляется только после `matedit_density.py` (раздел 4, «Отладка»). С раунда 37 между `FogMS_EmissiveInjection` и
выходом Emissive стоит узел `FogMS_ForwardLobe`: в режиме 2 при `FogMS_ForwardStrength` > 0 он умножает Emissive на
лепесток, иначе возвращает его без изменений (раздел 4, «Multiple Scattering Look»; с раунда 38 — узел v2).

## 2. Требования и ограничения

| Требование | Где проверяется / что будет без него |
|---|---|
| Windows (Win64), D3D12, Shader Model 6, одна видеокарта | Иначе статус «Live FogMS Box requires single-GPU D3D12/SM6» |
| Аппаратный ray tracing с inline RT (`r.RayTracing=1`, в проекте включён Support Hardware Ray Tracing), deferred shading (не Forward) | Иначе «FogMS Box transport requires inline hardware ray tracing…» или «World lighting requires single-GPU deferred D3D12/SM6 with inline hardware ray tracing…» |
| Lumen Global Illumination на виде | Иначе «World requires Lumen global illumination for this view.» |
| UE 5.8, проверено только на **5.8.2** | Lumen surface cache читается только на 5.8.2; иначе `Lumen Bounce` уходит в откат, решатель работает. Работа на других патчах (не проверено). В `.uplugin` стоит `EngineVersion 5.8.0` |
| `r.RayTracing.Culling 0` (по умолчанию в движке 3) и `r.Lumen.AsyncCompute 0` (по умолчанию 1) | Иначе «World requires r.RayTracing.Culling=0 (now 3)…». Box ставит их сам, см. «Кто ставит cvar» |
| `r.LumenScene.GPUDrivenUpdate 0` (так по умолчанию в 5.8) | Проверяется |
| `r.RDG.AsyncCompute` не больше 1 (2 — принудительный async — не поддержан) | Иначе «Forced RDG async compute is unsupported by World.» |
| `r.RayTracing.Nanite.Mode 0` (так по умолчанию) | Нужен только для `bounce: Lumen`, иначе откат |
| Exponential Height Fog с Volumetric Fog | **Ненулевая `Scattering Distribution` тумана требует `Emissive Injection`.** Без инъекции (overlay) нужна 0, иначе поле Box выключается целиком: статус «Overlay scattering requires fog Scattering Distribution=0 (now 0.7): enable Emissive Injection to use a non-zero fog phase.» |
| Один вид реального времени: не Scene Capture, не стерео | Иначе «World lighting requires one real-time perspective view…» |
| SkyLight в режиме **Real Time Capture**, если есть смена дня и ночи | Статический SkyLight ночью остаётся «дневным» (раздел 7) |

**Кто ставит cvar.** Box с включённым `Apply Required Render Settings` (по умолчанию вкл.) сам ставит
`r.RayTracing.Culling 0` и `r.Lumen.AsyncCompute 0` с низким приоритетом `SetByGameSetting` и пишет в лог одну строку
с прежними значениями (`Apply Required Render Settings (<где>, SetByGameSetting): r.RayTracing.Culling 3 -> 0 …`):
- в игре (упакованная сборка или `-game`) — в `BeginPlay` любого включённого Box в режиме Transport или World;
- в редакторе — когда включённый Transport-Box с `Emissive Injection` запускается сам: на первом тике в редакторе
  без `-BindlessAll` и в `BeginPlay` в PIE/Simulate. **Enable Indirect Preview** для такого Box не нужна.

Значение, заданное явно (ini проекта, device profile, командная строка, консоль; в редакторе также значение, которое
вернула **Restore Standard Lumen**), сильнее: Box его не трогает, в логе «kept… Transport stays off», в статусе
«World requires r.RayTracing.Culling=0 (now 3). A higher-priority value (SetByConsole) was kept…». Поставьте 0 сами.
Значения остаются до конца процесса/сессии редактора и после выключения Box. В редакторском мире с `-BindlessAll` Box
сам не запускается и cvar не ставит (его запускает кнопка **Enable Live Box**, которая cvar тоже не ставит): **любому** Box —
и overlay, и Transport с инъекцией — там нужна **Enable Indirect Preview** (она меняет и настройки Lumen Translucency Volume
до конца сессии; вернуть — **Restore Standard Lumen**) или оба cvar в `DefaultEngine.ini`. В PIE/Simulate их ставит
`BeginPlay` (`FogMS_BoxVolume.cpp`, `AutoStartRuntime` / `BeginPlay`).

**Translucency Volume и Transport (раунд 35).** Для Box в режиме Transport кнопка **Enable Indirect Preview** больше
не ставит `r.Lumen.TranslucencyVolume.SpatialFilter 0` и `r.Lumen.TranslucencyVolume.Temporal.Jitter 0`: остаются
значения проекта (в движке по умолчанию 1 и 1). Эти два нуля нужны только ослаблению A1c (`Indirect Shadowing` вне
режимов Transport): оно гасит каждый луч Translucency Volume средой Box по длине именно этого луча.
Transport Translucency Volume не читает, а выключенный фильтр делал штатный Lumen GI тумана блочным (сине-коричневые
квадраты при `Scattering Distribution` 0,7, раунд 34). Если в текущей сессии они уже 0 (прошлая сборка или Box с A1c),
верните `r.Lumen.TranslucencyVolume.SpatialFilter 1` и `r.Lumen.TranslucencyVolume.Temporal.Jitter 1` в консоли или
перезапустите редактор.

**Редактор без `-BindlessAll`.** Если у Box включена `Emissive Injection`, редактор можно запускать с одними
`-d3d12 -sm6`: Box запускается сам, cvar ставит сам. Overlay-путь по-прежнему требует `-BindlessAll`. Box без
инъекции без ключа сам не запускается; после **Enable Live Box** статус «Transport needs Emissive Injection or
-BindlessAll…» (Transport) или «This Scattering Mode requires -BindlessAll…» (другие режимы), `r.FogMS.BoxMode 1` не
ставится, плотность Box видна со штатным освещением тумана.

**Что требует `-BindlessAll` (только редактор, «Advanced»).** В упакованной игре ключ не читается, поэтому единственный
путь в игре — Transport + Emissive Injection. Без `-BindlessAll` недоступны overlay-доставка B2/B3, Octaves,
`Spatial (Experimental)`, `World (Current Frame)`, A1d/A1e (`Authored Sun Shadow`, `Cast Sun Shadow`, фильтрованный
shadow cache), `r.FogMS.ViewIntegration`, SSFS sky disk, отладочные виды overlay (`Field Only (Debug)` работает и без
ключа: это материал), `r.FogMS.BoxMode 1`. Статус тогда
«…requires -BindlessAll (injection-only: no BindlessAll; overlay features off)», туман освещается штатно.

**Источники света.** Решатель принимает directional, point и spot. Он **отказывает целиком** (Box переходит на штатное
освещение), если в зоне Box есть rect light, static-источник, источник с IES-профилем или light function или более 256
источников. Причина видна в `Spatial Status`: «B1 world source '<имя>': …». Солнце с `Cast Cloud Shadows` с раунда 47
принимается (раньше Box выключался с «native volumetric-cloud shadow visibility is not bound»): тень облачного хоста на
земле — раздел 4, «Мягкая тень облака на земле»; тени чужих облаков решатель не видит, статус об этом предупреждает.

## 3. Быстрый старт

1. Проверьте требования раздела 2. В логе при старте должна появиться строка
   `FogMS: bindless configuration …, inline RT yes; available modes: injection-only …` (её печатает только сборка под
   UE 5.8.2).
   Если режимы `all`, редактор запущен с `-BindlessAll`, и тогда после настройки нужна кнопка **Enable Live Box**.
2. Поставьте в уровень **FogMS Box Volume** и задайте размер через масштаб или Box Extent. Box с `Emissive
   Injection` может быть несколько (раздел 7, «Несколько Box»); без инъекции (overlay) — только один.
3. Плотность:
   - `Density Enabled` = вкл., в `Density Texture` назначьте Volume Texture;
   - текстура должна быть такой: исходник BGRA8, **Compression = VectorDisplacementmap**, **sRGB выкл.**, color/alpha
     adjustments сброшены, `Flip Green Channel` выкл., каждая ось 1…256, SizeY·SizeZ ≤ 16384, mip 0 не отброшен
     LOD bias или группой текстур;
   - `Density Channel` — канал с шумом. `Density` — пиковая экстинкция в 1/м (по умолчанию 0,1).
     `Density Albedo` — цвет рассеяния, каждый канал в пределах 0…1;
   - `T_FogMS_DefaultVolume` плагина — заглушка текстурных параметров материалов (линейная копия штатной Volume Texture
     движка), а не стартовая текстура плотности; свою Volume Texture нужно назначить (без неё добавка плотности выключена).
4. `Scattering Mode` = **Transport (B3 Angular)**, `Transport Preset` = **Production**.
5. `Emissive Injection` = вкл. (с раунда 36 это умолчание). Без `-BindlessAll` Box запускается сам: в редакторе на
   первом тике, в игре в `BeginPlay`. Нажимать ничего не нужно.
6. `Hybrid Single Scattering` тоже включён по умолчанию (раунд 36). Для заметного ореола солнца поставьте у тумана
   `Scattering Distribution` ≈ 0,3–0,6 (предложение, не измерено). На полное поле без гибрида она не влияет.
   **И гибрид, и ненулевая `Scattering Distribution` работают только с включённой `Emissive Injection`.** Без неё
   галка гибрида ничего не делает (статус `[Hybrid Single Scattering is ignored: enable Emissive Injection]`, одно
   предупреждение в логе), а ненулевая фаза тумана выключает поле Box целиком (раздел 2).
7. Если статус показывает `bounce: fallback`, задайте `Fallback Ground Albedo` под землю локации (раздел 4).
8. Анимация: `World Aligned Texture` = вкл. (без него анимация не работает, статус «Static: animation requires World
   Aligned Texture»), затем `Animate Density` = вкл. Направление ветра задаётся поворотом стрелки `WindDirectionComponent`,
   скорость — `Wind Speed`, медленное изменение контуров — `Edge Flow Speed` (работает при `Detail Strength` > 0).
9. Посмотрите `Spatial Status`. Рабочее состояние выглядит так:
   `Active B3 isotropic transport (16 directions, tol 1.00e-06; emissive injection via material; hybrid: native single
   scattering; solve every 2 frames; bounce: Lumen; see convergence diagnostics) (injection-only: no BindlessAll;
   overlay features off) [sky: Sky View LUT, 5-tap sector average]`.
   На кадрах удержания вместо `solve every 2 frames` стоит `hold 1/2`; с async-решателем добавится `; one frame late`.
   После скобки `[sky: …]` идут `[tau core ~X, upper bound]` (оценка оптической толщины, раздел 7), затем, если действуют,
   `[debug: field only, …]`, `[sun softness …]`, `[forward lobe …]` (при умолчаниях — `[forward lobe 0.50 g 0.60]`),
   `[sun detail …]`, `[Hybrid Single Scattering is ignored: …]`, статус `Render Path` (`[render: cloud host] […]` или
   `[cloud host: …, froxel fallback]`) и при нескольких Box — `[Box #N]` (`FogMS_BoxRuntime.cpp`, конец сборки статуса).
10. Плотное облако попиксельно: `Render Path` = `Cloud Host` (с раунда 46 — умолчание). Один раз на уровень нажмите на Box
   **Create Cloud Host** (статус без хоста это и подсказывает). Хост вытесняет облака неба; дымку и низкий туман над землёй
   оставляйте в `Froxel Fog` (раздел 4, «Render Path»).
11. Мягкая тень этого облака на земле и лучи в тумане за ним: в консоли `FogMS.CloudHost.SetupShadows` (раунд 47; ставит
   солнцу `Cast Cloud Shadows`, охват 5 км и разрешение ×2), затем сохраните уровень. Подробности — раздел 4, «Мягкая тень
   облака на земле».
12. Тени облаков погоды (раунд 48): **Place Actors → FogMS Weather**, `Weather State` = `DA_FogMS_Weather_Scattered` (или
   Broken / Overcast; ассеты строит `matedit_weather.py`, раздел 9), кнопка актёра **Setup Sun Shadows**. Смена погоды —
   `FogMS.Weather.Set Overcast 10`. Раздел 4а.

## 4. Настройки Box

Новый Box — куб 20 м (`Box Extent` 1000 см). Значения по умолчанию ниже — умолчания класса `AFogMSBoxVolume`
(`Source/MultiLobeSpec/Private/FogMS_BoxVolume.h`).

**Основные (категории FogMS и FogMS|Scattering):**

| Свойство | Что делает | Цена / рекомендация |
|---|---|---|
| `Enabled` | Включает Box | С `Emissive Injection` включённых Box может быть несколько; без инъекции (overlay) — один |
| `Scattering Mode` | По умолчанию `Off (A1 Only)`. Для продукта — `Transport (B3 Angular)`; `Transport (B2)` — 6 осевых направлений. Остальные режимы (`Octaves (Legacy, overlay)`, `Spatial (Experimental)`, `World (Current Frame)`) — legacy-оверлей, только редактор с `-BindlessAll` | B2 ≤8 итераций ≈ 1,04 мс, но J ярче эталона на ~11 % (ошибка 16 %): кандидат в «Economy», в пресеты не входит |
| `Transport Preset` | Записывает три поля ниже: Production 16 направлений / 16 итераций / 1e-6 (по умолчанию), High 48 / 16 / 1e-8, Cinematic 96 / 64 / 1e-14, Custom. В Details эти поля доступны только при `Custom`; запись любого из них из Python/Blueprint сама переключает пресет на `Custom`; Box, сохранённый с другими значениями, загружается как `Custom` | Production по умолчанию |
| `Angular Quality` | Число направлений переноса: 16/24/48/96 | Цена растёт линейно. Ошибка поля J к эталону 96/64: 16 → 2,2–2,5 %, 24 → 1,2 %, 48 → 0,8 %. Время решателя: 2,26 / 2,75 / 3,94 мс |
| `Transport Iterations` | До N итераций решателя за кадр (1…64). Благодаря warm start решение продолжается между кадрами | 2–4 уже рабочий бюджет. 96 направлений: 4 ит. — 11,1 мс, 16 ит. — 28,1 мс |
| `Transport Tolerance` (Advanced) | Порог сходимости: когда он достигнут, оставшиеся итерации кадра пропускаются (пропуск по допуску действует в B3). −1 — взять из `r.FogMS.Transport.Tolerance`, 0 — выполнять все итерации, диапазон −1…1 | 1e-4 слишком грубо (тусклые ячейки до 3–8 %), 1e-6 рабочий, 1e-8 для High. 16 направлений: 2,3 / 3,4 / 5,2 мс |
| `Emissive Injection` | Доставляет J через Volume-материал Box (раздел 1). **По умолчанию вкл. (раунд 36)** | На ~0,7 мс дешевле overlay в `LightScattering` (1,575 → 0,870 мс). Единственный путь без `-BindlessAll`. Убирает дрожание при движении вперёд/назад (тест «сдвиг слоёв», раунд 35: 0,189 → 0,042; Box выкл. 0,035) |
| `Hybrid Single Scattering` | Прямое солнце рендерит штатный туман, остальное идёт через поле. **Только с `Emissive Injection`**, без неё игнорируется (статус и лог это говорят). **По умолчанию вкл. (раунд 36)** | +0,02 мс к полной инъекции. Внутри Box на 1,5–2 % ярче overlay |
| `Lumen Bounce` | Свет поверхностей, в которые упираются граничные лучи решателя. Auto — Lumen surface cache (сборка 5.8.2, кэш готов), иначе откат. Off — всегда откат: `Fallback Ground Albedo` × (солнце × тень × пропускание среды Box + SH-небо) | Откат против Lumen: в облаке на 3–5 % светлее (раунд 25, до ослабления средой) |
| `Fallback Ground Albedo` | Линейный цвет земли для отката, по умолчанию серый 0,3. Не действует при `bounce: Lumen` | По локации: снег ~0,8, трава ~0,15, почва/камень 0,2–0,3. Правка пересчитывает поле и один раз сбрасывает историю тумана |
| `Apply Required Render Settings` | Ставит два обязательных cvar: в игре в `BeginPlay`, в редакторе/PIE/Simulate при самозапуске Box с инъекцией (раздел 2) | Держать вкл., если проект сам не задаёт эти cvar |
| `Field Only (Debug)` (FogMS\|Debug) | Отладка инъекции: вклад решателя без штатного однократного рассеяния. Материал получает режим 3: BaseColor 0, экстинкция прежняя, Emissive = σs·J полного поля (гибрид на это время выключается, поле пересчитывается как полное). Box без текущего поля чёрный | По умолчанию выкл. Нужен материал с контрактом v4: один раз запустите `matedit_density.py` в редакторе, со старым материалом Box светится дважды |
| `Feather Distance` (FogMS, 200 см) | Край области A1 / legacy-оверлея; в пути инъекции и у облачного хоста не действует | — |
| `Density Edge Feather` (FogMS\|Density, 100 см) | Мягкий край плотности внутри Box, см | — |
| `Spatial Strength` (0,35, 0…0,5), `Spatial Distance` (500 см, 10…2000) | Только legacy-режимы `Spatial` и `World` | — |

**Новые умолчания и уже сохранённые Box (раунд 36).** UE пишет в уровень только значения, отличные от умолчаний класса.
Box, сохранённые до раунда 36 с выключенными `Emissive Injection` и `Hybrid Single Scattering` (тогдашнее умолчание),
этих полей в файле не имеют и после загрузки получают новые умолчания: **инъекция и гибрид включатся сами**, Transport-Box
без `-BindlessAll` запустится автоматически. Box в других режимах это не меняет (инъекция действует только в Transport).
Чтобы оставить Box на overlay, снимите обе галки и пересохраните уровень; если снять только инъекцию, статус покажет
`[Hybrid Single Scattering is ignored: enable Emissive Injection]`. Функции overlay (FogMS|Sun, FogMS|Indirect, отладочные
виды) зависят от `-BindlessAll`, а не от этой галки, и ведут себя как раньше. Раунд 37: умолчание `Depth Prefilter`
стало 0 (было 1), поэтому Box, сохранённые в раунде 36 с тогдашним умолчанием 1, загрузятся с префильтром **выкл.**;
значение, выставленное вручную (0,5, 2…), сохраняется. Раунд 38: `MS Contribution` по умолчанию 0,5, поэтому у Transport-Box
с инъекцией и гибридом, где `Forward Scattering` не задавали, лепесток включится сам; свойства раунда 37 переименованы
(ниже, «Multiple Scattering Look», там же старые имена).

**Пресеты:**

| Пресет | Направления / итерации / tolerance | Решатель на тестовой сцене |
|---|---|---|
| Production | 16 / ≤16 / 1e-6 | ~2,2–3,5 мс |
| High | 48 / ≤16 / 1e-8 | 5–8 мс (оценка тира в отчёте) |
| Cinematic | 96 / 64 / 1e-14 | 40–78 мс (в разных сессиях); это эталон |

**Плотность (FogMS|Density):** `Density Enabled` (выкл.), `Density Texture`, `Density Channel` (R), `World Aligned Texture`
(выкл.) + `World Texture Size` (размер повтора, 2000 см, 1…1e8), `Texture Offset (World)`, `Tile Scale` (только без World
Aligned), `Threshold` (0,5) / `Softness` (0,1), `Detail Strength` (0) / `Detail Scale` (4, 0,1…32) / `Detail Second Octave`
(0,5), `Density` (0,1 1/м), `Density Albedo` (белый), `Density Edge Feather` (100 см), `Depth Prefilter` (0). Правки видны сразу.

**Префильтр по глубине (`Depth Prefilter`, FogMS|Density, раунд 36; с раунда 37 по умолчанию 0 — выключен, опция).**
Это обмен облика на стабильность. Замер раунда 36 (реальный проезд вперёд у края облака, разница кадров `d2_blur`,
`results/diag35/b_dolly.json`, ключи `*_r36`; кадры `results/diag36/pf_sheet.png`): 0 → 0,61, 1 → 0,54, 2 → 0,35; вбок
~0,5 при любом значении. Но 1 заметно смягчает облако и заполняет его просветы (покрытие растёт, картинка ярче в
×1,02–1,07), а 2 превращает облако в дымку. С `Emissive Injection` + гибридом дрожание при W/S уже примерно втрое ниже,
чем у overlay (проезд W: 1,85 → 0,62, раунд 35). Рекомендация: 0 для ближних облаков, 0,5–1 для дальних Box или Box
с мелкой деталью. Как это работает. Штатный туман берёт
плотность Box одной точкой на фроксель. Фроксель узкий на экране (4 px), но толстый по глубине: при `GridSizeZ 208`,
`DepthDistributionScale 32` и `View Distance` тумана 10 км слой у камеры ~2,5 м, на 50 м ~3,6 м, на 200 м ~6,8 м. При
движении вперёд/назад слои скользят по мелкому шуму — дрожание (раунд 35). Префильтр ограничивает полосу частот
плотности размером фрокселя в этой точке, как Nubis — расстоянием:

- ширина фильтра w = `Depth Prefilter` · max(dz, dxy). dz — толщина слоя на глубине d по распределению движка: слой
  Z(d) = S·log2(d·B + O), (B, O, S) = `View.VolumetricFogGridZParams`, отсюда dz = ln2·(d·B + O)/(S·B); dxy = 2d/(P00·GridSizeX)
  — ширина фрокселя (P00 = `ViewToClip[0][0]`). Материал читает это из View во время вокселизации, cvar в C++ не нужны;
- масштаб полосы шума λ = период тайла / 4 (код предполагает 4 ячейки шума на тайл, как у Perlin-Worley из
  `ProdProbe/gen_perlin_worley.py`; для других текстур не проверено): база, detail 0, detail 1 —
  `World Texture Size`, он же / `Detail Scale`, / (2·`Detail Scale`); без `World Aligned Texture` период базы — среднее
  геометрическое 2·Extent/`Tile Scale` по трём осям Box. Снятая доля полосы r = saturate(w/λ);
- (a) амплитуда detail-октав × (1 − r), узор эрозии стремится к 0,5; (b) база читается из mip со смещением
  log2(w/dxy) (только если у Volume Texture есть mip-уровни); (c) полоса порога расширяется на снятый шум:
  S_eff = √(Softness² + 20·Δn²), Δn² = Σ(A·σ·r)², σ = 0,2 на канал текстуры (0,4 для 2d − 1), A — амплитуда полосы.

По замыслу покрытие в среднем сохраняется, а мелкая деталь, которую фроксели всё равно не показывают, смягчается; на
деле при 1 покрытие растёт (замер выше). 0 (по умолчанию) — прежняя формула материала (выкл.: множители 1, слагаемые 0,
mip-смещение 0), 1 — один фроксель, 2 — вдвое мягче. Сила зависит от сетки тумана и размера
шума: при слоях ≥ 2,5 м октава с λ ≤ w пропадает целиком (при `World Texture Size` 2000 см и `Detail Scale` 2 — обе
detail-октавы и 50–100 % базы), при `World Texture Size` 20000 см база почти не меняется. Если облако стало слишком мягким,
0,5. Фильтруется только штатный туман (материал Box): решатель (ячейки 32³) видит полную плотность. **Работает только
с `Emissive Injection`** (и в режимах не Transport): overlay-Box сам кладёт в фроксели свою нефильтрованную плотность,
плотность материала у него 0.

**Как обновить материал.** Нужен `M_FogMS_Density` с узлом `FogMS_Extinction_v3`; со старым материалом `Depth Prefilter`
ни на что не влияет. Скрипт только правит уже существующий материал (создать его с нуля не может, раздел 9). Один раз в
редакторе, когда материал никто не редактирует: Python-консоль,
`py "<репо>/Tools/FogMSEnergyValidation/ProdProbe/matedit_density.py"`. Скрипт идемпотентный: если нужно, сначала
поднимает экстинкцию v1 → v2 и BaseColor до контракта v4, затем v2 → v3 (новые узлы `FogMS_DepthFootprint`, PixelDepth,
параметры `FogMS_DepthPrefilter`/`FogMS_PrefilterWavelengths` с умолчанием 0; выборке базового шума ставится Mip Bias).
Он ждёт компиляции шейдеров материала и при ошибке (трансляции или HLSL, по логу редактора) всё откатывает и не
сохраняет. Ожидаемый вывод: `EXTINCTION v2 -> v3 (…; base mip bias <узел>.Bias)`, `COMPILE v3: shader compiled (log …)`,
`PATCHED saved=True`; повторный запуск — `EXTINCTION already v3` и `ALREADY_PATCHED`. `MIP_BIAS skipped: …` значит,
что база читается без смещения mip (работают только (a) и (c)). `COMPILE v3: shader result NOT verified` — лог не найден:
проверьте Output Log на «Failed to compile Material … M_FogMS_Density». После патча поменяйте у Box `Depth Prefilter`
(MID получит новые параметры) или переоткройте уровень. Тот же скрипт с раунда 37 добавляет узел прямого лепестка,
с раунда 38 обновляет его до v2 (ниже, «Multiple Scattering Look»).

**Форма плотности (FogMS|Density, срезы S1/S2).** По умолчанию выключены, и плотность побитово прежняя.
Нужен материал с узлом `FogMS_Extinction` (скрипт `Tools/FogMSEnergyValidation/ProdProbe/matedit_density.py`);
со старым материалом Box эти свойства видят только решатель и тени, штатный туман их не видит.

| Свойство | Что делает | По умолчанию |
|---|---|---|
| `Erosion Strength` | Выедает нижнюю кромку полосы порога каналом второй detail-выборки (без новых чтений текстуры). Только убавляет плотность. Рабочий диапазон 0,1–0,35; 0,6 разбивает облако на клочья (раунд 32) | 0 = выкл. |
| `Erosion Depth` | Насколько глубоко (в единицах шума над `Threshold − Softness/2`) доходит эрозия, [0.01, 1]. Выше ~0,3 «краем» становится почти всё облако и эрозия утончает весь объём (раунд 32) | 0.15 |
| `Erosion Channel` | Канал узора эрозии: G/B/A (в Perlin-Worley там Worley FBM) | G |
| `Height Profile` | Умножает шум на профиль по высоте Box (0 — нижняя грань, 1 — верхняя, ось Z Box) | выкл. |
| `Height Bottom` / `Height Top` | Основание и верх облака, доли высоты Box, Bottom < Top | 0 / 1 |
| `Bottom Softness` / `Top Softness` | Ширина нарастания снизу и спада сверху (малое снизу = плоское основание, большое сверху = купол) | 0.05 / 0.1 |
| `Anvil Strength` | Расширяет покрытие в верхней половине профиля (наковальня), [0, 1] | 0 |
| `Height Profile Preset` | Только редактор: пишет пять значений (Stratus, Cumulus, Cumulonimbus, Valley Fog) и включает профиль. Правка любого из пяти значений вручную ставит None | None |

**Multiple Scattering Look (FogMS|Multiple Scattering Look, раунд 38; в редакторе проверено в раунде 40).** Один набор свойств
для обоих путей, которые добавляют «октавы» многократного рассеяния (Wrenninge 2013), с именами и смыслом, как у материала
UE Volumetric Cloud (узел Volumetric Advanced Material Output). До раунда 38 это были два набора с разными именами:
`Extra Octaves`/`MS Contribution`/`MS Occlusion`/`MS Eccentricity` в FogMS|Scattering (только Octaves) и
`Forward Scattering`/`Forward Anisotropy`/`Forward Depth`/`Back Floor` в FogMS|Look (только лепесток Transport). Набор
редактируется, когда `Scattering Mode` = `Octaves (Legacy, overlay)` или Transport с `Emissive Injection` +
`Hybrid Single Scattering`, кроме `MS Back Floor` (только лепесток Transport) и `Extra Octaves` (только Octaves); в остальных
режимах он серый и ни на что не влияет. В той же категории — `Sun Detail Shadow` / `Sun Detail Strength` (ниже).

| Свойство | Transport: лепесток (инъекция + гибрид) | Octaves (Legacy, overlay) | По умолчанию | Цена |
|---|---|---|---|---|
| `Phase G` | g первой октавы лепестка (вторая — `Phase G`·c): насколько многократно рассеянный свет летит дальше, прочь от солнца. Больше — ярче, когда смотришь сквозь облако на солнце, темнее с солнцем за спиной | g добавленных октав: октава i — HG(`Phase G`·cⁱ). Первый порядок остаётся штатным, с фазой `Scattering Distribution` тумана | 0,6 (0–0,9). 0,8–0,9 — узкий яркий ореол, но он отстаёт при быстром движении камеры (история тумана 0,9); для игры 0,6–0,7 | — |
| `MS Contribution` | Сила лепестка s: перераспределяет изотропное поле решателя к солнцу (контровая кромка и облако вокруг солнца ярче, сторона с солнцем за спиной чуть темнее; в глубине облака и ночью не действует). Энергию не добавляет: среднее по направлениям взгляда = 1. 0 — вид W36 | Вес добавленных октав a (a, a³); 0 — только A1. Добавляет свет, энергию не сохраняет | **0,5 — лепесток включён** (в W37 было 0). Рабочее 0,5–0,8 | Лепесток: ~50 инструкций DXIL (2 pow, 3 rsqrt) на фроксель Box при s > 0, решатель 0 мс. Octaves: 1–2 HG и exp на фроксель Box в `LightScattering` |
| `MS Occlusion` | b: октавы весят S^b и S^(b²), S = T_sun·k ячейки (S^b ≈ T^b). Меньше — лепесток глубже в тени облака, 1 — только освещённая кожа, 0 — тень среды не учитывается (ячейка без солнца, S = 0, лепестка не получает ни при каком b) | Множитель оптической толщины к солнцу: exp(−τ·b) и exp(−τ·b³), то есть T^b и T^(b³); тени геометрии прежние | 0,5 (0–1; в W37 минимум был 0,05) | — |
| `MS Eccentricity` | c: вторая октава берёт g·c. 1 — обе октавы одинаково узкие, 0 — вторая изотропная. 0,5 — лепесток W37 (там было фиксированное g/2), побитово | Октава i — HG(`Phase G`·cⁱ) (**изменено в W38**, см. ниже) | 0,5 | — |
| `MS Back Floor` (Advanced) | Пол F: сторона облака с солнцем за спиной не темнеет ниже 1 − s·(1 − F) от изотропной, пик вперёд уменьшается в ту же долю, среднее остаётся 1. 0 — чистый лепесток, 1 — лепестка нет | Не используется | 0,25 | — |
| `Extra Octaves` | Не используется (у лепестка всегда две октавы) | Сколько октав добавить к штатному первому порядку: 1 или 2 | 2 | вторая октава — ещё одна HG и exp |

**Соответствие UE Volumetric Cloud** (Volumetric Advanced Material Output): `Phase G` — Phase G (Phase G 2 и Phase Blend
не поддерживаются; у FogMS g от 0 до 0,9, положительный — вперёд), `MS Contribution` — Multi Scattering Contribution,
`MS Occlusion` — Multi Scattering Occlusion, `MS Eccentricity` — Multi Scattering Eccentricity, `Extra Octaves` —
Multi Scattering Approximation Octave Count (в UE 0–2). Веса a, a³ и экстинкция b, b³ в Octaves — та же рекуррентность,
что в `VolumetricCloud.usf` (`SetupParticipatingMediaContext`). Отличия: UE делает фазу каждой следующей октавы
изотропнее смешиванием lerp(изотропная, базовая фаза, c, c², …), FogMS уменьшает g (g·c на октаву); концы те же (c = 0 —
изотропная, c = 1 — базовая фаза). У лепестка веса нормированы (2/3 и 1/3 от s), а вторая октава видит S^(b²), а не b³:
так лепесток сохраняет энергию и при c = 0,5 побитово совпадает с W37. У `MS Back Floor` аналога в UE нет.

Как считается лепесток (узел `FogMS_ForwardLobe` v2, множитель к Emissive инъекции, то есть только к многократно
рассеянной части J_ms; однократное рассеяние солнца по-прежнему считает штатный туман со своей фазой):

- Lobe = 1 + (1 − F)·(f1·(p(g1) − 1) + f2·(p(g2) − 1)), μ = cos угла между направлением на солнце и взглядом (1 — смотрим на солнце);
- p(g) = (1 − g²)/(1 + g² − 2gμ)^1,5 = 4π·HG, её среднее по сфере ровно 1; g1 = `Phase G`, g2 = `Phase G`·`MS Eccentricity`;
- f1 = s·2/3·S^b, f2 = s·1/3·S^(b²) (октава i видит пропускание T^(bⁱ)), s = `MS Contribution`, b = `MS Occlusion` (в узле
  не меньше 0,001, поэтому 0^b = 0 и 0^0 не возникает), F = `MS Back Floor`, S = saturate(2A − 1) = T_sun·k из альфы
  гибридного поля;
- среднее Lobe по всем направлениям взгляда = 1 точно при любом c (каждая октава = F·изотропная + (1 − F)·4π·HG),
  минимум ≥ 1 − s·(1 − F); без солнца и ночью (k = 0) S = 0 и Lobe = 1 точно. Солнце — `View.AtmosphereLightDirection[0]`,
  тот же источник, что у решателя (Atmosphere Sun Light с индексом 0). Проверка на CPU: `fwd_lobe_check.py` (раздел 8)
  вычисляет сам HLSL узлов v1 и v2.

Во сколько раз меняется многократно рассеянный свет Box (`MS Occlusion` 0,5, `MS Back Floor` 0,25; S = 1 — освещённый край):

| s / g / c | S | на солнце | 15° от солнца | вбок | солнце за спиной |
|---|---|---|---|---|---|
| 0,5 / 0,6 / 0,5 (умолчания) | 1 | ×3,46 | ×2,71 | ×0,83 | ×0,72 |
| 0,5 / 0,6 / 0 | 1 | ×3,25 | ×2,53 | ×0,85 | ×0,79 |
| 0,5 / 0,6 / 1 | 1 | ×4,38 | ×3,29 | ×0,78 | ×0,68 |
| 0,8 / 0,6 / 0,5 | 1 | ×4,93 | ×3,74 | ×0,72 | ×0,55 |
| 0,8 / 0,6 / 0,5 | 0,1 | ×2,32 | ×1,94 | ×0,90 | ×0,83 |
| 0,8 / 0,8 / 0,5 | 1 | ×19,2 | ×6,05 | ×0,60 | ×0,49 |

Лепесток зависит от положения камеры, а не от её поворота: при повороте на месте он не «плывёт», при быстром движении
отстаёт на ~10 кадров истории тумана (узкий g заметнее). Он перераспределяет весь J_ms ячейки, включая свет неба и ламп;
вес S = T_sun·k это ограничивает там, где солнце не главное. Статус Box: `[forward lobe 0.50 g 0.60]`, пока лепесток
действует (s > 0 и гибридная инъекция активна).

Как считаются Octaves (Legacy, overlay; только редактор с `-BindlessAll`, один Box): к фазе штатного солнца в
`LightScatteringCS` добавляется Σ a^(2ⁱ−1)·exp(−τ·b^(2ⁱ−1))·HG(`Phase G`·cⁱ), i = 1…`Extra Octaves`, τ — оптическая
толщина к солнцу (A1). Пакет: row 5.z = `Extra Octaves`, row 6.xyz = (a, b, c), row 29.y = `Phase G` (раунд 38, только в
режиме Octaves; в других режимах 29.y = 0, ключ истории Transport прежний). **Изменение поведения в раунде 38:** раньше
фаза добавленных октав была lerp(изотропная, фаза тумана по `Scattering Distribution`, cⁱ), теперь HG(`Phase G`·cⁱ).
Старый Octaves-Box (по умолчанию `Phase G` 0,6, c 0,5: октавы с g 0,3 и 0,15) выглядит иначе, если `Scattering
Distribution` тумана не 0,6. Чтобы вернуть прежний вид, поставьте `Phase G` = `Scattering Distribution` тумана: при c = 0
и c = 1 результат совпадает точно, между ними он близок, но не равен.

**Старые имена и сохранённые уровни.** `Config/DefaultMultiLobeSpec.ini` (секция [CoreRedirects] плагина) при загрузке
переименовывает `Forward Anisotropy` → `Phase G`, `Forward Scattering` → `MS Contribution`, `Forward Depth` →
`MS Occlusion`, `Back Floor` → `MS Back Floor`, поэтому значения, сохранённые в раунде 37, не теряются. UE пишет в уровень
только значения, отличные от умолчаний класса: Box, у которого `Forward Scattering` в раунде 37 остался 0 (умолчание),
загрузится с `MS Contribution` 0,5 — **лепесток включится сам** (так задумано). Вид без лепестка — `MS Contribution` 0.
Python-имена: `phase_g`, `ms_contribution`, `ms_occlusion`, `ms_eccentricity`, `ms_back_floor`, `extra_octaves`. UE Python
регистрирует старые имена из CoreRedirects как устаревшие псевдонимы (в редакторе не проверено); скрипты `ProdProbe`
(`d37_lobe.py`, `d35_dolly.py`, `diag35lib.py`) уже используют новые имена.

**Как включить (раунд 38).** Нужна сборка 38 и материал с узлом `FogMS_ForwardLobe` v2: один раз в редакторе, когда
материал никто не редактирует, `py "<репо>/Tools/FogMSEnergyValidation/ProdProbe/matedit_density.py"`. Если узла нет,
скрипт сразу вставляет v2 между `FogMS_EmissiveInjection` и Emissive (параметры `FogMS_ForwardStrength`/`G`/`Depth`/
`Floor`/`Ecc` с умолчаниями 0 / 0,6 / 0,5 / 0,25 / 0,5, узел CameraVectorWS). Узел v1 раунда 37 обновляется на месте:
создаётся `FogMS_ForwardEcc` (0,5) и вход `Ecc`, прежние входы переподключаются к тем же источникам, код заменяется на v2.
Скрипт ждёт компиляции шейдера и при ошибке возвращает v1 и ничего не сохраняет. Ожидаемый вывод: `FORWARD_LOBE v1 -> v2
(…; Ecc <- …, created)` (или `FORWARD_LOBE patched, v2 (…)` для нового узла), `COMPILE forward lobe…: shader compiled
(log …)`, `PATCHED saved=True`; строки `NOTE pre-existing … default …: kept` — нормально (MID Box задаёт параметры сам).
Повтор — `FORWARD_LOBE already v2`, `ALREADY_PATCHED`. С материалом v1 `MS Eccentricity` ни на что не влияет (вторая
октава g/2) и `MS Occlusion` меньше 0,05 работает как 0,05; без узла лепестка нет вовсе. После патча поменяйте у Box любое
свойство набора (MID получит параметры) или переоткройте уровень.

**Исправление раунда 40: лепесток читает альфу поля.** В материалах, пропатченных в раундах 37–39, вход `FieldA` узла
лепестка был подключён к RGB поля, а не к A: S = saturate(2·J_ms.R − 1) вместо T_sun·k, лепесток работал в полную силу
на всей освещённой части облака, `MS Occlusion` ни на что не влиял. Исправленный `M_FogMS_Density.uasset` лежал в
репозитории до коммита `083341c` (теперь — локальный ассет, раздел 9); в проекте то же делает `matedit_density.py` (шаг
проверки пинов поля, запускается и на уже пропатченном материале):
`FIELD_PIN … WRONG` → `FIELD_WIRING repaired: FogMS_ForwardLobe.FieldA <- …A (was …RGB)`, `COMPILE field wiring: shader
compiled`, `PATCHED saved=True`; повтор — `FIELD_WIRING ok (5 pins)` (4 — у материала без узла `FogMS_SunDetail`),
`ALREADY_PATCHED`. После исправления лепесток
действует там, куда проникает солнце, и слабее прежнего: при 0,4 / 0,5 вместо +11–13 % против солнца теперь +3 %;
близко к прежнему — `MS Contribution` 0,8 и `MS Occlusion` 0,2 (+8 %). `MS Occlusion` теперь работает: меньше — сильнее.

**Облик (FogMS|Look, раунд 37): `Sun Softness`.** По умолчанию 0 — выключено, картинка прежняя.

| Свойство | Что делает | По умолчанию / рекомендация |
|---|---|---|
| `Sun Softness` | Мягкое солнце в решателе: лучи тени к солнцу из ячейки (их число прежнее) разводятся по конусу этого полуугла, граница свет/тень внутри облака и тени геометрии на тумане получают полутень. Энергия солнца та же, усредняется только видимость. В гибриде за ней следует и штатное однократное рассеяние (его множитель — смягчённый T_sun), карты теней движка остаются резкими. Transport-режимы, с инъекцией и без | 0° = выкл. Рабочее 2–5°; при больших углах тонкие тени двоятся. Правка пересчитывает поле и один раз сбрасывает историю тумана |

Как считается `Sun Softness` (проход 2 решателя): row 29.x пакета = tan θ; для directional-источника (солнца) луч каждой
подточки ячейки K отклоняется точкой i 8-точечного диска Фогеля, r = tan θ·√((i + 0,5)/8), φ = i·137,5°. Один и тот же
отклонённый луч идёт в тень геометрии, пропускание среды, T_sun и вычитаемое солнце гибрида. 0° — row 29 нулевой, пакет и
ревизия прежние. Статус: `[sun softness 3.0°]`, при лепестке — `[forward lobe 0.50 g 0.60]`.

**С раунда 46 при `Sun Softness` > 0 солнце всегда считается по всем 8 подточкам и всем 8 направлениям конуса, в каждом
решении.** Раньше при `DirectSamples 4` (умолчание) два чередующихся тетраэдра получали разные половины конуса, поэтому
поле менялось через решение: с облачным хостом без временной реконструкции (`r.VolumetricRenderTarget.Mode` 1 или 3)
интерьер облака заметно мигал, сильно от ~5°; при 0° мигания нет, при `DirectSamples 8` тоже. Цена: проход 2 решателя
~+0,2 мс на решение на Box (раунд 25: 0,107 → 0,313 мс при 8 точках; с `SolveInterval 2` в среднем ~+0,1 мс на кадр).
При `Sun Softness` 0 остаётся дешёвое чередование 4 точек. В ProfileGPU имя прохода показывает выбор:
`FogMS B2 direct cell average … [sun 8 points]` или `… [sun 4 points, alternating]`.

**Анимация (FogMS|Density Animation):** `Animate Density` (выкл.; нужна `World Aligned Texture`, режимы World или Transport),
`Wind Speed` и `Edge Flow Speed` (см/с, по умолчанию 0; видны в режиме Directional Wind), направление — поворот стрелки
`WindDirectionComponent`. `Density Motion Mode` (только чтение): новые Box — `Directional Wind`, сохранённые до него —
`Legacy Velocity Vectors`; кнопка **Use Directional Motion** переводит их без скачка фазы. Advanced: `Density Wind Velocity`
(Legacy), `Relative Detail Velocity`, `Relative Second Detail Velocity`, `Use Manual Animation Time` + `Manual Animation Time`
(воспроизводимый кадр), `Animation Time Offset`; только для чтения — `Density Animation Time`, `Density Animation Status`
(там же причина, например «Static: animation requires World Aligned Texture»). Кнопки **Freeze Density Animation** /
**Resume Density Animation**; `Reset Motion Origin`, `Use Legacy Motion`, `Restore Density Motion Reference` — только
Blueprint/Python. Каждое перемещение, поворот или изменение размера Box сбрасывает историю тумана и запускает решение с
холодного старта (warm start привязан к границам Box).

**Sun Detail Shadow (раунд 41; с раунда 45 по умолчанию включено — просьба владельца).** Точная тень солнца внутри облака
для пути инъекции с гибридом. Каждый кадр со стороны солнца строится карта Box (`r.FogMS.SunMap.Resolution` 256,
`r.FogMS.SunMap.Steps` 64) по той же функции плотности, что у материала. Материал перераспределяет долю солнца ячейки
решателя внутри ячейки: освещённая кромка ярче, тень за ней темнее, средняя энергия ячейки та же (двойного счёта с решателем
нет). Действует на штатное однократное рассеяние гибрида и на лепесток. `Sun Detail Strength` 0…1 — доля эффекта. Цена
~0,3 мс и 16,25 МБ на Box. Эффект заметнее всего против солнца и с фазой тумана (контраст кромки +4…5 %). В раунде 41 одна
из серий стабильности с быстрой анимацией (Edge Flow 500) на близкой камере была хуже (+10 % межкадрового изменения); если это
видно, уменьшите `Sun Detail Strength` или снимите галку. **Новое умолчание и сохранённые уровни:** UE пишет в уровень только
отличия от умолчания класса, поэтому Box, сохранённые раньше с выключенной галкой (тогдашнее умолчание), без явной правки,
загрузятся с ней **включённой**. Чтобы оставить Box без карты, снимите галку и пересохраните уровень. Для Box с
`Render Path = Cloud Host` карта не строится (облако тенит солнце само на каждом шаге луча), статус
`[sun detail off: the cloud host marches the sun per step]`.

**Render Path: попиксельный облачный хост (категория FogMS, раунд 45, срез P2 из `FogMS_PerPixelClouds_Design.md`;
с раунда 46 — путь по умолчанию).** `Cloud Host` (по умолчанию с раунда 46) — Box рисует штатный **Volumetric Cloud**
попиксельно (плотные облака): свой марш к солнцу на каждом шаге луча даёт резкую самотень, тёмное ядро и светлую кайму;
поле решателя по-прежнему приносит небо, отражённый свет, лампы и многократное рассеяние. `Froxel Fog` — Box рисует штатный
Volumetric Fog, как до раунда 46 (дымка, низкий туман над землёй, туман долин, реки); это же автоматический откат, когда
хоста нет. Путь данных за кадр:

```text
Box (плотность, анимация, решатель 32³ — как раньше)
  ├─► свой MID M_FogMS_Density: FogMS_FroxelWeight 0 → Box во фрокселях ничего не добавляет
  ├─► решатель → FogMS_TransportField (всегда гибрид: RGB = J − нерассеянное солнце, A = 0,5 + 0,5·T_sun·k)
  └─► MID хоста (MI_FogMS_Cloud → M_FogMS_Cloud): те же FogMS_* (плотность, шум, фазы анимации, лепесток, поле)
        + FogMS_CloudBoxCenter / FogMS_CloudWorldToLocal0..2 (где куб плотности), каждое обновление Box (каждый тик:
        сдвиг, поворот, масштаб, правки Box доходят до хоста сразу) + FogMS_CloudPrefilter / FogMS_PrefilterWavelengths (раунд 46)
подсистема хоста (каждый тик, CPU): слой хоста = полоса плотности Box ±10 м (перестраивается, когда Box уехал больше чем
  на 5 м), View Sample Count Scale, FogMS_CloudStep (шаг), cvar рендер-таргета облака (ниже), вблизи / вдали от Box
облачный хост (актёр Volumetric Cloud): луч на пиксель (у камеры, Mode 3) или один луч на блок 2×2 пикселя (вдали, Mode 1),
  шаг ≈ 2,6 м;
  на шаге: плотность Box (тот же HLSL FogMS_Extinction_v3, что у решателя, с префильтром хоста) → σt; марш к солнцу
  0,25 км × 32 → HG(Phase G Box); Emissive = σs·J·лепесток (из поля), AO 0 (небо уже в поле) → поверх тумана
```

**Новое умолчание и сохранённые уровни (раунд 46).** UE пишет в уровень только отличия от умолчания класса, поэтому Box,
сохранённые, пока умолчанием был `Froxel Fog`, без явной правки загрузятся с `Cloud Host`. Box хост **сам не создаёт** (он
вытеснил бы облака неба; создаёт его только актёр погоды, раздел 4а): если хоста в уровне нет, Box остаётся во фрокселях,
картинка прежняя, а статус подсказывает
`[cloud host: none: click 'Create Cloud Host' on this Box (or console FogMS.CloudHost.Create) to render it per pixel, froxel
fallback]`. Чтобы оставить Box во фрокселях без этой строки, поставьте `Render Path` = `Froxel Fog` и сохраните уровень.

Как включить (шаги):
1. Box: `Scattering Mode` Transport + `Emissive Injection`. Материалы `M_FogMS_Cloud` и `MI_FogMS_Cloud` в репозиторий не
   входят: их один раз строит в редакторе `py "<репо>/Tools/FogMSEnergyValidation/ProdProbe/matedit_cloud.py"` (идемпотентный,
   повтор печатает `ALREADY_PATCHED`; нужны `T_FogMS_DefaultVolume` и текстуры погоды от `matedit_weather.py`, раздел 9). Без
   материала статус Box — `[cloud host: none (M_FogMS_Cloud is missing: run matedit_cloud.py), froxel fallback]`.
2. На Box кнопка **Create Cloud Host** (категория FogMS). Она создаёт актёр `FogMS Cloud Host` (Volumetric Cloud с
   `MI_FogMS_Cloud` и настройками раунда 39: слой = полоса плотности Box ±10 м над землёй SkyAtmosphere, трасса 2 км от
   камеры, выборки ×8, марш к солнцу 0,25 км × 32, порог прозрачности 0,005, без захвата неба) и ставит Box
   `Render Path = Cloud Host`. Ничего не сохраняет; в логе предупреждение. Если хост уже есть, второй не создаётся.
   В игре и из скриптов то же делает консольная команда `FogMS.CloudHost.Create [имя Box]`. Нужен один раз на уровень.
3. Статус Box: `[render: cloud host] [cloud host 'FogMS Cloud Host': layer 0.075-0.175 km (fitted to the Box), step ~2.6 m,
   trace 2.0 km, view samples x8, rt mode 3, min samples 32, near settings (camera 0.15 km, near within 1 km); displaces
   Volumetric Cloud 'VolumetricCloud' (one cloud renders per scene)]`.
4. Оставить хост — сохранить уровень (его MID сохранится с актёром, после загрузки Box продолжит в него писать). Вернуть
   облака неба — удалить или скрыть хост; Box с `Cloud Host` без хоста сам вернётся во фроксели.

**Хост следует за Box (раунд 46).** Двигайте, поворачивайте и масштабируйте Box, меняйте профиль высоты — хост рисует его
дальше, без отката во фроксели. Положение куба плотности, размеры и полоса профиля уходят в MID хоста каждое обновление Box
(каждый тик). Слой хоста (`Layer Bottom Altitude` / `Layer Height`) плагин сам подгоняет под полосу плотности Box ±10 м
(правило кнопки), с гистерезисом: слой перестраивается, только когда цель ушла больше чем на 5 м (полоса всегда минимум
на 5 м внутри слоя), не каждый кадр; в логе не чаще строки в секунду `FogMS cloud host '…': layer A-B -> C-D km above the
ground, fitted to Box '…'`. Текущий слой — в статусе (`layer … km (fitted to the Box)`). Правка слоя руками при этом
перезаписывается; чтобы вести слой самому, `r.FogMS.CloudHost.FitLayer 0` (тогда слой, не накрывающий Box, снова даёт откат
во фроксели с причиной в статусе). Полоса ниже земли SkyAtmosphere (высота < 0) слоем не накрывается: статус
`… reaches below the ground of the cloud layer …` — поднимите Box или оставьте его во фрокселях.

Хост можно сделать и вручную: любой Volumetric Cloud, у материала которого базовый материал — `M_FogMS_Cloud` (например
`MI_FogMS_Cloud` или ваш дочерний MI). Box сам оборачивает материал хоста в MID (`MID_FogMS_CloudHost`) и пишет в него
каждое обновление. Облик хоста, не зависящий от Box, — параметры инстанса: `FogMS_CloudPhaseG2`, `FogMS_CloudPhaseBlend`
(второй лепесток HG облака), `FogMS_CloudFieldGain` (отладка: 0 — только собственное солнце облака). Фаза облака —
`Phase G` Box.

| Статус Box | Что значит (Box остаётся во фрокселях, кроме первой строки) |
|---|---|
| `[render: cloud host] [...]` | Box рисует хост; фроксельная копия выключена |
| `[cloud host: none: click 'Create Cloud Host' on this Box …, froxel fallback]` | в уровне нет хоста: нажмите кнопку (с раунда 46 `Cloud Host` — умолчание, Box хост сам не создаёт) |
| `[cloud host: none (M_FogMS_Cloud is missing: run matedit_cloud.py), froxel fallback]` | материала хоста нет в проекте (раздел 9) |
| `[cloud host: no cloud host subsystem in this world, froxel fallback]` | мир без подсистемы хоста (подсистема есть в мирах Editor, PIE и Game) |
| `[cloud host: '<хост>' exists but does not render (hidden or not visible) …]` | хост есть, но скрыт или невидим: покажите его |
| `[cloud host: needs Transport with Emissive Injection, froxel fallback]` | хосту нужно поле этого Box |
| `[cloud host: '<хост>': layer X–Y km does not cover the Box's density band A–B km above the ground …]` | только при `r.FogMS.CloudHost.FitLayer 0`: слой ведёте вы, поправьте `Layer Bottom Altitude` / `Layer Height` (при 1 плагин подгоняет слой сам) |
| `[cloud host: '<хост>': the Box's density band … reaches below the ground of the cloud layer …]` | низ Box ниже земли SkyAtmosphere: облачный слой там не начинается; поднимите Box или `Render Path` = `Froxel Fog` |
| `[cloud host: '<хост>': host material is Unlit …]`, `… lacks 'Used with Volumetric Cloud'`, `… is not in the Volume domain`, `… has N multiple-scattering octaves (must be 0 …)` | материал хоста испорчен (октавы проверяются только в редакторе); восстановите `M_FogMS_Cloud` скриптом |
| `[cloud host: busy with Box '<имя>' …]` | в этой версии один хост рисует один Box (несколько — срез P3) |
| `[cloud host: '<хост>': cannot assign a dynamic material …]` | у хоста Static mobility в игре; поставьте Movable |

Что меняется для Box с хостом (таблица 3.11 дизайна): решатель, анимация, `Transport Preset`, `Sun Softness`,
`Lumen Bounce` — как раньше. Поле всегда гибридное (при хосте гибрид включается сам, галка не нужна; `Field Only (Debug)`
даёт полное поле без собственного солнца облака). Не действуют: `Sun Detail Shadow` (не строится), `Depth Prefilter` (у хоста
свой `Host Prefilter`, ниже), фаза тумана `Scattering Distribution`, Octaves (legacy). Набор «Multiple Scattering Look»
(`MS Contribution` …) действует на поле (Emissive), как во фрокселях; `Phase G` — ещё и фаза собственного солнца облака.

**`Host Prefilter` (свойство Box, категория FogMS, раунд 46; по умолчанию 1, диапазон 0…4) — против «зерна» хоста.** Облако
идёт лучом шагами по несколько метров и одним лучом на пиксель; деталь плотности мельче шага или пикселя превращается в
пиксельный шум (старт луча дрожит от пикселя к пикселю и от кадра к кадру). Префильтр в духе Nubis ограничивает плотность Box
по полосе частот на каждой выборке: ширина w = `Host Prefilter` × max(шаг хоста, ширина трассируемого пикселя на этой дистанции);
мелкие октавы детали и эрозия гасятся на 1 − w/λ (λ — размер черты шума), полоса `Threshold` расширяется на снятую дисперсию
(тот же узел `FogMS_Extinction_v3`, что у `Depth Prefilter`; базовый шум читается с mip 0, его доля только расширяет полосу).
1 — фильтр на размер шага; 2–3 — сильнее (ожидаем мягче кромку и меньше зерна ценой мелкой детали; на глаз не проверено);
0 — облик раунда 45.
Только материал хоста: освещение решателя не меняется, пересчёта нет. Нужен материал `M_FogMS_Cloud` v2 (`matedit_cloud.py`,
узел `FogMS_CloudFootprint`); со старым материалом свойство ничего не делает. Цена — несколько десятков ALU на шаг луча,
без новых чтений текстуры; отдельной цифры префильтра нет (хост целиком с префильтром 1 у камеры владельца — 2,9 мс в
режиме 3, раунд 46).

**Настройки хоста, которые ставит плагин (cvar, раунд 46).** Пока хостом рисуется хотя бы один Box, подсистема каждый тик
держит у движка значения ниже с приоритетом game setting (одна строка в логе на каждое изменение
`FogMS cloud host settings (SetByGameSetting …): … -> …`); когда хостом не рисуется ни один Box, прежние значения
возвращаются (`… settings restored …`). Значение, заданное явно (ini проекта, консоль), сильнее — статус тогда пишет
`<cvar> <значение> kept (Console)`: так бывает и после того, как этот cvar однажды ввели в консоли в этой сессии — помогает
перезапуск редактора. Умолчания — сочетание, которое владелец подобрал на глаз против зерна и шлейфа (2026-09-26); у каждой
своя ручка `r.FogMS.CloudHost.*`, отрицательное значение = не трогать cvar движка:

| Cvar движка | Ставит плагин (ручка) | Зачем |
|---|---|---|
| `r.VolumetricCloud.DistanceToSampleMaxCount` | `Tracing Max Distance` хоста, 2 км (`StepSettings` 1) | шаг = дистанция / выборки: 2 км / 768 = 2,6 м вместо ~20 м при движковых 15 км |
| `r.VolumetricCloud.ViewRaySampleMaxCount` | 96 × `ViewSampleScale`, если больше 768 (`StepSettings` 1) | иначе `ViewSampleScale` > 8 упирается в 768 выборок |
| `r.VolumetricRenderTarget.Mode` | вблизи `RTMode` **3**, вдали `FarRTMode` **1** | 3 — трасса в полном разрешении, без реконструкции: резко, без шлейфа, дороже всего; 1 — половинное разрешение |
| `r.VolumetricCloud.SampleMinCount` | вблизи `SampleMinCount` **32**, вдали `FarSampleMinCount` **8** | минимум шагов на луч: короткие лучи (круто сквозь тонкий слой, камера в облаке) шагают мельче |
| `r.VolumetricRenderTarget.UpsamplingMode` | `UpsamplingMode` **2** | ближайший + тест глубины (что движок в режимах 2/3 ставит 2 сам — не проверено) |
| `r.VolumetricRenderTarget.ReprojectionBoxConstraint` | `ReprojectionBoxConstraint` **1** | зажим истории окрестностью кадра; действует только в режимах 0/2 (с реконструкцией) |
| `r.VolumetricRenderTarget.MinimumDistanceKmToEnableReprojection` | `ReprojectionMinKm` **4** | ближе 4 км — без истории; только режимы 0/2 |

Вблизи / вдали: «вблизи» — пока хоть одна отрисованная камера (перспективные вьюпорты редактора, вид игры) внутри Box или
ближе `r.FogMS.CloudHost.NearDistanceKm` (по умолчанию 1 км) к полосе плотности Box; «вдали» — дальше 1,1 × этого расстояния
(гистерезис 10 %); 0 — всегда «вблизи». Переключение — одна строка в логе `FogMS cloud host: camera … -> near/far settings`;
движок при смене режима пересоздаёт буферы облака (на смене возможен скачок кадра, не проверено).
`View Sample Count Scale` хоста ставит `r.FogMS.CloudHost.ViewSampleScale` (по умолчанию 8 = 768 выборок, шаг 2,6 м; 16 —
1536 выборок, шаг 1,3 м: меньше зерна, трасса примерно вдвое дороже; 0 — значение самого хоста).

**Зерно против шлейфа (компромисс).** Режимы 0/2 копят историю: зерно усредняется, но при быстром повороте камеры тянется
шлейф (раунд 39; зажим истории `ReprojectionBoxConstraint` его укорачивает). Режимы 1/3 историю не копят: шлейфа нет, но
видно зерно каждого кадра — его уменьшают полное разрешение (3), `SampleMinCount` 32, меньший шаг (`ViewSampleScale` 16) и
`Host Prefilter`. Цена по лучам: режим 3 трассирует в 4 раза больше лучей, чем 1, и в 16 раз больше, чем 0 (раунд 45: хост в
режиме 0 — 0,38 мс в окне проб 894 × 813); режим 3 у камеры владельца — 2,9 мс (2,55–3,56; раунд 46). Если режим 3 дорог,
оставьте `NearDistanceKm` ~1 км (дальние Box — режим 1) или поставьте `r.FogMS.CloudHost.RTMode 1`.

**Мягкая тень облака на земле (раунд 47, срез W47, P4 часть 1).** Хост — штатный Volumetric Cloud, поэтому движок умеет
сам строить по его материалу **карту теней облака** (Beer shadow map, BSM): в ней плотность Box, и тень Box получают земля,
высотный и объёмный туман (лучи сквозь разрывы за облаком), Lumen (отражённый свет под облаком темнеет), атмосфера и
полупрозрачность. Нужно только включить её у солнца и дать ей достаточное разрешение.

*Как включить (один раз на уровень):* в консоли редактора

```text
FogMS.CloudHost.SetupShadows            (по умолчанию: охват 5 км, разрешение ×2 → тексель 9,8 м)
FogMS.CloudHost.SetupShadows 10 2       (свои значения: охват в км, масштаб разрешения)
```

Команда находит солнце атмосферы (Directional Light с `Atmosphere Sun Light`, индекс 0; если таких несколько — самое
яркое, как выбирает движок) и ставит ему `Cast Cloud Shadows` = вкл., `Cloud Shadow Extent` и `Cloud Shadow Map
Resolution Scale`; в лог — одна строка с прежними и новыми значениями
(`FogMS.CloudHost.SetupShadows: sun 'DirectionalLight': Cast Cloud Shadows off -> on, Cloud Shadow Extent 150 -> 5 km, …`).
В редакторе это один шаг Ctrl+Z. **Ничего не сохраняется** — чтобы тень осталась, сохраните уровень. Сам плагин солнце не
трогает никогда. То же руками: у солнца галка `Cast Cloud Shadows` (категория Atmosphere and Cloud), в Advanced —
`Cloud Shadow Extent` 5 и `Cloud Shadow Map Resolution Scale` 2.

Тень есть, только пока Box рисуется хостом (статус `[render: cloud host]`). Box во фрокселях (`Froxel Fog` или откат) тени
на землю не даёт: штатный Volumetric Fog поверхности не затеняет. Частый случай — Box стоит на земле: если его полоса
плотности уходит ниже земли SkyAtmosphere (высота 0 планеты), хост его не рисует (статус
`[cloud host: … reaches below the ground of the cloud layer …, froxel fallback]`) — поднимите Box, пока нижняя граница
полосы не окажется выше земли.

*Путь данных за кадр:*

```text
солнце (Atmosphere Sun Light, Cast Cloud Shadows, охват E км, разрешение R = 512 × масштаб, не больше 2048)
  └─► движок: проход CloudShadow — луч к солнцу из каждого текселя карты (квадрат 2E × 2E км вокруг камеры) через материал
        хоста = плотность Box → на тексель: передняя глубина, средняя экстинкция, максимальная оптическая толщина
        → пространственный фильтр (r.VolumetricCloud.ShadowMap.SpatialFiltering итераций размытия: мягкий край)
  └─► потребители (каждый пиксель / фроксель — одно чтение карты): освещение поверхностей (земля под Box темнеет),
        Volumetric Fog (лучи в тумане за Box), Lumen scene, атмосфера (Cloud Shadow On Atmosphere Strength), полупрозрачность
решатель FogMS (поле J): карту не читает — его солнце = солнце × пропускание атмосферы × T_sun своей среды Box (на ячейку)
сам хост: тень своего солнца марширует по шагам сам (карту к себе не применяет)
```

*Почему ничего не считается дважды.* Каждое пропускание применяется на каждом пути света один раз. Путь «солнце → ячейка
облака»: плотность Box ослабляет солнце в решателе (T_sun) и в хосте (марш к солнцу), карту ни тот, ни другой не читает.
Путь «солнце → земля / туман / воздух»: плотность Box ослабляет его через карту, решатель в этом не участвует. Что
меняется в поле решателя — только отдельные пути, и это верно физически: при `Lumen Bounce` = Auto земля под облаком в
кэше Lumen темнее, поэтому отражённого от неё света в облаке меньше; при Off (откат) земля считается средой самого Box, и
поле с галкой и без неё одинаково (раунд 47: rel L2 2,7e-5 и 2,4e-6 при шумовом поле повтора 2,9e-5).

*Охват и разрешение — мягкость и цена.* Размер текселя = 2 × охват / разрешение:

| `Cloud Shadow Extent` | `Resolution Scale` (текселей) | Тексель | Что видно |
|---|---|---|---|
| 150 км (умолчание движка) | 1 (512) | 586 м | тень Box в 200–300 м — одно размытое пятно; статус пишет WARNING |
| 10 км | 2 (1024) | 19,5 м | мягкий край, форма облака читается |
| **5 км** (команда) | **2 (1024)** | **9,8 м** | форма и разрывы облака видны; край мягкий за счёт фильтра |
| 5 км | 4 (2048) | 4,9 м | резче, цена ~×4 (оценка) |

Мягче край — больше тексель (больше охват или меньше масштаб) и больше итераций фильтра; резче — наоборот. Физически край
тени облака мягкий на десятки метров (размытая кромка самого облака; полутень солнечного диска 0,53° на высоте 100 м — около
1 м), так что 5–20 м на тексель достаточно. Охват — это ещё и дальность: карта покрывает круг радиусом `Extent` вокруг
камеры; Box дальше не бросает тени (хост рисует Box только до 2 км, поэтому охват меньше 2–3 км не ставьте). Цена прохода
растёт с числом текселей (разрешение ×2 — примерно ×4, оценка): при 5 км / ×2 у камеры владельца — замер раунда 47 в разделе 6.

*Что ставит плагин.* Пока Box рисуется хостом **и** солнце бросает тени облаков, подсистема держит у движка (с приоритетом
game setting, как настройки хоста выше; одна строка лога на изменение, при выключении галки или без хоста — возврат
прежних значений строкой `FogMS cloud host settings restored …`):

| Cvar движка | Ставит плагин (ручка) | Зачем |
|---|---|---|
| `r.VolumetricCloud.ShadowMap.SpatialFiltering` | `ShadowSpatialFiltering` **2** (движок 1) | итерации размытия карты: мягкий край тени |
| `r.VolumetricCloud.ShadowMap.SnapLength` | `ShadowSnapFraction` **0,25** × охват (не больше 20 км) | движок сдвигает карту за камерой шагами этой длины; при движковых 20 км и охвате 5 км камера оказалась бы вне карты (тени рядом нет) |
| `r.VolumetricCloud.ShadowMap.SnapToPixelGrid` | **1** | карта привязана к сетке текселей: без мерцания при мелком шаге |

*Статус хоста* (в конце строки Box): `[cloud shadow: extent 5 km, res 1024, texel 9.8 m, filter 2]`. Без галки у солнца —
подсказка `[cloud shadow: off: the sun '…' has Cast Cloud Shadows off; … run FogMS.CloudHost.SetupShadows …]`; тексель
больше Box — `…; WARNING texel 586 m > Box 215 m: its shadow is one blurred blot, run FogMS.CloudHost.SetupShadows …`;
`Cloud Shadow Strength` 0 или `r.VolumetricCloud.ShadowMap 0` — `off` с причиной. Каждое изменение этой части — строка в
логе `FogMS cloud host '…' (Box '…'): [cloud shadow: …]`.

*Чужие облака.* Если сцену рисует не хост FogMS, а обычный Volumetric Cloud (облака неба UE; например, хост скрыт), и
солнце бросает тени облаков, Box работает, а статус предупреждает (пометка идёт внутри скобки `[sky: …]`):
`[sun cloud shadows: Volumetric Cloud '<материал>' is not a FogMS cloud host: its shadow darkens the ground and the fog,
not this Box's solver field]` (и одна строка Warning в логе): земля и туман, включая штатное однократное рассеяние
фроксельного Box, в тени этих облаков, а поле решателя — нет. Второй Box, которому досталось «busy», пишет то же про тень
первого Box.

**Ограничения хоста:**
- **Один Volumetric Cloud на сцену.** Пока хост видим, облака неба не рисуются (даже если хост пуст: Box во фрокселях или
  выключен). Сцена рисует последний добавленный облачный компонент; пока Box рисуется хостом, а другой облачный актёр
  показан, добавлен или изменён, хост в следующем кадре снова забирает рендер (строка лога `takes the render over …`).
- **Дальность.** Хост рисует Box только до `Tracing Max Distance` (2 км) от камеры, а фроксельная копия при хосте выключена:
  дальше Box не виден. Для далёких Box увеличьте дистанцию (шаг вырастет) или оставьте их во фрокселях (уровни детализации —
  срез P5).
- **Тени геометрии на солнце облака** — только из CSM/VSM: при RT-тенях солнца колонны не затеняют собственное солнце облака
  (в поле решателя их тень есть; вывод дизайна, в редакторе не проверено). Решение 2 дизайна (P4) открыто.
- **Быстрый Edge Flow**: в режиме 0 кромка облака «кипела» (реконструкция обновляет тексель раз в 4 кадра), история фрокселей
  то же движение размазывала (раунд 39). В режимах 1/3 (умолчания раунда 46) истории нет: каждый кадр трассируется заново,
  зерно гасят `Host Prefilter`, `SampleMinCount` и шаг. Сглаживание во времени без шлейфа — открыто.
- Облако не пишет скорость (TSR переносит его по глубине фона); полупрозрачное за Box рисуется поверх облака без
  `Apply Cloud Fogging` (оба пункта — поведение движка по дизайну, не проверено); в Real Time Capture неба Box хоста не
  попадает (хост создаётся с `Visible In Real Time Sky Captures` выкл.).
- Фроксельная копия при весе 0 всё ещё вокселизируется (экономия сетки тумана — срез P6).

**FogMS|Sun** (`Authored Sun Shadow`, `Cast Sun Shadow`, `Surface Shadow Strength` 1, `Surface Shadow Steps` 32,
`Filtered Sun Shadow`, `Shadow Filter Sigma` 100 см, `Filter Sun Inside Volume`) и **FogMS|Indirect** (`Indirect Shadowing`,
`Indirect Shadow Strength` 1, `Indirect Shadow Steps` 16, кнопки **Enable Indirect Preview** / **Restore Standard Lumen**)
работают только в редакторе с `-BindlessAll` у Box, который владеет пакетом оверлея, независимо от галки `Emissive
Injection`. Transport не использует `Indirect Shadowing`, `Spatial Strength` и `Spatial Distance`. Кнопка **Use Global A1**
(FogMS) включает A1 для всего тумана через оверлей (компиляция шейдеров, только редактор).

## 4а. Погода: актёр FogMS Weather (раунд 48, срез W48 из `FogMS_Weather_Design.md`)

**Что это.** Актёр уровня `FogMS Weather` (один на уровень) держит состояние погоды — слои облаков, их покрытие, тип, высоты,
плотность и ветер — и отдаёт его облачному хосту. С W48 погода видна по теням: движок строит по материалу хоста штатную карту
теней облака, и тени облаков погоды плывут по земле и туману у земли, по Lumen и по атмосфере; лучи в воздухе под облаками —
только при `Shadow Layer` = Extended (ниже). С W49 облака погоды видны и на небе (купол-небо, раздел 4б), а небесный свет мира
и решателя темнеет при пасмурности (раздел 4б в редакторе ещё не проверен). Чего ещё нет: влияния погоды на героические облака
Box и на солнце решателя (W50: под пасмурной декой героическое облако пока освещено солнцем как в ясную погоду, небо в нём уже
пасмурное), гроз и молний (W52), тумана и MPC по погоде (W51). Без актёра в уровне ничего не меняется. Ассеты погоды в
репозиторий не входят, их строит `matedit_weather.py` (раздел 9).

**Как поставить (один раз на уровень):**
1. **Place Actors → FogMS Weather** — поставьте актёр в центр уровня: он центр домена погоды (квадрат `Domain Size Km`, 20 км;
   за его краями узор погоды повторяется).
2. В **Weather State** выберите состояние: `DA_FogMS_Weather_Clear`, `_Scattered`, `_Broken`, `_Overcast` из
   `/MultiLobeSpec/FogMS/Weather` (или свой ассет: Content Browser → Miscellaneous → Data Asset → FogMS Weather State, или копия
   готового). Пусто = Clear. Если ассетов пресетов нет, `FogMS.Weather.Set <пресет>` берёт встроенные значения из C++.
3. Кнопка **Setup Sun Shadows** на актёре (или консоль `FogMS.Weather.SetupShadows`): солнцу — `Cast Cloud Shadows` вкл.,
   `Cloud Shadow Extent` 10 км, разрешение ×2 (1024 текселя по 19,5 м), выборок луча тени ×1 при `Shadow Layer` = Thin (16
   движка на полосу Box) или ×4 при Extended (64 на высокий слой; проход теней дороже пропорционально). Одна строка лога с
   прежними значениями, Ctrl+Z. Сам актёр солнце не трогает.
4. Облачный хост: если в уровне уже есть `FogMS Cloud Host` (кнопка Box **Create Cloud Host**), погода работает через него. Если
   хоста нет, актёр создаёт его сам тем же путём, что `FogMS.CloudHost.Create` (галка **Create Cloud Host** на актёре, по умолчанию
   вкл.; один раз за сессию) — в любом состоянии, **и в Clear тоже**, — и **хост вытесняет облака неба** (в сцене рисуется один
   Volumetric Cloud). Без Box такой хост работает «только для теней»: видимая трасса пустая. Созданный актёром хост удаляется
   вместе с актёром, если через него не рисуется Box. Чтобы хоста не было: снимите галку `Create Cloud Host` и удалите
   созданный хост (повторно в этой сессии актёр его не создаёт).
5. Сохраните уровень, если нравится (актёр, состояние и настройки солнца сохраняются с уровнем).

**Смена погоды.** Из Blueprint — `Set Weather (State, Seconds)` / `Set Weather Immediate`; из консоли —
`FogMS.Weather.Set Overcast 10` (пресет по имени Clear / Scattered / Broken / Overcast или путь к ассету, время в секундах). В
Details — поле Weather State (переход длится `Editor Transition Seconds`, по умолчанию 0 = сразу). В логе одна строка
`FogMS Weather '…': weather: Scattered -> Overcast (10 s).` и по окончании `… Overcast reached …`. Переход — одна плавная кривая
(smoothstep): покрытие растёт **порогом** (облака вырастают из ядер, без «призраков»), тип сдвигается по LUT, высоты, плотность и
ветер плавно меняются. Переход можно прервать новым — он начнётся с текущей смеси.

**Пресеты (физический масштаб, средние широты; покрытие = окты / 8):**

| Состояние | Нижний слой L0 | Дека L1 (As) | Перистые L2 (W49, только купол) | Ветер |
|---|---|---|---|---|
| Clear (SKC) | нет | нет | нет (купола нет; небо уровня как без актёра, если актёр не создал хост — см. п. 4) | 5 м/с |
| Scattered (SCT) | Cu mediocris: покрытие 0,40, тип 0,50, 1,0–2,5 км, σ 0,05 1/м | нет | Ci 0,30 на 8 км, τ 0,4 | 8 м/с |
| Broken (BKN) | Sc / Cu congestus: 0,75, тип 0,60, 0,8–3,0 км, σ 0,07 | 0,30, 2,5–3,5 км, σ 0,03 | Cs 0,30 на 7 км, τ 0,6 | 10 м/с |
| Overcast (OVC) | St / Sc: 1,0, тип 0,15, 0,5–1,2 км, σ 0,07, деталь 0,6 | 0,80, 2,0–3,5 км, σ 0,03 | нет (за декой не видно) | 10 м/с |

Тип облака: 0 St, 0,25 Sc, 0,5 Cu, 0,75 Cong, 1 Cb. σ физических облаков 0,02–0,3 1/м: через километр облака оптическая
толщина десятки, прямое солнце под облаком закрыто полностью (рассеянный свет неба остаётся). Направление ветра — куда плывут
облака (yaw вокруг +Z). Значения пресета правятся в ассете (выбор `Preset` пишет значения, правка значения переключает на
Custom); числа встроенных пресетов — в C++ (`FogMS_Weather.cpp`, `GetPresetValues`). Направление ветра всех пресетов — 30°.

Свойства состояния (ассет FogMS Weather State, `FFogMSWeatherValues`; в скобках — умолчания структуры, их получает новый
Custom-ассет): **Low Layer (L0)** — `Coverage` (0), `Cloud Type` (0,5), `Base` (1 км), `Top` (2 км), `Extinction` (0,05 1/м),
`Detail Strength` (1); **Middle Deck (L1)** — `Deck Coverage` (0), `Deck Base` (2,5 км), `Deck Top` (3,5 км), `Deck Extinction`
(0,03); **Wind** — `Wind Speed` (5 м/с), `Wind Direction` (30°); **High Layer (L2)** — раздел 4б. Поле `Preset` ассета по
умолчанию `Custom`.

Свойства актёра (категория FogMS Weather): `Enabled`, `Weather State`, `Editor Transition Seconds` (0), `Shadow Layer` (Thin),
`Weather Scale` (1, 0,01…10), `Create Cloud Host` (вкл.); Advanced — `Domain Size Km` (20, 2…64), `Detail Tile Km` (2,5,
0,2…20), `Curl Strength` (0,05, 0…0,5), `Coverage Edge` (0,04, 0,005…0,2), `Shadow Extent Km` (10, 1…200); группа Assets
(Advanced) — свои `Compose Material`, `Sun Material`, `Pattern Texture`, `Curl Texture`, `Type LUT`, `Sky Material` вместо
ассетов плагина; только для чтения — `Weather Status`, `Current Values`, Advanced `Wind Offset`, `Map Draw Count`, `Weather Map`,
`Weather Sun Map`, `Created Cloud Host`. Blueprint: `Set Weather`, `Set Weather Immediate`, `Get Weather Status`.

**Weather Scale (решение владельца 4 открыто).** 1 — физический масштаб. Меньше 1 — «диорама» для тестовой сцены и изометрии:
все длины (высоты, толщины, домен, тайл детали, скорость ветра) умножаются на него, экстинкция делится — оптические толщины и
темнота теней те же (0,1: облака на 100–250 м, домен 2 км).

**Путь данных:**

```text
AFogMSWeather (тик, редактор и игра)
  состояние: Weather State (или цель Set Weather), смесь от прежнего за время перехода
  RT_FogMS_WeatherMap 512² RGBA16F, wrap: один тайл узора = домен вокруг актёра
    ← M_FogMS_WeatherCompose (DrawMaterialToRenderTarget, Alpha Composite, карта очищена в (0,0,0,1)) из T_FogMS_WeatherPattern:
      R покрытие L0 (точная доля площади: каналы узора выровнены по гистограмме), G тип L0, B шторм (0 до W52), A дека L1
    перерисовывается только при смене состояния: ветер сдвигает поиск в хосте, статичная погода кадр не стоит;
    один раз после создания — проверка записи всех четырёх каналов (чтение пикселя калибровочной отрисовки)
  RT_FogMS_WeatherSun 512² R16F (только режим Thin): оптическая толщина столба погоды вдоль солнца на точку земли
    ← M_FogMS_WeatherSun (24 выборки той же функции плотности), при смене карты или повороте солнца > 0,05°
  → подсистема хоста (после тиков актёров): MID хоста ← FogMS_Weather* (текстуры, домен, ветер, слои, солнце)
КАДР ДВИЖКА (без правок движка)
  M_FogMS_Cloud v4 (Shadow Pass Switch — с v3): в проходе теней облака (и sky AO) экстинкция = Box + погода, в видимом проходе —
    только Box
  карта теней облака (Beer shadow map) → земля, Volumetric Fog, Lumen scene, атмосфера, полупрозрачность
```

Плотность погоды — одна функция (как у Nubis): σ = σ_L0 · saturate((шум − (1 − профиль)) / мягкость) + дека;
профиль = LUT типа по высоте в слое × покрытие из карты; шум — узор погоды в мелком тайле с curl-закруткой по высоте (из 2D
текстур, без 3D-шума); в проходе теней шум читается с грубого mip (тень всё равно размыта). Текст HLSL один в материале хоста и в
`M_FogMS_WeatherSun`.

**Shadow Layer — решение владельца 2 (свойство актёра; выбрано воротами раунда 48):**
- **Thin Layer** (по умолчанию с раунда 48b): слой хоста остаётся полосой Box (без Box — полоса 0,1 км прямо под основанием
  погоды); в проходе теней весь столб погоды вдоль солнца (`RT_FogMS_WeatherSun`) размазан по этому слою. Земля под ним получает
  верную тень (exp(−τ столба)), воздух выше полосы и рельеф выше неё — нет (лучей в воздухе от облаков погоды нет). Видимый проход
  не меняется вовсе.
- **Extended Host Layer**: слой хоста = полоса Box ±10 м ∪ высоты слоёв погоды (от основания нижнего до верха деки, с запасом на
  весь переход). Погода в проходе теней на своей высоте — тень получают все потребители, в том числе воздух под облаками (лучи) и
  рельеф на любой высоте ниже облаков. Видимый проход по-прежнему рисует только Box, но трассирует более высокий слой; пустые шаги
  пропускаются по `r.FogMS.Weather.SkipSteps` (8) за раз (плагин ставит `r.VolumetricCloud.StepSizeOnZeroConservativeDensity`), а
  консервативная область Box расширена на столько же (`FogMS_CloudSkipMargin` = пропуск × шаг хоста), поэтому вход в облако не
  перескакивается и сетка выборок та же. Цена — раздел 6: у камеры владельца около +1 мс видимого прохода (ворота 0,3 мс не
  прошли), поэтому не по умолчанию. Выбирайте для уровней, где важны лучи сквозь разрывы облаков.

**Карта теней: Box и погода вместе.** У солнца одна карта теней облака. Для одного Box хватает 5 км / ×2 (тексель 9,8 м,
`FogMS.CloudHost.SetupShadows`), погоде нужен охват 10–20 км. С погодой ставьте 10 км / ×2 (`Setup Sun Shadows`): тексель 19,5 м,
тень Box в 200+ м — ещё около 10 текселей; ×4 (2048) вернёт 9,8 м ценой ~×4 текселей. Карта следует за камерой шагами
`Extent/4`; за её краем тени погоды нет. Выборок на луч тени движок берёт 16 × `Cloud Shadow Ray Sample Count Scale` на весь
слой (у горизонта до ×2, не проверено): при Thin слой — полоса Box, хватает ×1; при Extended слой высокий (до 3,5 км), кнопка
ставит ×4 (64),
иначе тень Box в высоком слое может «теряться» между выборками — ценой прохода теней.

**Статус актёра** (`Weather Status` в Details, `FogMS.Weather.Status` в консоли), например:
`Active: 'Scattered' -> 'Overcast' 42 % (4.2 of 10.0 s): L0 coverage 0.66 type 0.33 0.79-1.99 km sigma 0.060/m; deck 0.34 …;
cirrus 0.17 at 8.0 km tau 0.23; wind 9.0 m/s toward 30 deg | map 512^2 over 20.0 km (39 m/texel), drawn 57 x | shadow layer: thin
(…) | host 'FogMS Cloud Host' (also renders Box 'FogMS - Live Box'): layer 0.109-0.209 km, thin layer: the weather column is spread
over it in the shadow pass | cloud shadow extent 10 km, texel 19.5 m | sky dome on (…)` (цифры примера условные; первое слово —
`Active`, `Cirrus only` или `Clear`). Подсказки: нет хоста / хост скрыт, материал хоста старше W48
(`run matedit_cloud.py`), нет ассетов погоды (`run matedit_weather.py`), солнце без `Cast Cloud Shadows`, охват солнца меньше
нужного, `Inactive: FogMS Weather '…' drives this world` (второй актёр), проверка RGBA не прошла (ниже).

**Что меняется и что возвращается.** Пока актёр активен (есть нижний слой или дека с покрытием > 0, экстинкцией > 0 и верхом
выше основания): в MID хоста — параметры погоды; при **Extended** слой хоста расширен до слоёв погоды и, пока через хост рисуется
Box, ставится `r.VolumetricCloud.StepSizeOnZeroConservativeDensity`; при **Thin** (по умолчанию) слой — полоса Box (без Box —
0,1 км под основанием погоды); cvar карты теней раунда 47 (и без Box); у хоста без Box — `Tracing Start Distance` = `Tracing Max
Distance` (пустая видимая трасса; Box, привязавшись, вернёт 0). **Clear** (все покрытия 0) — ветка погоды выключена, слой и cvar
как без актёра; но если актёр создал хост (п. 4), тот остаётся и вытесняет облака неба. **Удаление актёра** (или `Enabled` выкл.):
ветка выключается сразу, слой хоста с Box подгоняется к Box на следующем обновлении, хост без Box получает прежние слой и
стартовую дистанцию, cvar возвращаются на следующем тике — строки лога `FogMS Weather '…' stopped …`, `FogMS Weather leaves cloud
host …`, `FogMS cloud host settings …`.

**Ограничения W48.**
- Один актёр на уровень; второй пишет `Inactive` и ничего не делает.
- Погода ездит через облачный хост: хост вытесняет облака неба (раздел 4, «Ограничения хоста»).
- Солнце решателя и героические облака Box погоду пока не видят (W50): под декой облако Box освещено солнцем, как без погоды.
  Небо в поле решателя погоду видит, пока показан купол W49 (авто-источник неба — SH захвата, раздел 4б).
- Видимые облака погоды и небесный свет — купол-небо (раздел 4б, W49); цвет и плотность тумана по погоде — W51.
- Проверка RGBA: карта пишется материалом в режиме Alpha Composite (альфа = 1 − Opacity поверх очищенной карты). Если рендерер
  пишет каналы иначе (например, проект с Substrate), статус `Inactive: RT_FogMS_WeatherMap RGBA write check failed …` и погода
  выключена, а не рисует неверные тени (в тестовом проекте без Substrate проверка прошла: прочитано ровно 0,25 / 0,5 / 0,75 /
  0,125, раунд 48).
- Упакованная игра: актёр грузит ассеты погоды по пути; чтобы они попали в сборку, назначьте их в `Assets` актёра (тогда на них
  есть ссылка) или добавьте `/MultiLobeSpec/FogMS/Weather` в `Additional Asset Directories to Cook` (не проверено в упаковке).
- Ассеты генерируются воспроизводимо: `python Tools/FogMSEnergyValidation/ProdProbe/texgen/gen_weather_textures.py` (numpy +
  Pillow, seed 11, выход на `D:/FogMS_ProbeFrames/texgen`), затем в редакторе `matedit_weather.py` (импорт текстур без сжатия, sRGB
  выкл., wrap/clamp, материалы композиции, солнца и купола, пресеты), затем `matedit_cloud.py` (материал хоста v4). Оба
  скрипта идемпотентны (`ALREADY_PATCHED`), связи проверяются по T3D, при ошибке ничего не сохраняется. Сами ассеты в
  репозиторий не входят (раздел 9); выходную папку генератора для импорта задаёт `FOGMS_TEXGEN_DIR` в окружении редактора.

## 4б. Видимое небо погоды: купол-небо (раунд 49, срез W49 из `FogMS_Weather_Design.md`)

> **В редакторе ещё не проверено.** Раунд 49 установлен и материал `M_FogMS_WeatherSky` собран (`d49_sky.py matedit`), но
> проверки среза (`d49_sky.py check/cost`) и оценка владельцем не выполнены. Раздел описывает код и дизайн.

**Что это.** Актёр `FogMS Weather` сам держит **купол-небо**: сферу радиусом 1000 км вокруг себя (компонент `SkyDome`) с материалом
`M_FogMS_WeatherSky` (Unlit, **Is Sky**). Купол рисует видимые облака погоды — нижний слой L0, деку L1 и перистые L2 — **той же
функцией плотности и с теми же параметрами**, что и проход теней облачного хоста (карта погоды × LUT типа × шум узора, тот же сдвиг
ветром). Поэтому облако на небе стоит там же, где его тень на земле. Ставить ничего не нужно: купол появляется, когда в погоде есть
видимый слой (Scattered, Broken, Overcast), и исчезает на Clear — тогда небо уровня точно такое же, как без актёра. Купол не
сохраняется с уровнем (компонент transient), удаляется вместе с актёром, не выделяется щелчком по небу, не отбрасывает теней, не
попадает в ray tracing, distance fields, Lumen, HLOD и в границы уровня.

**Что нужно в уровне:** `SkyAtmosphere` (купол рисует атмосферу её нодами; без неё купол выключен со статусом) и `SkyLight` с
галкой **Real Time Capture** (иначе небесный свет не видит облаков — статус подскажет). Тени погоды (раздел 4а) — отдельно, через
солнце и хост; купол им не нужен.

**Путь данных за кадр:**

```text
AFogMSWeather (тик): смешанное состояние погоды -> тем же MID-параметрам, что хосту (origin, domain, wind, L0, L1, карта погоды,
  LUT, узор, curl) + перистые L2 + шаги и свет купола -> MID купола; видимость купола (есть слой? Sky Dome? r.FogMS.Weather.SkyDome?)
КАДР ДВИЖКА (без правок движка)
  depth prepass и base pass: купола НЕТ (Is Sky-меш движок туда не пускает)
  SkyPass (после base pass): купол рисуется только на пикселях неба (тест глубины, глубину НЕ пишет) -> пиксели неба остаются
    «дальними»; в материале: атмосфера (SkyAtmosphereViewLuminance + диск солнца) -> перистые -> луч 20 шагов через L0 и 8 через
    деку -> воздушная перспектива (SkyAtmosphereAerialPerspective) на средней глубине облаков
  SkyAtmosphere: видит Is Sky-меш и свои пиксели неба не рисует; перспективу на «дальние» пиксели купола не накладывает
  Height Fog / Volumetric Fog: на небо как раньше (пиксели купола для них — небо)
  облачный хост (героические Box): компонуется поверх неба как раньше -> без шва и ореолов вокруг героя
  Real Time Capture у SkyLight: рисует Is Sky-меши, т. е. купол (дешёвая ветка: Reflection Capture Pass Switch -> 6 + 4 шага,
    1 выборка к солнцу)
    вместо атмосферы -> кубмапа и SH неба -> свет неба на поверхностях, Lumen, Volumetric Fog, решатель FogMS
  решатель FogMS: авто-источник неба при куполе = SH захвата, статус Box `[sky: SH (Real Time Capture, weather clouds)]`
```

**Как освещены облака купола** (физически, без «художественных» множителей): солнце — `SkyAtmosphereLightIlluminance` в точке облака
(с атмосферным покраснением на закате и тенью планеты ночью) × фаза из двух лепестков Хеньи-Гринштейна (вперёд 0,8 — яркие кромки
против солнца; назад −0,3) × пропускание до солнца (4 выборки через нижний слой той же плотностью + колонна деки); плюс рассеянный
свет солнца, прошедший сквозь слои сверху, по двухпотоковой формуле T = 2μ/(2μ + (1 − g)τ), g = 0,85 (раздел 1.4 дизайна: под
плотной декой прямого солнца нет, основание деки «светится изнутри» серым); плюс небо — `SkyAtmosphereDistantLightScatteredLuminance`
× градиент по высоте в слое (у основания половина) × пропускание деки для небесного света. Альбедо 0,98. Облака за 60 км (× Weather
Scale) растворяются; перистые — до 120 км.

**Настройки актёра, группа Sky** (всё, кроме `Sky Dome`, — в Advanced):

| Свойство | По умолчанию | Что делает |
|---|---|---|
| `Sky Dome` | вкл. | Купол-небо. Выкл. — небо уровня, тени погоды остаются |
| `Sky Max Distance Km` | 60 | Докуда луч купола видит облака (× Weather Scale); за 60 % плавно гаснут |
| `Sky View Steps` / `Sky Deck Steps` | 20 / 8 | Шаги луча через нижний слой / деку. Цена ~ пропорциональна |
| `Sky Sun Samples` | 4 | Выборок к солнцу на освещённую точку нижнего слоя |
| `Sky Phase Forward` / `Back` / `Back Weight` | 0,8 / −0,3 / 0,25 | Фаза облаков неба |
| `Sky Bottom Visibility` | 0,5 | Доля небесного света у основания облака (у вершины — полная) |
| `Sky Dome Radius Km` | 1000 | Радиус сферы; должен охватывать все камеры и всю геометрию (на картинку не влияет) |

Перистые — в состоянии погоды (ассет FogMS Weather State, группа High Layer (L2)): `Cirrus Coverage`, `Cirrus Altitude` (км),
`Cirrus Optical Depth` (0…5 в Details, физически 0,1–3), `Cirrus Streak Direction` (направление полос). Ассеты-пресеты W48
получают новые поля при загрузке (пресет пишет свои числа), свои Custom-ассеты — умолчания структуры (`Cirrus Coverage` 0 —
перистых нет; высота 8 км, τ 0,5, полосы 30°).

**Статус актёра** получает часть `sky dome on (1000 km, 20/8 steps, 4 sun samples; clouds within 60 km): sky light: Real Time
Capture holds the dome; the FogMS solver takes its SH` или `sky dome off: <причина>` (`no visible layer (Clear)`, `Sky Dome unticked`,
`r.FogMS.Weather.SkyDome 0`, `no rendering SkyAtmosphere`, `M_FogMS_WeatherSky is missing or older than W49 (run
matedit_weather.py)`). Подсказки: SkyLight без Real Time Capture; облачный хост с галкой `Visible In Real Time Sky Captures` (тогда
героическое облако затемняет весь небесный свет и попадает в собственное небо — снимите галку; хост, созданный кнопкой, её не имеет).
Лог: одна строка `sky dome on (...)` при появлении и `sky dome off (...)` при исчезновении.

**Цена.** Купол считается только на пикселях неба, один раз за кадр (в профиле `ProfileGPU` — событие `SkyPassParallel` в
`BasePass`); в захвате неба — `CaptureSkyMeshReflection` (128² на грань, дешёвая ветка; имена событий и размер — не проверено).
Ворота среза — ≤ 1 мс на 1080p у камеры владельца; **цифры пока нет**: проверка раунда 49 не выполнена (не измерено). Если
дороже — следующий срез: панорама в render target (как Sky View LUT), купол читает её вместо луча.

**Ограничения W49.**
- Купол всегда **за** геометрией: гора не «входит» в деку, низкая облачность не обнимает вершину. Облака у рельефа — героические
  Box; «горы в облаках» — второй слой Height Fog (W51).
- Сам купол теней не отбрасывает: тени от тех же облаков даёт проход теней хоста (раздел 4а) — одна плотность, поэтому совпадают.
- Солнечный диск рисует купол; его яркость ограничена движком для эмиссии (32256 предэкспонированных единиц, не проверено) —
  блум от диска может быть слабее, чем у атмосферы без купола.
- Небесный свет — один захват у позиции SkyLight, без пространственных вариаций (решение 6 дизайна): под одиночной тучей и в ясном
  месте того же кадра амбиент одинаковый, различает их только солнце (карта теней).
- Облака купола перерисовываются каждый кадр со сдвигом шагов (джиттер, сглаживает TSR); при быстром полёте камеры возможен лёгкий
  шлейф на облаках, как у неба вообще (не проверено).
- Цвет дальнего тумана и туман по погоде — W51; погода внутри героических облаков и солнце решателя — W50.
- Упакованная игра: `M_FogMS_WeatherSky` грузится по пути, как остальные ассеты погоды: назначьте его в `Sky Material` (Assets
  актёра) или добавьте `/MultiLobeSpec/FogMS/Weather` в `Additional Asset Directories to Cook` (не проверено в упаковке).
- Материал создаётся скриптом `matedit_weather.py` (тот же, что для W48; идемпотентный, связи по T3D, HLSL проверен DXC); в
  репозиторий он не входит и в истории git его нет (раздел 9).

## 5. Консольные переменные

Все `r.FogMS.*` и команды `FogMS.*` кода перечислены ниже (сверено с `Source/**`, коммит `083341c`). Команды `MLS.*` относятся к
BRDF-части плагина (`README_RU.md`).

**Решатель и источники:**

| Cvar | По умолчанию | Когда трогать |
|---|---|---|
| `r.FogMS.Transport.Tolerance` | 1e-14 | Действует, только если у Box `Transport Tolerance` = −1 |
| `r.FogMS.Transport.WarmStart` | 1 | Не трогать: 0 заставляет решатель начинать с нуля каждый кадр |
| `r.FogMS.Transport.SunAligned` | 1 | Поворачивает набор направлений на солнце. При 16 направлениях ошибка падает с 3,1 до 2,5 % |
| `r.FogMS.Transport.SkipConverged` | 1 | Не трогать |
| `r.FogMS.Transport.SweepThreads` | 1024 | 256/512/1024. Результат тот же, 1024 быстрее на ~6 % |
| `r.FogMS.Transport.DirectSamples` | 4 | Точек на ячейку для **солнца** (directional; в гибриде также T_sun): 4 — тетраэдр, чередуется между решениями; 8 — все углы. Point и spot с раунда 35 всегда считаются по 8 точкам: при чередовании луч уже ячейки (spot 4,8° против ячейки 7 м) мигал прямоугольной волной с периодом 2·`SolveInterval` кадров (±20 % свечения, раунд 34). **Box с `Sun Softness` > 0 с раунда 46 всегда берёт 8 точек и 8 направлений конуса** (иначе поле мигало через решение; ~+0,2 мс на решение на Box). Если мигает граница солнечной тени внутри облака без мягкого солнца, ставьте 8 |
| `r.FogMS.Transport.DirectSkipEmpty` | 0 | Только диагностика: 1 затемняет края тумана |
| `r.FogMS.World.SkySource` | 0 | 0 авто: Real Time Capture + SkyAtmosphere → Sky View LUT; **с W49: Real Time Capture + купол-небо погоды → SH захвата** (статус `[sky: SH (Real Time Capture, weather clouds)]`: облака погоды есть только в захвате, в LUT их нет); статический захват → публичная кубмапа; иначе SH. 2/3/4 — принудительно LUT/кубмапа/SH (недоступный → SH; LUT при куполе — с подсказкой, что облаков в нём нет). 1 = 0 с предупреждением в логе. Источник — в статусе `[sky: …]` |
| `r.FogMS.World.SkyLutSamples` | 5 | Выборок Sky View LUT на сектор (1…13) |
| `r.FogMS.World.SunExcludeDegrees` | 3 | Только кубмапа: вырезает солнечный диск, чтобы не считать солнце дважды (не больше 30°) |
| `r.FogMS.World.SkyMipBias` | 0 | Только кубмапа: больше — небо размытее, меньше — резче |
| `r.FogMS.World.FallbackMedium` | 1 | Откат `Lumen Bounce`: 1 — солнце на земле ослаблено средой Box (под плотным облаком темнее), 0 — только тень геометрии |
| `r.FogMS.World.Indirect` | 1 | Диагностика: 0 — только прямые источники, без неба и отражённого света |

**Доставка, async, удержание:**

| Cvar | По умолчанию | Когда трогать |
|---|---|---|
| `r.FogMS.Transport.AsyncCompute` | 0 | 1: решатель (кроме проходов 0/1/2/14) уходит в async-очередь, J приходит на кадр позже (и для инъекции, и для overlay). Нужны `r.RDG.AsyncCompute` = 1 (значение 2 — принудительный async — Transport отклоняет) и async в RHI, иначе проходы тихо остаются на графике |
| `r.FogMS.Transport.SolveInterval` | 2 | Решать раз в N кадров (1…8), между решениями держать прошлый результат. 4 — для дальних Box, 1 — каждый кадр. Изменение Box, настроек или солнца пересчитывает сразу |
| `r.FogMS.MaxBoxesPerFrame` | 4 | Сколько Box решается за кадр на вид (1…16). Остальные держат прошлое решение, а если держать нечего — ждут (статус `Queued: r.FogMS.MaxBoxesPerFrame (N) reached in this view; …`, поле не очищается). Box пакета (overlay-Box, иначе Box с инъекцией с наименьшим номером) решается всегда и входит в лимит; дальше порядок: Box, ждавший ≥ 8 кадров (решается даже сверх лимита, не больше одного лишнего решения на вид и кадр) → камера внутри Box → крупнее на экране → ближе → меньший номер. Удержания в лимит не входят |
| `r.FogMS.SunMap.Resolution` / `.Steps` | 256 / 64 | Размер карты `Sun Detail Shadow` на Box: поперёк лучей солнца (кратно 8, 64…512) и число слоёв вдоль луча (кратно 8, 16…256). Память на Box — Resolution² × Steps × 4 байта + 256 КБ (16,25 МБ при умолчаниях); изменение пересоздаёт карту |
| `r.FogMS.DensityAtlas.ForceGPUCopy` | 0 | A/B-проверка пути упакованной игры в редакторе |
| `r.FogMS.CloudHost.StepSettings` | 1 | Облачный хост (раунд 45): пока Box рисуется хостом, ставит `r.VolumetricCloud.DistanceToSampleMaxCount` = Tracing Max Distance хоста (шаг 2,6 м при 2 км) и, если нужно, `r.VolumetricCloud.ViewRaySampleMaxCount` = 96 × `ViewSampleScale`; потом возвращает прежние. 0 — не трогать эти cvar. `SampleMinCount` с раунда 46 — свои ручки (ниже) |
| `r.FogMS.CloudHost.FitLayer` | 1 | Раунд 46: слой хоста следует за Box (полоса плотности ±10 м, гистерезис 5 м). 0 — слой ведёте вы |
| `r.FogMS.CloudHost.ViewSampleScale` | 8 | `View Sample Count Scale` хоста: 8 = 768 выборок (2,6 м), 16 = 1536 (1,3 м, трасса ~×2). 0 — значение хоста |
| `r.FogMS.CloudHost.RTMode` / `.FarRTMode` | 3 / 1 | `r.VolumetricRenderTarget.Mode` вблизи / вдали от Box (раздел 4, «Настройки хоста»). −1 — не трогать |
| `r.FogMS.CloudHost.SampleMinCount` / `.FarSampleMinCount` | 32 / 8 | `r.VolumetricCloud.SampleMinCount` вблизи / вдали. −1 — не трогать |
| `r.FogMS.CloudHost.NearDistanceKm` | 1 | «Вблизи» = камера внутри Box или ближе этого к его полосе плотности; гистерезис 10 %. 0 — всегда «вблизи» |
| `r.FogMS.CloudHost.UpsamplingMode` | 2 | `r.VolumetricRenderTarget.UpsamplingMode`. −1 — не трогать |
| `r.FogMS.CloudHost.ReprojectionBoxConstraint` / `.ReprojectionMinKm` | 1 / 4 | `r.VolumetricRenderTarget.ReprojectionBoxConstraint` / `.MinimumDistanceKmToEnableReprojection` (действуют в режимах 0/2). −1 — не трогать |
| `r.FogMS.CloudHost.ShadowSpatialFiltering` | 2 | Раунд 47: `r.VolumetricCloud.ShadowMap.SpatialFiltering` (итерации размытия карты теней облака, до 4), пока Box рисуется хостом и солнце бросает тени облаков. −1 — не трогать |
| `r.FogMS.CloudHost.ShadowSnapFraction` | 0,25 | Раунд 47: `r.VolumetricCloud.ShadowMap.SnapLength` = это × `Cloud Shadow Extent` солнца (не больше 20 км) и `SnapToPixelGrid 1`, при тех же условиях. 0 — не трогать. С раунда 48 оба cvar карты теней ставятся и при погоде без Box |
| `r.FogMS.Weather.SkipSteps` | 8 | Раунд 48: `r.VolumetricCloud.StepSizeOnZeroConservativeDensity`, пока погода расширяет слой хоста, через который рисуется Box (Shadow Layer = Extended): пустые шаги видимого луча пропускаются по стольку за раз; консервативная область Box расширяется на то же расстояние. 32 оказалось дороже 8 (раунд 48). 1 или меньше — не трогать (движок 1) |
| `r.FogMS.Weather.SkyDome` | 1 | Раунд 49: 0 — купол-небо погоды скрыт везде (A/B: небо уровня, тени погоды остаются, авто-источник неба решателя снова LUT); 1 — купол у актёров с галкой `Sky Dome` (раздел 4б) |

**Диагностика:**

| Команда / cvar | Что делает |
|---|---|
| `FogMS.DumpSpatial <абсолютный префикс пути>` | Выгружает атлас поля Box пакета (ровно один аргумент). Без `-BindlessAll` поле **не** выгружается: резидентного атласа нет |
| `FogMS.CloudHost.Create [Box]` | То же, что кнопка Box **Create Cloud Host** (в игре тоже): создаёт хост, если его нет, и ставит Box `Render Path = Cloud Host`. Без аргумента — первый включённый Box, иначе имя или метка Box. Ничего не сохраняет |
| `FogMS.CloudHost.SetupShadows [км] [масштаб]` | Раунд 47: солнцу атмосферы — `Cast Cloud Shadows` вкл., `Cloud Shadow Extent` (по умолчанию 5 км, 1…10000), `Cloud Shadow Map Resolution Scale` (по умолчанию 2, 0,25…16); одна строка лога с прежними значениями, в редакторе — шаг Ctrl+Z. Ничего не сохраняет (раздел 4, «Мягкая тень облака на земле») |
| `FogMS.Weather.Set <пресет или путь> [сек]` | Раунд 48: актёр FogMS Weather этого мира переходит к состоянию Clear / Scattered / Broken / Overcast (или SKC / SCT / BKN / OVC, регистр не важен; ассеты `DA_FogMS_Weather_*`, без ассета — встроенные значения) или к ассету FogMS Weather State за указанное время (по умолчанию 0 с); строка лога `weather: A -> B (N s)`. Ничего не сохраняет |
| `FogMS.Weather.SetupShadows [км] [масштаб] [выборки]` | Раунд 48: солнцу — `Cast Cloud Shadows`, `Cloud Shadow Extent` (по умолчанию `Shadow Extent Km` актёра, 10), `Cloud Shadow Map Resolution Scale` (2) и `Cloud Shadow Ray Sample Count Scale` (по умолчанию 1 при `Shadow Layer` = Thin, 4 при Extended); диапазоны 1…10000 км, 0,25…16, 0,25…16; как кнопка актёра Setup Sun Shadows; строка лога, Ctrl+Z |
| `FogMS.Weather.Status` | Раунд 48: строка статуса каждого актёра FogMS Weather в лог |
| `FogMS.Status` | То же, что `MLS.Status`: состояние engine-shader overlay, запрошенные cvar A1 (`r.FogMS.Enable`/`Steps`/…), отображение `/Engine` и настройки MLS. О Box и инъекции ничего не выводит. Только редактор |
| `r.FogMS.Transport.Test*` | Синтетические тестовые входы. После проверки верните умолчания: `Test`, `TestGeometry`, `TestBoundary`, `TestReconstruction` = 0; `TestTau` = 4; `TestAlbedo` = 1 |
| `r.FogMS.SSFS` (0), `.Amount` (0,5), `.Radius` (24 px, 1…128) | Экранное рассеяние FogMS (постфильтр, без истории); работает и без `-BindlessAll`, с любой доставкой Transport; по решению владельца выключено |

**Legacy-оверлей (только редактор; A1 и режимы этапов A–B):**

| Команда / cvar | По умолчанию | Что делает |
|---|---|---|
| `FogMS.Apply` | — | Собирает и применяет overlay шейдеров движка (общий с MLS); нужен после смены cvar A1. Только редактор |
| `FogMS.Debug 0…4` | — | Debug-виды overlay: 0 обычный, 1 экстинкция, 2 пропускание, 3 ошибка шва, 4 авторская плотность Box. На время выключает `r.VolumetricFog.TemporalReprojection` и ставит `r.GeneralPurposeTweak`; `FogMS.Debug 0` возвращает. Нужен overlay с `r.FogMS.DebugViews 1` |
| `r.FogMS.Enable` | 0 | A1: самозатенение солнца в штатном тумане (оверлей `VolumetricFog.usf`); кнопки Box **Enable Live Box** / **Use Global A1** ставят его сами |
| `r.FogMS.BoxMode` | 0 | 0 — A1 для всего тумана, 1 — живой Box (нужен `-BindlessAll`; без него остаётся 0, а Box работает по пути инъекции) |
| `r.FogMS.Steps` | 16 | Интервалов марша A1 (1…64); нужен `FogMS.Apply` |
| `r.FogMS.MarchDistance` | 0 | Предел марша плотности, см; 0 — дальняя граница сетки тумана |
| `r.FogMS.MaxDistance` | 2000000 | Полная длина луча к солнцу, см, с аналитическим продолжением |
| `r.FogMS.ExcludeGlobalLayer` | 0 | Вычитать оптическую толщину высотного тумана (0 — физично) |
| `r.FogMS.DebugViews` | 1 | Компилировать debug-виды для `FogMS.Debug` |
| `r.FogMS.ViewIntegration` | 0 | Интеграция Box по лучу камеры 0…3 (отклонена владельцем в пакете 6; только overlay) |
| `r.FogMS.ScreenScatteringSun` | 1 | Диск солнца для штатного FSSS в оверлее высотного тумана (только overlay) |

`r.FogMS.Enable`, `.Steps`, `.MarchDistance`, `.MaxDistance`, `.ExcludeGlobalLayer`, `.DebugViews` регистрируются только в
редакторе (`MultiLobeShaderPatcher.cpp`). Overlay принимает только UE 5.8.2 CL 56702186 (анкеры патчей сверены с этой версией).

## 6. Стоимость и производительность

Все цифры сняты на тестовой сцене с RTX 3070.

- **Production-решатель:** ≈ 2,2 мс GPU (16 направлений, ≤16 итераций, 1e-6).
- **Инъекция вместо overlay:** `LightScattering` дешевле на ~0,7 мс (1,575 → 0,870 мс), кадр 11,55 → 10,76 мс.
- **Async + инъекция:** на графике остаётся ~0,35–0,46 мс (проходы с ray tracing). Кадр дешевле на ~1,07 мс
  из 2,2 мс, но `ComputeVolumetricFog` дорожает (3,46 → 3,81 мс): делит GPU с async-очередью. Временные буферы
  ~25 МБ при 48 направлениях и ~50 МБ при 96.
- **`SolveInterval 2` (по умолчанию):** на кадрах удержания решатель стоит 0,00 мс. Картинка против N = 1 — 46 дБ
  при N = 2 и 44 дБ при N = 4.
- **`DirectSamples 4` (по умолчанию):** проход 2 на графической очереди 0,313 → 0,107 мс, поле в пределах 0,5 % от 8 точек
  (раунд 25, тогда 4 точки получали все источники). С раунда 35 4 точки только у солнца, point/spot — по 8: оценка
  +0,01–0,02 мс на узкий spot (лучи идут только из ячеек внутри его конуса), не больше +0,1 мс (не измерено).
- **Гибрид:** стоит столько же, сколько полная инъекция (+0,02 мс).
- **`Depth Prefilter`:** только ALU в вокселизации Box (узел `FogMS_DepthFootprint` и ~20 операций с одним sqrt в
  экстинкции), новых чтений текстуры нет; база читается из более грубого mip. Оценка < 0,1 мс (не измерено).
- **Лепесток (`MS Contribution` > 0, по умолчанию 0,5 — включён):** решатель 0 мс. В вокселизации Box ~50 инструкций
  DXIL (2 pow, 3 rsqrt, normalize) на фроксель при s > 0 в гибриде; иначе одно сравнение и однородная ветвь. Оценка
  ≤ 0,05 мс (не измерено). `MS Eccentricity` цену не меняет (одно умножение вместо константы).
- **Octaves (legacy):** 1–2 вычисления HG (sqrt, деление) и exp на фроксель внутри Box в `LightScattering`; с раунда 38
  HG вместо lerp к фазе тумана, на несколько ALU дороже (не измерено).
- **`Sun Softness`:** только проход 2 решателя. С раунда 46 при `Sun Softness` > 0 солнце считается по 8 точкам в каждом
  решении (без мигания через решение): проход 2 как при `DirectSamples 8`, ~0,31 мс вместо ~0,11 мс (раунд 25), то есть
  ~+0,2 мс на решение на Box, с `SolveInterval 2` ~+0,1 мс на кадр; плюс ~20 ALU на луч к солнцу на наклон в конусе.
  При 0° — прежние 4 точки с чередованием.
- **Облачный хост (`Render Path = Cloud Host`):** раунд 45, режим рендер-таргета 0 — 0,38 мс в окне проб 894 × 813
  (трасса 0,30 + реконструкция + наложение). Умолчания раунда 46 у камеры — режим 3 (трасса в полном разрешении: в 16 раз
  больше лучей, чем режим 0) и `SampleMinCount` 32: у камеры владельца облако (трасса режима 3 + наложение) **2,9 мс**
  (2,55–3,56; раунд 46; FogMS на кадре решения 3,65 мс, на кадре удержания 0,70). Рычаги цены:
  `r.FogMS.CloudHost.RTMode` (1 — вчетверо дешевле 3), `NearDistanceKm`, `ViewSampleScale`. `Host Prefilter` — только ALU.
- **Карта теней облака** (раунд 47, `FogMS.CloudHost.SetupShadows`: 5 км, 1024², фильтр 2): проход `VolumetricCloudShadow`
  у камеры владельца **0,14 мс** (трасса карты 0,117 + фильтр 0,020; медиана трёх ProfileGPU). Решатель от галки не дорожает.
  В тех же профилях хост с камерой в 15 м над слоем Box (облако почти во весь экран, режим 3) трассировал 6,9 мс — это цена
  хоста, не тени.
- **Погода (раунд 48, актёр FogMS Weather; Overcast, камера владельца, солнце ~4°, карта теней 10 км / 1024):** проход теней
  `VolumetricCloudShadow` 0,19 мс без погоды → **0,39 мс** с Thin (по умолчанию) / 0,47 мс с Extended (материал хоста v4, выборки
  луча ×1). Видимый проход: Thin — как без погоды (слой и шаги те же); Extended — около +1 мс (+1,04 мс в стабильном прогоне,
  медиана парных замеров +2,1 мс; одиночный ProfileGPU на этой камере шумит на ±1,5 мс). Карта погоды — 512² × одна выборка узора,
  рисуется только при смене погоды (статичная погода кадр не стоит); `RT_FogMS_WeatherSun` (Thin) — 512² × 24 выборки, при смене
  погоды или повороте солнца. Выборки луча тени ×4 при низком солнце (128 с добавкой движка) и материал v3 давали 10–16 мс —
  поэтому кнопка ставит ×1 для Thin.
- **Штатный туман** при 4 px / 208 слоях стоит ещё ~5–6 мс. Отчёт предполагает для продакшена 8–16 px.

**Что влияет на цену:** число направлений (линейно), итерации и tolerance (решатель останавливается, как только
сошёлся), интервал решения, async. Сетка фиксирована (32³). Зависимость от размера Box на экране и от числа
источников (не проверено).

## 7. Известные ограничения и типичные проблемы

- **Несколько Box (раунд 29; в редакторе частично проверено в раунде 32: три Box активны, выключение и удаление без ошибок,
  коммит `1d3aef2`; `Queued` и суффикс статуса не наблюдались).** Любое число включённых Box с `Emissive Injection`
  работает одновременно: у каждого своё поле 32³, свой warm start и своё удержание; в статусе появляется суффикс
  `[Box #N]` (у Box с инъекцией, не владеющих пакетом, — `[Box #N, injection]`). Без инъекции (overlay) допустим один Box;
  если таких два, отключаются они оба, Box с инъекцией
  продолжают работать. Ограничения: лучи и солнечная прозрачность Box не видят плотность других Box; в
  перекрытии Box их свет складывается (двойная подсветка — перекрытий лучше избегать); overlay и его функции
  (тень авторской плотности, кэш теней, SSFS-диск, `FogMS.DumpSpatial`) обслуживают один Box (overlay-Box, иначе
  Box с наименьшим номером); дополнительную точку стриминга Lumen получает только первый Box. Бюджет решений в
  кадре — `r.FogMS.MaxBoxesPerFrame` (раздел 5). Стоимость растёт линейно с числом решаемых Box; сбор источников
  света и флагов теней выполняется на каждый Box отдельно.
- **Ночь при статическом SkyLight.** Кубмапа статического SkyLight после заката остаётся дневной и продолжает
  подсвечивать Box (как и штатный туман). Для смены дня и ночи включите у SkyLight Real Time Capture.
  С ним в сумерках Box отличался от штатного освещения на 5–7 % (замер до перехода на Sky View LUT).
- **Небо при Real Time Capture** берётся из Sky View LUT: в нём нет облаков и HDRI, поэтому под облачным небом
  Box светлее захвата (кроме купола погоды W49: тогда авто-источник — SH захвата, в нём облака погоды есть). Со статическим
  захватом публичная кубмапа тождественна прежнему пути (раунд 25).
- **Задержка.** При `AsyncCompute 1` поле приходит на кадр позже. При `SolveInterval N` изменения, не запускающие
  пересчёт (локальные источники, небо, Lumen, фаза анимации плотности), приходят с задержкой до N−1 кадров
  (по умолчанию один). Движение солнца пересчитывается каждый кадр. При инъекции J ещё проходит через штатную
  временную историю тумана, поэтому отклик на смену света сглажен.
- **Мигание spot/point внутри облака (правка раунда 35, в редакторе ещё не проверено).** Было при `DirectSamples 4`:
  узкий луч мигал с периодом 2·`SolveInterval` кадров (6 Гц при 24 fps) и был в среднем на ~6 % темнее. Теперь
  локальные источники всегда считаются по 8 точкам. Солнце без `Sun Softness` по-прежнему чередует тетраэдры; если на резкой
  границе его тени в облаке видно мигание, `r.FogMS.Transport.DirectSamples 8`. С `Sun Softness` > 0 солнце с раунда 46 всегда
  идёт по 8 точкам (мигание интерьера облака хоста при мягком солнце — раздел 4, `Sun Softness`).
- **Дрожание при движении вперёд/назад (W/S), но не вбок.** Толстые по глубине слои фрокселей скользят по мелкому шуму
  плотности (раунд 35). Лечится `Emissive Injection` (с раунда 36 по умолчанию; дрожание примерно втрое ниже overlay) и,
  по желанию, `Depth Prefilter` (раздел 4; с раунда 37 выключен по умолчанию, потому что смягчает облако); overlay
  (инъекция выкл.) префильтр не лечит. Ещё помогает `r.VolumetricFog.GridSizeZ 384` (тоньше слои; цена не измерена)
  или меньший `View Distance` тумана.
- **Лампа внутри облака при гибриде.** Штатная часть однократного рассеяния от лампы тоже умножается на T_sun·k;
  остальной её свет идёт через поле 32³, без теней на разрешении фрокселей. Как это выглядит, (не проверено).
- **Приближения гибрида:** k считается по яркости, а не по каналам. Небо из штатного пути (SH Lumen) частично
  перекрывается с секторным небом в поле (+1,5–2 %).
- **Облик облака судите в режиме вьюпорта Lit (раунд 40).** Detail Lighting и Lighting Only заменяют альбедо всех
  материалов на 0,3 серого, в том числе в вокселизации штатного тумана (и у Volumetric Cloud). Тогда `Density Albedo`
  Box, гибридный множитель T_sun и режим инъекции на штатное однократное рассеяние не действуют: облако светлее
  (×1,10–1,17 к Lit) и плоское, без тёмного самозатенённого ядра; поле решателя (Emissive) при этом прежнее. Статус Box
  этого пока не сообщает. В сессии с `-BindlessAll` солнечный член гибрида ещё затеняет `Authored Sun Shadow` (A1d)
  поверх T_sun: −1…−2 % яркости облака, с фазой тумана 0,8 против солнца −4 %.
- **Движение Box:** отдельной обработки перетаскивания нет. Каждое перемещение, поворот или изменение размера меняет ревизию
  плотности: история тумана сбрасывается, решение начинается с холодного старта (warm start привязан к границам Box);
  отката к штатному освещению нет (по коду: `FogMS_BoxVolume.cpp`, `FogMS_WorldLighting.cpp`).
- **`[tau core ~X, upper bound]`** — оптическая толщина среды Box: минимум τ по трём осевым хордам через центр Box,
  Density × (затухание `Density Edge Feather`) × (профиль высоты и полоса `Threshold`/`Softness` при максимальном шуме —
  профиль + `Detail Strength`), считается на CPU раз в секунду. Шум текстуры и эрозия толщину только уменьшают, поэтому это верхняя граница: реальная τ ниже
  на долю хорды, где шум не дотягивает до порога. X заметно меньше 1 — среда тонкая, многократного рассеяния мало.
- **Статус «Requires…» / «…requires…»** — не выполнено требование раздела 2.
- **«Transport needs Emissive Injection or -BindlessAll…»** — включите `Emissive Injection`.
- **«Waiting for current-frame isotropic transport»** — поле ещё не опубликовано. Если статус не меняется,
  проверьте, что Box виден, а вьюпорт в режиме realtime (не проверено).
- **«Waiting for density atlas GPU upload» / «…waiting for … mip 0 to become resident»** — текстура ещё стримится
  или компилируется. Если статус не уходит, проверьте формат (раздел 3).
- **«Transport unavailable; native lighting with authored density: …»** (с инъекцией — «…authored density (emissive
  injection field cleared): …») — решатель отказал, причина после двоеточия. Часто это неподдерживаемый источник света.
- **`bounce: fallback (<причина>)`** — Lumen-кэш не используется: `Lumen Bounce` Off, сборка не 5.8.2 или
  «Lumen source waiting…» в первые кадры. Box работает; проверьте `Fallback Ground Albedo`.
- **Облачный хост (`Render Path = Cloud Host`, раунд 45; с раунда 46 — умолчание)**: ограничения, статусы, настройки хоста и
  компромисс «зерно против шлейфа» — раздел 4, «Render Path».
- **Погода (раунд 48–49)** — тени облаков погоды на земле, в тумане, Lumen и атмосфере (раздел 4а) и облака на небе (купол,
  раздел 4б, в редакторе не проверен); солнце решателя и героические облака Box погоду пока не видят (W50).
- **Нет тени облака на земле (раунд 47).** Проверьте по статусу Box: `[render: cloud host]` (иначе Box во фрокселях — например,
  его полоса плотности задевает землю SkyAtmosphere: поднимите Box), `[cloud shadow: extent …]` без `off` и без `WARNING`
  (иначе `FogMS.CloudHost.SetupShadows`). При низком солнце тень длинная и ложится далеко от облака (высота × 15 при 4°):
  на сцене без земли по ходу солнца её не видно. Лучи в тумане видны только в пределах дальности Volumetric Fog от камеры.
- **Упакованная сборка:** собирается (`UnrealGame` Development/Shipping), smoke-тест в `-game` пройден. Полный
  `BuildCookRun` и GPU-путь атласа плотности в настоящей упаковке (не проверено).

## 8. Инструменты проверки (в репозитории)

`Tools/FogMSEnergyValidation/ProdProbe/` — скрипты Python и shell для повтора замеров из журнала (описание групп и правила —
`Tools/FogMSEnergyValidation/ProdProbe/README.md`). Они управляют запущенным редактором через мост UE-MCP
(`ws://127.0.0.1:9877`, плагин `UE_MCP_Bridge` проекта, в репозиторий не входит); нужны Python 3, numpy и Pillow. Вьюпорт
должен рендериться: новые скрипты на время прогона выключают `bThrottleCPUWhenNotForeground`, старые требовали окна на
переднем плане.

Замер — `measure.py`, `gpuprofile.py`; A/B — `*ab.sh` и `abinject.sh` (тиры, доставка, гибрид, интервал, async, небо, `Lumen Bounce`,
проход 2); сравнение — `compare.py`, `fielddiff.py`, `resid_stats.py`; сценарии — `nightcmp.py`, `soak.py`,
`nobindless_test.sh`, `pie_test.py`; бесшовный 3D-шум для Volume Texture — `gen_perlin_worley.py`. Дрожание при
движении — `d35_slide.py` (камера стоит, слои сдвигаются) и `d35_dolly.py` (проезд W/S/A/D); варианты `ovl` (overlay),
`ih_pf0`, `ih_pf05`, `ih_pf1`, `ih_pf2` (инъекция + гибрид при `Depth Prefilter` 0/0,5/1/2). Облик раунда 37 —
`d37_lobe.py` (живое солнце, виды `against`/`front`/`mine`; W36, лепесток 0,5/0,6, 0,8/0,6, 0,8/0,8, `Sun Softness` 3°
и повтор W36 как шумовой пол; яркость в ROI Box против W36 и мерцание между кадрами; `results/diag37/lobe_sheet.png`)
и `fwd_lobe_check.py` (без редактора: узел v2 при c = 0/0,5/1 — среднее лепестка по сфере = 1, минимум ≥ пола,
тождество при s = 0 и S = 0, конечность при `MS Occlusion` 0; v2 при c = 0,5 побитово равен v1; диск конуса солнца).

Облачный хост (раунд 45): `matedit_cloud.py` (материал `M_FogMS_Cloud` + `MI_FogMS_Cloud`, связи проверяются по T3D;
с раунда 46 — v2 с узлом `FogMS_CloudFootprint`: запуск на v1 перестраивает материал на месте, откат при ошибке, повтор
печатает `ALREADY_PATCHED`), `d45_p2.py` (тождество при `Froxel Fog`, состояния хоста, дрожание, облик, цена; лист
`results/diag45/p2_sheet.png`). Раунд 46: `d46_host.py` — короткая проверка по статусу и логу (8 точек солнца при мягком
солнце, хост следует за перемещением и масштабом Box и подгоняет слой, настройки хоста применены и возвращаются, ошибок
нет) и одна цифра цены хоста с префильтром; без серий кадров. Раунд 47: `d47_shadow.py` — тень Box через карту теней облака:
статус и подсказки, `FogMS.CloudHost.SetupShadows` и его строка лога, возврат cvar без хоста, предупреждение о чужом облаке,
поле решателя с галкой и без (`FogMS.DumpSpatial`, небо и плотность заморожены, `Lumen Bounce` Off), пара кадров для глаз и
одна цифра — цена прохода `VolumetricCloudShadow`. Раунд 48 (погода): `texgen/gen_weather_textures.py` (узор погоды, curl,
LUT типов; офлайн, воспроизводимо), `matedit_weather.py` (текстуры, материалы композиции и солнца, пресеты), `matedit_cloud.py`
v4 (ветка погоды в проходе теней; Box в проходе теней считается только в своей маске), `d48_weather.py` — смена пресета за 10 с
(лог, статус, карта, ветер), Clear = без актёра (слой, cvar, пара кадров), удаление актёра возвращает слой и cvar, чистый лог,
цифры видимого прохода и прохода теней для Extended и Thin (`cost`) и парные чередующиеся замеры для ворот решения 2 (`gate`).
Раунд 49 (купол-небо): `matedit_weather.py` строит и `M_FogMS_WeatherSky`; `d49_sky.py` — статус источника неба, отсутствие купола
без актёра и на Clear, зонд амбиента (белая сфера в сцен-захвате), цена прохода купола (выполнена только подкоманда `matedit`).
`matedit_injection.py` — первая вставка графа инъекции в `M_FogMS_Density` (история; на текущем материале печатает
`ALREADY_PATCHED`).

Для A/B полей зафиксируйте небо (`r.SkyLight.RealTimeReflectionCapture 0` на время прогона) и сравнивайте
с шумовым полом одинаковой конфигурации. При Real Time Capture одинаковые прогоны сами расходятся до ~1,4 %.

## 9. Ассеты: что не входит в репозиторий и как их получить

**Политика (с коммита `083341c`).** Репозиторий содержит только код плагина. Ассеты Unreal (`.uasset`, `.umap`) из
`Content/` в git не хранятся (`.gitignore`: `Content/**/*.uasset`, `Content/**/*.umap`). Они живут локально: в проекте —
`<Project>/Plugins/MultiLobeSpec/Content/FogMS/…` (путь в движке `/MultiLobeSpec/FogMS/…`), у разработчика — ещё и в
игнорируемой папке `Content/` рабочей копии, которую сборка плагина копирует в пакет. Создаются и обновляются они в
запущенном редакторе скриптами `Tools/FogMSEnergyValidation/ProdProbe/matedit_*.py` (Python-консоль редактора:
`py "<репо>/Tools/FogMSEnergyValidation/ProdProbe/<скрипт>"`, когда эти ассеты никто не редактирует). Скрипты идемпотентны
(повтор печатает `ALREADY_PATCHED`), связи узлов проверяют по T3D-экспорту и при любой ошибке ничего не сохраняют.

| Ассет (`/MultiLobeSpec/FogMS/…`) | Кто его читает | Кто создаёт / правит | С нуля? |
|---|---|---|---|
| `M_FogMS_Density` | Box (`FogMS_BoxVolume.cpp`, конструктор): свой MID, путь Froxel Fog и инъекция | `matedit_injection.py`, затем `matedit_density.py` **правят существующий граф на месте** (им нужны узлы исходного графа: `MaterialExpressionCustom_3`, `MaterialExpressionTransformPosition_0`, параметры `FogMS_Albedo`, `FogMS_ZeroEmission`, `FogMS_Noise`; промежуточные правки инъекции коммитов `6c8e2a6`/`59ac907` делались скриптами вне репозитория) | **нет** |
| `T_FogMS_DefaultVolume` | заглушка Volume Texture для параметров-текстур материалов (линейная копия штатной текстуры движка) | скрипта нет; `matedit_cloud.py` без неё останавливается (`Missing …`) | **нет** |
| `M_FogMS_Cloud`, `MI_FogMS_Cloud` | облачный хост (`FogMS_CloudHost.cpp`) | `matedit_cloud.py` (строит граф целиком, версия v4; берёт HLSL из `matedit_density.py` и текстуры погоды) | да |
| `Weather/T_FogMS_WeatherPattern`, `T_FogMS_Curl2D`, `T_FogMS_CloudTypeLUT` | погода | `matedit_weather.py` импортирует PNG из `texgen/gen_weather_textures.py` (seed 11, SHA-256 сверяется) | да |
| `Weather/M_FogMS_WeatherCompose`, `M_FogMS_WeatherSun` | погода: карта погоды, столб к солнцу | `matedit_weather.py` | да |
| `Weather/M_FogMS_WeatherSky` | купол-небо (раунд 49) | `matedit_weather.py` | да (в истории git его нет) |
| `Weather/DA_FogMS_Weather_Clear` / `_Scattered` / `_Broken` / `_Overcast` | пресеты погоды | `matedit_weather.py` (числа пресетов — в C++) | да; без ассета `FogMS.Weather.Set <пресет>` берёт встроенные значения |

Без ассета плагин не падает, а пишет статус: нет `M_FogMS_Density` — плотность Box выключена («FogMS density cube or additive
Volume material is unavailable.»); нет `M_FogMS_Cloud` — Box во фрокселях (`[cloud host: none (M_FogMS_Cloud is missing: run
matedit_cloud.py), froxel fallback]`), кнопка / `FogMS.CloudHost.Create` ничего не создаёт («… are missing … nothing spawned»);
нет ассетов погоды — актёр `Inactive: weather assets missing … (run texgen/gen_weather_textures.py, then matedit_weather.py in the
editor)`; нет `M_FogMS_WeatherSky` — купол скрыт («… is missing or older than W49 (run matedit_weather.py)»).

**Чистый клон — порядок.**
1. Ассеты, которые скрипты не создают, взять из истории git (последний коммит, где они есть, — `201f33f`: 13 ассетов
   раунда 48, без `M_FogMS_WeatherSky`): `git restore --source=201f33f --worktree -- Content/FogMS`. Файлы попадут только в
   рабочую копию, индекс не меняется (они игнорируются).
2. Собрать плагин (`RunUAT BuildPlugin`, папка `Content/` войдёт в пакет) и установить в проект.
3. Текстуры погоды: `python Tools/FogMSEnergyValidation/ProdProbe/texgen/gen_weather_textures.py --out <папка>` (numpy +
   Pillow). Папку по умолчанию (`D:/FogMS_ProbeFrames/texgen`) переопределяет переменная окружения `FOGMS_TEXGEN_DIR`
   **процесса редактора** (скрипт импорта выполняется внутри редактора).
4. В редакторе по порядку: `matedit_weather.py` (текстуры, материалы погоды, пресеты, `M_FogMS_WeatherSky`; ожидается
   `WEATHER_OK` или `ALREADY_PATCHED`) → `matedit_cloud.py` (материал хоста; ожидается `MATERIAL_OK` или `ALREADY_PATCHED`) →
   `matedit_density.py` (на ассете из шага 1 ожидается `ALREADY_PATCHED`).
5. Чтобы следующая сборка из рабочей копии содержала эти ассеты, скопировать сохранённые `.uasset` из проекта в `Content/`
   рабочей копии (подкоманды `copyback` в `ProdProbe/d46_host.py`, `d48_weather.py`, `d49_sky.py`). В git их не добавлять.

Процедура чистого клона целиком не прогонялась (не проверено); шаги сверены с кодом скриптов.

**Упаковка игры.** `M_FogMS_Density` — жёсткая ссылка класса Box (`ConstructorHelpers` в конструкторе, свойство
`DensityMaterial`). `M_FogMS_Cloud` / `MI_FogMS_Cloud` и все ассеты погоды код ищет по пути (`FogMS_CloudHost.cpp`,
`FogMS_Weather.cpp`): в кук они попадут, только если на них ссылается уровень (сохранённый хост, назначенные ассеты в группе
`Assets` актёра погоды) или папка добавлена в `Additional Asset Directories to Cook`. Настроек кука в `Config/` плагина нет.
В упаковке не проверено.
