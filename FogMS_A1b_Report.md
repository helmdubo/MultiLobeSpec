# FogMS A1b — октавное приближение, реализация и проверка

2026-09-20. Заказчик принял визуальный результат TextureDensity («мне нравится результат») и разрешил следующий срез. Работа остаётся в существующем `FogMS_Box`; Engine не изменяется. Полученный список сохранён как `FogMS_References.md` — справочник, не новое задание.

## Разбор перед реализацией

Исполнитель выполняет разрешённую роль исследователя. Результаты A1, плотности и Audit2 рассмотрены: current VBufferA доступен, штатный source term умножается на sigma_s один раз; путь B с чужой историей/RT/volume RT пока не доказан. Для ограниченного следующего среза выбирается **A1b, художественное октавное приближение directional MS**. Это не пространственный перенос и не замена будущего B.

**VERIFIED, UE5.8.2:** `VolumetricCloud.usf:382–397` накапливает коэффициенты a, b с самоквадрированием множителя; `:414–431` использует phase weights c, c². Для single + двух дополнительных октав:

```text
A = (1, a, a³)
B = (1, b, b³)
P = (HG, lerp(Iso,HG,c), lerp(Iso,HG,c²))
```

Прежняя запись `C0=1; Cnext=C²` ошибочна — c в ней не участвовал. В cloud shadow-map ветке recurrence отличается от raymarch; переносится именно указанная конвенция коэффициентов. Кроме того, cloud renderer меняет интеграл view-сегмента отдельно по каждой октаве (`VolumetricCloud.usf:1359–1410`). **Наш вариант изменяет только directional source term**, сохраняя native fog extinction и интегрирование. Полная идентичность Cloud или сохранение энергии не заявляются.

Публичный контекст: [Epic: Volumetric Cloud](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine) описывает октавы как приближение с contribution/occlusion/eccentricity; [Wrenninge 2015](https://history.siggraph.org/learning/art-directable-multiple-volumetric-scattering-by-wrenninge/) рассматривает высокие порядки в высокоальбедной среде. [PBRT: Phase Functions](https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions) используется для проверки нормировки/конвенций фазы. Формулы из статей в shader не копируются. PDF Frostbite 2016 не прочитан целиком: web-инструмент отказал из-за размера 52.6 MB; источником точной реализации остаётся установленный UE.

## Контракт этого среза

Пусть w — существующий BoxWeight, tau — A1 optical depth, P0 — штатная phase. Тогда directional source до native умножения на sigma_s:

```text
F = P0 * lerp(1, exp(-tau), w)
  + w * sum(m=1..N) A_m * exp(-tau * B_m) * P_m
j_dir = E_dir * LightFunction * Shadow_geo * F * sigma_s
```

N=1..2, a/b/c в [0,1]. Это ограниченная арт-модель: при tau=0 добавочные коэффициенты не исчезают (для изотропной фазы максимум 1+a+a³); при sigma_s=0 итоговый вклад равен нулю. При полной геометрической тени добавка тоже нулевая. Она смягчает самозатенение среды, но не заполняет тень за препятствием светом из соседних ячеек. Ambient, Lumen, локальные источники и emissive не масштабируются. Текущая extinction и камера-ray transmittance не меняются. История — штатный фильтр, не рекурсивный solver.

Один enum на Box: **Off (A1 only)** / **Octaves**; будущий Spatial будет альтернативным значением, не дополнительным флажком. По умолчанию Off. Дополнительные свойства: Extra Octaves=2, Contribution=0.5, Occlusion=0.5, Eccentricity=0.5. Всё live; Mode Off / Contribution=0 используют прежний A1 shader path без изменения порядка его операций. Для полного native fog выключается прежний Enabled. A1b доступен только local Box; global A1 остаётся A1.

Данные: прежние rows0..4 неизменны, row5.x — history reset, row5.y/z — mode/orders, row6.xyz — a/b/c. Resident texture **7×1 RGBA32F**, 112 bytes. Сравнение канонического packet до записи reset охватывает все строки; прежний per-view revision защищает late SceneCapture. Невалидные MS-параметры выключают только добавку и предупреждают, A1 сохраняется.

## Изменяемые файлы и проверки

Ограничение: **четыре кодовых файла**, плюс документы и внешние test scripts:

1. `FogMS_BoxVolume.h` — enum и artist properties.
2. `FogMS_BoxRuntime.cpp` — packet/валидация/revision.
3. `Shaders/Private/FogMS_Common.ush` — чистая функция октав и чтение controls.
4. `MultiLobeShaderPatcher.cpp` — точка directional source, ABI define, сохранение прежнего A1 пути без двойного exp(-tau).

Критерии (7):

1. StrictIncludes C++ и D3D12/SM6 shader compile; Engine/Perlin/материал не меняются.
2. Mode Off и Contribution=0 совпадают с A1; полный FogMS Off остаётся native overlay.
3. Добавка конечна, неотрицательна; sigma_s=0, directional=0 или полная геометрическая тень дают нулевую добавку; нормировка фаз и границы модели проверены численно.
4. Extinction debug не меняется от MS; одно и два добавочных порядка дают ожидаемый рост source term на изотропном контроле.
5. Controls/transform/eye live, вне Box нет добавки; поздний capture не удерживает прежние MS controls.
6. A/B изображения в том же уровне, g=0 и направленная фаза; отдельно зафиксированы camera/frustum ограничения A1 и отсутствие пространственного переноса.
7. GPU стоимость и установка с backup; пользователь получает короткий протокол переключения в существующем Box.

Статус: четыре кодовых файла реализованы, StrictIncludes и перечисленные GPU-проверки пройдены; пакет установлен. Заказчик: «выглядит неплохо». Сообщение об освещении всего объёма за непрозрачной преградой **снято заказчиком после повторной проверки**: «А нет, всё работает, я ещё раз проверил, отключив SkyAtmosphere и Sky». Это подтверждает работоспособность теней в его сценарии; независимое разделение вкладов SkyAtmosphere/Sky Light/Lumen этим действием не выполнено. Исправление shader shadow path не потребовалось. Прежняя проверка нулевой geometry visibility была математической/source-review; отдельного изолированного GPU-контроля полностью перекрытого объёма не было.

Диагностика открытой сцены выполнена через установленный UE MCP Bridge (без нового уровня и перезапуска). Snapshot: Sun Cast Volumetric Shadow=true, RT Shadows/InjectRaytracedLights=1; Sky Light intensity=20, Height Fog density=.05, MS Contribution=1. Артефакты: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_Shadow_20260920`. Пробное автоматическое сравнение через SceneCapture LDR выдало почти чёрные одинаковые кадры и **не засчитывается как доказательство GPU-корректности теней**. Viewport screenshot был отложен, поэтому также не используется для синхронного A/B. `compare-receipt.json`: все временно изменённые свойства и CVars восстановлены, камера не менялась, временный capture удалён, cleanup errors отсутствуют. Скрипт карту не сохранял; после восстановления autosave редактор создал штатную автокопию. Отключение пользователем Sky/SkyAtmosphere не отменялось.

## Полученные доказательства

Артефакты: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_A1b_20260920`; действующий probe: `.codex-build\FogMS_Box_20260920_1810\Probe`. Новых авторских уровней не создано.

- **C++:** fresh staged `BuildPlugin -StrictIncludes`, Win64, non-unity/no-PCH, 25 actions, exit 0. D3D12/SM6 + BindlessAll overlay собран в probe. Независимый read-only review четырёх файлов не нашёл повторного attenuation/phase/sigma_s или расхождения packet ABI.
- **CPU double:** 971 кейс / 8488 проверок, 0 ошибок; 87 численных интегралов фазы, максимальная ошибка нормировки 4.18e-12. Проверены нулевые sigma_s/geometry visibility, tau=1e30/b=0, положительность, границы, Off/a=0; пять отрицательных контролей обнаружены. Это не доказательство float32/GPU-эквивалентности.
- **GPU regression:** 15 HDR readbacks, 256×144, ROI 16×16, white VolumeTexture, density=.005 1/m, jitter=0, PreExposure=1. Repeat, Contribution=0 и invalid NaN fallback дают **MAE=0** относительно A1. Одно дополнительное рассеяние увеличило RGB в среднем на .332449, второе ещё на .087717; числа зависят от контрольной сцены. b=0 ярче b=1; c=0/1 при native g=.6 меняет результат, все значения конечны.
- **История:** late persistent capture с HistoryWeight=1 после изменения Contribution=.5→.8 совпал с чистым capture при HistoryWeight=0: **MAE=0**, изменение относительно старого состояния .472122. Per-view revision применяется к новым MS controls.
- **Плотность:** Debug1 Off/Octaves **MAE=0**, значение .499268 при ожидаемом .5. Материал, камера-ray transmittance и интегратор не изменены. Восстановление изменённых свойств/объектов выполнено без cleanup errors.
- **GPU калибровка source term:** ещё 8 readbacks при g=0, b=1, a=.5, white density=.005. Для каждого режима вычтен собственный baseline с Directional VolumetricScatteringIntensity=0 (поверхностный свет сохранён). Ожидалось **1.5 / 1.625**, получено **1.500151 / 1.625027**; относительный L1 residual к A1 signal **.0377% / .0469%**, RGBA16F. При нулевом directional source добавка обоих порядков **MAE=0**. Вне перенесённого Box на неизменном height fog Off/Octaves **MAE=0**. Все временные controls, объекты и CVar восстановлены; cleanup errors отсутствуют.
- **GPU timing:** RTX3070, invariant editor viewport 772×410 / GPU event grid92×52×64, SS:4 LF, 24 samples forward/reverse, 8 на режим. Первый прогон имел выбросы в обратной группе Octaves1 (затронута и voxelization); сохранён целиком. По этой причине сделан один повтор, ещё 24 samples. Повтор: ComputeVolumetricFog median **.1715 / .171 / .172 ms** для A1 / one / two; LightScattering **.104 / .103 / .104 ms**; voxelization **.022 ms** во всех режимах. Стоимость добавки ниже различимой вариации этого небольшого теста; это не обещание нулевой стоимости на другой сетке/сцене. Compute queues не суммировались с Graphics повторно. Probe не измеряет основной HWRT-проект.
- **Установка:** 49 package-owned файлов с SHA256-проверкой, Intermediate исключён. Backup: `D:\PersonalProjects\UE5\MimirHead_portfolio 5.7 5.8 - 3\Saved\FogMS_Backups\FogMS_A1b_20260920_202839`.

**Ошибки тестовой обвязки:** стартовый probe Apply до настройки CVars жаловался на unrelated MLS r.AllowStaticLighting=0; последующий Apply успешен. В read-only inspection исправлен Python-вызов CameraActor.get_camera_component на get_component_by_class. Одно чтение request.json попало на частично записанный файл; доставка переведена на rename готового JSON. Это не ошибки FogMS shader/runtime; старые строки в логе сохранены. NaN warning ожидаем в специально негативном тесте.

**Nubis³:** по уточнению заказчика добавлен в `FogMS_References.md` как приоритетный референс локального воксельного объёма, формы, skipping и cloud-specific lighting. Описание первоисточника проверено; полный разбор слайдов впереди. A1b не объявляется реализацией Nubis³.

## Основной проект и протокол заказчика

Свежий запуск D3D12/SM6/BindlessAll с `r.RayTracing=1`, `r.Fog.ScreenSpaceScattering=0`: overlay применён, shader compile завершён, `main-runtime.json` status=ready, saved=true, accepted_settings_preserved=true. Сохранён **тот же** `/Game/FogMS_Test/FogMS_Box`, на существующем акторе изменены только пять новых MS properties. Pose, Density=.06, Albedo=.9, Threshold=.5, Softness=.25, Perlin, fog и exposure сохранены. Скрипт отключил свой callback и восстановил фоновый throttle; командный bridge в пользовательском редакторе не оставлен.

Просмотрены `Main_A1.png` и `Main_Octaves2.png`: структура плотности сохраняется, внутренняя часть облака светлее, геометрические тени остаются. В main log нет Python Error / Fatal / shader compile errors. Есть ранее существующий warning тонемаппера MLS об анкере `OutDeviceColor / 1.05`, сообщения штатной перекомпиляции и HTTP telemetry warning; они не объявляются ошибками FogMS. Шесть Engine anchor hashes, исходная Perlin, DefaultEngine.ini и старая карта A1 совпадают с backup/baseline. Native density material в этом срезе не менялся.

1. Запустить прежний `FogMS_Box_Test.cmd`, выбрать **FogMS - Live Box**, раскрыть **FogMS → Scattering**.
2. **Scattering Mode=Octaves** — новый срез; **Off (A1 Only)** — предыдущий принятый A1. Всё live, без Apply.
3. **MS Contribution=0** тоже возвращает A1. Начальные a/b/c=.5, Extra Octaves=2. Сравнить 1/2 порядка при одной камере. При g=0 Eccentricity не имеет эффекта; при ненулевом g меняет дополнительную фазу.
4. **Enabled=false** отключает A1 и октавы, оставляя native Perlin fog. **Density Enabled=false** отдельно удаляет локальную плотность. Полный native overlay: `FogMS.Debug 0`, `r.FogMS.Enable 0`, `FogMS.Apply`.
5. `FogMS.Debug 1` Off/Octaves должен совпадать; затем `FogMS.Debug 0`. Провал — NaN/чёрные блоки, изменение плотности от MS, добавка после Contribution=0 или след от старых controls.

Ограничения: directional only; полноценный spatial solver/обход препятствий, качество на произвольных камерах, все аппаратные/permutation сочетания и Shipping этим срезом не доказаны. За фрустумом A1 по-прежнему не видит локальную Perlin-плотность. История не используется как итерационный MS solver. Коммит/push не выполнялись.
