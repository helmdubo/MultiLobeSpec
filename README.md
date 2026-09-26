# MultiLobeSpec — плагин UE 5.8: FogMS (многократное рассеяние в тумане и облаках) и MLS (BRDF-оверлей)

*English summary: an Unreal Engine 5.8 plugin (Win64, D3D12 SM6, hardware ray tracing). Main part: **FogMS** — multiple
scattering and medium self-shadowing for local fog/cloud volumes (actor `FogMS Box Volume`), delivered through the engine's
own Volumetric Fog or Volumetric Cloud, plus a weather actor. Secondary, editor-only: the **MultiLobeSpec (MLS)** engine-shader
overlay (dual-lobe GGX, material micro-shadowing, AO baker), written for UE 5.7. Unreal assets (`.uasset`) are not in the
repository. The documentation is in Russian; start with [`FogMS_HANDOVER.md`](FogMS_HANDOVER.md).*

Состояние на 2026-09-27 (ветка `main`, срез для аудита — ветка `fogms/audit-2026-09-27`; код — коммит `083341c`). Экспериментальный плагин
(`IsBetaVersion`), проверен только на UE 5.8.2 (CL 56702186), Win64, D3D12 SM6, RTX 3070, на одной тестовой сцене
заказчика. Файлы движка не меняются: FogMS работает через штатные Volumetric Fog / Volumetric Cloud и scene view extension,
оверлей шейдеров движка (MLS и legacy-режимы FogMS) — только в редакторе.

## Что в репозитории

| Часть | Код | Где работает | Документация |
|---|---|---|---|
| **FogMS**: актёр `FogMS Box Volume` (решатель переноса 32³, доставка через облачный хост или штатный туман), подсистема облачного хоста, актёр `FogMS Weather` | `Source/MultiLobeSpec/Private/FogMS_*`, `Source/FogMSRender/**`, `Shaders/Private/FogMS_*`, `Config/*.ini` | редактор и игра (путь Transport + Emissive Injection); legacy-оверлей — только редактор с `-BindlessAll` | [`FogMS_HANDOVER.md`](FogMS_HANDOVER.md) — обзор; [`FogMS_UserGuide.md`](FogMS_UserGuide.md) — руководство |
| **MLS**: двухлепестковый GGX, микротени материала (Activision/CoD:WWII), политика непрямой видимости, Generic VNDF LUT (эксперимент), бейкер AO | `Source/MultiLobeSpec/Private/MultiLobeShaderPatcher.*`, `MLS*`, `MultiLobeSpec*`, `Source/MultiLobeSpecEditor/**`, `Resources/Generated/*` | только редактор (`WITH_EDITOR`); по умолчанию применяется при каждом старте редактора | [`README_RU.md`](README_RU.md) и документы MLS ниже |
| Инструменты проверки | `Tools/FogMSEnergyValidation/` (текущие — `ProdProbe/`), `Tools/MLSConeEnvValidation/`, `Tools/MLSMicroShadowValidation/` | Python вне редактора и в редакторе (через мост UE-MCP проекта) | README в каждой папке |

Модули (`MultiLobeSpec.uplugin`): `FogMSRender` (Runtime, `PostConfigInit`), `MultiLobeSpec` (Runtime, `PostEngineInit`),
`MultiLobeSpecEditor` (Editor); все с `PlatformAllowList: Win64`. `EngineVersion` 5.8.0, `VersionName` 0.15.3.

## Ассеты Unreal в репозитории не хранятся

С коммита `083341c` репозиторий содержит только код плагина: `.uasset`/`.umap` из `Content/` игнорируются (`.gitignore`).
Материалы, текстуры и пресеты FogMS живут локально в проекте (`/MultiLobeSpec/FogMS/...`) и пересобираются в редакторе
скриптами `Tools/FogMSEnergyValidation/ProdProbe/matedit_*.py`. Базовый материал `M_FogMS_Density` и текстуру
`T_FogMS_DefaultVolume` скрипты с нуля не создают — для чистого клона их нужно взять из истории git
(`git restore --source=201f33f --worktree -- Content/FogMS`). Порядок и ограничения — [`FogMS_UserGuide.md`](FogMS_UserGuide.md),
раздел 9.

## Документация

| Документ | Что это |
|---|---|
| [`FogMS_HANDOVER.md`](FogMS_HANDOVER.md) | обзор FogMS для аудитора: архитектура и поток данных, карта модулей, пути рендера, что плагин меняет в движке, сборка и проверка, приватные зависимости движка, ограничения и открытые срезы |
| [`FogMS_UserGuide.md`](FogMS_UserGuide.md) | руководство: свойства, cvar, команды, статусы, цены, ограничения, политика ассетов |
| `FogMS_PerPixelClouds_Design.md`, `FogMS_Weather_Design.md`, `FogMS_DensityAuthoring_Design.md`, `FogMS_ForwardLobe_Design.md`, `FogMS_LOD_Research.md`, `FogMS_Cloud_Lighting_Review.md`, `FogMS_NativeCloudShadows_Research.md`, `FogMS_Fab_Readiness.md`, `FogMS_References.md` | дизайн и исследования FogMS; в шапке каждого — статус реализации на 2026-09-27 |
| [`docs/history/FogMS_Prod_Report.md`](docs/history/FogMS_Prod_Report.md) | хронологический журнал этапа C (раунды 1–49) с замерами |
| [`docs/archive/README.md`](docs/archive/README.md) | архив: отчёты этапов A1–B3, ранние аудиты и исследования, прежний handover, устаревшие документы MLS |
| [`CHANGELOG.md`](CHANGELOG.md) | история версий |
| MLS: [`README_RU.md`](README_RU.md), [`ACTIVISION_MICROSHADOW_V015_RU.md`](ACTIVISION_MICROSHADOW_V015_RU.md), [`MICROVISIBILITY_ARCHITECTURE_RU.md`](MICROVISIBILITY_ARCHITECTURE_RU.md), [`SUPPORT_MATRIX_RU.md`](SUPPORT_MATRIX_RU.md), [`VALIDATION_RU.md`](VALIDATION_RU.md) | документация BRDF-части (UE 5.7; в шапках — что изменилось и что не проверено на 5.8) |

## Сборка

`RunUAT BuildPlugin -Plugin=<копия репозитория>/MultiLobeSpec.uplugin -Package=<папка> -TargetPlatforms=Win64 -StrictIncludes`
собирает UnrealEditor и UnrealGame Development/Shipping (журнал, раунд 49). Рабочие скрипты сборки, установки в проект и запуска
редактора лежат вне репозитория; в репозитории есть их копия `Tools/FogMSEnergyValidation/ProdProbe/build_plugin.sh` с
захардкоженными путями. Подробности — [`FogMS_HANDOVER.md`](FogMS_HANDOVER.md), раздел 7.

Требования рендерера для FogMS — `FogMS_UserGuide.md`, раздел 2 (D3D12 SM6, inline hardware ray tracing, Lumen GI; Box сам
ставит `r.RayTracing.Culling 0` и `r.Lumen.AsyncCompute 0`). Для MLS обязательны `r.Substrate=0` и
`r.AllowStaticLighting=0` (`README_RU.md`).
