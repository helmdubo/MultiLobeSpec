# v0.15 — Activision Direct Micro-Shadow

> **Статус на 2026-09-27 (код — коммит `083341c`): частично устарел.** Документ v0.15 (UE 5.7). Расхождения с кодом:
> - Формула ниже — без множителя v0.15.2: к прямому микрозатенению добавлен `Direct Cavity Depth` (по умолчанию 0,5) /
>   `Direct Cavity Power` (1): M ×= lerp(1, V^power, depth) (`MLS.CavityDepth`, `MultiLobeShaderPatcher.cpp`).
> - `r.GBufferDiffuseSampleOcclusion` допускается 0 или 1 (`MultiLobeSpec.cpp`, проверка перед Apply); обязательны только
>   `r.Substrate 0` и `r.AllowStaticLighting 0`.
> - Назначение AO в бейкере с 0.15.3: сначала `<NormalParam>_ao` (например `tex2 -> tex2_ao`), затем `AO1..AO3`; пустой
>   список имён — автоматический режим (`CHANGELOG.md`, v0.15.3).
> - UE 5.8.2: плагин собирается под UE 5.8; по логам и манифестам оверлеев в проекте заказчика (вне репозитория) все
>   обязательные анкеры MLS на 5.8.2 находятся, оверлей применяется; корректность затенения на 5.8.2 не проверена. Анкер
>   тонмаппера `OutDeviceColor / 1.05` в 5.8 не найден (предупреждение, влияние не проверено).
> - Generic VNDF LUT: квитанция `Resources/Generated/MLS_MicroShadowLUT.validation.json` привязана SHA-256 к точным байтам
>   манифеста и include. Манифест в git с хешем квитанции не совпадает, include совпадает только без преобразования концов
>   строк (на Windows с `core.autocrlf` — нет); проверено 2026-09-27 пересчётом SHA-256. Поэтому в установке из git режим 4
>   не допускается («fail closed»). Инструмента, пишущего эту квитанцию
>   (`MLSMicroShadowValidationReceiptV1`), в репозитории нет; `MLS.ExportVNDFValidation` пишет отчёт другого формата в тот же
>   путь и затирает квитанцию.

## Решение по Generic VNDF

`Generic VNDF LUT` сохранён как **Experimental / Research Only**, но больше не является рекомендуемым production-режимом. На реальных материалах текущая 4D LUT может чрезмерно подавлять direct lighting за пределами самых ярких участков. Reference-integrator остаётся полезен для исследований, однако default v0.15 — `Activision / CoD:WWII`.

Рекомендуемые настройки:

```text
Micro Shadowing = Activision / CoD:WWII
Diffuse Strength = 1.0
Specular Strength = 1.0
```

## Direct formula

Material AO трактуется как cosine-weighted visibility:

```text
1 = полностью открыто
0 = полностью закрыто
```

Runtime:

```hlsl
CosTheta = sqrt(saturate(1 - Visibility));
T = saturate(NoL / max(CosTheta, 1e-6));
MicroShadow = T * T;
```

При `Visibility ~= 1` функция возвращает `1`. Это sharpened-вариант, с которым Activision shipped WWII.

## `r.GBufferDiffuseSampleOcclusion`

В `Micro Shadowing = Off` используется stock legacy transport; проект может оставлять:

```ini
r.GBufferDiffuseSampleOcclusion=1
```

При любом активном micro-shadow mode immutable overlay:

1. сохраняет исходный `MaterialAO`;
2. переносит его через `GenericAO` как continuous scalar;
3. восстанавливает его в `GBufferAO`;
4. внутри active permutation выставляет `DiffuseIndirectSampleOcclusion=0`;
5. по умолчанию использует профиль `Indirect Visibility = Direct Only`, но не перезаписывает осознанный выбор Full/Legacy.

Глобальная CVar может оставаться `1`. Engine files не меняются.

Обязательный contract:

```ini
[/Script/Engine.RendererSettings]
r.Substrate=False
r.AllowStaticLighting=False

[ConsoleVariables]
r.GBufferDiffuseSampleOcclusion=1
```

## Shading-domain audit

| Domain | v0.15 behavior |
|---|---|
| Base Color | не меняется |
| Direct diffuse | готовый Lambert/UE Rough GGX contribution умножается на microshadow |
| Direct specular | каждый GGX-лоб затеняется отдельно, затем лобы смешиваются |
| Roughness | не входит в Activision cone formula; влияет на BRDF |
| Directional/Point/Spot | поддержаны |
| Rect LTC | stock fallback, без microshadow |
| Anisotropy | stock fallback |
| Lumen/Skylight/non-Lumen diffuse | Material AO не применяется |
| Reflection captures/Skylight specular | material visibility не применяется |
| Lumen/SSR specular | stock response |

Это соответствует **direct microshadowing** Activision. Default DirectOnly не переносит material visibility в GI/indirect specular. Full profile отдельно включает albedo-aware indirect diffuse; cone-aware indirect specular остаётся staged и fail-closed выключенным.

Начиная с v0.15.1 `Preset` управляет только dual-lobe BRDF. Поэтому `Preset=Off` вместе с активным Micro Shadow Mode означает `vanilla UE BRDF + Activision microshadow`, а не неактивный plugin. Reflection Environment имеет отдельный обязательный policy marker и в DirectOnly не смешивает material visibility со stock scalar GTSO.

## Baker presets

Все built-in presets сохраняют raw visibility:

```text
Strength = 1
Output Power = 1
```

| Preset | Relief Height | Occlusion Radius | Quality |
|---|---:|---:|---:|
| Debris / Deep | 5.0 cm | 15 cm | 520 spp |
| Stone / Brick / Medium | 2.0 cm | 6 cm | 320 spp |
| Sand / Earth / Shallow | 0.4 cm | 2 cm | 192 spp |
| Custom | editable values | | |

Surface Size задаётся пользователем (ширина одного UV-тайла в сантиметрах) и не
входит в пресеты. Всё остальное — relief multiplier (unit Poisson solve + robust
P1..P99 span), pixel radius (cm -> texels), slices/steps (из Quality), sampling
kernel (distribution 2.35, falloff `1 - smoothstep(0.6R, R, d)`) — вычисляется;
одинаковые настройки дают одинаковый физический AO на 1K/2K/4K. Каждый bake
пишет `.mlsbake.json` receipt рядом с AO-ассетом. Baker позволяет удалить элементы из списка и после успешного bake сразу назначает `<base>_tex_ao` в AO slots найденных material instances. Отдельный assignment workflow не нужен.

## Debug and no-fork

`MLS.DebugView 0..5` использует SceneViewExtension/UserFlags и не должен rebuild/remap/compile shaders.

Все shader changes выполняются только в:

```text
<Project>/Saved/MultiLobeSpec/Shaders_<hash>/
```

`Engine/Shaders` и `Engine/Source` не изменяются.

## Visual acceptance

1. `Visibility=1`: Activision mode совпадает с Off.
2. Cavity darkens only according to direct light direction.
3. Без direct light baked visibility не остаётся в Lumen/Skylight diffuse.
4. `r.GBufferDiffuseSampleOcclusion=1` работает в Activision mode; Off возвращает stock behavior.
5. Point/Spot меняют microshadow вместе с light vector.
6. Rect/anisotropy остаются stock.
7. Debug switches on the next frame without shader jobs.
