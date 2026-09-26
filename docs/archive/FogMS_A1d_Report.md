# FogMS A1d — детали плотности и солнечное самозатенение

Рабочий срез 2026-09-21. Статус: собран, установлен в основной проект, D3D12/SM6 проверки выполнены; полевая приёмка заказчиком ожидается.

## Решение после native HV comparison

В существующей `/Game/FogMS_Test/FogMS_Box` проверен native Heterogeneous Volume с обычной VolumeTexture Perlin, без VDB/SVT. Собственный материал HV даёт детализацию и самозатенение; диагностическое отключение indirect показывает выраженную тёмную глубину. Однако сравнение потребовало VSM на тестовом солнце: native HV shadow pipeline исключает RT-shadowed lights (`HeterogeneousVolumesLiveShadingPipeline.cpp:3053`, `HeterogeneousVolumesVoxelGridPipeline.cpp:4328`, UE 5.8.2). Поэтому HV не выбран заменой рабочего FogMS при текущем требовании RT-теней. Контрольные картинки — `.codex-build/FogMS_HV_20260920/HV_*`; это визуальный prototype, не доказательство точного совпадения оптической плотности (HV интегрирует в local units при неравномерном scale).

Временный HV actor удалён, исходный Sun CastRayTracedShadow возвращён в Use Project Setting. Основная карта перед экспериментом сохранена и скопирована в `Saved/FogMS_Backups/FogMS_HV_20260920_235251/FogMS_Box_authored.umap`. Новых уровней нет. Engine не изменяется.

## Контракт A1d

- `Authored Sun Shadow`: отдельный live opt-in. Интегрирует собственную Box+Perlin плотность по пути к солнцу до выхода из OBB или SMax, включая часть за пределами камеры. Добавляет аналитическую height fog один раз; VBuffer не добавляется повторно. Остальные локальные среды в этом режиме не учитываются при самозатенении. Native geometry shadow factor сохраняется.
- Off возвращает прежний A1 с VBuffer и аналитическим продолжением. Invalid density/atlas даёт fallback и причину в Sun Shadow Status.
- `Detail Strength=0`, `Detail Scale=4`, `Detail Second Octave=0.5` — defaults. Две более частые выборки той же VolumeTexture и того же канала добавляют signed perturbation перед прежним threshold/softness. Ноль сохраняет старую форму. Материал и atlas используют одинаковую формулу и mip 0.
- ABI остаётся 12 строк: row6.w — sun flag; row11.yzw — detail controls. Atlas нужен при Sun OR Indirect; проверки допустимости A1c блокируют только indirect strength.
- Это самозатенение внутри тумана. Тень тумана на непрозрачной земле и пространственный перенос MS здесь не реализованы.
- Debug 2 показывает солнечное пропускание, Debug 4 — авторскую плотность. Debug 3 внутри authored Box показывает оранжевый: подходящего homogeneous seam reference нет. Снаружи Box остаётся синий sentinel. Это диагностические значения во froxel на глубине поверхности, не тень тумана на этой поверхности.

## Изменяемые части

`FogMS_BoxVolume.h/.cpp`, `FogMS_BoxRuntime.cpp`, `FogMS_Indirect.ush`, directional call и diagnostic sentinel в `MultiLobeShaderPatcher.cpp`, sentinel display в `FogMS_Common.ush`, существующий `Content/FogMS/M_FogMS_Density.uasset`. Формула детализации не требует менять исходную texture.

## Проверки и полевая доставка

Evidence root: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_HV_20260920`.

- `BuildPlugin -TargetPlatforms=Win64 -StrictIncludes`, `-NoPCH -NoSharedPCH -DisableUnity`: PASS. Пакет собран из свежего source staging, установлен и сверены 52 файла (`install-receipt.json`). После исправления shader-only slab соответствующий `.ush` синхронизирован в repo/staging/package/install и скомпилирован на GPU.
- Математические проверки: 25 810 assertions, в том числе 9 380 atlas vs независимый Texture3D tent-kernel, 640 affine/negative-scale cases, 511 OBB rays (`a1d-math-result.json`). Найден и исправлен почти параллельный луч: epsilon-cutoff ошибочно давал путь 500 cm вместо 23.84185791 cm; проверка exact-zero проходит.
- D3D12/SM6 в основном проекте: Global A1 → Box → debug2/3/4 → Lit, shaders compiled, текущий log без `Error:`/fatal/shader compile failure. Sun Shadow Status и Indirect Shadow Status Active. Кадры `A1c_Baseline`, `A1d_QualityOnly`, `A1d_Sun`, `A1d_Detail` показывают отдельные изменения; high-res кадры сами по себе не являются temporal-проверкой.
- Temporal: отдельные 72 обычных viewport PNG с Jitter=1, по 24 на N=3/6/8. Размер 1367×954, все decoded RGB frames различаются, признаков stale capture нет. В центральном ROI среднее temporal luma sigma 0.6234 / 0.4767 / 0.4357 по шкале 0–255. Mean pixel variance N6/N3=0.488, N8/N3=0.381. Контрольный ROI меняется существенно меньше. Это короткая последовательная неподвижная серия с коррелированными выборками, ROI включает и поверхности; не доказательство отсутствия мерцания/шлейфов при движении (`temporal-analysis.json`).
- GPU: по два ProfileGPU на режим, одна камера, grid4/Z128, 64 sun steps, RTX 3070. A1d N6: ComputeVolumetricFog 7.940–8.174 ms; TranslucencyVolumeLighting 2.213–2.629 ms. N8: TLV 3.982–4.256 ms; fog 7.895–15.219 ms (один выброс). Это inclusive scopes, вложенные строки не суммируются; не полный frame и не сравнение со старым поставленным quality preset. Выбран N6 как рабочий компромисс (`a1d-profile-summary.json`).
- Проверенные пять Engine shader files совпадают с исходными SHA256 (`EngineAfter.json`). Engine не редактировался.

Сохранены последние параметры карты с диска: **density 0.9, threshold 0.58, density feather 100 cm**, канал G, transform/scale и scattering controls. Они отличаются от начального live snapshot (.7/.5/10): карта была повторно сохранена после начала сравнения; обе версии зарезервированы, более поздняя не перезаписана старой. Рабочий renderer сохраняет HWRT, RT Shadows, SkyLight/SkyAtmosphere и A1c incoming attenuation. A1c не заменён native Lumen в итоговом preset; количество направлений поднято с 9 до 36, spatial filtering по-прежнему выключен для корректного соответствия radiance и hit distance.

Полевая конфигурация: Authored Sun Shadow on; Detail Strength 0.18, Scale 4, Second Octave 0.5; GridPixelSize4, GridSizeZ128, sun Steps64; indirect Steps32, N6; fog HistoryWeight0.9/Jitter1. `Saved/FogMS/enable_box.py` обновлён, не меняет authored density или камеру. Новых уровней нет.

## Сбой инструмента обновления и восстановление

Попытка заменить граф загруженного материала через `DeleteAllMaterialExpressions` вызвала UE assertion `!IsRooted()` в основном редакторе. Материал на диске остался побайтно прежним; сцена имела сохранённые копии. Не повторять этот путь: прямой запуск `upgrade_fog_material.py` заблокирован. Новый граф создан с нуля в существующем изолированном Probe, со старым probe asset, предварительно перенесённым в backup; результат скомпилирован/сохранён и установлен после завершения редактора. Используется линейная placeholder texture плагина, runtime Perlin заказчика не менялся. Основной редактор восстановлен, карта сохранена. Это ошибка tooling обновления графа, не runtime GPU crash нового FogMS.

## Протокол заказчика

1. Открыт прежний `/Game/FogMS_Test/FogMS_Box`. Выбрать **FogMS - Live Box**. Для старта после нового открытия проекта использовать прежний `FogMS_Box_Test.cmd`: он вызывает обновлённый field preset.
2. В **FogMS → Density** менять **Detail Strength** между **0** и **0.18**. Ноль возвращает прежнюю форму, 0.18 добавляет мелкие разрежения. Scale управляет их частотой, Second Octave — долей второй октавы. Apply не нужен. Очень высокая частота ограничивается froxel-сеткой; это не попиксельный cloud renderer.
3. В **FogMS → Sun** выключать/включать **Authored Sun Shadow**. Off — прежний A1; On — световой путь через весь авторский Box, включая невидимую камерой часть. На полностью видимом боксе отличия могут быть умеренными: самозатенение существовало и раньше. Для выразительных теней сравнивать плотный участок с освещённой границей, сохраняя положение солнца/камеры. Shadow Status должен быть Active.
4. Чтобы временно убрать дополнительное освещение октав и лучше увидеть одиночное рассеяние, выставить **MS Contribution=0**, затем вернуть своё значение **1**. Небо/indirect может заполнять солнечную тень; не трактовать это как отсутствие самозатенения.
5. При желании сравнить только indirect, переключать **Indirect Shadowing** вживую. **Restore Standard Lumen** отключает эксперимент и возвращает штатные настройки с перекомпиляцией; **Enable Indirect Preview** включает его снова. Для облегчения текущего preset: `r.Lumen.TranslucencyVolume.TracingOctahedronResolution 3`; вернуть — `... 6`. Это направления, а не march steps.
6. Пройти камерой вдоль границы, повернуть/передвинуть Box, остановиться. Провал: форма деталей или тень отстаёт от объёма, исчезает при небольшом повороте камеры, мерцание/шлейфы мешают сцене, Status сообщает fallback. Небольшое сглаживание при движении возможно из-за native temporal fog; движение ещё требует полевой оценки.
7. `FogMS.Debug 2` — пропускание солнечного света (тёмный=сильнее ослаблен); `FogMS.Debug 4` — локальная авторская плотность. Вернуть изображение: `FogMS.Debug 0`. Debug3 оранжевый при Authored Sun Shadow — ожидаемо.
8. Для прежней формы и A1: **Detail Strength=0**, **Authored Sun Shadow off**. Для полного отключения FogMS света: **Enabled off**, **Indirect Shadowing off**; плотность независима — **Density Enabled off** убирает и её. Исходный height fog остаётся.

Полноценный пространственный MS, тени от тумана на земле, отдельная detail texture, mip/footprint filtering и production-решение temporal A1c остаются дальнейшими этапами. Этот срез не выдаёт их за реализованные.
