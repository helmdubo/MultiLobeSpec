# Support matrix v0.14.3

| Pipeline / feature | Direct dual-lobe | Generic VNDF direct | RGB indirect diffuse | Cone-aware per-lobe IBL | Примечание |
|---|---:|---:|---:|---:|---|
| Legacy deferred Default Lit isotropic static mesh | да | да | да | staging, недоступно | CARD-09 static storage compile gate failed |
| Legacy deferred Default Lit anisotropic | stock/частично | нет | да | нет | stock fallback для specular |
| Rect lights | dual-lobe LTC approx | нет | n/a | n/a | mean-direction adapter |
| Reflection captures | да | n/a | n/a | staging, недоступно | R1/R2 direction+radiance+response реализованы; нужен Texture3D/SRV |
| Skylight specular | да | n/a | n/a | staging, недоступно | DFAO остаётся geometric; нужен Texture3D/SRV |
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
