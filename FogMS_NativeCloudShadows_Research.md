# FogMS — повторное использование штатных cloud shadows UE 5.8

Дата: 2026-09-21. Статус: **исследование исходников; native-cloud GPU comparison НЕ проводился**.

Проверен локальный Engine `D:/PersonalProjects/UE5/UE_5.8/Engine`: UE **5.8.2**, CL **56702186**, ветка `++UE5+Release-5.8` — [Build.version](D:/PersonalProjects/UE5/UE_5.8/Engine/Build/Build.version:1). Engine не изменялся; fork запрещён. Эта записка не означает готовность нового режима или разрешение менять настройки сцены.

Штатная архитектура — заранее построить карту пропускания и дешёво читать её на поверхностях — подходит для следующей оптимизации FogMS. Однако имеющаяся карта UE принадлежит единственному активному глобальному облачному слою и строится по его материалу. Подключение текущего FogMS Box к ней **не является готовым переключателем**. Текущий A1e per-pixel march остаётся численной отправной точкой; утверждения, что он дешевле штатной карты, нет.

## 1. Что действительно делает native Beer Shadow Map

**VERIFIED.** `InitVolumetricCloudsForViews` создаёт карты для атмосферных directional lights. Генерация — отдельный `CloudShadow` raster pass, вычисляющий volume material вдоль луча света. Это не извлечение плотности из `VBufferA` Volumetric Fog.

| Факт | Анкер локального Engine |
|---|---|
| Cloud shadow включается при `r.VolumetricCloud.ShadowMap > 0`, наличии atmospheric light, `CastCloudShadows` и положительном `CloudShadowStrength` | [VolumetricCloudRendering.cpp:343](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:343) |
| Вызов инициализации cloud maps выполняется до позднего освещения и fog | [DeferredShadingRenderer.cpp:2827](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp:2827) |
| Отдельный `CloudShadow` pass, cloud material, один triangle, `PrimitiveSceneProxy = nullptr` | [VolumetricCloudRendering.cpp:1995](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:1995), строки 1995–2023 |
| Карта `PF_FloatR11G11B10`, размер от directional light; при temporal filtering трассируется половина разрешения по каждой оси | [VolumetricCloudRendering.cpp:2084](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:2084), строки 2084–2113 |
| Четырёхкадровое чередование offsets, затем temporal и необязательная spatial фильтрация | [VolumetricCloudRendering.cpp:1788](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:1788), [cpp:2163](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:2163) |
| Плотность вычисляется материалом внутри сферической оболочки планеты; вдоль луча суммируются extinction и optical depth | [VolumetricCloud.usf:2105](D:/PersonalProjects/UE5/UE_5.8/Engine/Shaders/Private/VolumetricCloud.usf:2105), строки 2105–2162 и [usf:2182](D:/PersonalProjects/UE5/UE_5.8/Engine/Shaders/Private/VolumetricCloud.usf:2182), строки 2182–2215 |

BSM сохраняет три величины: `frontDepthKm`, `meanExtinction` в 1/м, `maxOpticalDepth`. Для конкретного receiver shader делает один bilinear lookup и вычисляет:

```text
tau(receiver) = min(maxOpticalDepth,
                    meanExtinction * max(0, receiverDepthKm - frontDepthKm) * 1000)
T(receiver) = saturate(exp(-tau(receiver)))
```

Это **аппроксимация распределения плотности по глубине**, а не сохранённый интеграл для каждой глубины. `meanExtinction` усредняется по отсчётам с ненулевой средой; пустые промежутки внутри неоднородной колонны не представлены отдельно. Ниже всей колонны ограничение `maxOpticalDepth` даёт накопленную оптическую толщину с точностью дискретизации/фильтрации карты. Внутри неоднородного локального Box линейное нарастание от front depth может отличаться от интеграла до конкретного receiver. Это существенный критерий качества для FogMS, где поверхности находятся и внутри объёма. Проверка: [VolumetricCloudCommon.ush:54](D:/PersonalProjects/UE5/UE_5.8/Engine/Shaders/Private/VolumetricCloudCommon.ush:54), строки 54–67; [VolumetricCloud.usf:2209](D:/PersonalProjects/UE5/UE_5.8/Engine/Shaders/Private/VolumetricCloud.usf:2209), строки 2209–2215.

**VERIFIED consumer.** Opaque deferred lighting домножает attenuation на `lerp(1, GetCloudVolumetricShadow(...), CloudShadowmapStrength)`: [DeferredLightPixelShaders.usf:187](D:/PersonalProjects/UE5/UE_5.8/Engine/Shaders/Private/DeferredLightPixelShaders.usf:187), строки 187–204. C++ привязывает карту только к совпадающему atmosphere light 0/1 и требует положительную surface strength: [VolumetricCloudRendering.cpp:3201](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:3201), строки 3201–3229.

Эту же cloud attenuation уже использует native Volumetric Fog: [VolumetricFog.usf:969](D:/PersonalProjects/UE5/UE_5.8/Engine/Shaders/Private/VolumetricFog.usf:969), строки 969–974. Если FogMS density начать включать в native cloud map, нельзя одновременно повторно применять к тому же свету A1d/A1e attenuation той же плотности: получится двойное затенение.

## 2. Почему FogMS material нельзя просто назначить второму локальному облаку

**VERIFIED.** `UVolumetricCloudComponent` — `USceneComponent`, представляющий материал вокруг планеты, с высотой основания и толщиной слоя в километрах. Это не облачный `UPrimitiveComponent` с OBB. Центр/радиус берутся из Sky Atmosphere, а без неё — из запасной планеты с центром `(0, 0, -PlanetRadius)`: [VolumetricCloudComponent.h:24](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Engine/Classes/Components/VolumetricCloudComponent.h:24), строки 24–40; [VolumetricCloudRendering.cpp:1746](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:1746), строки 1746–1762.

В сцене поддерживается стек компонентов, но renderer использует **только последний включённый** `Scene->VolumetricCloud`. Добавление помощника может вытеснить существующее небо: [VolumetricCloudRendering.cpp:592](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:592), строки 592–629. Проверенного публичного пути добавить независимый локальный cloud primitive, суммирующийся с существующим облачным слоем, в исследованном пути не найдено.

Карта привязана к light projection вокруг поверхности планеты под камерой, с camera snapping; это не проекция, подогнанная под bounds FogMS Box: [VolumetricCloudRendering.cpp:1817](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:1817), строки 1817–1852. Локальную плотность можно описать world-space маской в cloud material, но приходится согласовывать layer bounds, coverage, разрешение и длину трассировки.

Текущий FogMS density material использует `ObjectPositionWS` и World→Local transform своего cube carrier: [create_density_material.py:87](E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_A1e_20260921/create_density_material.py:87), строки 87–108. Native cloud pass использует triangle без этого primitive. Поэтому одинаковый `UMaterialInterface`/MID не обеспечивает ту же плотность и OBB. Требуется адаптация материала с явными world-space параметрами Box. Также нужен cloud usage flag: проверка cloud shader permutations требует `bIsUsedWithVolumetricCloud && MD_Volume` — [VolumetricCloudRendering.cpp:654](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:654).

## 3. Можно ли оставить от компонента только shadow map

**VERIFIED API.** Из плагина доступны `SetMaterial`, `SetRenderInMainPass(false)` и `SetVisibleInRealTimeSkyCaptures(false)`: [VolumetricCloudComponent.h:184](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Engine/Classes/Components/VolumetricCloudComponent.h:184), строки 184–205. Условие генерации cloud shadow map не требует `bRenderInMainPass`.

Однако **выключить видимую композицию недостаточно, чтобы заявить отсутствие затрат на CloudView**:

- При стандартном `r.VolumetricRenderTarget=1` cloud renderer проходит ветвь трассировки в volumetric RT: [VolumetricCloudRendering.cpp:2837](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:2837). Main-pass flag проверяется позднее в композиции: [VolumetricRenderTarget.cpp:1072](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricRenderTarget.cpp:1072). Tracing RT является persistent external texture: [VolumetricRenderTarget.cpp:362](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricRenderTarget.cpp:362).
- В direct-to-scene ветви `bRenderInMainPass=false` действительно даёт `continue` до view tracing: [VolumetricCloudRendering.cpp:2903](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:2903), строки 2903–2908. `r.VolumetricRenderTarget=0` выбирает этот путь: [VolumetricRenderTarget.cpp:92](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricRenderTarget.cpp:92).
- Это **source-backed кандидат для ограниченного shadow-only эксперимента**, а не измеренный рабочий режим FogMS. CVar глобален; reflection captures имеют отдельные условия. Отсутствие всех лишних cloud passes, корректность и полная стоимость должны подтверждаться GPU capture. Такой эксперимент ещё не выполнен.

Есть и материал `Shadow Pass Switch`: cloud shadow shader устанавливает `SHADOW_DEPTH_SHADER=1` — [VolumetricCloudRendering.cpp:1405](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.cpp:1405). Он позволяет отличать density для shadow pass от видимого материала, но сам по себе не устраняет dispatch/view tracing и не решает ограничение единственного активного cloud layer.

## 4. Граница публичного API и следующий путь оптимизации

Публичный `FSceneViewExtension` даёт `FRDGBuilder` и `FSceneView`, в том числе hook после deferred base pass: [SceneViewExtension.h:180](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Engine/Public/SceneViewExtension.h:180), [h:195](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Engine/Public/SceneViewExtension.h:195); вызов [BasePassRendering.cpp:1162](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp:1162). Это место для собственного plugin RDG pass, если его порядок и lifetime явно согласованы с consumer.

Штатные cloud textures находятся в **Renderer Private `FViewInfo`**, [SceneRendering.h:1578](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/SceneRendering.h:1578). Cloud-specific helper declarations также private и без `RENDERER_API`: [VolumetricCloudRendering.h:185](D:/PersonalProjects/UE5/UE_5.8/Engine/Source/Runtime/Renderer/Private/VolumetricCloudRendering.h:185), строки 185–201. Публичного API «добавить FogMS density в native cloud shadow map / заменить её своим volume» в исследованном пути не найдено. Подключение private headers не превращает это в стабильный расширяемый контракт; compile/link и корректный момент доступа отдельно не проверялись.

**INFERRED design direction, не реализация.** Для сохранения существующего sky/cloud и локального Box разумен отдельный plugin cache пропускания в пространстве directional light. Он строится по тому же авторскому sampler FogMS; deferred surface consumer заменяет текущий march на lookup. Для receivers внутри неоднородного объёма cache должен учитывать глубину — например, иметь накопленную optical depth в нескольких depth layers. Одна 2D карта полной колонны не воспроизводит этот случай; native front/mean/max можно рассматривать только как отдельно оцениваемый компромисс качества.

Архитектурное сравнение стоимости:

| Подход | Где вычисляется плотность | Стоимость на surface receiver |
|---|---|---|
| Текущий A1e reference | Несколько отсчётов на каждом подходящем shaded pixel | Пересечение OBB + march/sample loop |
| Native cloud BSM | На texels light-space карты; затем temporal/spatial filtering | Matrix transform + bilinear lookup + приближённый `exp(-tau)` |
| Предлагаемый локальный plugin cache | На ограниченной light-space сетке Box, с depth representation | Lookup/interpolation нужной optical depth |

Перенос работы с каждого pixel на общую карту способен уменьшить стоимость, но добавляет построение, память, фильтрацию и правила обновления. Выигрыш зависит от размеров Box на экране, разрешения карты, шага по глубине, материала, движения и выбранного GPU. **Численного ускорения здесь не заявлено.** Новая реализация сегодня из этой записки не следует.

## 5. Внешняя проверка и границы доказательства

Официальная страница Epic, открытая 2026-09-21, помечена UE 5.8: [Volumetric Cloud Component](https://dev.epicgames.com/documentation/unreal-engine/volumetric-cloud-component-in-unreal-engine). Она подтверждает компромисс Beer Shadow Maps: быстрее вторичного volume ray marching, но менее точны, не сохраняют цвет volumetric self-shadow; качество/стоимость карты регулируются light settings. Это сравнение архитектур Epic, **не замер FogMS против native cloud**. Конкретные ограничения выше выведены из установленного UE 5.8.2, а не перенесены с описания другой версии.

Выполнено: чтение локальных исходников и официальной документации. Не выполнено: изменение кода Engine/плагина, запуск UE, установка cloud helper, GPU comparison, проверка отсутствия view passes в shadow-only режиме или оценка времени native BSM в текущей сцене.
