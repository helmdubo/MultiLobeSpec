# FogMS B2 — проверяемый перенос света

21.09.2026. Ветка `codex/fogms-b2`, исходная база main `d769a7f`. Работа после внешнего аудита B1. UE 5.8.2 CL56702186, D3D12/SM6/BindlessAll; Engine не изменяется. Пользовательская карта — прежняя `/Game/FogMS_Test/FogMS_Box`.

## Что изменено

Добавлен режим **Transport (B2)**. B1 **World (Current Frame)** сохранён для сравнения. B2 решает изотропный перенос в мировой сетке Box 32³ заново в текущем кадре. Он не использует camera fog history, per-order Strength, ограничение пути 500 cm или смешивание пропускания с единицей через Indirect Shadow Strength. `Transport Iterations` (4–64, по умолчанию 24) управляет бюджетом решения, а не его художественной яркостью.

Плотность усредняется в восьми точках ячейки. Прямой свет Sun/Point/Spot усредняется в тех же восьми точках с затенением геометрией и DDA-интегралом этой плотности до источника/выхода из Box. Общее поле `J` используется и для видимого первого порядка Box, и для дополнительных рассеяний. Прежнее расхождение Point/Spot между native first order и MS seed для этой среды устранено.

Native `MaterialSetupCS` получает authored Box sigma_t/sigma_s. При выбранном B2 raster MID плотности равен нулю, чтобы не добавить её дважды. В `LightScatteringCS` вклад Box заменяется на `sigma_s_Box * J_B2`, остальная native среда и emissive сохраняются. `View.PreExposure` применяется один раз. При недоступном producer сохраняется Box density и native освещение; статус сообщает причину fallback. Выход из B2 восстанавливает raster MID. Это также проверено переключениями режимов, g и visibility.

В финальном overlay позиция Box density и её lighting receiver берётся общим helper: XY=center, Z=`FrameJitterOffsets[0].z`, затем native depth constraint. Это возвращает temporal slice jitter, который native volume raster использует для сглаживания контура. Первая версия с фиксированным Z=.5 дала видимые горизонтальные ступеньки и была исправлена. World-grid coefficients/PCG не получают этот jitter. Полная побитовая raster parity не заявляется: остаются различия View TAA/ClipRatio/depth constraints. Старый cubic camera-distance fade raster-материала намеренно не возвращается в B2.

## Численная модель и единицы

- Расстояния — cm; sigma_t и sigma_s — cm⁻¹, `sigma_s = albedo * sigma_t`, `sigma_a = sigma_t - sigma_s`.
- `J` — scene-linear средняя incident radiance. Direct seed уже содержит изотропную фазу 1/(4π), native light exposure scale и volumetric intensity. Sky/surface radiance не получает фазу второй раз.
- Шесть направлений ±X/±Y/±Z Box с весами 1/6. Формальное решение сегмента вычисляет точные для постоянных коэффициентов cell-average radiance и outgoing radiance. Общая открытая грань передаёт одинаковый outgoing/incoming поток соседям. Вакуум не обрывает путь.
- `J = B_direct + B_boundary + Lambda(sigma_s J)`. При `u = sqrt(sigma_s) J` PCG решает `(I - sqrt(sigma_s) Lambda sqrt(sigma_s))u = sqrt(sigma_s)B`. На равномерной декартовой сетке с взаимными границами оператор симметричен. Предобусловливатель использует диагональ формального решения. Промежуточные знаковые значения не обрезаются.
- Проверяемый **диффузный** баланс: outgoing + absorption = incoming + 4π·sum(V·sigma_s·B_direct). Это не полный баланс внешнего прямого пучка, native поверхностей и Lumen.

CPU-эталон: `Tools/FogMSEnergyValidation/transport_reference.py`, описание — соседний `transport_reference.md`. Основание уравнения: [PBRT — Equation of Transfer](https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/The_Equation_of_Transfer).

## Геометрия и Distance Field

Каждая внутренняя грань проверяется лучами между центрами в обе стороны. Попадание закрывает её для обоих направлений, с отдельным падающим излучением поверхности с каждой стороны. Внешний поиск начинается в соседнем центре, поэтому учитывается и наружная половина ячейки. Тонкая стенка не теряется только из-за малого шага/толщины, если пересекает проверяемые связи.

Реконструкция `J` использует только связную часть восьми соседей: нельзя смешать свет сквозь закрытую грань. Геометрия при этом **квантуется на грани сетки**; положение границы имеет погрешность порядка половины ячейки. Произвольная малая геометрия между проверяемыми лучами не становится точной поверхностью.

Прямые тени используют `OPAQUE_SHADOW` и native per-segment CastShadow bit из `LumenHardwareRayTracingHitDataBuffer`; отсутствие этого буфера вызывает fallback. Для преград переноса используется OPAQUE. Alpha-masked геометрия в этом эталоне рассматривается как сплошная; material opacity/тонкие полупрозрачные поверхности не вычисляются.

Distance Field исследован по локальным исходникам. Global Distance Field зависит от camera clipmaps, может быть недоступен в HWRT-конфигурации и теряет тонкие детали. Поэтому эталон преград оставлен на RT. DF возможен как последующее ускорение с проверкой покрытия; его выигрыш здесь не измерен. [Epic — Mesh Distance Fields](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine).

## Проверки и измерения

Evidence: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_B2_20260921`. Численные файлы и логи сохраняются отдельно от authored map; harness не создаёт уровни и восстанавливает изменения. Исходные глобальные preferences в свежем снимке — throttle=true, autosave=true. Базовая карта и последний пользовательский ракурс взяты из свежего снимка 14:12 UTC, не из старого B1 отчёта.

| Проверка | Результат |
|---|---|
| CPU formal solution, symmetry, dense solve, furnace, absorption, vacuum, reciprocity, convergence | 44/44 PASS |
| GPU uniform furnace, tau=.1,1,4,8,16; pure absorption; albedo=.9; vacuum; two slabs/gap | 11/11 PASS против независимого CPU reference |
| Максимальное GPU/CPU отклонение во всех 11 случаях | RMS 0.012716%; peak-normalized max 0.035143% |
| Максимальный модуль дефекта диффузного потока | 0.000233% |
| 5 cm wall: центр, +0.25dx, внешняя half-cell; matched no-wall controls | 6/6 PASS, утечка в затенённых ячейках 0 |
| Sun, Point, Spot: независимые 1×/2×, остальные источники исключены | Побитово 2×; при нулевых источниках J=0 |
| Камера: повтор, FOV30/90, сдвиг 600 cm, возврат | Максимальный RMS 0.0583% при полном GI |
| Режимы B1/B2/Off, unsupported g, hidden, Enabled=false, восстановление | PASS; 1800 кадров / 93.2 s, без сбоя; crash guard=0 сохранён |
| Native receiver reconstruction возле wall | Package6 и финальный Package7: 6/6 PASS; 8 проб на ячейку, RGB конечен/неотрицателен, утечка за каждой стенкой 0 |

В scene-01 первый тест мощности ошибочно оставил авторский Spot включённым. Это не PASS линейности; проверка исправлена в scene-02, где источники действительно изолированы. Первый wall script использовал недоступный Python `MathLibrary.make_transform`; он остановился с успешным восстановлением, затем wall-02 прошёл с Actor API. Initial build/shader ошибки исправлены; failed logs сохранены. HTTP datarouter предупреждения не являются ошибками FogMS.

Численный набор повторён на Package6 и финальном Package7: `gpu_numerical-numerical-02-analysis.json`, `gpu_numerical-numerical-03-analysis.json`, 11/11 PASS. `gpu_wall-wall-reconstruction-01-reconstruction-analysis.json` и повтор `gpu_wall-wall-reconstruction-02-reconstruction-analysis.json` проверяют ту же HLSL-функцию интерполяции, что вызывается из native fog, в восьми точках (.02/.98 по каждой оси) каждой ячейки. В этом диагностическом режиме четвёртый slab занят maxReceiverRGB/valid и **не содержит потоков**; анализатор отклоняет старую flux-схему. Во всех harness восстановление подтверждено свежими значениями.

### Пространственное и угловое уточнение CPU

Однородный бесконечный по Y/Z слой, tau=4, albedo=.9, одинаковое падающее излучение с двух сторон:

| Ячеек по толщине | J в центре, 6 осей | J в центре, Gauss 64 ordinates | RMS отличия поля |
|---|---:|---:|---:|
| 32 | .391487 | .447165 | 16.6173% |
| 64 | .391205 | .446343 | 16.5872% |
| 128 | .391135 | .446133 | 16.5793% |

Gauss 32/64/128 при 128 ячейках даёт центральные .446133503/.446133470/.446133470. Пространственное уточнение не убирает ошибку шести направлений. Это отдельный CPU-тест (`discretization-refinement.json`), а не подтверждение сходимости GPU Perlin-картинки.

### Стоимость

RTX 3070, driver 616.92; viewport 1272×954; исходный полный preview preset, фиксированный ракурс и авторская сцена. По три `profilegpu` кадра каждого режима, без readback в измеряемом кадре:

| Режим | Graphics Frame Time median | Собственные solver passes, сумма exclusive |
|---|---:|---:|
| Off (A1 Only) | 15.18 ms | 0 |
| World / B1 | 17.39 ms | 1.713 ms |
| Transport / B2, 24 iterations | 16.31 ms | 2.043 ms |

Полный Graphics-кадр B2 в данном сравнении дешевле B1, хотя собственный solver дороже. Часть прежних native attenuation операций B2 не использует. Это небольшой локальный замер, не обещание ускорения на других сценах и не стоимость всей системы относительно stock UE с другими глобальными настройками. Async GPU queues не суммируются с Graphics как последовательное время. Evidence: `profile-summary.json`, `MainGPU5.log`.

## Что ещё НЕ подтверждено

1. **Угловая точность.** Шесть направлений дают 2πL/3 вместо πL для isotropic face irradiance. На слое tau4/albedo.9 отличие J от 64-ordinate Gauss reference составляет около 16.59% RMS. Баланс своей дискретной системы не устраняет эту ошибку. Не компенсировать её множителем яркости. Следующий численный этап — уточнение углового представления и его независимая оценка.
2. **Финальная Perlin-картинка.** Solver использует 8-sample cell-average density, native камера — исходную point density и восстановленное J. Их совместная пространственная сходимость не доказана. PASS потока сетки нельзя выдавать за полный энергобаланс итогового изображения.
3. **Lumen coverage.** В текущем scene dump 3028 отсутствующих surface-boundary samples из 196608 face-side обращений (включая внутренние/закрытые поверхности). Они дают нулевой источник, а не выдуманное освещение. Missing mask хранится в старших шести битах primary alpha и в metadata. Уточнение coverage остаётся отдельной задачей.
4. **Coupled media и поверхности.** Height fog/другие native среды сохраняются, но не входят в этот Box-only решатель. Нет обратного физического обмена fog→surface→fog или гарантии полного native Cloud/Lumen совпадения. Существующие filtered sun/surface shadow paths сохранены отдельно.
5. **Scope источников.** g=0, single realtime perspective view, HWRT, Nanite fallback mode0. Ограничения Rect, IES, Light Function, baked-static и native cloud-shadow provider сохранены; unsupported case должен сообщать fallback. Point/Spot используют фиксированную регуляризацию 1 cm² и 8-sample quadrature, а не точную finite-emitter интеграцию.
6. Wind/advection/evolution, направленные ранние порядки, дополнительная эрозия и stochastic reconstruction краёв ещё не добавлены. SSFS не заменяет пространственный перенос.

Полевой протокол: `FogMS_B2_Verification.md`. GPU/CPU доказательства относятся к перечисленным тестам; художественную приёмку проводит владелец.

## Итоговая доставка

- **Package7** собран из отдельного свежего Source7: `BuildPlugin -StrictIncludes`, фактически `-NoPCH -NoSharedPCH -DisableUnity`; `Build7.receipt.json` PASS. D3D12/SM6 глобальные и включённые overlay shaders скомпилированы, `MainGPU7.log` содержит startup complete без Error/Fatal/Assert.
- **84/84** файлов установленного пакета сверены по SHA256; runtime source текущей ветки совпадает со staged source. Шесть Engine source anchors совпали с исходными; crash guard совпал с Git baseline с нормализацией CRLF→LF. Live CVar остаётся false/0. Итог — `delivery7-verification.json`.
- Backup перед установкой: `Saved/FogMS_Backups/FogMS_B2_20260921_192345_8014_Round7`. Сохранена прежняя карта, 16 исходных актёров, текущая камера/FOV55, авторские Density=.6, TextureSize8000, IndirectShadowStrength=.6 и остальные controls. Выбран Transport/24. Throttle/autosave возвращены в true, diagnostic CVars отключены, временных тестовых актёров нет.
- Реальный realtime snapshot `scene-inspect-20260921T152920_069060Z-3bd81c2f.json` подтверждает **Active B2**. Финальный snapshot после восстановления preferences отдельно подтверждает сохранность сцены. В фоне native **Background Process** override выключает realtime и статус закономерно становится `World requires one realtime view.`; это отдельно записано в receipt, а не выдано за Active. Engine source: EditorEngine.cpp:1803–1815, EditorViewportClient.h:414–416, EditorViewportClient.cpp:4817. При проверке нужно активное realtime окно UE.
- Снимки одного viewport/ракурса: `scene-b1-package7.png` и `scene-b2-package7.png`. Нижний контур B2 после Z-jitter заметно ровнее, чем в `scene-b2-final.png` Package6; яркость не подгонялась. Более ранний `baseline-b1.png` из HighResShot не является валидным сравнением: highres включает non-realtime view и вызывает fallback.
- Компактные результаты для передачи аудитору: [`Tools/FogMSEnergyValidation/Results/B2_20260921.summary.json`](Tools/FogMSEnergyValidation/Results/B2_20260921.summary.json), с SHA исходных receipts. Raw GPU arrays, логи и изображения остаются в указанной evidence-папке. Переносимый harness — [`B2Probe/README.md`](Tools/FogMSEnergyValidation/B2Probe/README.md).

Первичная сверка доставки обнаружила ошибки самого verifier: сравнение Git LF hash с CRLF checkout и ожидание строки `0` вместо boolean `false`. Они исправлены с прямой сверкой Git baseline; исходный failed receipt сохранён. Следующая проверка отдельно выделила нормальный background realtime guard. Это не замалчивается и не считается shader/runtime crash. Короткие soak-тесты не доказывают отсутствие падения через несколько часов; прежний compatibility guard сохранён.

Изменения B2 находятся в локальной `codex/fogms-b2`; commit/push/merge этого нового среза не выполнялись. Предыдущая публикация B1 остаётся в main.
