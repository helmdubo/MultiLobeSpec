# Архитектура micro-visibility v0.15.1

## 1. Контракт данных

Material AO — непрерывная cosine-weighted micro-visibility. В legacy deferred UE 5.7 стандартный `GBufferAO` не гарантирует эту семантику: BasePass может заменить значение directional sample mask, единицей или результатом `AOMultiBounce`. Поэтому overlay перехватывает ровно один store после чтения `GetMaterialAmbientOcclusion` и сохраняет raw значение.

Transport включается единым predicate:

```text
direct micro-shadow requested OR RGB indirect diffuse requested OR cone-aware indirect specular requested
```

И preflight, и HLSL guard, и capability manifest используют тот же predicate. Несовместимые permutations получают compile-time error. Обязательны `r.Substrate=0`, `r.AllowStaticLighting=0`; `r.GBufferDiffuseSampleOcclusion=0` и `1` поддерживаются overlay transport.

## 2. Direct Generic VNDF

Offline generator интегрирует isotropic GGX VNDF estimator. Канонический V2 artifact содержит две overlapping RGBA8 roughness banks с piecewise single-bank selection. Runtime выбирает одну bank, выполняет одну пространственную трилинейную интерполяцию и roughness interpolation внутри RGBA.

Direct Default Lit вычисляет multiplier отдельно для authored R1 и widened R2 и применяет его к полному specular contribution лоба. Diffuse имеет отдельную силу. Rect LTC и anisotropic overload не получают Generic VNDF term.

## 3. Indirect diffuse

Material visibility отделена от geometric visibility:

- Lumen diffuse: 3 exact sites;
- non-Lumen diffuse: 2 exact sites;
- skylight diffuse: 8 exact sites.

`RGB Interreflection` применяется непосредственно к RGB diffuse contribution. Screen AO/DFAO сохраняются в stock geometric path. Это предотвращает scalar multiply и повторное применение material AO.

## 4. Cone-aware capture/skylight IBL

Artifact CARD-09 имеет оси:

```text
NoV:              (i + 0.5) / 32
Roughness:        (j + 0.5) / 32
AdjustedCosCone:  k / 7
index:            NoV + 32 * (Roughness + 32 * Cone)
```

Каждая ячейка повторяет UE 5.7 `SystemTextures.cpp` PreIntegratedGF NDF-H estimator и добавляет strict reject `NoL > AdjustedCosCone`. A/B делятся на общий N. Output: `F0*A + F90*B`. Legacy single-scatter использует stock `F90=saturate(50*F0.g)`.

Runtime для каждого лоба:

```text
rawCos = sqrt(saturate(1 - MaterialVisibility))
c_i    = MLS_AdjustSpecularCone(rawCos, R_i, NoV)
AB_i   = LUT(NoV, R_i, c_i)
IBL    = (1-W)*Radiance(dir1,R1)*Response1 + W*Radiance(dir2,R2)*Response2
```

Material AO исключается из stock scalar GTSO для eligible cone path; screen AO остаётся. DFAO остаётся внутри `GatherRadiance`.

Независимый patch `ReflectionEnvironmentPixelShader.usf` применяет ту же material-visibility policy даже при выключенных paired IBL и cone LUT. В DirectOnly/Full eligible Default Lit material visibility удаляется из stock scalar GTSO, а screen/geometric AO сохраняется; Legacy и unsupported shading models остаются stock.

## 5. Energy conservation

UE `EnergyTerms.E` — multi-scatter directional albedo, не A/B. Exact cone-aware multi-scatter LUT не определена. Реализация масштабирует `EnergyTerms.E` поканальным `singleScatterCone/singleScatterOpen` ratio и помечает путь approximate. Для этой ветки F90 совпадает с `F0RGBToMicroOcclusion`: `saturate(50*max(F0))`.

## 6. Pipeline boundaries

Per-lobe pairing реализован только там, где плагин контролирует capture/skylight gather и response в одном shader. Lumen/SSR buffer не переносит надёжную lobe identity, поэтому сохраняет stock response authored roughness. Clear Coat, anisotropy, forward/mobile, Path Tracer и cooked runtime не входят в v0.14 coverage.

Текущий CARD-09 runtime — staging implementation. Три overlay-only раскладки packed LUT (scalar, `uint4`, eight cone planes) провалили bounded UE 5.7 SM6 compile-cost gate; поэтому настройка default-off, capability `StaticStorageCompileAdmitted=false`, а явный запрос отклоняется до remap. Production transport требует Texture3D/SRV с renderer binding и не может быть добавлен как неявный shader-overlay fallback.

## 7. Transactional overlay

Build ID включает engine version, patch version, настройки и digest generated artifacts. Новый overlay строится в отдельной content-addressed директории. Все requested anchors проверяются до сохранения; config и capability записываются после patching; stamp — последним. Ошибка не remap-ит новый overlay.
