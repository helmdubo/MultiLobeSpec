# Support matrix v0.15.1

> **Статус на 2026-09-27 (код — коммит `083341c`): частично устарел.** Матрица v0.15.1 (UE 5.7). Нет столбца для
> рекомендуемого режима `Activision / CoD:WWII` и других аналитических режимов; строка MegaLights «условно» — в манифесте
> возможностей `GenericVNDFDirect_MegaLights` всегда false (`MultiLobeShaderPatcher.cpp`); «Generic VNDF direct: да» — только
> при допущенной квитанции (ниже). Строка Cooked/package «нет» верна: MLS работает только в редакторе (`WITH_EDITOR`).
> - UE 5.8.2: плагин собирается под UE 5.8; по логам и манифестам оверлеев в проекте заказчика (вне репозитория) все
>   обязательные анкеры MLS на 5.8.2 находятся, оверлей применяется; корректность затенения на 5.8.2 не проверена. Анкер
>   тонмаппера `OutDeviceColor / 1.05` в 5.8 не найден (предупреждение, влияние не проверено).
> - Generic VNDF LUT: квитанция `Resources/Generated/MLS_MicroShadowLUT.validation.json` привязана SHA-256 к точным байтам
>   манифеста и include. Манифест в git с хешем квитанции не совпадает, include совпадает только без преобразования концов
>   строк (на Windows с `core.autocrlf` — нет); проверено 2026-09-27 пересчётом SHA-256. Поэтому в установке из git режим 4
>   не допускается («fail closed»). Инструмента, пишущего эту квитанцию
>   (`MLSMicroShadowValidationReceiptV1`), в репозитории нет; `MLS.ExportVNDFValidation` пишет отчёт другого формата в тот же
>   путь и затирает квитанцию.

| Pipeline / feature | Direct dual-lobe | Generic VNDF direct | RGB indirect diffuse | Cone-aware per-lobe IBL | Примечание |
|---|---:|---:|---:|---:|---|
| Legacy deferred Default Lit isotropic static mesh | да | да | да | staging, недоступно | CARD-09 static storage compile gate failed |
| Legacy deferred Default Lit anisotropic | stock/частично | нет | да | нет | stock fallback для specular |
| Rect lights | dual-lobe LTC approx | нет | n/a | n/a | mean-direction adapter |
| Reflection captures | да | n/a | n/a | staging, недоступно | DirectOnly/Full удаляют material AO из stock scalar GTSO; screen AO сохраняется |
| Skylight specular | да | n/a | n/a | staging, недоступно | DirectOnly/Full удаляют material AO; DFAO остаётся geometric |
| Lumen diffuse | n/a | n/a | да | n/a | 3/3 material visibility sites |
| Lumen reflections | stochastic dual blur optional | n/a | n/a | нет | lobe identity отсутствует |
| SSR | stock | n/a | n/a | нет | stock authored-roughness response |
| Clear Coat | stock | нет | не заявлено | нет | отдельная shading model семантика |
| MegaLights legacy Default Lit | условно | условно | не валидировано | не валидировано | capability зависит от exact legacy route |
| Forward/mobile | не заявлено | нет | нет | нет | вне target |
| Path Tracer | stock | нет | нет | нет | raster overlay не oracle PT |
| Cooked/package | нет | нет | нет | нет | editor-only overlay |

## Capability terms

- `available=true` означает: requested config, artifacts, renderer prerequisites и patch anchors присутствуют.
- `implemented=true` не означает numerical admission.
- Direct Generic VNDF admission требует exact canonical receipt + SHA-256 binding; relative
  azimuth остаётся measured-but-not-gated для `Isotropic4D Phi0 approximation`.
- CARD-09 оставляет `NumericallyAdmitted=false`, пока отдельно не утверждены mean/p95/p99/max thresholds.
- CARD-09 оставляет `StaticStorageCompileAdmitted=false`; настройка выключена по умолчанию и fail-closed.
- `Transactional_PreRemapSmokeCompile=false`: текущая архитектура компилирует после remap; полный compile receipt не переименовывается в pre-remap smoke.
- `MLS.DebugView 0..5` использует plugin-owned биты stock `View.PostVolumeUserFlags`
  и переключается без shader recompile и без UE source patch.
- Debug 1..5 — lighting-weighted masks в legacy deferred Default Lit hook; это не
  абсолютный full-screen scalar и не selected-light diagnostic. Captures hard-gated off.
