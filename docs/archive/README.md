# Архив документов (FogMS и MLS)

Здесь лежат документы, которые **не описывают текущее поведение кода**: отчёты и протоколы этапов A1–B3 (19–22.09.2026),
ранние аудиты и исследования, решения которых уже реализованы, переделаны или отменены, и прежний handover. Они сохранены
как история решений и доказательств. Перенесены сюда `git mv` 2026-09-27; история каждого файла доступна через
`git log --follow docs/archive/<файл>`.

**Текущая документация:** [`FogMS_HANDOVER.md`](../../FogMS_HANDOVER.md) — обзор для аудитора (архитектура, модули, пути
рендера, сборка, приватные зависимости движка, открытые задачи); [`FogMS_UserGuide.md`](../../FogMS_UserGuide.md) —
свойства, cvar, команды, политика ассетов; дизайн-документы в корне репозитория (статус реализации — в шапке каждого);
хронологический журнал этапа C — [`docs/history/FogMS_Prod_Report.md`](../history/FogMS_Prod_Report.md).

Как читать архивные документы. Имена свойств, значения по умолчанию, статусы, номера пакетов (`Package1…6`), пути к
логам и доказательствам в `E:/GITHUB/MultiLobeSpec/.codex-build/…` (папка вне репозитория) и карта `/Game/FogMS_Test/FogMS_Box`
(в проекте заказчика) относятся к моменту написания. Ссылки между архивными документами работают (они лежат рядом);
упоминания `FogMS_HANDOVER.md` внутри них означают прежний handover — теперь это `FogMS_HANDOVER_2026-09-23.md`.
Режимы этапов A1–B1 (оверлей A1, Octaves, Spatial, World, `Authored/Cast Sun Shadow`, `Indirect Shadowing`, SSFS,
View Integration) в коде остались как legacy-путь (только редактор с `-BindlessAll`); их текущее описание — в
`FogMS_UserGuide.md`, а не в этих отчётах.

## Индекс

| Документ | Что это было | Почему в архиве | Что его заменяет |
|---|---|---|---|
| **Handover и аудиты** | | | |
| [`FogMS_HANDOVER_2026-09-23.md`](FogMS_HANDOVER_2026-09-23.md) | прежний handover: журнал этапов A–C и поручения исполнителю, состояние на 23.09 (раунды до 30) | хронологические записи, большинство состояний и планов устарело | новый [`FogMS_HANDOVER.md`](../../FogMS_HANDOVER.md) |
| [`FogMS_Audit_UE58.md`](FogMS_Audit_UE58.md) | аудит Volumetric Fog UE 5.8.2 (19–20.09): контракт данных, анкеры для оверлея, план этапов A | план этапов выполнен или заменён; факты движка с файл:строка остаются справкой для legacy-оверлея | `FogMS_HANDOVER.md` (архитектура), код оверлея `MultiLobeShaderPatcher.cpp` |
| [`FogMS_Audit2_UE58.md`](FogMS_Audit2_UE58.md) | мини-аудит №2 (20.09): доступ плагина к истории тумана и source term, вопросы Q1–Q8 | вопросы этапов A2/B закрыты: решатель B2/B3 не использует историю тумана | `FogMS_HANDOVER.md` |
| [`FogMS_Research_Note_01.md`](FogMS_Research_Note_01.md) | заметка исследователя v1.5 (20.09): математика A1, октавы A1b, задание мини-аудита | модель A1 и октавы — legacy; октавы переделаны в W38 | `FogMS_UserGuide.md` («Multiple Scattering Look») |
| [`FogMS_External_Audit.md`](FogMS_External_Audit.md) | пакет для внешнего аудита среза B1 (21.09) | срез B1 заменён переносом B2/B3 и этапом C | `FogMS_HANDOVER.md`; доказательства B1 — [`evidence_B1_20260921/`](evidence_B1_20260921/) |
| [`FogMS_Energy_Audit.md`](FogMS_Energy_Audit.md) | энергетика B1 и контракт B2 (21.09) | B1 не был принят как энергосохраняющий и заменён B2/B3; контракт B2 реализован | архивные `FogMS_B2_Report.md`, `FogMS_B3_Report.md`; CPU-эталоны `Tools/FogMSEnergyValidation/` |
| [`evidence_B1_20260921/`](evidence_B1_20260921/) | сводки GPU-проверок и хеши исходников среза B1 (JSON; до 27.09 лежали в `Docs/FogMS/Validation/B1_20260921/`) | доказательства устаревшего среза; перенесены, чтобы в репозитории не было двух папок `Docs/` и `docs/`, различающихся регистром | — |
| **Этап A (20–21.09): оверлей A1, Box, плотность** | | | |
| [`FogMS_A1_Report.md`](FogMS_A1_Report.md) | A1: самозатенение directional в оверлее `VolumetricFog.usf`, `r.FogMS.Enable`, `FogMS.Apply`, debug-виды | legacy-путь; отчёт — о сборке и приёмке 20.09 | `FogMS_UserGuide.md` (раздел 5, legacy-оверлей) |
| [`FogMS_Project_Install.md`](FogMS_Project_Install.md) | история установок пакетов 1–3 в проект (20–21.09) | установки этапа A; процесс сборки с тех пор другой | `FogMS_HANDOVER.md` («Сборка, установка, проверка») |
| [`FogMS_Box_Report.md`](FogMS_Box_Report.md) | живой Box без Apply (20.09) | Box с тех пор получил плотность, решатель, доставку | `FogMS_UserGuide.md` |
| [`FogMS_Visibility_Fix.md`](FogMS_Visibility_Fix.md) | скрытие Box через Outliner (20.09) | исправление в коде (`FogMS_BoxRuntime.cpp`, статус «Off (Box hidden)») | код |
| [`FogMS_TextureDensity_Plan.md`](FogMS_TextureDensity_Plan.md), [`FogMS_TextureDensity_Report.md`](FogMS_TextureDensity_Report.md) | плотность из Volume Texture через Volume-материал (20.09), появление `M_FogMS_Density` и `T_FogMS_DefaultVolume` | реализовано; параметры и материал с тех пор расширены (эрозия, профиль, инъекция) | `FogMS_UserGuide.md` (разделы 3, 4, 9) |
| [`FogMS_A1b_Report.md`](FogMS_A1b_Report.md) | A1b: октавное приближение в source term (20.09) | октавы стали `Octaves (Legacy, overlay)`, фаза октав изменена в W38 | `FogMS_UserGuide.md` («Multiple Scattering Look») |
| [`FogMS_A1c_Research.md`](FogMS_A1c_Research.md), [`FogMS_A1c_Report.md`](FogMS_A1c_Report.md) | A1c: ослабление входящего Lumen-света средой Box (20.09); визуальная приёмка не пройдена | legacy (`Indirect Shadowing`, только вне Transport) | `FogMS_UserGuide.md` (FogMS\|Indirect) |
| [`FogMS_A1d_Report.md`](FogMS_A1d_Report.md) | A1d: детальные октавы и авторская тень солнца (21.09) | детали плотности живут в Box; тень солнца — legacy `Authored Sun Shadow` | `FogMS_UserGuide.md` |
| [`FogMS_A1e_Report.md`](FogMS_A1e_Report.md) | A1e: мировая привязка плотности и тень солнца на поверхностях (21.09) | мировая привязка — в Box; `Cast Sun Shadow` — legacy; тень облака на земле теперь через карту теней облака (раунд 47) | `FogMS_UserGuide.md` |
| [`FogMS_A1f_Research.md`](FogMS_A1f_Research.md) | мягкое освещение под облаком, разбор RDR2 (21.09) | исследование заменено более полными обзором и дизайном | `FogMS_Cloud_Lighting_Review.md`, `FogMS_Weather_Design.md` |
| [`FogMS_Fields_Report.md`](FogMS_Fields_Report.md), [`FogMS_Fields_Test_Protocol.md`](FogMS_Fields_Test_Protocol.md), [`FogMS_Fields_Worklog.md`](FogMS_Fields_Worklog.md) | фильтрованный кэш тени солнца и Spatial (A2) (21.09) | legacy-оверлей (`Filtered Sun Shadow`, `Spatial (Experimental)`) | `FogMS_UserGuide.md` (FogMS\|Sun) |
| [`FogMS_Banding_Report.md`](FogMS_Banding_Report.md) | `Filter Sun Inside Volume` (21.09) | legacy-оверлей | `FogMS_UserGuide.md` |
| [`FogMS_Stability_Report.md`](FogMS_Stability_Report.md) | стабильность и дальность preview, первые падения D3D12 (21.09) | причина падения найдена позже (`FogMS_Crash_Report.md`); preview-настройки этапа A | `FogMS_HANDOVER.md` |
| [`FogMS_Crash_Report.md`](FogMS_Crash_Report.md) | разбор падения D3D12 при `-BindlessAll` и защита `r.RHICmd.ParallelTranslate.Enable 0` (21.09) | отчёт о сбое; **сама защита в коде осталась** (`Source/FogMSRender/Private/FogMS_RHICompatibility.cpp`), обоснование актуально | `FogMS_HANDOVER.md` (техдолг) |
| **Этап B (21–22.09): перенос, анимация, View Integration, SSFS** | | | |
| [`FogMS_B1_Report.md`](FogMS_B1_Report.md) | B1 `World (Current Frame)`: три порядка рассеяния в сетке 32³ (21.09) | заменён переносом B2/B3; режим остался как legacy | `FogMS_HANDOVER.md` (решатель) |
| [`FogMS_B2_Report.md`](FogMS_B2_Report.md), [`FogMS_B2_Verification.md`](FogMS_B2_Verification.md) | `Transport (B2)`: шесть направлений, PCG (21.09) | режим в коде есть, но решатель с тех пор изменён (warm start, пресеты, доставка, источники) | `FogMS_HANDOVER.md`, `FogMS_UserGuide.md`, журнал этапа C |
| [`FogMS_B3_Report.md`](FogMS_B3_Report.md), [`FogMS_B3_Verification.md`](FogMS_B3_Verification.md) | `Transport (B3 Angular)` 48/96 направлений и анимация плотности (21.09) | то же; 16/24 направления, квадратура по солнцу и пресеты добавлены в этапе C | то же |
| [`FogMS_Motion_Research.md`](FogMS_Motion_Research.md), [`FogMS_Motion_Report.md`](FogMS_Motion_Report.md), [`FogMS_Motion_Verification.md`](FogMS_Motion_Verification.md) | согласованное движение плотности и реконструкция во фрокселях (пакет 4, 21.09) | реконструкция оверлея — legacy; основной путь — инъекция через материал и облачный хост | `FogMS_UserGuide.md`, `FogMS_PerPixelClouds_Design.md` |
| [`FogMS_EdgeFlow_Report.md`](FogMS_EdgeFlow_Report.md), [`FogMS_EdgeFlow_Verification.md`](FogMS_EdgeFlow_Verification.md) | `Edge Flow Speed` (пакет 5, 21.09) | свойство в коде есть; отчёт — о приёмке пакета 5 | `FogMS_UserGuide.md` (анимация) |
| [`FogMS_Dolly_Research.md`](FogMS_Dolly_Research.md) | дрожание контуров при движении W/S (21.09) | причина найдена в раундах 35–39, ответ — инъекция и облачный хост | журнал этапа C (раунды 35–39), `FogMS_PerPixelClouds_Design.md` |
| [`FogMS_ViewIntegration_Report.md`](FogMS_ViewIntegration_Report.md), [`FogMS_ViewIntegration_Verification.md`](FogMS_ViewIntegration_Verification.md) | View Integration, пакет 1 (22.09) | отклонено владельцем; `r.FogMS.ViewIntegration` остался legacy (по умолчанию 0) | `FogMS_UserGuide.md` (раздел 5) |
| [`FogMS_ScreenScattering_Research.md`](FogMS_ScreenScattering_Research.md) | штатный FSSS UE как экранное рассеяние (22.09) | отклонено владельцем | — |
| [`FogMS_SSFS_Report.md`](FogMS_SSFS_Report.md), [`FogMS_SSFS_FogLight_Fix.md`](FogMS_SSFS_FogLight_Fix.md), [`FogMS_SSFS_Verification.md`](FogMS_SSFS_Verification.md) | собственный SSFS-постфильтр (22.09) | по решению владельца выключен по умолчанию (`r.FogMS.SSFS 0`); код остался как опция | `FogMS_ForwardLobe_Design.md` (лепесток вместо SSFS), `FogMS_UserGuide.md` |
| [`FogMS_Nubis_Review.md`](FogMS_Nubis_Review.md) | разбор Nubis³ и тогдашнего непрямого света (20.09), предложение прототипа на Heterogeneous Volumes | прототип HV проверен и отклонён 20.09 (`FogMS_A1d_Report.md`) и повторно в `FogMS_PerPixelClouds_Design.md` §2.3; ссылки на строки плагина устарели | `FogMS_Cloud_Lighting_Review.md` §2.5 |
| **Этап C (23.09): исследования, решения которых реализованы** | | | |
| [`FogMS_BindlessFree_Research.md`](FogMS_BindlessFree_Research.md) | маршрут доставки без `-BindlessAll` через emissive Volume-материала и список изменений (23.09) | реализовано: Emissive Injection (раунд 7, `0d87392`), без `-BindlessAll` (раунд 12, `3975e25`), по умолчанию с W36, гибрид v2 вместо `BaseColor = 0` | `FogMS_HANDOVER.md` (доставка), журнал этапа C (раунды 7, 12, 18–20), `FogMS_Fab_Readiness.md` |
| **MLS (BRDF-часть плагина)** | | | |
| [`MultiLobeSpec_v0.14_Generic_VNDF_Execution_Spec_RU.md`](MultiLobeSpec_v0.14_Generic_VNDF_Execution_Spec_RU.md) | нормативная спецификация v0.14 Generic VNDF (август 2026, UE 5.7) | код отличается во многих пунктах: умолчание индиректа `Direct Only`, имена UI, размер и раскладка LUT (V2 33×49×12, 7 узлов, 2 банка), последовательность выборок, откат анизотропии Off, нет smoke-компиляции до ремапа, CARD-09 выключен; разделы 3, 4, 10 — основа Python-валидатора `Tools/MLSMicroShadowValidation/` | `README_RU.md`, `MICROVISIBILITY_ARCHITECTURE_RU.md` (со статусом в шапке) |
| [`MICROVISIBILITY_REVIEW_RU.md`](MICROVISIBILITY_REVIEW_RU.md) | ревью v0.12.1 | входные параметры бейкера и кнопка, о которых оно пишет, удалены; находки исправлены в v0.14–v0.15.1 | `README_RU.md`, `CHANGELOG.md` |
| [`BUILD_FIX_RU.md`](BUILD_FIX_RU.md) | исправление сборки 0.11.3 (линковка `EKeys`, UE 5.7) | исправление давно в коде (`MultiLobeSpecEditor.Build.cs`: `InputCore`), пути проекта 5.7 | `CHANGELOG.md` (v0.11.3) |

## Документы инструментов, оставленные рядом с кодом

Описания GPU-проб и CPU-эталонов этапов B1–B3 и пакетов 4–6 (`Tools/FogMSEnergyValidation/*Probe/`, `ScreenScattering/`,
`*_reference.md`, `*_contract.md`) не перенесены, потому что описывают код, лежащий рядом. В каждом стоит пометка об
историческом статусе; сводка — [`Tools/FogMSEnergyValidation/README.md`](../../Tools/FogMSEnergyValidation/README.md).
