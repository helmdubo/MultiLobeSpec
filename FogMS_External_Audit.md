# FogMS — пакет для независимого аудита

Срез: **B1 / World (Current Frame)**, UE **5.8.2 CL56702186**, 21.09.2026. Владелец запросил фиксацию текущего состояния в main; развитие алгоритма остановлено до результатов внешнего аудита. Это экспериментальный срез для проверки, не релиз физически сертифицированного volumetric renderer.

## Что читать

1. `FogMS_Energy_Audit.md` — актуальные замечания к физике, независимый эталон и кандидат критериев B2.
2. `FogMS_B1_Report.md` — реализация, runtime-протокол, пройденные camera/linearity проверки и ограничения.
3. `FogMS_Crash_Report.md` — D3D12 BindlessAll compatibility guard, сохраняемый до конца процесса.
4. `FogMS_HANDOVER.md` — решения владельца и история; верхние записи новее нижних исторических снимков.
5. `FogMS_Audit_UE58.md`, `FogMS_Audit2_UE58.md`, `FogMS_Research_Note_01.md` — source audit и первоначальная математика. Старые ограничения camera-history относятся к предыдущему Spatial; в World источник иной.

Компактные результаты и хеши текущего исходного среза: `Docs/FogMS/Validation/B1_20260921/summary.json` и `source-manifest.json`. Полные локальные GPU readbacks/logs находятся по путям из B1 Report; в Git не включены Engine shaders, бинарники сборки, пользовательский проект, дампы и логи с идентификаторами аккаунта. Численные summaries сами по себе не заменяют независимого повторного GPU-прогона.

## Устройство текущего кода

| Узел | Файлы | Данные |
|---|---|---|
| Actor, density, runtime controls | `Source/MultiLobeSpec/Private/FogMS_BoxVolume.*`, `FogMS_DensityAtlas.*`, `FogMS_BoxRuntime.*`; `Content/FogMS/*` | Один живой Box, мировой Perlin/detail, native Volume material; плотность в native fog и копия для world-освещения |
| Shader overlay | `FogMS_ShaderPatcher.h`, `MultiLobeShaderPatcher.*`; `Shaders/Private/FogMS_Common.ush`, `FogMS_Indirect.ush` | Изолированные FogMS gates; overlay в Saved, исходники Engine read-only |
| Sun transmittance cache | `FogMS_ShadowCache.h`, `FogMSRender.cpp`, `FogMS_ShadowCache.usf` | Фильтрованная передача солнечного света для fog и deferred surfaces |
| Старый Spatial | `FogMS_Spatial.*` | Previous fog history/frustum, экспериментальный comparator |
| World B1 | `FogMS_WorldLighting.*` | Box 32³: density, source, base indirect, три добавленных порядка, atlas |
| Native source adapter | `FogMS_WorldSources.*`, `FogMS_LumenSource.*` | Scene lights, sky cubemap и Lumen surface cache; HWRT видимость |
| RHI guard | `FogMS_RHICompatibility.*` | UE5.8.2/D3D12/SM6/BindlessAll/single-GPU: ParallelTranslate выключен; RT/Lumen сохранены |

Плотность не передаётся между ячейками. По лучам интегрируется свет: ячейка рассеивает входящий свет по своим sigma_s/phase, этот source ослабляется на пути до следующих приёмников. Пустая среда пропускает свет, геометрия ограничивает путь. `32³` world-поле и camera froxel-сетка финального fog — разные носители. Три added orders означают число событий рассеяния, а не три соседних voxel и не три каскада камеры.

## Вопросы аудитору

- Правильны ли radiance/irradiance/SH, световые единицы, фаза и exposure на всех стыках? Особое внимание native TLV .886227 normalization и Sky VSI.
- Корректны ли source/receiver sigma_s, optical depth, смешение Box и height fog, границы источников, terrain/mesh emissive? Где требуется исключить/заменить native вклад для устранения двойного учёта?
- Насколько корректна замена native base indirect в Box и различие filtered direct против unfiltered MS seed? Какой общий контракт сохраняет интеграцию с UE без маскирующих brightness controls?
- Подтверждаются ли выявленные per-order/range/truncation потери и контрпример shadow-strength blend? Какая дискретизация/сходимость нужна для физического режима?
- Достаточны ли source coverage, TLAS metadata/normal conventions, lifecycle SRV/RDG и ранняя загрузка? Учесть неподдержанные источники/режимы и изменяемые preview CVars.
- Где кончается поддержка static source и начинается необходимая работа для animated density, world-cache invalidation и временной реконструкции?

## Воспроизведение

Из **свежей копии плагина** вне Engine запустить (папка Package должна быть новой):

```powershell
& '<UE_5.8>/Engine/Build/BatchFiles/RunUAT.bat' BuildPlugin '-Plugin=<checkout>/MultiLobeSpec.uplugin' '-Package=<new output>/MultiLobeSpec' -TargetPlatforms=Win64 -StrictIncludes
python '<checkout>/Tools/FogMSEnergyValidation/reference.py' --output '<output>/energy-reference.json'
```

Python-эталон требует NumPy, ничего не меняет в UE. Его PASS подтверждает только независимый CPU-эталон. B1-like контрпримеры в том же JSON не являются GPU-измерением текущей сцены.

Для runtime требуется лицензированная UE5.8.2 и D3D12/SM6/BindlessAll, один realtime perspective viewport, Lumen HWRT, g=0. Подробные кнопки, presets, ограничения и восстановление — B1 Report. У владельца испытания остаются в единственной `/Game/FogMS_Test/FogMS_Box`; копия его проекта/карты в репозиторий не публикуется. Default VolumeTexture плагина включена; пользовательский Perlin asset принадлежит проекту.

Доставленные DLL Package6 собраны из тех же Source/Shaders; последующие изменения перед публикацией касались документации и CPU-эталона. Не считать сборку/GPU smoke доказательством Shipping, capture/stereo или всех комбинаций MLS/renderer.

## Требования после аудита

- Визуальный ориентир — native UE Volumetric Cloud: объём, мягкое освещение и устойчивые естественные края.
- World-locked density должна поддержать движение и эволюцию: advection/domain warping, изменение формы/расширение без растяжения текстуры масштабом Box. Конкретная модель выбирается после аудита; готовой реализации анимации сейчас нет.
- Отдельно — sampling/reconstruction для уменьшения видимости froxel/voxel дискретности. Stochastic/blue-noise sampling может декоррелировать ошибку с последующей реконструкцией, но не возвращает невыбранные субвоксельные детали само по себе. Приёмка включает движение камеры **и самой density**, без накопления/шлейфов/мерцания.
- SSFS, полный volume↔surface GI coupling и физически откалиброванный B2 пока не завершены. Новый visual polish не должен скрывать неразобранный энергетический дефект.
