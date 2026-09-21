# FogMS — мини-аудит №2 UE 5.8.2: доступ плагина к истории и volume source term

**Дата:** 2026-09-20. **Основание:** `FogMS_Research_Note_01.md` §7, вопросы Q1–Q8. Режим аудита — чтение исходников и конфигурации; сборка, запуск Editor и GPU-проверка не проводились.

**Движок:** UE 5.8.2, `++UE5+Release-5.8`, CL 56702186, `D:\PersonalProjects\UE5\UE_5.8\Engine`. **Проект:** `D:\PersonalProjects\UE5\MimirHead_portfolio 5.7 5.8 - 3`. **Репозиторий:** `E:\GITHUB\MultiLobeSpec\MultiLobeSpec`.

В ссылках префиксы `Renderer/`, `Engine/`, `RenderCore/`, `D3D12RHI/` означают соответствующие каталоги `Source/Runtime/` движка. `Shaders/`, `Config/` и `Plugins/` даны относительно корня движка. Ссылки на файлы проекта и плагина помечены отдельно.

**VERIFIED (файл:строка)** — подтверждено исходником или конфигурацией; **NOT FOUND** — не найдено в указанной области поиска; **ASSUMED** — вывод о возможной реализации, не подтверждённый компиляцией или выполнением. Запрет заказчика: **Engine FORK/PATCH недопустимы**. Чтение private-заголовков не означает разрешения менять Engine.

## 0. Результат для принятия решений

| Вопрос | Вывод |
|---|---|
| Q1 — история из плагина в launcher-сборке | Поля и путь получения `FSceneViewState` найдены. Чтение не требует найденных неэкспортированных конструкторов/сброса. **Compile/link плагина не проверены** |
| Q2 — история в `PostTLASBuild` | Есть GraphBuilder, View и экспортированный `RegisterExternalTexture`; сохранение истории через pooled RT используется самим VF. Нужно снять согласованный снимок history-параметров до их обновления текущим кадром |
| Q3 — RDG → volume RT → материал в том же кадре | Составные части найдены. **Целый путь остаётся ASSUMED** до GPU-проверки: скрытое RHI-чтение материала, переходы ресурсов, per-view lifetime и преобразование emissive требуют отдельного решения |
| Q4 — D3D12 SM6 и bindless | Проект на DX12/SM6; отдельных `r.D3D12.Bindless.*` в конфиге не найдено. Платформенный default — `BindlessConfiguration=RayTracing`; фактическое состояние нового запуска здесь не проверено |
| Q5 — плотность за conservative depth | `VBufferA` сохраняет плотность; `LightScatteringCS` обнуляет освещение **и α истории**. История не является полным кэшем плотности |
| Q6 — Fog UB у локальных инжекций | **NOT FOUND** в общих параметрах, PS и RGS. Доступа к двум height-fog слоям через этот UB нет |
| Q7 — GDF при Lumen | Параметры GDF привязаны, но могут содержать dummy-данные. Валидность поля не следует из `LUMEN_GI` или включённой генерации mesh distance fields |
| Q8 — MegaLights | Global-only compute; точка per-light оценки найдена. История тумана и текущая плотность в проверенной структуре параметров отсутствуют |

**Вывод:** B без изменения Engine остаётся **гипотезой**. Этот аудит снимает часть вопросов о доступности API, но не подтверждает готовый канал передачи MS и не запускает его реализацию. Решения заказчика сохраняются: MegaLights не используется; размещение FogMS внутри MultiLobeSpec подтверждено.

## 1. Q1 — launcher-сборка, private-заголовки и чтение истории

**VERIFIED — тип установки.** В `C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat:52–57` указаны путь `D:\PersonalProjects\UE5\UE_5.8` и версия `5.8.2-56702186+++UE5+Release-5.8-Windows`; файл `Build/InstalledBuild.txt` присутствует. Это установленная launcher-сборка, а не собственный source build.

**VERIFIED — доступные данные и путь к ним:**

- `FSceneView::State` доступен в `Engine/Public/SceneView.h:1483–1486`. Интерфейс имеет виртуальный `GetConcreteViewState` (`Engine/Public/SceneManagement.h:128–130`), а реализация у `FSceneViewState` inline (`Renderer/Private/SceneViewState.h:703–705`).
- `LightScatteringHistory`, `LightScatteringHistoryPreExposure`, предыдущие XY-преобразования, UV-max и размер ресурса — public-поля (`Renderer/Private/SceneViewState.h:415–421`). Тип истории — `TRefCountPtr<IPooledRenderTarget>`.
- Сам класс не помечен `RENDERER_API` (`SceneViewState.h:59`). Конструктор, private-деструктор и `ResetVolumetricFogState` не экспортированы (`:527–534`), но прямое чтение уже существующих полей не требует создавать, разрушать или сбрасывать ViewState.
- Текущий `PreExposure` имеет inline-геттер (`SceneViewState.h:720–724`), однако для распаковки истории нужен **`LightScatteringHistoryPreExposure`**, как в штатном VF (`Renderer/Private/VolumetricFog.cpp:1837–1847`). Текущая экспозиция не заменяет экспозицию кадра записи истории.
- Штатные предыдущие матрицы берутся из `FViewInfo::PrevViewInfo` (`Renderer/Private/SceneRendering.h:1533`; `VolumetricFog.cpp:318–319`). Комментарий `SceneViewState.h:296–304` предупреждает о моменте обновления `PrevFrameViewInfo`; читать его позднее как гарантированно «прошлый кадр» нельзя.

**NOT FOUND:** отдельные предыдущие `GridZParams` среди проверенных VF history-полей `SceneViewState.h:415–421`. Штатная репроекция использует текущие Z-параметры и предыдущую матрицу (`Shaders/Private/HeightFogCommon.ush:592–599, 638–647`); это не доказательство корректности произвольной смены сетки между кадрами.

**ASSUMED — граница реализуемости:** plugin translation unit с `Renderer/Private` в include paths и зависимостями `Renderer`/`RHI` может читать эти поля без вызова перечисленных неэкспортированных функций. Это нужно проверить отдельной compile/link-пробой на данной installed-сборке. В текущем `Source/MultiLobeSpec/MultiLobeSpec.Build.cs` плагина есть `RenderCore`, но нет `Renderer`, `RHI` и private include пути; такой probe не выполнялся. Private layout остаётся зависимостью от точной версии UE.

## 2. Q2 — история в `PostTLASBuild_RenderThread`

**VERIFIED:** callback принимает `FRDGBuilder&` и `FSceneView&` (`Engine/Public/SceneViewExtension.h:202–205`). Флаги подписки и необходимости inline RT объявлены в `:107–114`; вызов расположен в `Renderer/Private/DeferredShadingRenderer.cpp:3309–3328`, до VF (`:3660–3663`). Публичный TLAS доступен через `Renderer/Public/FXRenderingUtils.h:83–91`.

**VERIFIED:** `FRDGBuilder::RegisterExternalTexture` экспортирован (`RenderCore/Public/RenderGraphBuilder.h:78–87`). Сам VF регистрирует `ViewState->LightScatteringHistory` тем же API (`Renderer/Private/VolumetricFog.cpp:1839–1847`). Следовательно, pooled-ресурс истории имеет подходящий тип для регистрации в текущем графе.

**VERIFIED — момент снимка:** `QueueTextureExtraction` завершает извлечение в конце выполнения графа (`RenderCore/Public/RenderGraphBuilder.h:341–346`; вызов VF — `VolumetricFog.cpp:2038`). Но связанные CPU-поля экспозиции и сетки обновляются при построении текущего графа (`VolumetricFog.cpp:2039–2045`). Плагин должен передать в свои pass-параметры согласованный снимок истории, её экспозиции, UV-параметров и матриц, взятый до этого обновления. Позднее чтение изменяемого ViewState внутри GPU/CPU callback не заменяет такой снимок.

**VERIFIED — не любая история пригодна:** штатная валидность учитывает temporal reprojection, наличие ViewState/истории, camera cut, сброс предыдущих transform и realtime update (`VolumetricFog.cpp:1617–1626`); ветка без сохранения освобождает историю (`:2047–2055`). Наличие ненулевого указателя само по себе не покрывает все условия.

**ASSUMED:** регистрация и чтение валидной истории из plugin pass на этом callback осуществимы. Полный proof зависит от Q1, согласованного lifetime и проверки первого кадра/сбросов; такого запуска нет. Текущий `VBufferA` на этой стадии ещё не создан.

## 3. Q3 — запись RDG в volume RT и чтение volume-материалом

### 3.1 Найденные составные части

**VERIFIED:**

- `UTextureRenderTargetVolume` предусматривает UAV и инициализацию volume target (`Engine/Classes/Engine/TextureRenderTargetVolume.h:16–17, 52–69`). Он имеет тип материала `MCT_VolumeTexture` (`Engine/Private/TextureRenderTargetVolume.cpp:135–137`); ресурс создаёт target/sample RHI и UAV (`:248–271`).
- Материальные uniform expressions получают texture reference (`Engine/Private/Materials/MaterialUniformExpressions.cpp:1871–1900`), а компилятор генерирует `Texture3DSample` (`Engine/Private/Materials/HLSLMaterialTranslator.cpp:7485–7487`).
- VF-вокселизация принимает `MD_Volume` (`Renderer/Private/VolumetricFogVoxelization.cpp:272–277`) и выполняет материал (`Shaders/Private/VolumetricFogVoxelization.usf:20, 321–326`).
- Регистрация внешней RHI-текстуры поддержана `RenderCore/Public/RenderGraphUtils.h:271–283`; пример для volume render target есть в `Plugins/FX/Niagara/Source/Niagara/Private/NiagaraDataInterfaceRenderTargetVolume.cpp:456–462`.

### 3.2 Порядок доступа в одном кадре

**VERIFIED — API синхронизации:** материал читает underlying RHI через texture reference; это не явный `SHADER_PARAMETER_RDG_TEXTURE` будущего прохода вокселизации. Для такого внешнего чтения `UseExternalAccessMode` переводит зарегистрированный ресурс в read-only состояние для последующих проходов (`RenderCore/Public/RenderGraphBuilder.h:378–384`). Перед чтением материала нужен переход вида `UseExternalAccessMode(Texture, ERHIAccess::SRVMask, ERHIPipeline::Graphics)` после записи compute. Перед следующей RDG-записью — возврат в internal access (`:394–398`).

`SetTextureAccessFinal` задаёт состояние **после исполнения всего графа** (`RenderGraphBuilder.h:372–373`), поэтому не заменяет переход перед вокселизацией в середине того же графа.

**ASSUMED:** при корректных lifetime, регистрации, очередях и переходах материал увидит запись того же кадра. Требуется проверка RenderDoc/RDG validation или эквивалентное GPU-доказательство. Общий volume RT для нескольких View создаёт риск перезаписи данных одного View другим; одним барьером этот вопрос владения не решается. Multi-view/SceneCapture-поведение не проверено.

### 3.3 Source term не равен сырому значению Emissive

**VERIFIED:** перед записью volume-материала вычисляется

`Scale = 0.01 × SliceFadeAlpha³ × ShapeMask`

`VBufferB.rgb += MaterialEmissive × Scale`

где затухание начинается с `0.6 × VolumetricFog.MaxDistance` и доходит до нуля на дальней границе (`Shaders/Private/VolumetricFogVoxelization.usf:330–344`). Запись аддитивна (`Renderer/Private/VolumetricFogVoxelization.cpp:427–430`). Затем `LightScatteringCS` умножает суммарный source term на **текущий PreExposure** (`Shaders/Private/VolumetricFog.usf:1169`).

Следствие по данным: для передачи q_MS материал должен получать величину **без pre-exposure**, с согласованными единицами и указанной пространственной маской. Снятие экспозиции истории использует её собственный scale (Q1). Одного `sample(q_MS) → Emissive` недостаточно; даже компенсация коэффициента 0.01 не убирает fade и ShapeMask. Деление на fade, обращающийся в ноль, не является допустимым решением для всего объёма.

**VERIFIED — включение эмиссии:** нужен `r.VolumetricFog.Emissive > 0`, иначе `VBufferB` не создаётся (`Renderer/Private/VolumetricFog.cpp:1653, 1711–1714`).

**ASSUMED:** управляемый emissive-объём может служить каналом добавления source term B. Пространственная область, сохранение энергии, fade, обновление RHI и per-view привязка ещё не определены и не проверены. Наличие API не подтверждает цельный pipeline.

## 4. Q4 — RHI, SM6 и bindless проекта

**VERIFIED — конфигурация проекта:** `Config/DefaultEngine.ini:70–74` задаёт D3D12 и SM6. **NOT FOUND:** явные `r.D3D12.Bindless.*` в проверенном конфиге проекта.

**VERIFIED — значение по умолчанию и требование платформы:** `Config/Windows/BaseWindowsEngine.ini:54–55` задаёт `[ShaderPlatformConfig PCD3D_SM6]` → `BindlessConfiguration=RayTracing`. `Config/Windows/DataDrivenPlatformInfo.ini:78–89` разрешает inline RT и задаёт `bInlineRayTracingRequiresBindless=true`; runtime-проверка находится в `D3D12RHI/Private/D3D12Adapter.cpp:1316–1318`.

**ASSUMED:** проектная конфигурация совместима с требованием bindless для inline RT. Фактическое состояние нового запуска, поддержки устройства и shader permutation в этом аудите не измерялось. Отсутствие отдельного cvar в проекте не означает, что bindless выключен; платформенный config — отдельный источник значения.

## 5. Q5 — conservative depth, текущая плотность и история

**VERIFIED — MaterialSetup:** плотность вычисляется в `Shaders/Private/VolumetricFog.usf:153–178`, `VBufferA/B` пишутся по границам ресурса в `:287–297`. Параметры MaterialSetup не содержат conservative-depth texture (`Renderer/Private/VolumetricFog.cpp:350–364`). `ApplyDepthConstraintsToOffset` корректирует центр выборки (`VolumetricFog.usf:90–114, 165`), а не удаляет все ячейки за depth.

**VERIFIED — volume-материалы:** вычисление extinction/emissive и запись `VBufferA/B` проходят в `Shaders/Private/VolumetricFogVoxelization.usf:324–344`; blend аддитивный, depth test выключен (`Renderer/Private/VolumetricFogVoxelization.cpp:427–430`). Проверенное отсечение освещения по conservative depth не применяется как отсечение их плотности.

**VERIFIED — LightScattering:** при попадании froxel за conservative depth `LightScatteringCS` пишет `float4(0,0,0,0)` и выходит **до смешивания истории** (`VolumetricFog.usf:860–878`). Извлекается именно этот `LightScattering` (`VolumetricFog.cpp:2038`), поэтому за depth история теряет rgb **и σt в α**, несмотря на наличие текущей плотности в `VBufferA`.

**VERIFIED — область валидности:** MaterialSetup диспатчится по **ViewGrid** (`VolumetricFog.cpp:1759–1766`), а ресурс может быть больше. Padding между ViewGrid и ResourceGrid нельзя считать заполненной сеткой среды.

**ASSUMED — влияние на B:** использование `LightScatteringHistory.a` вместо текущей плотности создаёт нули за depth и на невалидных участках истории. Это потеря данных, а не физически пустая среда. Алгоритм на истории обязан учитывать такую границу; доступ к одному history RT не восстанавливает текущий `VBufferA` за неё.

## 6. Q6 — Fog UB у `InjectShadowedLocalLightPS/RGS`

**NOT FOUND** в проверенных структурах `Renderer/Private/VolumetricFog.cpp`: общие параметры локальной инжекции `:403–413`, PS `:454–462`, RGS `:519–527`. Параметры интегрирования в `Renderer/Private/VolumetricFogShared.h:42–51` также не содержат `Fog` UB или двух слоёв аналитической плотности.

**VERIFIED — граница вывода:** наличие View/VolumetricFog-параметров не равнозначно наличию `FFogUniformParameters`. Готовой привязки Fog UB для аналитического самозатенения локального света здесь нет. В текущей границе оверлея её нельзя считать доступной; изменение C++-привязок штатных проходов Engine запрещено.

## 7. Q7 — Global Distance Field при `LUMEN_GI`

**VERIFIED:** GDF-параметры объявлены и заполняются у `LightScatteringCS` (`Renderer/Private/VolumetricFog.cpp:1170, 1834`). Однако remap пермутаций с Lumen отключает DF sky occlusion (`:1226–1228`), а GDF include в шейдере защищён соответствующим условием (`Shaders/Private/VolumetricFog.usf:15–18`). Ubershader-ветка (`VolumetricFog.cpp:1236–1246`) не доказывает, что реальное поле построено.

**VERIFIED:** `Renderer/Private/GlobalDistanceField.cpp:354–379` умеет выдавать чёрные dummy-ресурсы при отсутствии данных; нужно учитывать также `NumClipmaps`. Построение GDF зависит от потребителя (`Renderer/Private/DistanceFieldAmbientOcclusion.cpp:764–810`), а Lumen с HWRT для всех нужных путей может не требовать GDF (`Renderer/Private/Lumen/Lumen.cpp:269–277`).

**VERIFIED — проект:** в `Config/DefaultEngine.ini` проекта генерация mesh distance fields включена (`:24`), задан `TraceMeshSDFs` (`:26`) и HWRT (`:46`). Это подтверждает настройки, но не наличие актуальных GDF clipmaps у конкретного View.

**ASSUMED / не проверено на GPU:** поле может быть валидным при наличии другого потребителя, но данная конфигурация сама по себе этого не гарантирует. Пока нет runtime-проверки ресурсов/clipmaps, использовать GDF как безусловно доступную видимость в FogMS нельзя.

## 8. Q8 — MegaLights volume shading

**VERIFIED — класс перекомпиляции:** `FVolumeShadeLightSamplesCS` — global compute shader (`Renderer/Private/MegaLights/MegaLightsResolve.cpp:311–331, 435`). Правка его `.usf` относится к global-only области, а не к полной перекомпиляции материалов.

**NOT FOUND:** `Fog` UB, `LightScatteringHistory` и `VBufferA` в проверенных общих параметрах MegaLights volume shading (`Renderer/Private/MegaLights/MegaLightsInternal.h:27–102`). `RenderMegaLights` вызывается раньше `ComputeVolumetricFog` (`Renderer/Private/DeferredShadingRenderer.cpp:3471, 3660–3663`), поэтому текущий `VBufferA` этого кадра ещё не существует при MegaLights shading.

**VERIFIED — per-light точка:** в `Shaders/Private/MegaLights/MegaLightsVolumeShading.usf:286–316` отдельный источник оценивается через `GetMegaLightsVolumeLighting` (`:311`) и затем добавляется к сумме (`:316`). Сам evaluator включает attenuation и фазу (`Shaders/Private/MegaLights/MegaLightsVolume.ush:166–235`, attenuation `:210`, phase `:213`).

**ASSUMED — будущая вставка:** per-light `exp(−τ)` логически применяется после оценки отдельного света и до суммирования, с охраной `!TRANSLUCENCY_LIGHTING_VOLUME`, чтобы не изменить другой потребитель общего шейдера. Это лишь точка исследования: плотность к проходу не привязана, ресурсный путь не реализован. Масштабирование уже готового `MegaLightsVolume` не заменяет per-light самозатенение.

MegaLights по решению заказчика не используется. Сейчас требуется сохранение компиляции его пермутаций и штатного вклада; поддержку MegaLights этот аудит не вводит.

## 9. Дополнение A1 — масштаб аналитического height fog

**VERIFIED — контракт Engine:** `GlobalExtinctionScale` привязан к MaterialSetup (`Renderer/Private/VolumetricFog.cpp:353, 1737`), но отсутствует у `LightScatteringCS` (`:1145–1201`) и в `Fog` UB (`Renderer/Private/FogRendering.h:16–44`). В `Shaders/Private/VolumetricFog.usf:169–178` extinction использует оба height-слоя, множитель 0.5 и этот scale; `PROJECT_EXPFOG_MATCHES_VFOG` не добавляет здесь другой множитель extinction.

**VERIFIED по текущему коду плагина, без runtime-приёмки:** A1 считывает `VolumetricFogExtinctionScale` активных fog-компонентов и отвергает невалидные/различающиеся значения между компонентами или мирами (`Source/MultiLobeSpec/Private/MultiLobeShaderPatcher.cpp:510–526` в репозитории). Полученное значение включается в `FOGMS_GLOBAL_EXTINCTION_SCALE` (`:609–615`). Команда `FogMS.Apply` вызывает применение общего оверлея (`Source/MultiLobeSpec/Private/MultiLobeSpec.cpp:106`).

Это снимок параметра при Apply, а не новая runtime-привязка Engine. **После изменения Extinction Scale нужен новый `FogMS.Apply`**; согласованность выбранного компонента с отрисованным View и прохождение шовного теста остаются частью проверки A1. Файл `FogMS_A1_Report.md` содержит пользовательский протокол.

## 10. Вопросы исследователю и оставшиеся доказательства

Вопросы по модели для следующего разбора:

1. Как ограничить область инжекции q_MS через material emissive с учётом обязательных `SliceFadeAlpha³` и `ShapeMask`, сохранив предсказуемый source term без сингулярного деления у границ?
2. Как измерять ошибку использования history α в качестве плотности, если conservative depth обнуляет её, а текущий `VBufferA` сохраняет среду? Отдельно остаётся уже описанная ошибка глобального albedo для цветных локальных объёмов.
3. Для истории как solver: как согласовать q_MS_prev, снятие pre-exposure и два temporal-фильтра при оценке `j₁ ≈ H − q_MS_prev`, и каким тестом подтвердить устойчивость всего дискретного оператора? При g ≠ 0 фазовая ошибка истории сохраняется.

Оставшиеся доказательства до вывода о работоспособности B:

- **Компиляция и линковка:** минимальное чтение полей ViewState из плагина с private headers в installed UE 5.8.2; доступность всех реально используемых символов; нужные shader permutations inline RT.
- **GPU и RDG:** фактический режим bindless; запись/read-after-write volume RT в одном кадре; external-access переходы; camera cuts/первый кадр; несколько View/SceneCapture; валидность истории и GDF там, где он нужен.
- **Единицы и результат:** контрольный известный q через material emissive, масштаб 0.01, pre-exposure, fade/ShapeMask и линейный HDR-результат до tonemap; это не заменяется успешной C++-сборкой.

Выборочно повторно сверены при составлении документа: `SceneViewState.h:415–421`, `VolumetricFog.cpp:1837–1847`, `RenderGraphBuilder.h:372–398`, `VolumetricFogVoxelization.usf:324–344`; также launcher manifest и наличие InstalledBuild marker. Остальные анкеры включены из проверенных исследовательских receipts этой сессии. **Новых compile/link или GPU-доказательств аудит не добавляет.** A1, локальный Box Volume и B — отдельные задачи реализации; здесь их код не менялся.
