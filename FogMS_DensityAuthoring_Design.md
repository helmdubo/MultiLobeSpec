<!-- Design note by worker W31 (Opus), 2026-09-23, verified against HEAD 08de197. No code changed. -->

# FogMS: дизайн плотности Box (эрозия, профили высоты, рельеф, weather map)

Это только дизайн, код не менялся. Ссылки проверены на HEAD `08de197`. Метки: **V** = VERIFIED (проверено по файлу), **NF** = NOT FOUND (не найдено), **A** = ASSUMED (предположение).

## 1. Как сейчас согласованы материал и `.ush`

**Автоматической синхронизации нет (NF).** В репозитории нет ни генератора материала, ни теста на совпадение текстов. Генераторы лежат вне репо:
- `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_A1e_20260921\create_density_material.py`;
- его библиотека формул `…\FogMS_HV_20260920\upgrade_fog_material.py:32-53` (`UVW_CODE`, `DETAIL*_UVW_CODE`, `EXTINCTION_CODE`).

HLSL внутри `M_FogMS_Density.uasset` прочитан как ASCII-строки (смещения 15811–19335). Код `Custom_3` совпадает с `EXTINCTION_CODE` дословно (V). Позднейшие правки инъекции (коммиты `6c8e2a6`, `59ac907`) делались скретч-скриптами, которых в репо нет (NF). В репо есть только `matedit_injection.py`: `Custom_3` берётся по имени (:17) и подключается к пину `Extinction` узла инъекции (:47) (V).

**Параметры** (MID ставит их в `FogMS_BoxVolume.cpp:756-772`, пакет пишет `FogMS_BoxRuntime.cpp:577-592`), V:

| Материал | Пакет | `.ush` |
|---|---|---|
| `FogMS_Density` [1/м] | 7.w = `Density*0.01` [1/см] | `DensityPerCm` |
| `FogMS_TileScale` | 8.xyz | `TileScale` |
| `FogMS_Threshold` | 8.w | `Threshold` |
| `FogMS_Softness` / `FogMS_DensityFeather` | 9.x / 9.y | `Softness` / `DensityFeather` |
| знак масштаба (зеркалит сам куб) | 9.zw, 10.x | `UVSign` |
| `FogMS_ChannelMask` | 10.y | `Channel` |
| `FogMS_DetailStrength/Scale/SecondOctave` | 11.yzw | то же |
| `FogMS_WorldAligned/Frequencies/Phase0..2` | 12–15 | то же |
| `FogMS_WorldExtent` | 2–4.w | `Extent` |
| `FogMS_Albedo` | 23.rgb | — |

Единицы 1/м → 1/см: штатная вокселизация делит на 100. Это подтверждено калибровкой с отклонением ≤0.147% (`FogMS_TextureDensity_Report.md:71`, V).

**Материал** (`Custom_3`, строка в uasset):
```hlsl
float mask = Softness > 0.0f ? smoothstep(Threshold - Softness*0.5f, Threshold + Softness*0.5f, n) : step(Threshold, n);
float3 edge = (1.0f - abs(LocalPosition) * 0.02f) * WorldExtent;   // LocalPosition в кубе -50..50
float width = min(Feather, min(WorldExtent.x, min(WorldExtent.y, WorldExtent.z)));
float fade = width > 0.0f ? smoothstep(0.0f, width, inside) : step(0.0f, inside);
return max(Density, 0.0f) * mask * fade;
```
**`.ush`** (`FogMS_Indirect.ush:138-169`):
```hlsl
float3 UVW = (LocalPosition / Data.Extent * Data.UVSign * 0.5f + 0.5f) * Data.TileScale;
if (Noise < Lo - Data.DetailStrength - 1.e-6f) return 0.0f;           // :148
if (Noise >= Hi + Data.DetailStrength + 1.e-6f) Mask = 1.0f;          // :150
Mask = Data.Softness > 0.0f ? smoothstep(Lo, Hi, Noise) : step(Data.Threshold, Noise);
return Data.DensityPerCm * Mask * Fade;
```
Отсюда соответствие `LocalPosition*0.01 ≡ Local/Extent*UVSign*0.5`.

**Побайтного совпадения сегодня нет, совпадают формулы.** Причина в фильтрации:
- материал сэмплирует аппаратно, `TMVM_MIP_LEVEL 0` с wrap-сэмплером (`create_density_material.py:136-137`, V);
- `.ush` делает 8 `Load` и считает трилинейку во float (`:105-130`, V).

Байты текстуры одни и те же: атлас BGRA8 UNORM, копия mip 0 без конверсии (`FogMS_DensityAtlas.usf:1-4`, V).

**Проверка согласованности существует одна.** Это `FogMS.Debug 1` (σt материала из VBuffer, `MultiLobeShaderPatcher.cpp:829`) против `FogMS.Debug 4` (`FogMS_AuthoredDensity` в центре фроксела, :833). Шкала одна: `σt[1/см]·1e4` (`FogMS_Common.ush:234`). После вычитания фона height fog MAE было: white **1.91e-5**, Perlin **5.96e-5**, повёрнутый/отрицательный масштаб **5.60e-5** (`FogMS_A1c_Report.md:49-53,80`, V). После A1c (detail, world-aligned) замер не повторяли (NF).

## 2. (a) Эрозия краёв

**Свободных компонент в строках 0–23 нет.** Раскладка в `FogMS_BoxRuntime.cpp:66-80`, V:
- 5.z (Lumen Bounce), 6.xyz (fallback albedo) и 16.w (tolerance) уже заняты;
- 17–20 — кэш теней;
- 7, 9–12, 21–23 заняты полностью.

У текстуры пакета есть второй ряд (`y=1`, в основном пустой, :396-405), но его читают только потребители overlay. Продюсеры получают `BoxRows[24]` как uniform (`FogMS_Transport.cpp:89`, `FogMS_WorldLighting.cpp:95`, `FogMSRender.cpp:55`, V). Поэтому новые параметры идут в **новые строки 24+** (срез S0 ниже).

**Источник Worley уже есть.** Генератор пишет в R Perlin-Worley, в G/B/A Worley FBM (`gen_perlin_worley.py:4`, V). `FogMS_IndirectSampleNoise` и так загружает `float4` и отбрасывает три канала (:119-126). Если он будет возвращать `float4`, эрозия берёт свой канал из той же выборки `Detail1` без новых загрузок и без новой частоты или фазы. В материале входы `Detail0`/`Detail1` уже RGBA (`create_density_material.py:154`, V).

**Формула** (в пространстве шума, после detail):
```hlsl
float EdgeW = 1 - saturate((n - Lo) / ErosionDepth);           // 1 на нижней кромке, 0 в глубине
n -= ErosionStrength * EdgeW * dot(Detail1Sample, ErosionMask); // эквивалент Nubis remap(b, e·w, 1, 0, 1)
```
Эрозия только вычитает (e ≥ 0), поэтому плотность ниже полосы порога появиться не может.

| Параметр | Диапазон | По умолчанию |
|---|---|---|
| `ErosionStrength` | [0,1] | 0, т.е. выключено и результат побитово прежний |
| `ErosionDepth` | [0.01,1] в единицах шума | 0.3 |
| `ErosionChannel` | G/B/A | G |

Строка **24** = (Strength, Depth, Channel, резерв).

**Ранние выходы:**
- нижний `:148` не меняется: он остаётся верным, потому что эрозия ≥ 0;
- верхний `:150` меняется на `Noise ≥ max(Hi, ES>0 ? Lo+ErosionDepth : Hi) + DS + 1e-6`. Логика: n ≥ Noise−DS ≥ Lo+Depth ⇒ EdgeW = 0;
- ветку detail надо выполнять и при `DetailStrength == 0`, если `ES > 0`.

## 3. (b) Профили высоты: одно семейство из 6 скаляров

Высота считается одинаково в обоих местах: `h = Local.z/Extent.z*UVSign.z*0.5+0.5` ≡ `LocalPosition.z*0.01+0.5`. Отрицательный Z-масштаб переворачивает профиль так же, как текстуру.

```hlsl
float SS(float a, float w, float x) { return smoothstep(a, a + max(w, 1e-4f), x); } // один helper в обоих местах
P = SS(B, SB, h) * (1 - SS(T - ST, ST, h)) * (1 + A * SS(B + 0.5*(T-B), max(T-ST-(B+0.5*(T-B)), 1e-4f), h));
Noise *= P * Coverage;   // до ранних выходов
```
Скаляры: `HeightBottom` B, `HeightTop` T, `BottomSoftness` SB, `TopSoftness` ST, `AnvilStrength` A ∈ [0,1], `ProfileEnabled` (флаг `Invert` не нужен). Строки: **25** = B, T, SB, ST; **26** = A, флаги.

| Тип | B | T | SB | ST | A |
|---|---|---|---|---|---|
| stratus | .40 | .60 | .05 | .10 | 0 |
| cumulus (плоское основание, купол) | .10 | .70 | .02 | .45 | 0 |
| cumulonimbus (колонна + наковальня) | .05 | .98 | .02 | .10 | .6 |
| туман в долине | 0 | .35 | 0 | .30 | 0 |

Купол получается порогом: где P мало, выживают только пики шума (как в Nubis). Наковальня даёт P > 1, то есть расширение покрытия. Ранние выходы сравнивают уже сформированный шум, поэтому остаются верными. Добавляется выход **до** выборки текстуры: `P*Coverage < Lo − DS − 1e-6 → 0`. Он корректен, потому что UNORM ≤ 1.

Оговорка (A): `DensityEdgeFeather` гасит плотность у всех 6 граней (`:168`), включая дно. Для тумана в долине позже понадобится feather по осям.

## 4. (c) Прилипание к рельефу

| Источник | Материал | Compute (продюсер и overlay) | Стоимость | Работа автора | Корректность |
|---|---|---|---|---|---|
| 2D heightmap по XY Box | `TextureSampleParameter2D`, mip 0 | привязанный SRV (как `FogMSDensityAtlas`, `Transport.cpp:147`) + дескриптор в heap для overlay | 4 `Load` | текстура от автора | стабильна; при переносе Box нужна новая карта |
| Global Distance Field | узел есть, выставляет `bUsesGlobalDistanceField` (`HLSLMaterialTranslator.cpp:12167-12181`, V); в материалах GDF идёт через View UB (`GlobalDistanceFieldShared.ush:5-6,21-27`, V); в вокселизации не проверено (A) | продюсеры привязывают View UB (`FogMS_Transport.cpp:82,308`) и могут задать `DISTANCE_FIELD_IN_VIEW_UB 1` (A) | цикл по clipmap с page table (:214), дорого (A) | нулевая | **неверно**: это расстояние до любой геометрии (деревья, крыши), а не высота над землёй; clipmap центрированы на камере, поэтому плотность меняется при движении камеры, а подпись кэша теней (`FogMSRender.cpp:253-258`) этого не видит; на первом кадре задержка релевантности (`DistanceFieldAmbientOcclusion.cpp:777`) |
| Высота, запечённая SceneCapture | как у heightmap | как у heightmap | 4 `Load` + разовый бейк | одна кнопка «Capture Terrain» | как у heightmap; перезапечь после переноса |

**Рекомендация для v1:** запечённая высота в канале **A** той же 2D-карты Box, что и weather map (раздел 5). Один биндинг на обе фичи. Формат BGRA8 с той же проверкой приёма, что у атласа (`FogMS_DensityAtlas.cpp:204-236`); 8 бит на высоту Box. Рельеф подключается к семейству профилей, а не отдельной формулой:
`h_ref = lerp(h, saturate((h − g)/max(1 − g, 1e-4)), TerrainFollow)`, плюс ноль под землёй.

## 5. (d) Weather map

Текстура `FogMS_BoxMap`, каналы R coverage, G type, B storm, A высота земли. UV: `LocalPosition.xy*0.01+0.5` в материале, `Local.xy/Extent.xy*UVSign.xy*0.5+0.5` в `.ush`. Адресация clamp, mip 0. В продюсере ручная билинейка через `Load`.

- **Coverage** входит множителем в `Noise` (§3).
- **Type** вместо смешивания двух профилей (это 12 скаляров) задаёт верх колонны: `T_eff = lerp(B + TypeMinThickness, T, type)`. Наковальня: `A_eff = A·SS(0.7, 0.3, type)`.
- **Storm**: `σt ×= 1 + StormDensityBoost·s`, `albedo ×= lerp(1, StormAlbedoScale, s)`.

Строка **27** = (StormDensityBoost, StormAlbedoScale, TypeMinThickness, дескриптор карты для overlay); строка **28** = TerrainFollow + резерв.

**Что нужно решателю.** Если альбедо остаётся на уровне Box, ничего нового. Для потемнения ячейки в шторм нужно менять только pass 0. Он уже пишет `float4(Density*Albedo, Density)` (`FogMS_Transport.usf:249-255`, V), а дальше решатель восстанавливает альбедо по ячейке через `S.rgb/S.a` (:98). Значит, в pass 0 достаточно копить σs = Σ dᵢ·albedoᵢ / 8. Оставшиеся читатели 23.rgb надо перевести на альбедо в точке:
- `FogMS_Indirect.ush:246`;
- `FogMS_Reconstruction.ush:89,119,266`;
- `FogMS_WorldLighting.usf:163,187`;
- в материале BaseColor и Emissive инъекции (Emissive = J·Albedo·σt).

## 6. Срезы, правки материала, A/B

**Предусловие:** дождаться слияния multi-Box. Все срезы трогают `FogMS_BoxRuntime.cpp` и `FogMS_BoxVolume.*`, которые сейчас правит другой воркер. Раскладка пакета может стать per-Box.

**S0a** (без изменения поведения): заменить литералы `24` на `FogMSRender::BoxRowCount`, расширить `FShadowCacheSignature` на строки 24+. Файлы: `FogMS_WorldLighting.h:37`, `FogMS_Transport.cpp:89,168,317`, `FogMS_WorldLighting.cpp:95,132,543`, `FogMSRender.cpp:55,108,253`.

**S0b:** `BoxRowCount` 24→32 (`FogMS_ShadowCache.h:12`), `FogMS_BoxPacketRowCount` и `static_assert(384)` (`BoxRuntime.cpp:62,85`), define в патчере (`MultiLobeShaderPatcher.cpp:864`), `#error` (`FogMS_Common.ush:21`). Затем `FogMS.Apply`.

Критерии S0: сборка проходит; дамп при заморозке `fielddiff` rel L2 = **0.0000**; Debug1/Debug4 в пределах чисел A1c; `nightcheck` без регрессий.

**S1 эрозия.** Файлы: `.ush`, `BoxVolume.h/.cpp` (свойства, валидация как в :596-605, `HasSameDensityParameters` :498-511, MID), `BoxRuntime.cpp` (строка 24), ассет.

Критерии S1:
1. при ES = 0 дамп побитово равен прежнему;
2. Debug1/4 MAE не хуже **5.96e-5**;
3. PSNR внутри Box в `abinject.sh` не ниже базы (**37.8 дБ**, `FogMS_Prod_Report.md:131-134`; в HANDOVER 44 дБ для раунда 12);
4. стоимость `transport` в GPU-профиле ±5%;
5. визуальная оценка за владельцем.

**S2 профиль** (те же 4 файла + ассет). Критерии те же, плюс 4 пресета на скриншотах.

**S3a инфраструктура `FogMS_BoxMap`:** `BoxVolume.h/.cpp`, `BoxRuntime.cpp`, `FogMS_WorldLighting.h` (поле запроса), `FogMS_Transport.cpp`, `FogMS_WorldLighting.cpp`.
**S3b** coverage/type/storm-σt: `.ush` + ассет.
**S4 рельеф:** кнопка захвата в `BoxVolume.cpp` + `.ush` + ассет.
**S5 альбедо в ячейке:** `Transport.usf`, `WorldLighting.usf`, `Reconstruction.ush`, `Indirect.ush` + ассет.

**Правки материала в редакторе (Python, по образцу `matedit_injection.py`).** Скрипт должен быть идемпотентным (проверка `ALREADY_PATCHED`) и запускаться, когда владелец не работает в редакторе. Шаги:
1. Создать параметры `FogMS_ErosionStrength` (0), `FogMS_ErosionDepth` (0.3), вектор `FogMS_ErosionMask` (0,1,0,0).
2. Создать **новый** Custom-узел `FogMS_Extinction` с прежними 12 входами плюс новыми. Прежние входы находятся по имени параметра и по узлам сэмплов с их UV.
3. Подключить его к `MP_SUBSURFACE_COLOR` (там сейчас extinction, `create_density_material.py:164`) и к пину `Extinction` узла инъекции. Он сейчас идёт от `Custom_3` (`matedit_injection.py:47`); так ли в текущем ассете, не проверено (A).
4. Отключить `Custom_3`, перекомпилировать, сохранить, сверить значения по умолчанию.

Для S3: `TextureSampleParameter2D` `FogMS_BoxMap` (clamp, `TMVM_MIP_LEVEL 0`) с UV из `LocalPosition.xy*0.01+0.5`.

**Метод A/B.** Плотность замораживается через `freeze_density_animation`.
1. `fielddiff` по слою σs/σt дампа (`FogMS_WorldLighting.cpp:839`) между «до» и «после при значениях по умолчанию»: цель rel L2 = 0.
2. Debug1 минус baseline с чёрной текстурой против Debug4 по методике A1c. Нужны `-BindlessAll`, Indirect Shadowing при Strength 0, непрозрачная поверхность в ядре Box ближе 0.6×Fog View Distance, режим с включённой плотностью MID (Emissive Injection или не-Transport).
3. `abinject.sh`: PSNR внутри Box, overlay (`.ush`) против инъекции (плотность из материала).

Рекомендуется добавить в репо сверку формул на CPU: numpy по байтам атласа, обе формулы, случайные точки. Это ловит расхождение текстов формул, но не поведение GPU.
