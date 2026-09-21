# FogMS A1 — инвентаризация, реализация и протокол приёмки

Дата: 2026-09-20. Исходная ревизия: `954a165`. Engine: UE 5.8.2, CL 56702186.
Статус обновлён после просьбы заказчика установить FogMS: **BuildPlugin -StrictIncludes прошёл; пакет установлен с backup и сверкой 49 файлов; D3D12/SM6 editor скомпилировал допущенные fog permutations и отрисовал On/Off и три debug-вида.** Полная математическая/визуальная приёмка и ProfileGPU ещё не выполнены. Подробности установки: `FogMS_Project_Install.md`. Исходная локальная проверка ниже сохранена как история этапа до установки.

## 0. Инвентаризация до изменений

- Основной checkout `E:\GITHUB\MultiLobeSpec\MultiLobeSpec`, `main`, чистый. Три входных документа отслеживаются Git.
- Сохранён worktree `.claude/worktrees/volumetric-fog-multiple-scattering-audit-494905`, ветка `claude/volumetric-fog-multiple-scattering-audit-494905`, HEAD `055bbb2`. Единственный untracked файл — `FogMS_Audit_UE58.md`; до правок он побайтово совпадал с копией в основном checkout, SHA-256 `8450906A6774640EFCA3F288AE67301C1A0650601DE09DFC87C92DA3094DED34`.
- Незавершённого FogMS-кода не найдено. Старого `KICKOFF_FogMS_UE58.md` не найдено, включая скрытые/игнорируемые каталоги. Удалений не выполнялось.
- В родительском `.codex-build` найдены старые штатные shader overlays проверки UE от 18 сентября, не реализация FogMS. Они сохранены.
- Размещение внутри MultiLobeSpec отдельной группой **подтверждено заказчиком в этой сессии**.

Все Engine-пути ниже относительно `D:\PersonalProjects\UE5\UE_5.8\Engine`.

| Анкер | Статус и свидетельство |
|---|---|
| а: directional + фаза | VERIFIED `Shaders/Private/VolumetricFog.usf:943–989` |
| б: плотность, source term, история | VERIFIED тот же файл `1161–1179` |
| в: два height-слоя и масштаб | VERIFIED тот же файл `169–180`: `0.5 * GlobalExtinctionScale`; условие PROJECT_EXPFOG_MATCHES_VFOG здесь **не меняет extinction** |
| г: VBufferA + Fog UB | VERIFIED `Source/Runtime/Renderer/Private/VolumetricFog.cpp:1149–1156,1792–1795` |
| д: потребление RT-видимости | VERIFIED `VolumetricFog.usf:962–967` |
| е: HeightFogCommon include | VERIFIED `VolumetricFog.usf:13` |
| ж: GeneralPurposeTweak | VERIFIED `Source/Runtime/Renderer/Private/SceneRendering.cpp:437–454,2028–2038`; обычное значение 1, доступно вне Shipping/Test |
| з: overlay | VERIFIED исходный `Source/MultiLobeSpec/Private/MultiLobeShaderPatcher.h:67–98`, реализация `.cpp:511–541,2713–2963` |

Патчер в пяти строках (пути плагина, номера до правок):
1. `BuildOverlay` копирует Engine/Shaders только в `Saved/MultiLobeSpec/Shaders_<hash>`; Engine не является целью записи.
2. Патчи находят точный текст/функцию и вставляют guarded-код в копию; обязательные анкеры при несовпадении возвращают ошибку.
3. `WriteConfigFile` доставляет defines в `Private/MultiLobeSpecConfig.ush`; FogMS получает отдельный `FogMS_Config.ush`.
4. `GetOverlayBuildId` включает Engine version, PatchVersion и shader config; существующий stamped overlay не перезаписывается при Apply.
5. Apply создаёт/выбирает overlay, ремапит `/Engine`, сбрасывает shader cache и вызывает `RecompileShaders Changed`; C++ плагина требует отдельной сборки.

## 1. План A1 до кода

Пять файлов реализации и этот протокол: `Source/MultiLobeSpec/Private/FogMS_ShaderPatcher.h`, `Source/MultiLobeSpec/Private/MultiLobeShaderPatcher.h/.cpp`, `Source/MultiLobeSpec/Private/MultiLobeSpec.cpp`, `Shaders/Private/FogMS_Common.ush`, `FogMS_A1_Report.md`. Гигиена трёх входных документов и отдельный Audit2 относятся к задачам 1/3. Временный проверочный стенд находится вне репозитория, в родительском `.codex-build`.

Анкеры overlay в штатном `VolumetricFog.usf`: перед `#ifdef LightScatteringCS` — include; **трёхстрочный блок** `GridCoordinate / LightScattering / NumSuperSamples` — локальный результат (одной строки NumSuperSamples недостаточно: она встречается дважды); directional `float ShadowFactor = 1` — марш; перед комментарием `Visualize history rejection` — диагностические значения; после записи `RWIntegratedLightScattering[LayerCoordinate]` — показ диагностического среза. Каждый полный анкер обязан встретиться ровно один раз. `HeightFogCommon.ush`, `LocalFogVolumeCommon.ush` и C++ движка не меняются.

Defines: `FOGMS_ENABLED`, `FOGMS_STEPS`, `FOGMS_SMARCH_CM`, `FOGMS_SMAX_CM`, `FOGMS_EXCLUDE_GLOBAL_LAYER`, `FOGMS_GLOBAL_EXTINCTION_SCALE`, `FOGMS_DEBUG_VIEWS`. Плотность читается через `FOGMS_SAMPLE_EXTINCTION(uvw)`. В A1 нет MS/октав и нет второго режима переноса.

Уточнения, выведенные из данных и математики:

- **VERIFIED:** `GlobalExtinctionScale` привязан только к MaterialSetup (`VolumetricFog.cpp:353,1737`), отсутствует в LightScattering (`1145–1201`) и Fog UB (`FogRendering.h:16–44`). Плагин читает `VolumetricFogExtinctionScale` из активных компонентов при Apply и фиксирует define. После изменения этого свойства обязателен новый Apply. Неоднозначные значения между компонентами/мирами отвергаются.
- **DERIVED:** `S_max` — общий конец луча от исходной точки. Продолжение имеет длину `S_max − s_exit`; `τ_ref` интегрируется на `[0,S_max]`. Добавлять целый `S_max` после выхода означало бы зависимость общей длины от камеры.
- **DERIVED:** квадратные интервалы должны полностью покрывать `[0,S_march]`: края 0/1 фиксированы, внутренние границы сдвигает Halton-jitter, сэмпл — середина каждого интервала. Сдвиг первого края оставлял бы неинтегрированный отрезок.
- **VERIFIED:** MaterialSetup заполняет view-grid внутри resource-grid; padding не является данными. Выход определяется шестью плоскостями frustum; выборки переводятся через view-UVW только в валидные resource texel. Штатный `ComputeVolumeUVFromNDC` уже clamp-ит координаты и не годится для обнаружения выхода. Матрица `TranslatedWorldToCameraView` соответствует обратной froxel-матрице; `TranslatedWorldToView` может быть overridden (`SceneView.cpp:2749–2753`, `VolumetricFog.cpp:314`).
- **VERIFIED:** плотность за ConservativeDepth сохраняется; обнуляется только LightScattering/история (`VolumetricFog.usf:860–878`). Сдвиг центра плотности у геометрии остаётся ограничением; шовный тест использует `r.VolumetricFog.GridCenterOffsetFromDepthBuffer -1`.

## 2. Реализация и локальная проверка

Поток данных: `VBufferA.a → τ_in` плюс `Fog UB + captured Extinction Scale → τ_out/τ_ref`; затем только directional `ShadowFactor *= exp(−τ_eff)`. LocalShadowedLightScattering, MegaLightsVolume, Lumen/sky, emissive и extinction на обычном пути не меняются. При `DirectionalLightHandledByMegaLights` штатная ветка пропускается — FogMS там no-op.

Отдельный `FogMS_Config.ush` включается только из FogMS_Common, который подключён только к VolumetricFog.usf. При выключенном FogMS этот `.usf` вообще не патчится. Build identity включает параметры FogMS и hash его include; неудачный Apply сохраняет прежний remap. Поддержанные анкеры ограничены UE 5.8.2 CL 56702186.

Параметры по умолчанию: Enable=0, Steps=16, MarchDistance=0 (far depth сетки), MaxDistance=2 000 000 см (20 км), ExcludeGlobalLayer=0, DebugViews=1. Все они фиксируются по Apply. Debug-переключение не требует компиляции. Масштаб extinction берётся из активного компонента, а не из cvar и не из загрязнённой LFV плотности. Изменение карты/Extinction Scale требует Apply; **анимация Extinction Scale без повторного Apply не поддержана**. Неоднозначные scale в одновременно активных мирах отвергаются. Fog Density/falloff/height поступают из текущего Fog UB.

`FogMS.Debug 1/2/3` устанавливает `View.GeneralPurposeTweak` в 101/102/103 и временно отключает историю тумана. `FogMS.Debug 0`, успешный Apply с FogMS Off/DebugViews Off, `MLS.Disable` и выгрузка модуля восстанавливают прежние значения. Это устраняет попадание диагностических RGB в обычную историю; проверка C++ поведения пока статическая. Диагностика отвергается, если активный overlay не содержит FogMS/DebugViews.

Проверочный стенд: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_A1_20260920\verify_fogms.py`; итог — `verification.json`, DXC-журнал — `dxc-results.json` в том же каталоге.

| Проверка | Результат и граница доказательства |
|---|---|
| Применение пяти вставок, извлечённых из реальных C++ строк | PASS: все пять анкеров имеют count=1 на установленном UE shader; получен отдельный `VolumetricFog.patched.usf` вне Engine |
| FogMS Off | Временная копия fog shader побайтово совпала со штатной; код патчера возвращается до записи. Рендер-сравнение UE ещё нужно |
| DXC SM6 helper harness | PASS 48 комбинаций entry-point guard, Enable, ExcludeGlobalLayer, MegaLights/Ubershader flags. Это **только FogMS include со стендовыми UB**, не компиляция полного UE shader и не доказательство реальных permutations |
| Аналитический интеграл против независимого Simpson-интегрирования | PASS 300 случаев: два направления/горизонт/нулевой falloff/малый показатель; max relative error ~7.14e−15 в CPU double oracle |
| Разбиение на in/out с общим концом | PASS, max relative error ~3.66e−16 в CPU double |
| Пересечение frustum и полное покрытие jitter-интервалами | PASS 1000 лучей; ошибка суммы длин ≤1.82e−12 см в CPU double |
| FP16 плотность + трилинейный stencil + padding | PASS 108 случаев height fog, N=32, азимуты 0…350°, три jitter; max seam error **0.0306%**. Это синтетическая сетка, не кадр UE |
| Предельный контрпример FP16 | **Ограничение подтверждено:** σt=1e−7/см → half 1.19209e−7; S_in=10 км, S_max=20 км даёт **9.60%** seam error независимо от N. Порог 5% не доказан универсально |
| C++ / UE global shader permutations / D3D12 / ProfileGPU | При первоначальной передаче NOT RUN. Затем C++ strict build, допущенные fog shaders и D3D12 smoke проверены; ProfileGPU и полная приёмка остаются NOT RUN. См. `FogMS_Project_Install.md` |

Проведён независимый read-only review. Исправлены несоответствие overridden-матрицы и восстановление temporal/debug cvar при отключении. На момент первоначальной передачи build/install не выполнялись; последующая установка по прямой просьбе заказчика описана отдельно. Commit/push не выполнялись.

**Производительность — оценка, не замер:** при 1920×1080, 16 px × 64 слоя — 522 240 ячеек, N=16 даёт до 8 355 840 выборок плотности за кадр без учёта отсечения/служебной математики. Самозатенение считается один раз на ячейку и переиспользуется при supersampling. A1 пока обрабатывает видимый объём камеры, не весь мир и не Box Volume. Заказчик утвердил **завершить A1; локальный Box Volume сделать следующим этапом**. Глобальную realtime-пригодность не заявляем.

## 3. Протокол заказчика

### 3.1. Собрать свежий пакет и установить с резервной копией

Закройте Unreal Editor этого проекта. Откройте PowerShell и выполните блок целиком. Он создаёт свежий staging только из исходников плагина, собирает пакет, сохраняет старую установленную версию и ставит новую. Это команды для заказчика: в текущей сессии они **не запускались**.

```powershell
$ErrorActionPreference = 'Stop'
$FogMSRepo = 'E:\GITHUB\MultiLobeSpec\MultiLobeSpec'
$FogMSProject = 'D:\PersonalProjects\UE5\MimirHead_portfolio 5.7 5.8 - 3'
$FogMSStamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$FogMSBuild = "E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_Owner_$FogMSStamp"
$FogMSStage = Join-Path $FogMSBuild 'Source\MultiLobeSpec'
$FogMSPackage = Join-Path $FogMSBuild 'Package'
New-Item -ItemType Directory -Path $FogMSStage | Out-Null
Copy-Item -LiteralPath (Join-Path $FogMSRepo 'MultiLobeSpec.uplugin') -Destination $FogMSStage
foreach ($FogMSPart in @('Source', 'Resources', 'Shaders')) {
    Copy-Item -LiteralPath (Join-Path $FogMSRepo $FogMSPart) -Destination $FogMSStage -Recurse
}
& 'D:\PersonalProjects\UE5\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat' BuildPlugin "-Plugin=$FogMSStage\MultiLobeSpec.uplugin" "-Package=$FogMSPackage" -TargetPlatforms=Win64 -StrictIncludes
if ($LASTEXITCODE -ne 0) { throw 'BuildPlugin failed. Do not install. Send the build log.' }
if (!(Test-Path -LiteralPath "$FogMSPackage\Binaries\Win64\UnrealEditor-MultiLobeSpec.dll")) { throw 'Built DLL missing. Do not install.' }
if (!(Test-Path -LiteralPath "$FogMSPackage\Shaders\Private\FogMS_Common.ush")) { throw 'FogMS shader missing from package. Do not install.' }
$FogMSInstalled = [IO.Path]::GetFullPath((Join-Path $FogMSProject 'Plugins\MultiLobeSpec'))
$FogMSBackup = [IO.Path]::GetFullPath((Join-Path $FogMSProject "Saved\FogMS_Backups\$FogMSStamp\MultiLobeSpec"))
$FogMSRoot = [IO.Path]::GetFullPath($FogMSProject).TrimEnd('\') + '\'
if (!$FogMSInstalled.StartsWith($FogMSRoot, [StringComparison]::OrdinalIgnoreCase) -or !$FogMSBackup.StartsWith($FogMSRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected install/backup path.' }
if (Test-Path -LiteralPath $FogMSInstalled) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $FogMSBackup) | Out-Null
    Move-Item -LiteralPath $FogMSInstalled -Destination $FogMSBackup
}
Copy-Item -LiteralPath $FogMSPackage -Destination $FogMSInstalled -Recurse
foreach ($FogMSBuiltFile in Get-ChildItem -LiteralPath $FogMSPackage -File -Recurse) {
    $FogMSRelative = $FogMSBuiltFile.FullName.Substring($FogMSPackage.Length).TrimStart('\')
    $FogMSInstalledFile = Join-Path $FogMSInstalled $FogMSRelative
    if ((Get-FileHash -LiteralPath $FogMSBuiltFile.FullName).Hash -ne (Get-FileHash -LiteralPath $FogMSInstalledFile).Hash) { throw "Package mismatch: $FogMSRelative" }
}
Write-Host "Package verified. Backup: $FogMSBackup"
& 'D:\PersonalProjects\UE5\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' "$FogMSProject\MimirHead_portfolio.uproject" -d3d12 -sm6
```

`-StrictIncludes` действительно передаёт `-NoPCH -NoSharedPCH -DisableUnity` (`BuildPluginCommand.Automation.cs:133–136`). Исходники Engine не редактировать, source-engine build не запускать. При ошибке C++/shader остановить приёмку и передать журнал; успешный BuildPlugin ещё не подтверждает GPU-часть.

### 3.2. Подготовить отдельную тестовую карту

1. Создайте **новую пустую карту**, не пересохраняйте рабочую сцену. Добавьте один Exponential Height Fog и один Movable Directional Light. Уберите SkyLight/атмосферу/облака/LFV из этой карты; volume-материалов пока нет.
2. Для Exponential Height Fog: Location Z=0 см; Fog Density=0.02; Fog Height Falloff=0.2; Second Fog Density=0; Fog Max Opacity=1; Volumetric Fog=On; Albedo=белый; Emissive=чёрный; **Scattering Distribution=0**; Extinction Scale=1; View Distance=10 000 см; Start Distance=0; Near Fade In Distance=0. Включать FSSS не нужно.
3. Для directional: белый цвет, Intensity=10 lux, Volumetric Scattering Intensity=1, Cast Volumetric Shadow=On. Поверните его так, чтобы солнце было примерно на 30° над горизонтом. Позже повторите тест на 10°. Для RT-ветки задайте Cast Ray Traced Shadows=Enabled.
4. Добавьте непрозрачный куб как ориентир и для теста геометрической тени. Тестовую камеру держите примерно на Z=300 см. Включите Realtime viewport (Ctrl+R).
5. Добавьте Post Process Volume, включите Infinite Extent (Unbound). Exposure: Metering Mode=Manual, Exposure Compensation=0, Apply Physical Camera Exposure=Off. Bloom Intensity=0. Экспозицию между сравниваемыми кадрами не менять. Если изображение слишком тёмное, один раз выберите удобную фиксированную Exposure Compensation и оставьте её одинаковой для всех вариантов.
6. Откройте **Window → Developer Tools → Output Log**. Команды ниже вводите в строку консоли по одной; дождитесь завершения shader compilation после каждого Apply. Названия cvar английские, независимо от языка интерфейса.
7. Сначала выполните `MLS.Status` и сохраните вывод/скрин настроек MLS. Для честного сравнения с vanilla отключите MLS-эффекты следующими командами. **Эти четыре команды сохраняют настройки MLS**, после теста верните записанные значения.

```text
MLS.Preset 0
MLS.MicroShadow 0
MLS.IndirectVisibility 1
MLS.Tonemap 0
r.Fog.SeparateComposition 0
r.Fog.ScreenSpaceScattering 0
r.VolumetricFog 1
r.VolumetricFog.InjectRaytracedLights 1
r.VolumetricFog.GridPixelSize 16
r.VolumetricFog.GridSizeZ 64
r.VolumetricFog.ConservativeDepth 0
r.VolumetricFog.GridCenterOffsetFromDepthBuffer -1
r.VolumetricFog.LightScatteringSampleJitterMultiplier 0
r.VolumetricFog.TemporalReprojection 1
r.MegaLights.DirectionalLights 0
```

### 3.3. Baseline, включение и контроль Apply

```text
FogMS.Debug 0
r.FogMS.Enable 0
FogMS.Apply
FogMS.Status
```

При выключенных MLS-политиках ожидается `Overlay active: NO`. Сохраните baseline-кадр с неподвижной камеры. Для строгого сравнения временных кадров установите одинаковые `r.VolumetricFog.TemporalReprojection 0` и `r.VolumetricFog.Jitter 0` **до обеих съёмок**, используйте одинаковую экспозицию и рендер-разрешение. Побитное совпадение не выводится из визуального сходства; для критерия 1 нужны одинаковые линейные HDR-буферы и их численное сравнение.

Включите A1:

```text
r.FogMS.Steps 32
r.FogMS.MarchDistance 0
r.FogMS.MaxDistance 2000000
r.FogMS.ExcludeGlobalLayer 0
r.FogMS.DebugViews 1
r.FogMS.Enable 1
FogMS.Apply
FogMS.Status
```

Ожидается `Overlay active: YES`, строка target содержит **`matches active`**, в FogMS config `enabled=1`, `steps=32`, `exclude_global=0`, `extinction_scale=1`, `error=none`. Дождитесь завершения компиляции. Ошибка `Apply FAILED`, отсутствующий include, новый unbound parameter, ошибка shader compilation или `NOT the active overlay` — **провал**, визуальные тесты пока не продолжать. Логические ошибки могут оставить прежний overlay активным; одно `YES` без `matches active` не доказывает применение.

Нормальный вид: directional-вклад в туман может стать темнее, особенно при низком солнце. Это ожидаемое физичное ослабление. Поверхности и сами геометрические тени не получают нового fog attenuation.

### 3.4. Три debug-вида — ожидаемая картинка и провал

| Команда | Что показано | Что ожидать | Что считать провалом |
|---|---|---|---|
| `FogMS.Debug 1` | `VBufferA.a`, показ как `saturate(σt × 10000)` | Серый height-gradient, плотнее внизу; σt=1e−4/см и выше — белый. Это **значение ячейки**, не накопленная оптическая толщина | NaN/мигание/полосы padding; рисунок следует экранным краям вместо высоты; плотность не реагирует на Fog Density |
| `FogMS.Debug 2` | `exp(−τ_eff)` | Белый=1, чёрный=0. В физичном режиме уменьшается при росте плотности/Extinction Scale. На одинаковой высоте чистый height fog близок по T при разных направлениях камеры | Цвет меняется рывком на границе сетки; яркость привязана к повороту камеры. **Маджента** означает отсутствие поддержанной directional-ветки — это не успешный тест |
| `FogMS.Debug 3` | `abs(τ_in+τ_out−τ_ref)/max(τ_ref,1e−4)` | Чёрный=0 ошибки; зелёный=ошибка до 5%; красный=больше 5%. Проверять только чистый height fog | Красные области в исходной тестовой сцене. LFV специально создаёт расхождение с analytic height-only reference и для этого теста должен быть убран |
| `FogMS.Debug 0` | Обычный рендер | Возвращается освещение, восстанавливается предыдущий режим истории | Остался диагностический цвет или temporal cvar не восстановился |

Техническое соответствие: режимы используют `r.GeneralPurposeTweak` 101/102/103. **Пользуйтесь `FogMS.Debug`**, он отключает/восстанавливает историю; прямое изменение GeneralPurposeTweak обходило бы это управление. Дайте хотя бы один кадр отрисоваться после входа/выхода из debug. Диагностическое отображение проходит tonemapper, поэтому точный HDR-пиксель нельзя считывать как показанный на мониторе процент серого.

Для шва повторите: Fog Density 0.02 → 0.05; Second Fog Density 0 → 0.01 с другой Second Fog Height Offset; Extinction Scale 1 → 2 (**после изменения scale выполните `FogMS.Apply`**); солнце 30° → 10° → почти горизонт. Верните исходные параметры. При экстремально малой плотности/километровом View Distance возможен описанный FP16-контрпример; это фиксируется как ограничение, не скрывается повышением N.

### 3.5. Поворот камеры, исключение global layer и локальные источники

1. При `FogMS.Debug 2`, чистом height fog и неподвижном свете поверните камеру на 360°. Сравнивайте одинаковую **мировую высоту/точку** в перекрывающихся ракурсах. Рисунок T не должен «ехать» вместе с границей frustum. Для сравнения обычной яркости используйте `FogMS.Debug 0` и **g=0**: иначе штатная HG-фаза сама меняет вид при повороте.
2. Выполните `FogMS.Debug 0`, затем `r.FogMS.ExcludeGlobalLayer 1`, `FogMS.Apply`. В чистом height fog ожидается визуальный baseline. Небольшой положительный остаток возможен из-за дискретизации/FP16; измерьте его в `FogMS.Debug 2`. Выраженное дополнительное затемнение — провал критерия 5, не художественная настройка.
3. Верните `r.FogMS.ExcludeGlobalLayer 0`, `FogMS.Apply`. Добавьте LFV, сравните On/Off: он может добавлять самозатенение **только внутри текущей сетки**. LFV за камерой/за frustum не является плотностью для A1. Box Volume пока отсутствует.
4. Для проверки изоляции добавьте Point или Spot Light с Volumetric Scattering Intensity=1. Временно поставьте Volumetric Scattering Intensity directional=0, выйдите из debug и сравните FogMS On/Off. Вклад локальной лампы должен совпадать. Самозатенение лампы, sky и Lumen A1 **не добавляет**.
5. Отдельно верните `r.VolumetricFog.ConservativeDepth 1` и `r.VolumetricFog.GridCenterOffsetFromDepthBuffer 0.5`, осмотрите около куба. Отклонение от чистого шовного теста фиксируется отдельно: near-depth bias меняет позиции density samples. Оно не устраняется только ростом N.

### 3.6. Производительность и пермутации

Для измерения: `FogMS.Debug 0`, фиксированная камера/разрешение, `r.VolumetricFog.TemporalReprojection 1`, `r.VolumetricFog.Jitter 1`, FSSS Off. После каждого Apply дождитесь shaders и 100 кадров стабилизации. Для каждого варианта выполните `ProfileGPU`, раскройте **VolumetricFog → LightScattering** и запишите мс и общий GPU frame time:

```text
r.FogMS.Enable 0
FogMS.Apply
ProfileGPU
r.FogMS.Enable 1
r.FogMS.Steps 8
FogMS.Apply
ProfileGPU
r.FogMS.Steps 16
FogMS.Apply
ProfileGPU
r.FogMS.Steps 32
FogMS.Apply
ProfileGPU
```

Для каждого режима снимите три замера; передайте медиану, разрешение, GridPixelSize/GridSizeZ, видеокарту и число активных views. Значительного realtime-бюджета мы ещё не утверждали. При неприемлемой стоимости — вернуть `r.FogMS.Enable 0; FogMS.Apply` (две отдельные команды).

Критерий «все permutations» требует shader compile-прогона на реальном UE: temporal On/Off, emissive On/Off, RT-shadow-volume, VSM, Lumen, supersampling, Ubershader и MegaLights. Локальный DXC-стенд **этого не заменяет**. После успешного включения A1 выполните `RecompileShaders Changed`, дождитесь окончания и сохраните весь Output Log; любые shader errors — FAIL. В этом CL Ubershader выбирается автоматически при неготовом специализированном PSO (`VolumetricFog.cpp:1969–1975`); отдельный fog-cvar для принудительного выбора **NOT FOUND**. Полноту набора скомпилированных допустимых SM6 permutations должен подтвердить технический аудит журнала/артефактов, один видимый кадр этого не доказывает. MegaLights не включать в рабочей сцене ради проверки: compile-permutation acceptance остаётся **PENDING**, пока этот контроль не выполнен.

### 3.7. Завершение и что передать на аудит

Выполните `FogMS.Debug 0`, `r.FogMS.Enable 0`, `FogMS.Apply`. Верните исходные MLS-настройки и перезапустите Editor, чтобы сбросить временные console overrides. Не сохраняйте тестовые значения в DefaultEngine.ini.

Передайте build/shader logs, вывод `FogMS.Status`, три debug-снимка, результат поворота/ExcludeGlobalLayer, таблицу ProfileGPU и условия съёмки. В журнале приёмки отметить отдельно все семь критериев: **PASS / FAIL / NOT RUN**. Пока нет этих данных, ни «работает», ни «A1 принят» не ставить.

Откат установленного пакета: при закрытом Editor перенесите новую папку `Plugins\MultiLobeSpec` в отдельную папку рядом с backup, затем верните сохранённую `Saved\FogMS_Backups\<stamp>\MultiLobeSpec` на её место. Ничего из backup не удаляйте.

## 4. Вопросы исследователю

- Принять общий конечный отрезок `[0,S_max]` и зафиксированные края jitter-интервалов как устранение неоднозначностей §3 заметки.
- Для следующего этапа оценить выводы Audit2; A1b до разбора не начинается.
- Зафиксировать область применимости порога 5% с учётом FP16 и depth bias; автоматически объявлять критерий выполненным для любых плотностей/дистанций нельзя.
- Спроектировать локальный Box Volume: ограничение расчёта и плавный переход, согласование падающего внешнего света/видимости и собственного MS-поля, без изменения Engine.
