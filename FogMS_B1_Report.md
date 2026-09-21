# FogMS B1 — World (Current Frame)

Протокол от 21.09.2026 для Package6. **Сборка, исполнение на GPU, камера и ограниченные проверки линейности/нулевых вкладов пройдены. Владелец подтвердил улучшение, но отметил слишком тёмный indirect. Сохранение энергии и полная физическая согласованность с UE не подтверждены.** Проект остаётся в прежней карте; новое рассеяние выбирается через **World (Current Frame)**. Последующий разбор — `FogMS_Energy_Audit.md`. По решению владельца срез публикуется для независимого аудита; развитие алгоритма ждёт его результатов.

B1 вычисляет освещение в пространственной сетке Box и три дополнительных порядка рассеяния внутри текущего кадра. История volumetric fog больше не служит источником этого solver. Работа остаётся внутри MultiLobeSpec и существующего уровня `/Game/FogMS_Test/FogMS_Box`; Engine fork/patch запрещён.

## 1. Что сравнивать

| Scattering Mode | Источник и поведение |
|---|---|
| **Off** | Дополнительного MS нет. Существующие A1, authored sun shadow, indirect shadowing и density управляются своими настройками. Это не выключение всего FogMS. |
| **Octaves** | Прежняя художественная аппроксимация дополнительных directional orders, без переноса между ячейками. |
| **Spatial (Experimental)** | Прежний режим: источник берётся из fog history предыдущего вида камеры. Ограничения frustum/history сохраняются. |
| **World (Current Frame)** | Новый источник в сетке Box: собственный список lights сцены, sky и Lumen surface radiance; три прохода переноса без чтения fog history. |

В **World** значение `Spatial Strength=0` выключает дополнительные порядки, но **оставляет новый базовый indirect**. Для возврата к предыдущему способу получения базового света выбирается **Off**. Режимы взаимоисключающие; World не складывается с Octaves или старым Spatial.

## 2. Что именно рассчитывается

Сетка содержит `32³` ячейки и следует трансформу Box. Координаты ячеек и направления выборок не зависят от положения камеры. Каждый кадр заново формируется источник: 64 фиксированных направления для sky/поверхностей, затем три порядка переноса по 12 направлениям с 16 midpoint-сегментами на луч. Это ограниченная аппроксимация, а не решение бесконечного ряда рассеяний.

```text
q0 = sigma_s * (Jdirect + Jindirect)
J(k+1) = SpatialStrength * Transport(qk)
q(k+1) = sigma_s * J(k+1)
MSOnly = J1 + J2 + J3
```

`Spatial Strength` в диапазоне `[0, 0.5]` ослабляет каждый новый порядок отдельно. `Spatial Distance` задаёт конечную дальность одного переноса, в сантиметрах; текущий диапазон UI — 10–2000 cm. Меньший радиус может убрать дальнее заполнение даже при той же плотности.

`sigma_s` источника получается из авторской плотности Box и `Density Albedo`. Прямой source включает directional/point/spot из `Scene->Lights`, без camera light-grid culling. Геометрия ограничивает лучи; medium transmittance считается по Box. Surface radiance берётся из ресурсов Lumen, зарегистрированных в текущем RDG-графе, с нормалью реального попадания и обратным cached exposure. Геометрическое попадание без подходящей Lumen card остаётся препятствием с недоступным освещением; оно не превращается в sky miss. Sky берётся из штатного обработанного cubemap или готового RTC capture; его volumetric intensity применяется один раз только к escaped sky rays.

В native fog заменяется только базовый Lumen indirect **внутри Box**, с существующим feather. `MSOnly` добавляется перед единственным native умножением на receiver `sigma_s` и `PreExposure`. Native first-order sun/local lighting, density/extinction, emission и финальная интеграция fog сохраняются. Источники низших порядков повторно не суммируются.

«Текущий кадр» означает отсутствие fog-history feedback в B1 и использование ресурсов текущего графа. Сам Lumen surface cache и RTC sky имеют собственное расписание обновления. Native final fog reprojection тоже остаётся: B1 не обещает полностью убрать temporal-зависимость изображения.

## 3. Протокол в существующем уровне

1. Открыть `/Game/FogMS_Test/FogMS_Box` в подготовленном D3D12/SM6 editor с `-BindlessAll`. Использовать один realtime perspective viewport. Выбрать существующий `FogMS - Live Box`; не создавать второй Box или новый уровень. У Exponential Height Fog оставить `Scattering Distribution=0`.
2. Если live overlay ещё не включён, нажать **Enable Live Box** и дождаться компиляции. Нажать **Enable Indirect Preview**. Этот preset меняет общие renderer settings, в том числе `r.Lumen.AsyncCompute=0` и `r.RayTracing.Culling=0`; прежние значения сохраняются для обратного восстановления. Для World также требуются `r.LumenScene.GPUDrivenUpdate=0` и `r.RayTracing.Nanite.Mode=0`. Это проверяемые условия, а не повод молча переключать остальные RT-возможности. При несовпадении прочитать `Spatial Status`.
3. Выбрать **World (Current Frame)**. Ожидаемый статус: `Active World primary + three current-frame scattering orders (no fog history)`. Для первого сравнения использовать `Spatial Strength=0.35`, `Spatial Distance=500 cm`, сохранив исходные density/albedo/lights и экспозицию. Сообщение `Waiting` во время инициализации не считается подтверждением активного эффекта.
4. В одной неподвижной позиции сравнить **Off → World Strength 0 → World Strength 0.35**. Первый переход меняет базовый indirect; второй показывает добавленные порядки. Затем отдельно сравнить старый **Spatial (Experimental)**. В каждом сравнении оставлять одинаковыми Indirect Shadowing, sun-shadow controls, density и renderer settings. Переключения работают live без повторного Apply.
5. Для World медленно облететь Box, приблизиться/отдалиться, отвернуться и вернуться; отдельно изменить FOV при неподвижной камере. Оценивать одни и те же участки среды. Внезапное исчезновение заполнения, потемнение после поворота, нарастание света от кадра к кадру или статус fallback фиксируются как проблема, а не как успешная стабилизация. Численное сравнение выполняется по HDR-полю в фиксированных world cells: разные экранные пиксели при движении не являются сопоставимыми образцами.
6. Вернуть исходные трансформ камеры/FOV и controls. Для отключения B1 выбрать **Off**; для возврата session renderer settings нажать **Restore Standard Lumen** и дождаться перекомпиляции. Эта кнопка возвращает настройки, сохранённые preview preset, но не откатывает произвольные ручные правки сцены или других CVars. Для сравнения со старым результатом достаточно смены Scattering Mode: кнопку восстановления Lumen нажимать не требуется.

Диагностический `r.FogMS.World.Indirect 0` оставляет только direct source и обнуляет новый базовый indirect внутри Box; это не пользовательский эквивалент Off. После такой проверки вернуть `r.FogMS.World.Indirect 1`. Compatibility guard из `FogMS_Crash_Report.md` сохраняется.

## 4. Проверки Package6

| Проверка | Результат |
|---|---|
| Свежий `BuildPlugin -StrictIncludes`, Package6 | **PASS:** 36 actions, `Result: Succeeded`, `BUILD SUCCESSFUL`, ExitCode 0 — `Build6.log`. |
| D3D12/SM6 World | **PASS:** MainGPU6.log; активный статус, свежие world-field dumps, все world passes. |
| Камера: direct-only | **PASS:** 39 сравнений; максимум MS RMS/baseline **0.000501%**. BaseIndirect равен нулю по условию опыта. |
| Камера: полный GI | **PASS:** 39 сравнений; максимум MS **0.026503%**, BaseIndirect **0.062475%**. Порог каждого поля 3%. |
| Неподвижность и возврат камеры | **PASS:** drift обоих полей ниже 0.5%; worst full GI за последние четыре измерения **0.05893%**. Camera/FOV/autosave восстановлены. |
| Радиометрия | **PASS:** все поля finite/nonnegative; источник 200 против 100 даёт **точно 2× по всем texels**; нулевой источник и albedo дают нулевой MS; Strength0 обнуляет MS, сохраняя BaseIndirect. |
| Lumen Radiance Cache 0↔1 в World | **PASS:** MS отличается на **0.02238%**, BaseIndirect на **0.05388%**; world attenuation остаётся Active. Это уровень изменения native cache, без прежнего отключения attenuation при RC1. |
| Переключения / fallback | **PASS:** старый Spatial и Off доступны; Enabled=false и скрытый Box отключают эффект; g=.25 даёт явный статус требования g=0; возврат World снова Active. Screenshots `qa6-*`. |
| Resource soak | **PASS:** 1800 engine frames / 93 s, 15 переключений World/Spatial/Off, затем 900 кадров World; `soak6.json`. Падений/renderer ensure нет. Это короткая регрессия нового пути, не гарантия многочасовой устойчивости. |
| Файлы и границы | **PASS:** 81 установленный файл совпадает с Package6, 57 source/shader files со свежим Source6, 6 Engine anchors и 2 crash-guard files не изменены. `file-verification.json`. |
| Отзыв владельца | **Лучше, но indirect слишком тёмный; Shadow Strength уменьшен до .5.** Физическая приёмка открыта, требуется независимый аудит. |

Финальная карта сохранена, редактор оставлен открытым в World. `scene-restoration6.json` сравнил 15 акторов, наблюдаемые authored controls/CVars и последний ракурс с `live-authored-5.json`; изменился только служебный порядковый номер transient Density MID после hide/disable. `delivery-runtime6.json` подтверждает HWRT1, Indirect1, RC0 и сохранённый compatibility guard. Временное отключение background throttle возвращено в исходное true; profile UI возвращён в 1.

При завершении один Python assert ошибочно ожидал строку `"0"` у bool-CVar ParallelTranslate, хотя Unreal возвращает `"false"`. Проверка исправлена на числовой getter и успешно повторена; это tooling-ошибка `finish6.py` в логе, не изменение guard и не ошибка renderer. Guard оставался выключающим parallel translation весь запуск.

В `MainGPU6.log:5496` и следующих строках: density 0.012 ms; primary 64 rays 0.664 ms; transport 0.308 + 0.309 + 0.308 ms; publish 0.004 ms, отдельно CopyTexture. Собственные compute passes вместе **1.605 ms**; предыдущий профиль Round5 с copy — **1.708 ms** на RTX 3070. Это отдельные GPU-профили, не среднее по длительному прогону и не полная стоимость preset: отключение RT culling может увеличить native RT-работу отдельно.

`world-first-5.json` описывает resident atlas `32×2048`, RGBA32F: нижняя половина `MSOnly`, верхняя `BaseIndirect`. `renderFrame=sourceProducedRenderFrame=448`; base alpha хранит долю лучей, попавших в поверхности без доступной card. Этот счётчик нужен для диагностики потери source coverage.

В baseline полного GI Package6 средняя missing-ray fraction 4.0791% объясняется началами лучей внутри solid-геометрии: все 1364 ячейки с alpha>0 (858 из них alpha=1) находятся внутри точных кубов пола/колонн. Вне solids 31 404 центра, **0 missing из 2 009 856 лучей**. Проверка центров по трансформам — `missing_cells_review.md`; она не доказывает отсутствие утечек при интерполяции возле стен и не заменяет тест источника emissive.

Камера прошла 12 точек орбиты, 4 расстояния, отворачивание на 90/180°, FOV40/90/120 и две неподвижные серии. Во всех 156 raw dumps producer frame совпадает с dump frame; между barrier и measurement прошло 6 render frames. Это не проверка застывшего cache. Оба поля анализировались отдельно, без тонемаппинга и экспозиционной подгонки. Первый `direct-5` был остановлен guard из-за ручного перемещения камеры и не засчитан; `direct-6`/`full-6` выполнены после согласия владельца не управлять viewport. Измеренная независимость world field не означает независимость всех экранных пикселей: native froxel sampling и финальное временное сглаживание сохраняются.

Артефакты: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_B1_20260921\` — `contract.md`, `Build6.log`, `MainGPU6.log`, `world-probe-{direct,full}-6-{camera,analysis}.json`, `camera_review_6.md`, `radiometry6-*`, `qa6-*`, `file-verification.json`. Исходная карта скопирована в `D:\PersonalProjects\UE5\MimirHead_portfolio 5.7 5.8 - 3\Saved\FogMS_Backups\FogMS_B1_20260921_141955`; последний пользовательский ракурс и свойства — `live-authored-5.json` (он новее первоначального `live-pre-restart.json`).

## 5. Границы B1 и дальнейшая работа

- Поддержан `g=0`, один realtime perspective view, deferred D3D12 SM6, single GPU, HWRT/bindless, адаптер UE **5.8.2**. Captures, stereo, Shipping и forced `r.RDG.AsyncCompute=2` не заявлены.
- Source cells фиксированы относительно Box, но геометрический RT LOD и native Lumen card allocation/обновление ещё могут зависеть от камеры. Дополнительный streaming origin помещается в центр Box; native лимиты coverage этим не отменяются. `GPUDrivenUpdate=0` — отдельное условие нынешнего пути.
- Surface rays ограничены `max(1000 cm, LumenMaxTraceDistance)`. Native default 20000 cm означает **200 m**, а не 20 km. Геометрия за этим пределом не проверяется и miss становится sky. Это отдельная дальность от `Spatial Distance` и EHF View Distance.
- Треугольники с opacity mask считаются opaque; procedural primitives пропускаются; требуется Nanite RT fallback (`r.RayTracing.Nanite.Mode=0`). Visibility lights использует центральный луч, без точного area-light penumbra.
- Значимые rect/IES/light-function/baked-static/native-cloud-shadow источники и превышение 256 relevant lights дают явную причину недоступности. Недоступные ресурсы не должны оставлять старое поле: действует fallback на native базовый indirect и нулевой дополнительный MS.
- Сетка `32³`, фиксированные направления и trilinear interpolation дают ограниченную детализацию, угловое смещение результата и возможные утечки через стены тоньше voxel. Три порядка с damping и конечной дальностью не доказывают энергетическую сходимость при большой optical depth.
- Дополнительный transport рождается в авторской Box density. Он пока не собирает произвольные внешние fog media и их volume emission. Mesh emissive может входить через Lumen surface cache; сохранённый native `MaterialEmissive` сам по себе не означает, что его пространственное рассеяние уже реализовано.

Следующий B2: качество и полнота source coverage, границы/многообразие сред, emission transport, точность пространственной сетки и более широкая фазовая модель. Полный coupling среды с поверхностями и отражениями, направленные sky-transmittance/sky-irradiance поля, согласованный diffuse/specular GI и SSFS в B1 не реализованы. Эквивалентность SSFS или RDR2 этим срезом не заявляется.

Анкеры реализации: `FogMS_WorldLighting.cpp` / `.usf` — RDG producer и три orders; `FogMS_LumenSource.cpp` / `.ush` — current-graph cache contract; `FogMS_WorldSources.cpp` / `.ush` — scene lights и sky; `FogMS_Indirect.ush:225–259` — раздельное получение MS и base indirect; `MultiLobeShaderPatcher.cpp:755–762` — native consumer; `FogMS_BoxRuntime.cpp:150–170,486–530` — guard/status и streaming origin. Это описание исходников, а не замена GPU-приёмке.
