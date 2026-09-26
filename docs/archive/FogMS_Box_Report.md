# FogMS — live Box, реализация и проверка

2026-09-20. Заказчик просмотрел A1, дефектов не заметил и разрешил следующий срез. Отдельно потребовал движение объёма **вживую, без Apply**. Snapshot transform → defines отклонён.

## Обратная связь после установки

Заказчик протестировал live Box: внутри визуально ощущается отсутствие тумана, объёмность уменьшается; переотражение/рассеяние не видно. **Визуальная цель не принята.** Технические проверки ниже подтверждают ограниченный механизм A1/Box, а не достижение вида многократного рассеяния.

Повторная сверка исходников: Box применяется как `ShadowFactor *= lerp(1, exp(-tau), weight)` только к directional; штатная extinction и интегрирование камеры сохраняются. Собственной плотности Box и нового вклада многократного рассеяния нет. Переход между legacy снаружи и ослабленным прямым светом внутри способен выглядеть как исчезновение светлой дымки. Это объяснение направления эффекта, **не доказательство корректной величины tau** и не основание считать отзыв заказчика закрытым.

Перед следующим визуальным срезом требуется раздельная проверка плотности/пропускания луча камеры и освещения, затем величины tau_in/tau_out против аналитического height-fog эталона. Следующий результат должен содержать настоящую локальную плотность и контрольные On/Off на одной среде; Perlin меняет плотность, но сам по себе не добавляет MS. A1b/MS остаются отдельным этапом после разбора исследователем.

Заказчик также указал на два traceback скрипта `capture_main.py`. Исполнитель их видел и исправил, но упустил явное сообщение в итоговой выдаче. В журнале: 15:11:42/15:11:58 UTC — ошибочные обращения к class/property; 15:12:17 — успешный снимок; 15:13:24 — восстановлен background throttle и снят временный request callback. HTTP datarouter относится к AnalyticsET Epic; причина transport failure `Other` не установлена. DDC Maintenance — информационная строка обслуживания кеша.

## Контракт реализации

Один ориентированный Box на мир задаёт область применения A1, а не новую плотность среды. Вне box пропускаются марш плотности и аналитика. Внутри остаётся полный солнечный путь A1: границы box не обрезают внешний свет, внешнюю плотность в доступной сетке или штатную геометрическую тень. Плотность/камера-extinction не меняются.

Плавная граница: `w=smoothstep(0, Feather, min(Extent-abs(LocalPosition)))`; за границей w=0; при Feather=0 внутри w=1. Применяется `lerp(1, exp(-tau), w)`. Вне box остаются базовый UE dispatch, чтение параметров box, проверка границ и GPU divergence: нулевая стоимость всего прохода не обещается.

Передача данных: постоянная plugin-owned external Texture2D (6×1 RGBA32F) и SRV; descriptor index фиксируется при начальном Apply. Native committed allocation создаётся resident и исключается из Engine eviction через публичный external wrapper. Обычный buffer с literal descriptor не обеспечивает residency gathering штатного Fog pass. UpdateTexture2D передаёт 96 байт каждый view family перед rendering commands и возвращает resource в SRV state. Трансформ в identity не входит. Локальный режим требует одного GPU, D3D12/SM6 и UE 5.8 `-BindlessAll`; этот путь проверен отдельным GPU-запуском без изменения Engine/config.

Причина: static-only UB не требует поля в FParameters, но каждый RDG pass заменяет static bindings (`RenderGraphPass.h:703`, `D3D12Commands.cpp:180–182`). Ранний пользовательский UB не доживает до fog pass. Bindless SRV не меняет штатные View/Fog UB и не занимает чужие поля. Экспортированные RHI buffer/SRV и SceneViewExtension API сверяются перед кодом. Режим `-BindlessAll` существует в `RHI.cpp:1151–1155` и учитывается `ShaderPlatformConfig.cpp:33–36`.

При изменении box сбрасывается только blend истории тумана на два кадра через shader overlay. Persistent views также запоминают revision: редкий SceneCapture отвергнет старую форму при следующем рендере. В пределах одного кадра reset объединяется для всех families данного мира. Истории TAA/Lumen и глобальная cvar temporal reprojection не меняются. Reset стоит после выбора штатного supersampling: движение не включает дорогие дополнительные fog samples; во время движения возможен более шумный туман. Отсутствующий/выключенный/невалидный/неоднозначный box даёт bypass FogMS, а не глобальный дорогой расчёт.

## Ограниченные задачи

1. Actor UI: `FogMS_BoxVolume.h/.cpp` (worker, 2 файла).
2. Runtime transport: `FogMS_BoxRuntime.h/.cpp`, `MultiLobeSpec.Build.cs`, module lifecycle `MultiLobeSpec.cpp` (4 файла).
3. Интеграция и приёмка: `FogMS_ShaderPatcher.h`, `MultiLobeShaderPatcher.cpp`, `FogMS_Common.ush`, этот отчёт; синхронизация handover/research после результата.

## Семь критериев

1. Master Off и global A1 сохраняют исходные режимы; strict C++ и допущенные UE shader permutations проходят.
2. Снаружи нет raymarch/аналитики; в ядре результат совпадает с global A1; extinction неизменен.
3. Translation/rotation/non-uniform scale/feather/enabled обновляются live без Apply, нового overlay или shader compile.
4. Feather ограничен [0, min(Extent)], конечен и непрерывен; нулевой feather определяет резкий край.
5. Грань box не обрезает солнечный путь; перемещение грани при сохранении внутреннего ядра не меняет tau.
6. Измерена реакция истории, удаление/дублирование/смена мира, диагностика пропущенных ячеек; descriptor остаётся валиден весь срок активного overlay.
7. Сравнены GPU времена legacy / global A1 / small box / box outside view при одинаковой сцене; сравнение отдельно от цены нового bindless режима.

## Проверки реализации

- Актуальная сборка: `.codex-build/FogMS_Box_20260920_1810/Package3` в родительском workspace. Свежий source staging; BuildPlugin Win64 StrictIncludes, без PCH/unity: SUCCESS, 25 действий. Native carrier использует только public D3D12RHI API.
- `verify_box_math.py` / `box-math-result.json`: CPU float32/Decimal инварианты OBB, feather, transmittance и split-center — PASS. Все 6 анкеров уникальны; семь функций полного пути A1 совпадают с до-Box версией. Это не GPU-доказательство.
- Независимый review обнаружил и закрыл: отсутствие guard для HistoryAlpha при Temporal=0; отрицательный debug sentinel, уничтожаемый MakePositiveFinite; пропущенный reset истории редкого persistent capture.
- `EngineHashCheck.json`: 6/6 контрольных исходников/версии Engine совпали с исходными SHA-256.
- D3D12/SM6 `-BindlessAll`, RTX 3070: итоговая серия `Probe/capture-result.json` завершила 18 кадров. В `Probe/Shots` записаны translation, rotation, non-uniform/negative scale, feather 0/500, Enabled Off, duplicate bypass, удаление дубликата, outside, global A1, legacy и три debug-вида. Transform/property-изменения не вызывают Apply; Apply используется только при смене режима. Это фиксированная камера 1280×720, Sun=10 lux, Exposure Compensation=2.
- Редкий persistent SceneCapture: `Probe/history-result.json`, PASS. После отключения Box первый поздний кадр B при HistoryWeight=1 сравнивался с контрольным C при HistoryWeight=0. MAE(B,C)=0.00001168 против MAE(A,C)=0.02554 для старого включённого Box; отношение 0.000457. Измерено 21185 изменяющихся пикселей, jitter временно выключен и затем восстановлен. Старый Box не остался в истории.
- `screenshot-comparison.json`: MAE RGB в шкале 0–255 для Legacy/Disabled=0.150, Legacy/Duplicate=0.448, Legacy/Outside=0.353, Rotated/NegativeScale=0.258. Для Center/Left=6.976 и Center/Right=7.608. Близость первых пар и выраженное изменение при движении согласуются с контрактом; pixel-perfect равенство не заявляется, jitter/history кадров отдельно не вычитались.
- Пакет установлен после подтверждённого закрытия редактора. Все 55 package-файлов совпали по SHA-256 (`install-receipt.json`). Backup: `Saved/FogMS_Backups/FogMS_Box_20260920_1810/MultiLobeSpec` в проекте заказчика.
- GPU: 56/56 событий прочитаны из LogRHI, прямой и обратный порядок, по 14 измерений на режим. `profile-gpu-times.json/.csv/.md`, run `20260920T185301`. RTX 3070, editor viewport **754×410**, froxel grid **46×26×64**, одинаковая камера/качество, все режимы `-BindlessAll`. Первый замер отклонён: скрытый viewport не перерисовывался и profiler видел только Slate; затем добавлена принудительная перерисовка. Эти старые значения не считаются стоимостью тумана.

| Режим | ComputeVolumetricFog median [min,max], ms | LightScattering median [min,max], ms |
|---|---|---|
| Legacy | 0.079 [0.076,0.080] | 0.050 [0.048,0.051] |
| Global A1 | 0.114 [0.110,0.115] | 0.065 [0.062,0.068] |
| Small Box | 0.099 [0.098,0.103] | 0.051 [0.049,0.053] |
| Box вне кадра | 0.099 [0.098,0.102] | 0.051 [0.049,0.053] |

Small Box сократил LightScattering на 0.014 ms (около 21.5%) относительно global A1 в этой маленькой сцене. Весь fog-pass сократился на 0.015 ms (13.2%); outside всё ещё дороже legacy примерно на 0.020 ms. Это подтверждает ненулевую служебную стоимость. Замер не масштабируется автоматически на большую сцену/разрешение и не измеряет цену переключения проекта с прежнего bindless-режима на All. 1280×720 — размер отдельных скриншотов, не этого benchmark.

Установлена отдельная `/Game/FogMS_Test/FogMS_Box`, launcher и инструкция `FogMS_Box_Test.cmd` / `FogMS_Box_README.md` в корне проекта. SHA-256 исходной карты A1, DefaultEngine.ini и исходного perlin после установки карты не изменились.

Основной проект запущен на D3D12/SM6 с сохранённым `r.RayTracing=1`, существующими MLS-настройками и `-BindlessAll`. `MainProject_Final.log`: FogMS requested enabled=1/error=none, target overlay matches active; активный `FogMS_Config.ush` содержит `FOGMS_BOX_MODE=1`. `main-runtime-receipt.json` подтверждает нужный мир и один включённый actor. Штатная первая компиляция и компиляция overlay завершены. Для наглядности **только новой карте** выставлены Sun=10 lux и Exposure Compensation=4: probe с упрощённым освещением использовал 2, но в основном проекте такой кадр оказался слишком тёмным. Исходная A1 сохранена. Итоговый кадр: `MainProject_Box_Final.png`, просмотрен исполнителем.

При контрольной съёмке основного проекта фоновое throttling блокировало отрисовку. Для снимка временно снят `bThrottleCPUWhenNotForeground` через reflection и затем восстановлен. Вспомогательный Python сначала дважды ошибся в имени API/property, что осталось в `MainProject_Final.log`; исправлено на `load_class` и нативное имя свойства. Это ошибки инструмента съёмки; fog shader/GPU errors в итоговом запуске не обнаружены. После съёмки возвращены обычный viewport и выбранный Box; временный управляющий callback снят.

В журнале сохраняются отклонённые попытки настройки: Apply при несовместимом MLS/static-lighting конфиге probe, handled ensure при снятии несуществующего realtime override, переэкспонированные начальные кадры и рекурсивный Python callback во время Apply. Probe-конфиг и callback исправлены до итоговых кадров. Весь накопленный журнал не объявляется «без ошибок»; успешные результаты относятся к указанным итоговым сериям.

## Протокол проверки заказчика

1. Запустить редактор с D3D12/SM6 и `-BindlessAll`. Это переключает конфигурацию шейдеров UE и при первом запуске требует компиляции. Обычный global A1 не требует этого флага.
2. Добавить **FogMS Box Volume** через Place Actors. Нужен существующий Exponential Height Fog с включённым Volumetric Fog; Box не создаёт плотность. В мире должен быть ровно один включённый Box.
3. В Details нажать **Enable Live Box** и дождаться первоначальной компиляции. Эквивалент: `r.FogMS.Enable 1`, `r.FogMS.BoxMode 1`, `FogMS.Apply`.
4. Двигать, вращать и масштабировать Box. Менять **Feather Distance** в сантиметрах и флажок **Enabled**. Результат должен следовать изменениям без Apply, новых overlays и shader compile. При выключенном Enabled дорогой A1 полностью пропускается.
5. Для общего A1 нажать **Use Global A1** (перекомпиляция при смене режима допустима). Для полного legacy: `FogMS.Debug 0`, `r.FogMS.Enable 0`, `FogMS.Apply`. Остальные эффекты MLS сохраняются.
6. `FogMS.Debug 1`: плотность должна совпадать при любом transform/Enabled. `FogMS.Debug 2`: внутри меньшее пропускание, снаружи белое. `FogMS.Debug 3`: синий означает «A1 не вычислялся вне Box», красный — seam error >5%, зелёный/чёрный — <=5%. Фиолетовый означает отсутствие поддерживаемой directional ветки. Вернуться: `FogMS.Debug 0`.
7. Провал: изменение трансформации требует Apply; дорогое затемнение осталось на старом месте; в ядре появилась зависимость tau от грани Box; плотность поменялась; потерян legacy; GPU crash/shader error. Пограничная фильтрация froxel-сетки и накопление вдоль луча камеры остаются штатными: пиксель объекта снаружи может видеть туман внутри Box перед собой.

Срез editor-only, как и существующий MLS overlay. Live Coding/dynamic module reload не применяется: shader descriptor живёт до выхода редактора. Cook/Shipping, multi-GPU, несколько одновременно включённых boxes, PIE с несколькими мирами и MRQ пока не приняты. Полная независимость от камеры/универсальный seam-порог 5% остаются ограничениями A1 из его отчёта. Box/MS/A1b не смешиваются: многократное рассеяние не входит в этот срез. Визуальная приёмка live Box заказчиком ещё впереди.
