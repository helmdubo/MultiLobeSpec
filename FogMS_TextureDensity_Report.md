# FogMS — локальная плотность из VolumeTexture

2026-09-20. Реализована добавляемая плотность внутри существующего `FogMS Box Volume`. Используется штатная вокселизация Volume material, затем штатное освещение/интегрирование Volumetric Fog и опциональное A1 самозатенение directional. Это не реализация многократного рассеяния. Новые уровни не создаются: пользователь проверяет существующий `/Game/FogMS_Test/FogMS_Box`.

## Управление

В акторе **FogMS - Live Box**, раздел **FogMS / Density**:

- **Density Enabled** включает добавляемую среду. По умолчанию у новых акторов Off.
- **Density Texture** — существующая 3D-текстура; в тесте `/Game/MimirHead/Textures/perlin`, 64³, linear. Оригинал не изменяется.
- **Density** — максимальная extinction в 1/м. **Threshold** выше — больше разреженных областей; **Softness** задаёт полную ширину плавного перехода вокруг порога.
- **Tile Scale** — число повторений по локальным осям; **Density Channel** — R/G/B/A.
- **Density Albedo** — доля рассеяния; **Density Edge Feather** — сглаживание плотности внутрь от граней в мировых сантиметрах.

Transform и параметры обновляются live, без Apply. Плотность и A1 независимы: обычный **Enabled** выключает только A1; **Density Enabled** выключает именно облако. Outliner eye скрывает оба эффекта. Компонент рамки Hidden In Game не выключает среду.

Для полного штатного shader path: `FogMS.Debug 0`, `r.FogMS.Enable 0`, `FogMS.Apply`. Плотность Volume material при этом остаётся штатным источником UE; чтобы убрать и её, снять Density Enabled. Вернуть local A1: `r.FogMS.Enable 1`, `r.FogMS.BoxMode 1`, `FogMS.Apply`.

## Данные и границы

Native cube следует Box Component; его material вычисляет UVW из local position, сэмплирует VolumeTexture с wrap / explicit LOD 0, выделяет канал, применяет threshold и edge feather. Emissive равен нулю. Extinction проходит штатное преобразование 1/м → 1/см. MID-параметры не перезаписываются на неизменных кадрах. Texture/resource/параметры/transform меняют revision для существующего сброса fog history.

Missing/compiling/invalid texture и невалидные числовые параметры временно выключают добавку с предупреждением, без однородной подстановки. Плотность 0 отключает компонент. Отсутствие текстуры не использует default material texture как fallback.

- Слабый положительный height-fog фон нужен для регистрации native Volumetric Fog. Добавляемая среда не вычитает этот фон. Настройки авторских уровней автоматически не меняются; настройка фона выполняется только в существующем тестовом уровне.
- Native volume material начинает затухать после 60% Fog View Distance. Предел разрешения задаётся froxel-сеткой и самой текстурой; камера влияет на дискретизацию.
- A1 читает noise-плотность в пределах сетки. В аналитическом продолжении за фрустумом пока только height fog: внекадровая часть noise не затеняет свет. Солнечный путь не обрезается Box.
- Density revision сбрасывает fog history только при активном **локальном A1 overlay**. При полном `r.FogMS.Enable 0` или global A1 остаётся штатная история UE.
- Один активный A1 Box на мир. Editor-only, UE 5.8.2, D3D12/SM6, `-BindlessAll`, один GPU. Cook/Shipping не заявлены.

## Проверки и доставка

Fresh staging `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_Density_20260920_r2`: **BuildPlugin Win64 -StrictIncludes PASS**, 25 actions, без unity/PCH. Первая сборка обнаружила неподдерживаемое поле shape-компонента у StaticMeshComponent; поле удалено, успешная сборка выполнена из нового staging. Пакет содержит оба plugin-owned uasset.

Материал скомпилирован и реально отображается в D3D12/SM6 на новом actor component; пример `Probe/Shots/Density_Actor.png`. Контрольные black/white VolumeTexture 8³ созданы только в probe; source min/max проверены для всех RGBA.

### GPU регрессия актора

Evidence root: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_Box_20260920_1810`.
`Probe/density-actor-result.json`: 21 HDR capture, ROI 192×88, все значения конечны, cleanup errors нет, Apply между состояниями отсутствует.

- White/off MAE **0.411479**; Perlin/off **0.25118**: среда существует при A1 Off.
- Zero/missing/black residual MAE **2.02–2.75e-5**, при repeat noise **1.009e-5**. Строгий критерий «в пределах измеренного шума» не пройден. Остаток всего **0.0049–0.0067%** white-effect; max residual 0.00048828, signed mean delta менее 1.9e-7. Это не доказательство дополнительной extinction; численная калибровка проверяется отдельно. Pixel-identical возврат не заявлен.
- Eye hidden совпадает с Off **точно**. Late/clean после появления и изменения формы: **7.00e-6 / 4.10e-6**, в пределах repeat noise. Между изменением и поздним capture — 18 callbacks; старое освещение не сохраняется.
- Threshold, Density, channel R→G, TileScale, rotation, negative/nonuniform scale дали различимый отклик **0.081–0.195 MAE**. Это подтверждает live-обновление, но не полную точность реконструкции UVW во всех преобразованиях.
- A1 On/Off сохраняет параметры native density и component. Изменяется свет: Perlin MAE **0.084917**, white **0.197590**. A1 затемняет прямой свет; многократное рассеяние этим тестом не доказано.

### GPU стоимость

`density-gpu-times.json/.csv/.md`, source `Probe/DensityActor.log`, run `20260920T195145`: **24/24** однозначно сопоставленных замера, 8 на режим, прямой/обратный порядок. RTX 3070, фактический viewport **772×410**, dispatch **92×52×64**, GridPixelSize 8. Медианы Graphics queue, мс:

| Режим | ComputeVolumetricFog | VoxelizePrimitives | LightScattering |
|---|---:|---:|---:|
| Density Off / A1 On | 0.1615 | событие отсутствует | 0.102 |
| Density On / A1 Off | 0.1620 | 0.023 | 0.094 |
| Density On / A1 On | 0.1695 | 0.022 | 0.102 |

Печать UE округлена до 0.001 мс. Дополнительные события Compute queue сохранены отдельно, медиана 0.001 мс; очереди и вложенные события **не складываются**. Эти числа не прогнозируют стоимость всей пользовательской сцены, 1080p/4K или большего числа объёмов.

### Численная extinction

`Probe/density-calibration-result.json`: Debug 1, PreExposureOverride=1, temporal/jitter Off, Softness=0, white/black controls, ROI 16×16. Временная непрозрачная поверхность находится внутри ядра объёма; SceneDepth 1868–1949 см, MAE относительно аналитической плоскости 0.40 см. Тестовые акторы удалены, сцена/CVars восстановлены, уровень не сохранялся.

| Density, 1/м | Ожидаемый Debug 1 | Измеренный HDR |
|---:|---:|---:|
| black | 0 | 0 |
| 0.0025 | 0.25 | 0.2496338 |
| 0.0050 | 0.50 | 0.4992676 |
| 0.0075 | 0.75 | 0.7495117 |

Относительное отклонение до **0.147%**, отношения 2.000 / 3.0024 вместо 2 / 3. Повторы и black start/end совпадают точно. При Density=0.005 A1 On/Off даёт **одинаковую extinction, MAE 0**. Это подтверждает масштаб 1/м → 1/см и независимость плотности от A1 на данном GPU-тесте; не является доказательством полного баланса энергии или MS.

### Установка

Пакет `FogMS_Density_20260920_r2/Package`: **49 package-owned файлов** скопированы в основной проект, SHA256 совпали. `Intermediate` не установлен; существующие посторонние файлы плагина сохранены. Резервная копия плагина, текущей `FogMS_Box`, оригинального Perlin и настроек: `Saved/FogMS_Backups/FogMS_Density_20260920_195739`. Шесть контрольных Engine-файлов совпали с прежним hash baseline.

Основной проект открыт на **том же `FogMS_Box`**, actor density/A1 включены; сохранение успешно. `Main_Density_A1_On.png` и `Main_Density_A1_Off.png` просмотрены: облако остаётся в обеих версиях, A1 меняет освещение. `r.RayTracing=1`; временный background throttle восстановлен, Python callback снят. В свежем `MainProject.log` нет Python Error, shader compilation error или fatal; остаются служебные предупреждения (включая HTTP analytics и краткое ожидание компиляции текстуры).

`saved-map-verification.json` дополнительно подтверждает после нового чтения сохранённой карты: pose **(0,500,650), rotation 0, scale (0.8,1.4,0.9)** сохранена, texture ссылка и Density=0.06 пережили сохранение/загрузку. Этот повторный data-check выполнен NullRHI; доказательство картинки — отдельные D3D12 кадры выше. В исходном `main-runtime.json` строковое сравнение Unreal Transform дало ложный `false` из-за адреса памяти в repr; числовая независимая проверка это устранила. Исправлен только способ записи сравнения в script, исходный receipt сохранён с пояснением.

`user-authored-check.json`: исходный Perlin, `FogMS_A1.umap` и `DefaultEngine.ini` побитово сохранены. `source-package-check.json`: C++ и два uasset совпадают с repository source; `.uplugin` штатно нормализован BuildPlugin (Installed, EngineVersion и пустые URL). Установленные файлы совпадают именно с собранным пакетом.

Заказчик визуально принял этот срез: «мне нравится результат, давай следующий срез». Это приёмка локальной плотности, а не ещё не реализованного пространственного MS. Следующий A1b описан в `FogMS_A1b_Report.md`.
