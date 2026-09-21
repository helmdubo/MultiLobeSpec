# FogMS — стабильность и дальность preview

**Уточнение после полевой проверки 21.09.2026:** сбой повторился через 4ч41м и через 255 секунд. Отложенный Apply не был исправлением причины. По трём одинаковым стекам и установленной DLL локализован null-heap restore в native D3D12 RT при BindlessAll. Новый plugin-only compatibility guard и его проверки описаны в `FogMS_Crash_Report.md`. Упоминания неизвестной причины/успешного startup ниже относятся к состоянию предыдущей доставки и не доказывают стабильность без этого guard.

**Доставка от 21.09.2026.** StrictIncludes Build1/Package1 установлен, 70 файлов сверены. Новый depth/clip-код прошёл D3D12/SM6, HDR, camera-motion и повторный startup. Первый немедленный startup завершился аварией; точная причина не установлена, два отложенных запуска того же пакета успешны. Итоговая сверка — `delivery-receipt.json`. Визуальная приёмка владельцем ожидается. Раздел 3 содержит предварительные сравнения; окончательные измерения — раздел 8.

Работа остаётся внутри плагина MultiLobeSpec. Исходники UE 5.8 используются для чтения; Engine fork/patch запрещён. Это улучшение экспериментального Spatial/A1c preview, а не завершённая физическая модель B и не обещание отсутствия мерцания.

## 1. Почему RadianceCache=1 меняет освещение

`r.Lumen.TranslucencyVolume.RadianceCache=1` меняет источник TLV-освещения. При попадании в область radiance cache штатный HWRT-луч ограничивается расстоянием до интерполяции; затем недостающая радиация берётся из соседних probes. Возвращаемый `TraceHitDistance` может быть интерполированной глубиной, измеренной от **probe**, а не от текущего fog froxel.

A1c интегрирует authored density между текущим froxel и endpoint конкретного луча. Подставлять вместо него глубину probe некорректно: начало отрезка другое, а radiance уже содержит смешанные источники. Поэтому текущий guard требует RC=0. При RC=1 indirect attenuation A1c становится неактивным; это не режим «тот же A1c, но с denoiser». Density, directional shadow и Spatial имеют отдельные условия включения.

Анкеры локальной UE 5.8:

- `Engine/Shaders/Private/Lumen/LumenTranslucencyVolumeHardwareRayTracing.usf:82–95,125–158` — ограничение луча, выбор cache/sky, запись radiance/depth.
- `Engine/Shaders/Private/Lumen/LumenRadianceCacheInterpolation.ush:179–190,287–288,406–425,499–511` — расстояние до интерполяции, probe depth, смешивание samples/sky и замена hit distance.
- `Engine/Shaders/Private/Lumen/LumenRadianceCacheHardwareRayTracing.usf:235,298–303` — начало луча в probe и sky на miss. Sky не исключён из RC по определению.
- `Source/MultiLobeSpec/Private/FogMS_BoxRuntime.cpp:65–82,110–133` — необходимые настройки A1c и guard.
- `Source/MultiLobeSpec/Private/MultiLobeShaderPatcher.cpp:549–616` — применение Beer attenuation к отдельным TLV-лучам и запись отдельного fog SH.

Кэширование и фильтрация probes могут сглаживать результат и менять локальные детали; точную причину пропавшего света в конкретном кадре нельзя вывести только из переключения одного CVar. Поддержка RC=1 потребует отдельного контракта передачи света между probe и fog receiver с разделением уже учтённого attenuation. Простое умножение всего Lumen GI или второй march по probe depth этот контракт не создаёт. **RC=1 для A1c пока unsupported.**

## 2. Что стабилизирует новый preset

Fog получает raw TLV SH **до** штатной temporal-фильтрации TLV. В `LumenTranslucencyVolumeLighting.usf:278–288` записываются fog-выходы; затем на строках `290–293` применяется scale ×4, а на `297–337` обновляется штатная history. Эту history нельзя подставлять в FogMS как готовый сглаженный результат: у неё другие единицы и она не содержит отдельного A1c attenuation для fog.

`Enable Indirect Preview` теперь дополнительно задаёт:

```text
r.Lumen.TranslucencyVolume.Temporal.Jitter 0
r.Lumen.TranslucencyVolume.TracingOctahedronResolution 8
```

Это **настройки качества по умолчанию для preview**, а не обязательные условия guard. Их можно менять для live A/B без отключения A1c. Обязательными остаются RC=0, ShareRadianceCacheWithOpaque=0, SpatialFilter=0, GridCenterOffsetFromDepthBuffer=-1 и остальные требования preview, включая Lumen.AsyncCompute=0. Принудительный RDG.AsyncCompute=2 не поддерживается.

При Jitter=0 направления не становятся центрами всех angular texels. UE фиксирует фазу blue noise на 0: `LumenTranslucencyVolumeLightingShared.ush:103–116`; выбор индекса выполняется в `LumenTranslucencyVolumeLighting.cpp:614–618`. Это убирает смену angular sample pattern между кадрами, но сохраняет пространственную ошибку фиксированного набора лучей. Возможны устойчивое смещение яркости и пропуск мелких источников. Другие источники temporal variation остаются.

N8 означает 64 angular rays на froxel против 36 при N6 в исходном тестовом preset: +77,8% rays данного этапа, **не** +77,8% полного GPU frame time. N6 здесь — исходная настройка сцены, не утверждение о заводском default UE. N12 даёт 144 rays и используется как дополнительное сравнение, а не доказательство сходимости к точному решению.

Настройки сохраняются на время сессии. `Restore Standard Lumen` переприменяет overlay без A1c TLV patch, дожидается компиляции/GPU и возвращает сохранённые значения. Последующие ручные изменения CVar сохраняются, если значение уже отличается от установленного preset. Анкеры: `FogMS_BoxRuntime.cpp:64–95,719–749`.

## 3. Уже полученные замеры — до установки

Артефакты: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Stability_20260921/temporal-metrics.json` и соседний `analyze_temporal.py`. По 16 обычных viewport PNG на вариант. Метрика — RMS стандартного отклонения каждого RGB-канала пикселя по кадрам, в диапазоне **RGB 0–255**, после tonemapping; это не luminance и не HDR variance.

Основной ROI `fog_all`: x=[380,1040), y=[190,840). Он содержит и fog/sky, поэтому не изолирует шум FogMS от native sky. Дополнительный `fog_pillar`: x=[490,585), y=[535,770) — непрозрачный приёмник в тумане. HDR `J` — отдельный сохранённый Spatial field 32³, среднее RGB одного dump, не временное среднее поля.

| RC / Jitter / N | Temporal RMS, fog_all | Temporal RMS, fog_pillar | Среднее HDR J |
|---|---:|---:|---:|
| 0 / 1 / 6 | 0,71830 | 0,42197 | 1,724384 |
| 0 / 0 / 8 | 0,47730 | 0,38306 | 1,607078 |
| 0 / 0 / 12 | 0,47296 | 0,37471 | 1,607561 |
| 0 / 1 / 12 | 0,68967 | 0,41371 | 1,721500 |
| 1 / 1 / 6 | 0,53190 | 0,41619 | 3,159106 |

При переходе RC0/J1/N6 → RC0/J0/N8 temporal RMS основного ROI уменьшился на **33,55%**, одновременно среднее HDR J уменьшилось на **6,80%**. N12 при J0 изменил J относительно N8 только на **+0,030%**, но J1/N12 остаётся ближе по яркости к исходному J1/N6: фиксированную sample bias нельзя объявлять устранённой. RC1-строка демонстрирует изменение режима и A1c bypass; это не эквивалентный baseline для проверки сохранения освещения.

Все семь HDR dumps в исходном JSON конечны, имеют положительный минимум и valid fraction 1. Это проверка сохранённых полей, не доказательство всех GPU permutations. `median_tick_ms` в этом JSON — интервал editor tick; он **не является GPU profile** и не используется для вывода о стоимости.

## 4. Причина раннего исчезновения density и выбранная дальность

Native voxelization ослабляет volume material до конца EHF View Distance. В `Engine/Shaders/Private/VolumetricFogVoxelization.usf:330–344`:

```text
a = 1 - saturate((viewDepth / F - 0.6) / 0.4)
material scattering, extinction, emissive *= 0.01 * a^3 * ShapeMask
```

`F` — Volumetric Fog View Distance, а `viewDepth` — глубина конкретного voxel вдоль камеры, не расстояние до центра Box. При F=100 м fade начинается уже на 60 м:

| View depth | Множитель a³ |
|---|---:|
| ≤60 м | 1 |
| 70 м | 0,421875 |
| 80 м | 0,125 |
| 90 м | 0,015625 |
| ≥100 м | 0 |

Поэтому даже при камере примерно в 49 м дальняя часть протяжённого объёма может уже попадать в fade. Собственный camera-distance fade в FogMS density sampler не найден; Box feather задан в мировых единицах, а atlas использует mip 0. Изменение View Distance не заменяет качество froxel sampling и не устраняет native fade — оно переносит его дальше. Компенсация делением на `a³` не применяется.

Выбранное сравнение: **View Distance=50000 см (500 м), GridSizeZ=208**, GridPixelSize=4 сохраняется. Native fade теперь 300–500 м. Альтернатива 400 м начинает fade на 240 м.

Чтобы не потерять ближнюю Z-точность, число слоёв увеличивается вместе с дальностью. Native распределение (`Engine/Source/Runtime/RenderCore/Public/RenderUtils.h:721–739`, `Renderer/Private/VolumetricFog.cpp:1364–1369`):

```text
N = max(NearClip, FogStartDistance) + 9.5 см
S = r.VolumetricFog.DepthDistributionScale                 // здесь 32
B = (2^(Z/S) - 1) / (F - N)
O = 1 - B*N
slice(z) = S * log2(z*B + O)
z(slice) = (2^(slice/S) - O) / B
Z_new = S * log2(1 + B_old * (F_new - N))                  // сохранение B/O
```

При NearClip=10 см и FogStartDistance=0 переход 100 м/Z128 → 500 м требует Z≈200,005 для прежнего распределения вблизи. Выбрано Z208 с небольшим запасом. Непрерывный шаг на один slice на глубинах 10/49 м составляет примерно 0,337/1,191 м против исходных 0,360/1,214 м. Если оставить Z128 при 500 м, шаг ухудшится до 0,944/1,798 м. Эти числа зависят от NearClip/StartDistance; формула выше — нормативная часть расчёта.

При одинаковом XY объём fog grid увеличивается в **208/128 = 1,625 раза**. Это множитель количества froxels и связанных 3D allocations/grid work, а не измеренный множитель полного GPU времени. Замеры нового depth validation приведены в разделе 8.

Предустановочные distance A/B сохранены в том же taskdir: `distance-8000-{old,extended}.png`, `distance-15000-{old,extended}.png`, `distance-25000-{old,extended}.png`; параметры и положения камеры — `distance-cases.json`. После установки повторены 150/250 м: `coverage-15000.png`, `coverage-25000.png`; объём видим, все runtime statuses Active.

Дальность TLV — отдельный параметр. `LumenTranslucencyVolumeLighting.cpp:74–75,263–268` задаёт EndDistanceFromCamera, умноженный на `clamp(LumenSceneViewDistance/20000,0.1,100)`. За границей TLV native sampler повторяет последний SH-слой через Clamp. Поэтому preset проекта также задаёт **EndDistanceFromCamera=50000 см**. В текущем PP LumenSceneViewDistance=20000, override выключен; отдельный управляемый CameraActor в viewport не используется. LogZScale=.01, Offset=1, ZScale=4 и GridPixelSize32 сохранены. Количество TLV Z-слоёв меняется с 26 на 36 (+38,5%), ближние sample positions не растягиваются. Это настройка сцены, не глобальное требование A1c.

На 250 м TLV остаётся грубым (около 47,5 м между Z samples при ZScale4). Расширение grid не расширяет автоматически Lumen Surface Cache/геометрию или max trace distance и не гарантирует точный дальний local/emissive GI. Дальность Spatial transfer остаётся пользовательскими 2000 см. Эти ограничения не скрываются повышением общей яркости.

## 5. Что меняет новый Spatial clip/depth validation

Spatial использует **предыдущую native Volumetric Fog history**, её собственный pre-exposure, параметры grid и матрицы. Новая проверка ограничивает каждый интегрируемый отрезок достоверной областью этой history:

1. До выбора шага ray segment обрезается шестью плоскостями предыдущего view volume, включая границы логарифмического Z и безопасные UV границы. Clamp sampler больше не должен продолжать освещённый край history наружу. Если receiver изначально вне известной области, неизвестный промежуток не трактуется как vacuum.
2. Проверяются все восемь contributing texels trilinear footprint по соответствующей **предыдущему кадру** ConservativeDepth и матрицам. Native `LightScatteringCS` зануляет ячейки за depth; такой ноль считается отсутствующими данными, а не нулевой плотностью.
3. Проверка повторяет near-face test производителя (`z−0.5`) и reversed-Z сравнение. При недостоверном footprint интегрирование этого луча останавливается; оставшиеся taps не перенормируются. Отсутствующие/несовместимые history metadata, depth и camera cut приводят к отказу от использования этой history.

Анкеры: `Shaders/Private/FogMS_Spatial.usf:46–130,194–207`; `Source/FogMSRender/Private/FogMS_Spatial.cpp:281–311,328–363,393–412`. Native producer/extraction: `Engine/Shaders/Private/VolumetricFog.usf:860–878`; `Engine/Source/Runtime/Renderer/Private/VolumetricFog.cpp:2038–2064`.

Штатная history не переписывается. Clip/depth validation не восстанавливает источники за камерой, вне прежнего frustum или за поверхностями. При движении камеры меняется доступная область интегрирования; возможны disocclusion и расхождение текущей геометрии с освещением предыдущего кадра. Фиксированный angular pattern и конечное число направлений сохраняют bias. Это ограниченный перенос света из доступной camera history, а не полноценное world-space решение B.

## 6. Сборка, установка и незакрытая приёмка

| Проверка | Статус доставки |
|---|---|
| Предустановочные temporal/distance A/B | Получены; границы интерпретации выше |
| Read-only проверка clip/depth source | Выполнена; GPU корректность этим не доказана |
| `BuildPlugin -StrictIncludes`, Build1 | PASS: `Build1.log`, `BUILD SUCCESSFUL`, ExitCode=0 |
| Package1 → существующий проект | PASS по `install-receipt.json`, 70 файлов |
| Первый запуск с новым Package1 | FAIL: CPU access violation в D3D12RHI; причина не установлена |
| Контролируемый повторный запуск того же Package1 | Запуск завершён: все четыре статуса Active, в `MainGPU2.log` ошибок не найдено |
| Финальные GPU-сравнения нового кода | PASS в проверенной сцене; раздел 8 |
| Повторная проверка запуска с постоянным startup wrapper | PASS: MainGPUFinal.log, FOGMS_STARTUP_COMPLETE |
| Новый GPU profile и camera-motion comparison | Выполнены; пределы и численные результаты в разделе 8 |
| Визуальная приёмка владельцем | **PENDING** |

Taskdir: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Stability_20260921`.

Первый запуск завершился на frame 7 CPU access violation по адресу 0x10 в D3D12RHI при native sun ShadowBatch. Shader/RDG error перед сбоем не найден. По диагностике главного исполнителя: frame 2 — overlay Apply, frame 3 — изменение persistent SBT с 1 до 2 slots, frame 5 — RTPSO fallback, frame 6 — новый PSO; авария произошла при исполнении работы frame 5. Это последовательность наблюдений, **не доказанная причинная цепочка и не подтверждённый native engine bug**.

При втором запуске того же пакета Enable был отложен до 120 editor ticks с разрешённым background rendering, RecaptureSky — до 240, ready — до 360. Все четыре статуса стали Active; `MainGPU2.log` не содержит ошибок. Третий запуск через установленный `Saved/FogMS/start_box.py` также прошёл, `MainGPUFinal.log` без ошибок. Wrapper вызывается прежним `FogMS_Box_Test.cmd`; он прогревает native view перед `enable_box.py` и возвращает настройку background CPU. Это два успешных контролируемых старта, **не доказательство установленной и устранённой первопричины** первого AV.

Установка: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Plugins/MultiLobeSpec`.

Backup из install receipt: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/FogMS_Backups/FogMS_Stability_20260921_050523`.

## 7. Короткий протокол сравнения и возврата

Работать в существующем уровне FogMS_Box с существующими actor/camera; новые уровни не нужны. Освещение, exposure, density, Box transform и MS mode должны совпадать между сравниваемыми вариантами.

1. Выбрать `FogMS - Live Box`, нажать **Enable Indirect Preview**, дождаться компиляции. Проверить отсутствие сообщения о неактивном A1c в статусе/логе. Для effect A/B менять **Indirect Shadow Strength 0/1** при одинаковых renderer settings, не RC. RC оставлять 0.
2. Зафиксировать камеру. Сравнить Jitter1/N6 и Jitter0/N8, дождавшись одинакового прогрева. Смотреть и на временной шум, и на изменение яркости/локальных источников. N12/J0 — дополнительная проверка зависимости от числа лучей. Полностью неподвижный результат сам по себе не доказывает правильность освещения.
3. Для дальности сравнить EHF View Distance 10000/Z128 и 50000/Z208 на тех же camera positions примерно 80/150/250 м, с одинаковыми TLV settings. Проверять сохранение формы density и освещения отдельно. Не смешивать дальность камеры с глубиной каждой точки Box.
4. После нового clip/depth-кода выполнить медленный pan, движение вперёд/назад и возврат камеры. Проверять края прошлого frustum, силуэты непрозрачных объектов, появление света после disocclusion и отсутствие световых полос от clamp. Автоматический pan/strafe с возвратом выполнен; субъективная полевая оценка остаётся за владельцем.

Для возврата к исходному **контролируемому A1c preset** установить EHF **Volumetric Fog → View Distance = 10000 см**, затем:

```text
r.VolumetricFog.GridSizeZ 128
r.Lumen.TranslucencyVolume.Temporal.Jitter 1
r.Lumen.TranslucencyVolume.TracingOctahedronResolution 6
r.Lumen.TranslucencyVolume.EndDistanceFromCamera 8000
```

RC=0 и остальные обязательные A1c settings при таком сравнении сохраняются. Для завершения всей экспериментальной A1c-сессии нажать **Restore Standard Lumen** и дождаться перекомпиляции. Это возвращает сохранённую renderer-конфигурацию по описанному правилу; оно не является режимом «A1c c RC1», не возвращает EHF View Distance/GridSizeZ и не отключает автоматически density/directional/Spatial. Их сравнение проводится собственными контролами.

## 8. Окончательные проверки нового пакета

После установки в той же сцене, при RC0/N8 и расширенной TLV-дальности, сняты по 16 обычных viewport PNG (`delivery-j1`, `delivery-j0`). В основном ROI:

| Показатель | Jitter1 | Jitter0 |
|---|---:|---:|
| Среднее RGB изображения, 0–255 | 113,29469 | 110,87151 |
| Pixel temporal RMS std, 0–255 | 0,82075 | 0,61486 |
| Std среднего RGB по кадрам, 0–255 | 0,14106 | 0,02921 |
| Среднее HDR J одного dump | 1,59355 | 1,46902 |

Пульсация среднего RGB уменьшилась в **4,83 раза**, pixel temporal RMS — примерно на **25,1%**. Это разные метрики; утверждение «весь шум меньше в пять раз» неверно. Среднее HDR J стало ниже на **7,81%**: fixed sampling не эквивалентен усреднённому stochastic решению. Источники не выключены и не заменены ambient-константой; оставшаяся spatial sampling bias требует оценки на других сценах.

Spatial Off → On под одинаковым стабильным источником поднял среднее RGB fog ROI примерно с98,88 до110,83. При нулевых Box/global scattering albedo все98304 RGB-компонента J строго нулевые. Четырнадцать readbacks во время 240-tick pan/strafe и180-tick прогрева после возврата конечны и неотрицательны; все записанные статусы Active. После возврата relative L2 J=2,43% к исходному snapshot. При уходе Box к краю frustum доступная доля источников снижалась примерно с90% до63%, и свет менялся: camera-independent B **по-прежнему не реализован**. Полная temporal-инвариантность при движении не заявляется.

GPU profile на RTX3070, viewport1479×954: Spatial с depth validation **0,845–0,882ms**, TLV Lighting **4,595–4,653ms**, native VolumetricFog LightScattering **4,317–4,321ms** в двух захватах. Это отдельные GPU events, не среднее время всего FogMS и не гарантированный FPS. Предыдущий Spatial без depth-validity в другом quality capture был0,217ms; добавленная корректность имеет измеримую цену. GridPixelSize16 для TLV исследован и не оставлен: стоимость выше, улучшение среднего света не подтверждено.

StrictIncludes без unity/PCH — PASS; 12 новых аналитических проверок, включая3000 случайных лучей, — PASS. Сверены70 установленых package files, staged Source/Shaders и6 Engine shader hashes. В сцене осталось15 акторов; неожиданных изменений авторских свойств нет. Density=.6, Spatial Strength=.5, Distance=2000cm и IndirectSteps32 сохранены. Фоновые настройки редактора возвращены к исходным после теста. README и протокол находятся в существующем проекте; редактор оставлен открытым.

Открыто: визуальная приёмка владельцем, fixed-sample bias на разных источниках, точный дальний local/emissive GI, перенос вне camera history, причина первоначального startup AV. Commit/push/merge не выполнялись.
