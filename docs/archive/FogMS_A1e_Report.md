# FogMS A1e — мировая плотность и солнечная тень на поверхностях

Запрос 2026-09-21: облако должно ослаблять солнце на земле и объектах; Box transform не должен растягивать и перемещать Perlin. Статус: **собрано, установлено, D3D12/SM6 и полевой запуск проверены исполнителем. Последующий отзыв заказчика: тени грубые, желаемый характер освещения не достигнут.** Пересмотр следующего среза и исследование RDR2 — `FogMS_A1f_Research.md`.

## Контракт

- World Aligned Texture, World Texture Size (cm): рисунок привязан к мировым осям/началу координат; Box вырезает его область. Размер одного повторения задаётся независимо от Box scale. В world mode прежний Tile Scale не используется. Legacy Box-relative mapping сохраняется как выключенное состояние нового режима, defaults не меняют существующие сцены автоматически.
- Base и обе detail-октавы имеют независимые фазы, вычисленные на CPU из Box center и тех же float-частот, которые передаются на GPU. Это предотвращает смену фазы при пересечении границы тайла с нецелым Detail Scale. Материал получает LWC-разность AbsoluteWorldPosition−ObjectPositionWS до Custom; raymarch восстанавливает соответствующий world offset из OBB.
- Cast Sun Shadow: Beer transmittance по собственной плотности FogMS между точкой поверхности и выходом луча из Box. Receiver может находиться снаружи Box. Shadow Strength и Steps — живые настройки. Никакого умножения на BoxWeight(receiver), никакой дополнительной height fog в тени поверхности.
- Один overlay anchor `DeferredLightPixelShaders.usf::UpdateLightDataColor` после `LightDataColor *= AttenuationRGB;`; guard `LIGHT_SOURCE_SHAPE == 0 && USE_HAIR_LIGHTING == 0`. Ослабляется direct directional radiance до BRDF, а RT/VSM тени геометрии, native cloud/atmosphere и light functions сохраняются. Light direction = +DeferredLightUniforms.Direction.
- Покрытие: opaque/masked deferred и соответствующая ветка Substrate; применимо ко всем directional lights этой ветки. Forward/translucency/strands hair, Lumen surface-cache lighting и finite SourceAngle penumbra этим anchor не покрыты. Это первый срез surface shadow, не новая GI-система.
- ABI 16 rows: 12=(f0,f1,f2,worldmode), 13=(phase0.xyz,surfaceflag), 14=(phase1.xyz,strength), 15=(phase2.xyz,steps). Старые12 rows сохраняют смысл. Atlas активируется при любой из функций Sun/Indirect/CastSun и отказывает безопасно при invalid data.

Engine не редактируется. Работа только в существующем `/Game/FogMS_Test/FogMS_Box`. Снимок последних live-параметров/backup — `.codex-build/FogMS_A1e_20260921`; в нём Box center=(-77.06566,2591.69190,1000), extent1000 по всем осям, scale1, density.9/threshold.58/feather100, indirectSteps16. Сохранять именно свежие настройки заказчика.

## Проверки

До установки: StrictIncludes, world-lock CPU oracle (scale/rotation/translation, noninteger frequencies, negative coordinates/large world), D3D12 compile и реальные A/B-кадры surface shadow с временно отключённым отображением тумана для изоляции; затем финальный Lit. Сохранить native RT Shadows. Измерить GPU цену per-pixel march: 32 steps × до24 atlas loads на пересёкший Box пиксель; это не бесплатная cloud-shadow cache.

Материал собирать только с нуля в изолированном Probe. Не повторять DeleteAllMaterialExpressions на загруженном в основном проекте материале: предыдущий срез выявил UE rooted-expression assertion.

### Полученные доказательства

- `BuildPlugin -StrictIncludes`: SUCCESS, 26 actions, no-PCH/non-unity, свежий staging. Установлены и сверены SHA256 все60 файлов пакета (включая8 generated Intermediate-файлов). Главный редактор к установке уже был закрыт; лог подтверждает штатный выход.
- World mapping: 111403 CPU assertions, включая нецелые detail frequencies, отрицательные координаты, вращения и8 знаков масштаба. Это CPU oracle, не доказательство GPU precision во всех масштабах.
- Surface ray: 22211 assertions;600 случайных OBB сравнены с независимым пересечением бесконечного луча с12 transformed triangles. Максимальная ошибка длины8.05e-9cm (double CPU). Homogeneous Beer, outside receivers, misses и native-shadow multiplication прошли.
- Первый Probe запуск отклонён: старый конфиг имел AllowStaticLighting=1; Apply завершился ошибкой. Этот запуск не засчитан. Только в Probe исправлены prerequisites; недостающий экземпляр emissive-материала скопирован из основной сцены. Основной DefaultEngine.ini не менялся.
- Повторный D3D12/SM6 Probe: overlay `Shaders_a97afe6f1265b8fd40354937`, FogMS enabled=1. `Surface_Off/On.png` при r.Fog=0 показали attenuation на земле и нескольких столбах, включая receiver вне Box; native geometry shadows сохранены. `Full_Off/On.png` и дополнительный Surface_On64 просмотрены. Ошибок shader compile/fatal не было. В harness был один JSON read/write race (устранён атомарной публикацией requests); первый profile отвергнут из-за сравнения адресов Python wrappers вместо значений, затем исправлен и повторён. Эти ошибки не выданы за успешные проверки.
- Backup: `Saved/FogMS_Backups/FogMS_A1e_20260921_005731` в основном проекте; прежний плагин, карта до/после сохранения пользовательской сцены и перед установкой, launcher script. Engine shader SHA2566/6 совпали с началом работы.
- Основной Editor: `MainGPU.log`, overlay `Shaders_b3736e2fdb9b46da0f876af8`, target=active, error=none. В числе допущенных shader permutations скомпилированы180 FDeferredLightPS из98304 и4 FDeferredLightApplyToonDiffusePS; это не проверка остальных исключённых платформой permutations. `main-ready.json`: authored_controls_preserved=true, saved=true; WorldAligned=true, Size2000cm, CastSun=true, Steps64, три статуса Active. MID readback подтвердил frequencies .0005/.002/.004, исходную Perlin и world mode1. RT Shadows/HWRT оба1. `Main_Final.png` просмотрен: неоднородная тень присутствует на земле и столбах. Ошибок Python/shader compile/fatal в итоговом MainGPU.log нет; присутствуют UE performance warnings о повторных FindConsoleObject в runtime validation, поэтому «лог полностью чист» не заявляется.

### GPU стоимость нового surface path

RTX 3070, D3D12/SM6, Probe viewport 1367×954, fixed camera/Box/world tile 2000cm, detail .18/4/.5, strength 1. Выполнены 12 captures (Off/16/32/64 по 3). Во время замера второй Editor закрыт. `surface-profile-summary.json`: measured, 12 valid, errors=[]; парсер извлекает именно `RenderLight Light::StandardDeferred: FogMS - Sun`, не одноимённое освещение тумана.

| Режим | Медиана surface directional pass | Медиана разницы с Off в том же раунде |
|---|---:|---:|
| Off | 0.066ms | — |
|16 steps|0.264ms|+0.200ms|
|32 steps|0.462ms|+0.394ms|
|64 steps|0.787ms|+0.719ms|

Для существующей полевой сцены выбраны64 steps; C++ default новых акторов остаётся32. Полный frame time имел большой разброс, поэтому прирост FPS из него не выводится. Стоимость зависит от экранной площади тени и плотности; это не сравнение с native BSM, не гарантия для4K/всей сцены и не полная цена всех FogMS проходов.

## Протокол полевой проверки

1. В прежнем `FogMS_Box` выбрать `FogMS - Live Box`.
2. `FogMS > Sun > Cast Sun Shadow`: On/Off должен менять солнечное освещение земли и поверхностей столбов под облаком. Тени геометрии остаются. `Authored Sun Shadow` отдельно управляет самозатенением тумана.
3. Для изоляции сравнить с `r.Fog 0`: туман скрыт, но его тень на поверхности остаётся. Затем обязательно `r.Fog 1`. Это диагностический режим, не выключатель всей системы.
4. `FogMS > Density > World Aligned Texture`=On. Масштабировать/поворачивать/перемещать Box: меняется область отсечения, узор остаётся в мировых координатах. У границы естественно меняются feather и суммарная толщина.
5. `World Texture Size`=2000cm — исходный размер тайла;1000cm даёт вдвое мельче,4000cm вдвое крупнее. `Tile Scale` используется только при выключенной мировой привязке. Detail Strength/Scale/Second Octave действуют и в world mode.
6. `Surface Shadow Strength`=0 возвращает штатное солнце на поверхностях; Steps16/32/64 меняет точность интегрирования. Полный legacy: `FogMS.Debug 0`, `r.FogMS.Enable 0`, `FogMS.Apply`. Локальная Volume-плотность независима: чтобы убрать её, выключить Density Enabled.

Провал: тень обрезается границами Box на земле; поворот/масштаб Box растягивает world noise; новые шейдерные ошибки; исчезают native RT-тени; World Texture Size не действует. Finite SourceAngle penumbra и затенение Lumen surface GI в этот срез не входят.

## Штатные облачные тени и стоимость

Текущий surface march — прямое вычисление оптической толщины, а не обещание более дешёвого renderer. UE Beer Shadow Map заранее строит front depth / mean extinction / max optical depth и повторно использует их. Для неоднородных точек внутри Box это приближение; current ray integral полезен как reference. Прямая подстановка VolumetricCloudComponent не является drop-in: один active cloud, другой material context, а RenderInMainPass=false не гарантирует отсутствие всех cloud tracing затрат во всех режимах. Исследование plugin-only reuse и кандидата на локальную карту — `FogMS_NativeCloudShadows_Research.md`. Сравнительного GPU-замера с native BSM пока нет.
