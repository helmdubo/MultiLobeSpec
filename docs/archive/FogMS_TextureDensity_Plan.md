# FogMS TextureDensity — план следующего среза

**Статус: реализовано, проверено, установлено и визуально принято заказчиком.** Дата: 2026-09-20. Заказчик: «мне нравится результат, давай следующий срез». Цель выполненного среза — локальная плотность из готовой 3D-текстуры, штатное освещение Volumetric Fog и самозатенение directional через A1. Итог: `FogMS_TextureDensity_Report.md`; следующий срез — `FogMS_A1b_Report.md`. Engine не изменять и не форкать.

Ограниченная реализация: `FogMS_BoxVolume.h/.cpp` (actor/MID, worker), `FogMS_BoxRuntime.cpp` (history revision), `MultiLobeSpec.uplugin` (Content mount), `Content/FogMS/M_FogMS_Density.uasset` (Volume material), `Content/FogMS/T_FogMS_DefaultVolume.uasset` (собственная линейная копия штатной default VolumeTexture для типизации параметра): **6 авторских файлов/активов**. Default Engine asset оказался sRGB; его менять нельзя, поэтому для Linear Color sampler нужна собственная копия. Без назначенной пользовательской текстуры actor всё равно отключает добавку. Генератор материала и тестовые scripts/receipts находятся отдельно в `.codex-build`. Документы синхронизируются по результату. Фикс visibility уже собран и входит в этот пакет. По последующему указанию заказчика **новые уровни не создаются**: демонстрация/переключатели остаются в существующей `FogMS_Box`.

## Подтверждённые факты

Исходники: **UE 5.8.2, CL 56702186, `++UE5+Release-5.8`**; корень — `D:\PersonalProjects\UE5\UE_5.8\Engine` (`Build/Build.version:2–9`). Сокращения путей: **R** = `Source/Runtime/Renderer/Private/`, **E** = `Source/Runtime/Engine/`, **S** = `Shaders/Private/`.

- **VERIFIED, asset:** `/Game/MimirHead/Textures/perlin.perlin` — `VolumeTexture`, **64×64×64**, `B8G8R8A8`, `sRGB=false`, `TC_VECTOR_DISPLACEMENTMAP`. Подтверждение из UE: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_Box_20260920_1810\Probe\texture-result.json`. Распределение значений отдельных каналов и бесшовность текстуры ещё не проверены. Исходный asset сохраняем без изменений.
- **VERIFIED, cube подходит:** static meshes с Volume domain входят в `VolumetricMeshBatches` (`R/SceneVisibility.cpp:1952–1966`). Служебный quad заменяет mesh (`R/VolumetricFogVoxelization.cpp:650–670`); плотность ограничивается преобразованными local bounds (`S/VolumetricFogVoxelization.usf:276–287`). Заполняется **bounding box**, а не произвольная форма mesh. Niagara не обязателен; sprites используют сферический вариант (`R/VolumetricFogVoxelization.cpp:654–656`, `S/VolumetricFogVoxelization.usf:265–273`).
- **VERIFIED, материал:** нужен `MD_Volume + BLEND_Additive` (`E/Private/Materials/MaterialShared.cpp:6398–6404`). Legacy Default Lit предоставляет Albedo/Extinction; Unlit обнуляет их (`S/VolumetricFogVoxelization.usf:29–40,55–64`). Extinction неотрицательна; коэффициент **1/100** переводит вход материала **1/м** в `VBufferA.a` **1/см** (`S/VolumetricFogVoxelization.usf:330–344`). Запись аддитивная (`R/VolumetricFogVoxelization.cpp:427–429`).
- **VERIFIED, ограничения:** после **60% Fog View Distance** вклад volume-материала кубически затухает до нуля на дальней границе (`S/VolumetricFogVoxelization.usf:330–334`). HeterogeneousVolume proxies исключаются из прохода (`R/VolumetricFogVoxelization.cpp:768–773`); их для этого среза не использовать.
- **VERIFIED, A1:** в репозитории `E:\GITHUB\MultiLobeSpec\MultiLobeSpec`, `Shaders/Private/FogMS_Common.ush:60–61,152` читает суммарную extinction из `VBufferA`; `τ_out`/`τ_ref` — только height fog (`:156–160`). Новая плотность внутри сетки попадёт в A1 без дополнительного noise-сэмплирования.

## Реализованный контракт

**IMPLEMENTED:** дочерний **non-Nanite cube** в `AFogMSBoxVolume`, один material slot, без коллизии/поверхностной тени. Volume-материал через MID получает texture/параметры; cube следует transform и размерам `UBoxComponent`. Public API подтверждены: `SetStaticMesh` — `E/Classes/Components/StaticMeshComponent.h:450`; `SetMaterial` — `E/Classes/Components/MeshComponent.h:121`; attachment/scale — `E/Classes/Components/SceneComponent.h:734,473`; MID Create/texture/scalar/vector setters — `E/Public/Materials/MaterialInstanceDynamic.h:176,65,24,109`.

**DensityEnabled по умолчанию Off**, независимо от переключателей A1/BoxMode. Выключение самозатенения не удаляет плотность: сравнение A1 On/Off происходит на одном и том же тумане. Невалидная текстура или нечисловые параметры отключают добавляемую плотность с понятным сообщением; однородный fallback не подставляется.

| Параметр | Смысл |
|---|---|
| Texture | Ссылка на существующую `UVolumeTexture`; первоначально подтверждённый `perlin` |
| Channel | Один из R/G/B/A; начальный R, после проверки содержимого каналов |
| TileScale | Положительный конечный XYZ; `(1,1,1)` — одна копия текстуры на box, больше — повторения |
| Threshold | Порог плотности `[0,1]` |
| Softness | Полная ширина перехода вокруг Threshold `[0,1]`; 0 — резкий порог |
| Density | Неотрицательная конечная максимальная extinction в **1/м** |
| Albedo / DensityEdgeFeather | Albedo `[0,1]`; отдельное сглаживание края плотности в мировых см, не существующий feather области A1 |

Поток данных материала:

```text
Absolute World Position → TransformPosition World→Local
UVW = (LocalPosition − LocalBoundsMin) / (LocalBoundsMax − LocalBoundsMin)
n = VolumeTexture(UVW × TileScale)[Channel]  // wrap sampler в материале
mask = smoothstep(Threshold − Softness/2, Threshold + Softness/2, n)
        либо step(Threshold, n), если Softness = 0
Extinction = Density × mask × DensityEdgeMask
Albedo → отдельный вход; Emissive = 0
```

World Position восстанавливается в точке вокселизации (`S/MaterialTemplate.ush:4748–4757`); mesh UV/WPO не использовать. Масштабирование box растягивает шум, TileScale регулирует повторения. Wrap задаётся в материале; исходный asset не меняется. Материал/sampler скомпилированы и проверены на D3D12/SM6; итоговые receipts перечислены в `FogMS_TextureDensity_Report.md`.

Изменения transform, Texture и перечисленных параметров должны обновляться **live через компонент/MID**, без `FogMS.Apply` и пересборки overlay; первоначальная компиляция нового материала ожидается. Изменения плотности должны уведомлять существующий механизм revision/reset **истории тумана для persistent views**, включая редкие SceneCapture; истории TAA/Lumen не сбрасывать. Реализованный reset действует при локальном A1 overlay; при полном выключении FogMS или global A1 используется штатная история UE.

**Глобальный фон:** additive-вокселизация не вычитает height fog; пустоты noise не разрежают существующий фон. Для локального облака нужен слабый глобальный фон, без автоматической правки авторской сцены. Нулевая Fog Density может убрать регистрацию Exponential Height Fog (`E/Private/Components/ExponentialHeightFogComponent.cpp:127–147`), необходимую VF (`R/VolumetricFog.cpp:1353–1361`).

**Граница A1 сохраняется:** части noise-облака за фрустумом не входят в `τ_out`; полной независимости от камеры этот срез не обещает. Нативный far fade также зависит от сетки камеры. Проверки проводить с облаком внутри сетки и ближе 60% Fog View Distance. Seam debug сравнивает с чистым height fog: ненулевой результат на noise сам по себе не является ошибкой. Его исходный математический тест выполнять с DensityEnabled Off.

## Приёмка следующего среза — не более семи проверок

1. **Исходное состояние:** DensityEnabled Off/нулевая Density возвращают прежнюю плотность; исходный `perlin`, Engine и посторонние материалы не изменены.
2. **Native injection:** noise виден при A1 Off; Extinction debug проверяется отдельно с активным debug-overlay. При Threshold=0.5/Softness=0.1 чёрная контрольная текстура добавляет 0, белая в ядре даёт заданную extinction × 1/100, с ожидаемым far fade.
3. **Live управление:** Texture/Channel/TileScale/Threshold/Softness/Density и transform обновляются без Apply/нового overlay; rotation, non-uniform/negative scale и UVW согласованы с box.
4. **Границы/числа:** плотность неотрицательна, снаружи cube равна прежнему фону; feather непрерывен, Softness=0 определён; невалидные значения дают безопасное отключение добавки.
5. **A1:** On/Off на неизменной noise-плотности меняет directional self-shadowing; `VBufferA` и camera-ray extinction от самого переключения A1 не меняются. Солнечный луч не обрезается гранью box.
6. **История:** изменение текстуры/параметров/формы и поздний persistent capture не сохраняют старую форму облака; исходный чистый height-fog seam-тест сохраняется.
7. **Доказательства:** компиляция материала и D3D12/SM6 GPU-проверка, сравнения Off/On и стоимость `VoxelizePrimitives`/`LightScattering`; отдельно записаны far fade, пределы froxel-разрешения и отсутствие noise вне фрустума в `τ_out`.

Реализация выполнена ограниченным срезом в шести перечисленных авторских файлах/активах. **A1b/MS и модификация глобальной плотности вычитанием сюда не входят.**
