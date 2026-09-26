# MultiLobeSpec v0.14 — Generic VNDF-Based Micro-Shadowing

## Самодостаточная спецификация реализации и приёмки для внешнего AI-агента

**Статус документа:** нормативный execution spec.
**Целевая база:** MultiLobeSpec v0.12.3 и последующие незавершённые изменения v0.13.0.
**Целевой движок:** Unreal Engine 5.7, legacy materials / legacy Default Lit.
**Главный результат:** корректная реализация **Generic VNDF-Based Micro-Shadowing** из Pablo Zurita, *Improving Direct Lighting Material Occlusion — Part 2*, с LUT-аппроксимацией, точной интеграцией в dual-lobe direct lighting и согласованной обработкой material visibility в indirect diffuse/specular.

---

## 1. Ключевое терминологическое решение

Переименование `VNDF Cone (generic)` в `Roughness-Aware Cone Overlap (Experimental)` относилось **только к старой эвристике v0.12.3**, потому что та функция не выполняла VNDF sampling, не использовала `G2/G1`, не делала cone/hemisphere normalization и не зависела от `N·V`.

Целевой алгоритм из Part 2 должен называться именно:

> **Generic VNDF-Based Micro-Shadowing (Isotropic LUT)**

Слово **Generic** здесь не означает «примерный» или «временный». Это термин автора статьи: модель учитывает GGX microsurface/VNDF, но ещё не учитывает конкретную micro-BRDF каждого микрофасета. Поэтому она является generic surface micro-shadowing term и, по замечанию автора, склонна к некоторому under-occlusion.

Старую cone-overlap эвристику разрешено:

1. оставить отдельным экспериментальным режимом;
2. скрыть под `Advanced/Legacy Experiments`;
3. удалить после визуального сравнения.

Нельзя называть её VNDF. Нельзя, наоборот, переименовывать настоящую VNDF LUT-модель в Cone Overlap.

---

## 2. Обязательный набор режимов UI

Заменить старый двухзначный `EMLSMicroShadow` на явный enum:

```cpp
enum class EMLSMicroShadowMode : uint8
{
    Off,
    ReferenceStep,
    ActivisionWWII,
    PZAnalytical,
    GenericVNDFIsotropicLUT,
    ConeOverlapExperimental
};
```

Display names:

| Enum | UI name | Назначение |
|---|---|---|
| `Off` | `Off` | Без material direct micro-shadowing |
| `ReferenceStep` | `Reference Step (Part 1)` | Жёсткая эталонная граница cone, только debug/reference |
| `ActivisionWWII` | `Activision / CoD:WWII` | Shipped squared NoL-cone formula |
| `PZAnalytical` | `PZ Analytical (Part 1)` | Аналитическая альтернатива Part 1, а не step reference |
| `GenericVNDFIsotropicLUT` | `Generic VNDF-Based (Isotropic LUT)` | Целевая production-модель Part 2 |
| `ConeOverlapExperimental` | `Cone Overlap (Experimental)` | Старая эвристика, без заявлений о VNDF |

### Важная коррекция документа v0.13.0

`Reference Step (Part 1)` **не является** аналитическим режимом Part 1. Это reference implementation:

```hlsl
step(sqrt(1.0 - Visibility), NoL)
```

Отдельный `PZ Analytical` обязан использовать формулу:

```hlsl
float MLS_Pow5(float X)
{
    float X2 = X * X;
    return X2 * X2 * X;
}

float MLS_MicroShadow_PZAnalytical(float Visibility, float NoL)
{
    if (Visibility <= 0.0) return 0.0;
    if (Visibility >= 1.0) return 1.0;

    float CosThetaPrime = sqrt(saturate(1.0 - MLS_Pow5(Visibility)));
    return pow(
        saturate(NoL / max(CosThetaPrime, 1e-6)),
        0.75 * rcp(Visibility));
}
```

---

## 3. Нормативная математика Generic VNDF-Based Micro-Shadowing

### 3.1. Семантика входной карты

`MaterialAO` в данном модуле трактуется как **cosine-weighted material surface visibility**:

```text
Visibility = 0 → верхняя полусфера полностью закрыта
Visibility = 1 → верхняя полусфера полностью открыта
```

Соответствующий cone threshold:

\[
\cos\theta=\sqrt{1-Visibility}.
\]

Для P2 Baker физический режим обязан сохранять:

```text
Strength = 1
Output Power = 1
sRGB = false
```

Иначе LUT и direct term получают неверную геометрическую семантику.

### 3.2. Coordinate convention

В reference generator:

```cpp
N = float3(0, 0, 1);
V = float3(sqrt(1 - NoV*NoV), 0, NoV);
L = float3(sqrt(1 - NoL*NoL) * cos(Phi),
           sqrt(1 - NoL*NoL) * sin(Phi),
           NoL);
```

`V` — view/incident direction для VNDF sampling в терминах UE.
`L` — light/outgoing direction.

### 3.3. Относительный азимут

Функция в общем случае зависит от relative azimuth между `V` и `L`, тогда как naive LUT статьи хранит только:

```text
Visibility, Roughness, NoL, NoV
```

Поэтому реализация обязана:

1. определить canonical azimuth convention;
2. записать её в LUT manifest;
3. проверить полный sweep `Phi ∈ [0, 2π)`;
4. не называть выбранную плоскость «worst-case», пока это не доказано численно.

Для первого article-compatible варианта использовать:

```text
Phi = 0, V и L копланарны и имеют одинаковый tangent-space azimuth.
```

Дополнительно вывести статистику по azimuth sweep:

```text
canonical error vs azimuth-average
canonical error vs min-over-azimuth
canonical error vs max-over-azimuth
```

Если canonical convention даёт неприемлемое расхождение, разрешается добавить второй LUT mode `Azimuth-Averaged`, но нельзя тихо менять смысл таблицы.

### 3.4. GGX roughness

Для isotropic GGX:

\[
\alpha=Roughness^2.
\]

Runtime и generator должны использовать одинаковое преобразование.

LUT roughness clamp:

```cpp
Roughness = clamp(Roughness, 0.1f, 1.0f);
```

Это соответствует предложенному в статье production-компромиссу и смягчает почти бинарный предел около нулевой roughness.

### 3.5. VNDF sample

Для каждой quasi-Monte-Carlo точки `Xi`:

```cpp
M = ImportanceSampleGGXVNDF_Isotropic(V, Alpha, Xi);
```

Предпочтительный sampler для буквального следования статье:

```text
Dupuy & Benyoub 2023 — Sampling Visible GGX Normals with Spherical Caps
```

Допустим statistically equivalent Heitz sampler, но только если unit test подтверждает одинаковое VNDF распределение и одинаковый интеграл в пределах допуска.

### 3.6. Height-correlated Smith weight

Использовать:

\[
\frac{G_2(V,L)}{G_1(V)}=
\frac{1+\Lambda(V)}{1+\Lambda(V)+\Lambda(L)}.
\]

Для isotropic GGX:

```cpp
float LambdaGGX(float NoX, float Alpha)
{
    NoX = max(NoX, 1e-6f);
    float Sin2 = max(1.0f - NoX * NoX, 0.0f);
    float Tan2 = Sin2 / (NoX * NoX);
    return 0.5f * (sqrt(1.0f + Alpha * Alpha * Tan2) - 1.0f);
}
```

Weight:

```cpp
float Weight = (1.0f + LambdaV)
             / max(1.0f + LambdaV + LambdaL, 1e-6f);
```

### 3.7. Per-sample terms

```cpp
float NdotM = saturate(dot(N, M));
float MdotL = saturate(dot(M, L));
float CosTheta = sqrt(saturate(1.0f - Visibility));

float Hemisphere = StepPositive(NdotM)
                 * StepPositive(MdotL)
                 * Weight;

float Cone = StepPositive(NdotM - CosTheta)
           * StepPositive(MdotL - CosTheta)
           * Weight;
```

`StepPositive(X)` обязан быть `1` только при `X > 0`; на точной границе — `0`, как в статье.

### 3.8. Surface integration

\[
M_g=
\frac{\sum_i Cone_i}
     {\sum_i Hemisphere_i}.
\]

```cpp
if (HemisphereSum <= 1e-8f)
{
    Result = 0.0f;
}
else
{
    Result = saturate(ConeSum / HemisphereSum);
}
```

Endpoints задаются аналитически до Monte Carlo:

```cpp
if (Visibility <= 0.0f) return 0.0f;
if (Visibility >= 1.0f) return 1.0f;
```

### 3.9. Early-out

Runtime:

```hlsl
if (2.0 * Visibility + NoL < 1.0)
{
    return 0.0;
}
```

Early-out применяется до LUT fetch. Он не должен применяться к Rect Light и к направлениям ниже геометрической полусферы, которые уже отбрасываются `DefaultLitBxDF`.

---

## 4. LUT generation

### 4.1. Generator architecture

Не генерировать LUT автоматически при каждом `Apply Changes`.

Добавить:

```text
Source/MultiLobeSpecEditor/Private/MLSMicroShadowLUTGenerator.h
Source/MultiLobeSpecEditor/Private/MLSMicroShadowLUTGenerator.cpp
Source/MultiLobeSpecEditor/Private/MLSMicroShadowLUTValidation.cpp
```

UI commands:

```text
Regenerate Generic VNDF LUT
Validate Generic VNDF LUT
Export Validation CSV
```

Canonical generated artifact:

```text
Resources/Generated/MLS_MicroShadowLUT.ush
Resources/Generated/MLS_MicroShadowLUT.manifest.json
```

Overlay builder копирует `.ush` в:

```text
Saved/MultiLobeSpec/Shaders_<hash>/Private/MLS_MicroShadowLUT.ush
```

### 4.2. Determinism

Generator обязан быть детерминированным:

```text
fixed seed
fixed sequence version
fixed axis mapping
fixed sampler implementation
fixed sample count
```

Использовать scrambled Sobol, Hammersley или другую low-discrepancy sequence. Обычный PRNG разрешён только как independent validation reference.

### 4.3. Base dimensions

Article-compatible baseline:

```text
Visibility: 16
NoL:        32
NoV:         8
Roughness:   4 channels = {0.1, 0.4, 0.7, 1.0}
Format:      RGBA8 UNORM
```

Но baseline нельзя принять только потому, что mean error низкий.

Отчёт v0.13.0 содержит `max |err| = 0.42` около низкой visibility/roughness и высокой `NoL`. Это **неприемлемо** для shipping LUT.

### 4.4. Warped axes

Сначала сохранить компактный 16×32×8 layout, но применить non-linear axes:

```cpp
// More samples near Visibility = 0
Visibility = TV * TV;
RuntimeTV  = sqrt(Visibility);

// More samples near NoL = 1
NoL       = 1.0f - (1.0f - TL) * (1.0f - TL);
RuntimeTL = 1.0f - sqrt(1.0f - NoL);

// More samples near grazing NoV
NoV       = TVew * TVew;
RuntimeTVew = sqrt(NoV);
```

Generator должен сравнить linear и warped layouts. Выбрать layout по acceptance metrics, а не по субъективному виду.

Если 16×32×8 после warping всё ещё не проходит max/p99 threshold, перейти на:

```text
32 × 48 × 12
```

или на piecewise boundary-aware coordinate. Изменение разрешения обязательно отражается в manifest и runtime sampler.

### 4.5. Roughness interpolation

Сравнить два варианта:

```text
linear in perceptual roughness R
linear in alpha = R²
```

Выбрать вариант с меньшей p95/max error. Не предполагать линейность без теста.

### 4.6. Sample count

Canonical bake:

```text
8192 QMC samples per cell/channel
```

4096 допускается только если validation подтверждает thresholds ниже.

Fresh independent reference:

```text
65536 samples for targeted worst-case cells
16384 samples for random sweep
```

### 4.7. Storage in shader include

Поскольку текущий overlay не имеет безопасного C++ binding для нового `Texture3D`, использовать packed static data:

```hlsl
static const uint MLS_VNDF_LUT_PACKED[NumTexels] = { ... };
```

Один `uint` хранит RGBA8.

Обязательные проверки:

```text
shader compile time
shader binary growth
constant/read-only data limit
all target SM6 permutations
```

Если static array вызывает compile/binary regression, следующий вариант — настоящее `Texture3D` с явным renderer parameter plumbing. Нельзя тихо падать обратно на cone overlap.

### 4.8. Manifest

Пример:

```json
{
  "algorithm": "GenericVNDFBasedMicroShadowing",
  "distribution": "IsotropicGGX",
  "vndfSampler": "DupuyBenyoub2023",
  "smithCorrelation": "HeightCorrelated",
  "visibilityWeighting": "CosineWeightedProjectedSolidAngle",
  "dimensions": [16, 32, 8],
  "roughnessNodes": [0.1, 0.4, 0.7, 1.0],
  "axisWarp": {
    "visibility": "square",
    "NoL": "oneMinusSquare",
    "NoV": "square"
  },
  "relativeAzimuth": "coplanar_same_azimuth",
  "samplesPerCell": 8192,
  "sequence": "OwenScrambledSobol",
  "seed": 1337,
  "quantization": "RGBA8_UNORM",
  "generatorVersion": 1,
  "dataSHA256": "..."
}
```

---

## 5. Runtime LUT sampling

Generated include должен предоставлять:

```hlsl
float MLS_SampleGenericVNDFMicroShadow(
    float Visibility,
    float Roughness,
    float NoL,
    float NoV);
```

Порядок:

```hlsl
Visibility = saturate(Visibility);
NoL = saturate(NoL);
NoV = saturate(NoV);

if (Visibility <= 0.0) return 0.0;
if (Visibility >= 1.0) return 1.0;
if (2.0 * Visibility + NoL < 1.0) return 0.0;

Roughness = clamp(Roughness, 0.1, 1.0);
```

Затем:

1. warp runtime coordinates;
2. выполнить manual trilinear fetch 8 соседних texels;
3. распаковать RGBA8;
4. выбрать два roughness nodes;
5. интерполировать по выбранной roughness coordinate;
6. `saturate` результата.

Нельзя использовать nearest filtering.

---

## 6. Direct-light integration

### 6.1. Где интегрировать

Не умножать уже готовый dual-lobe результат одним общим multiplier.

Целевой путь — exact call-site integration внутри legacy `DefaultLitBxDF`, до окончательного суммирования лобов.

Добавить специализированную функцию:

```hlsl
struct FMLSDualSpecular
{
    float3 Lobe1;
    float3 Lobe2;
    float  Weight;
};
```

Оценивать:

```hlsl
S1 = SpecularGGX_Orig(R1, ...);
S2 = SpecularGGX_Orig(R2, ...);
```

Per-lobe energy compensation остаётся внутри каждого лоба:

```hlsl
S1 *= EnergyComp1;
S2 *= EnergyComp2;
```

Затем:

```hlsl
M1 = MLS_SampleGenericVNDFMicroShadow(Visibility, R1, NoL, NoV);
M2 = MLS_SampleGenericVNDFMicroShadow(Visibility, R2, NoL, NoV);

Specular = (1-W) * S1 * M1 + W * S2 * M2;
```

### 6.2. Запрет side-channel store

Не использовать скрытые global/thread-local HLSL variables, которые wrapper заполняет, а `DefaultLitBxDF` затем читает.

Формула ratio:

\[
\frac{(1-w)S_1M_1+wS_2M_2}{(1-w)S_1+wS_2}
\]

алгебраически может восстановить exact multiplier, но хрупка при:

```text
нескольких вызовах wrapper
RGB denominator около нуля
разных energy stages
анизотропных overloads
изменении call order Epic
```

Допускается только как временный fallback, явно логируемый как `Approximate recomposition`. Production path обязан владеть `S1` и `S2` в одном call-site.

### 6.3. Diffuse

В режиме `GenericVNDFIsotropicLUT` generic surface term должен применяться и к diffuse:

```hlsl
MDiffuse = MLS_SampleGenericVNDFMicroShadow(
    Visibility,
    GBuffer.Roughness,
    NoL,
    NoV);

Lighting.Diffuse *= MDiffuse;
```

Это ближе к Part 2, где generic term действует на microsurface в целом. Автор отдельно предупреждает, что term ещё не micro-BRDF-aware и поэтому under-occludes, но это не основание заменять diffuse на CoD formula без отдельного режима.

Если нужен художественный гибрид, добавить отдельный enum:

```text
WWII Diffuse + VNDF Specular (Hybrid)
```

Он не должен называться exact Part 2.

### 6.4. Strength

```text
Diffuse Strength  = 1.0 default
Specular Strength = 1.0 default
```

Значения ниже 1 — art-directed override. UI должен это писать.

### 6.5. Supported lights

Обязательная поддержка:

```text
Directional
Point
Spot
IES point/spot
```

Punctual lights с source radius используют representative direction UE и помечаются как approximation.

### 6.6. Rect Light

Для `IsRectLight(AreaLight)` Generic VNDF direct micro-shadowing не применяется:

```hlsl
if (!IsRectLight(AreaLight))
{
    ...
}
```

Area lights без representative point названы в Part 2 отдельной нерешённой задачей. Нельзя применять `AreaLight.NoL` к уже интегрированному LTC и считать это article-compliant.

### 6.7. Anisotropy

Текущий LUT является isotropic compromise.

При active anisotropy:

```text
fallback = PZ Analytical
```

либо `Off`, выбираемо настройкой. Нельзя без валидации подставлять average roughness и утверждать anisotropic VNDF support.

Будущая anisotropic версия требует дополнительных параметров/проекции roughness и отдельной validation matrix.

### 6.8. MegaLights

Если MegaLights включён и bypass-ит legacy `DefaultLitBxDF`, плагин обязан:

1. либо патчить MegaLights material/direct-light evaluation;
2. либо вывести явный capability warning;
3. не сообщать `Generic VNDF active` для неподдержанного path.

---

## 7. Indirect diffuse — полная WWII-подобная интерпретация

`AO = Direct Only` полезен как diagnostic mode, но не является полной WWII semantics.

Raw material visibility должна использоваться в indirect diffuse через RGB interreflection:

\[
V_{indirect}(\rho)=
\frac{V}{1-\rho(1-V)}.
\]

```hlsl
float3 MLS_InterreflectionVisibility(
    float Visibility,
    float3 DiffuseAlbedo)
{
    return saturate(
        Visibility /
        max(1.0 - DiffuseAlbedo * (1.0 - Visibility), 1e-4));
}
```

Использовать `GBuffer.DiffuseColor`, не `BaseColor` у металлов.

### 7.1. Запрещённый fallback

Константа:

```hlsl
MLS_INTERREFLECT_ALBEDO 0.35
```

не считается завершённой реализацией. Она допустима только как явно названный debug fallback для path, где diffuse albedo действительно недоступен.

### 7.2. Lumen Screen Probe Gather

Нужно:

1. отключить штатное scalar MaterialAO/AOMultiBounce только для material visibility;
2. не отключать Screen AO, short-range AO и geometric occlusion;
3. применить `MLS_InterreflectionVisibility(Material.MaterialAO, Material.GBufferData.DiffuseColor)` к RGB diffuse indirect;
4. не применять correction к specular indirect;
5. сохранить directional sample occlusion только если её семантика не дублирует material visibility; иначе отключить и задокументировать.

Patch success только при точном expected count. `1/3` и `2/3` — hard failure, исходный shader file не изменяется.

### 7.3. Skylight diffuse

`SkyLightingDiffuseShared.ush` должен разделять:

```text
Material Visibility
Screen/DFAO/Geometry Visibility
```

Нельзя смешать их в один scalar через `min/mul`, а затем пытаться сделать RGB correction.

Примерная архитектура:

```hlsl
float3 MaterialIndirectVis =
    MLS_InterreflectionVisibility(GBuffer.GBufferAO, GBuffer.DiffuseColor);

float GeometryVis = ... ScreenAO / DFAO ...;

float3 FinalVis = MaterialIndirectVis * GeometryVis;
```

Для combine mode Minimum:

```hlsl
float3 FinalVis = min(MaterialIndirectVis, GeometryVis.xxx);
```

### 7.4. Non-Lumen indirect composite

Не использовать scalar `FinalAmbientOcclusion` для цветной material interreflection, поскольку этот multiply может затронуть уже накопленный scene color.

Material visibility удаляется из scalar multiply и применяется непосредственно к RGB indirect diffuse contribution.

### 7.5. Diagnostic modes

UI:

```text
Indirect Material Visibility:
- Direct Only (diagnostic)
- Raw Scalar AO (legacy)
- RGB Interreflection (WWII-like, default)
```

Default после завершения:

```text
RGB Interreflection
```

---

## 8. Indirect specular — обязательная отдельная задача

Generic VNDF direct micro-shadowing не решает indirect specular.

Для соответствия Material Advances in CoD:WWII требуется cone-aware Environment BRDF:

1. интегрировать specular BRDF только по directions внутри visibility cone;
2. отклонять samples снаружи cone;
3. хранить cone angle/visibility как третье измерение EnvBRDF LUT;
4. применять response отдельно для каждого dual-lobe roughness.

Baseline CoD representation:

```text
32 × 32 × 8
axes: NoV × Roughness × Cone/Visibility
output: scale + bias against F0
```

### 8.1. Cone adjustment

Чтобы не переоценивать occlusion для glossy/grazing cases, реализовать documented adjustment:

```hlsl
float MLS_AdjustSpecularCone(
    float CosCone,
    float Roughness,
    float NoV)
{
    float Gloss = 1.0 - Roughness;
    Gloss = saturate(Gloss * 1.5);

    float Gloss2 = Gloss * Gloss;
    float Gloss4 = Gloss2 * Gloss2;
    float Gloss8 = Gloss4 * Gloss4;

    float OneMinusNoV2 = (1.0 - NoV) * (1.0 - NoV);
    float OneMinusNoV4 = OneMinusNoV2 * OneMinusNoV2;

    return lerp(
        0.0,
        CosCone,
        (1.0 - Gloss8) * (1.0 - OneMinusNoV4));
}
```

### 8.2. Dual-lobe IBL

Для каждого лоба:

```hlsl
Response1 = ConeAwareEnvBRDF(F0, R1, NoV, Visibility);
Response2 = ConeAwareEnvBRDF(F0, R2, NoV, Visibility);

IBL = (1-W) * Radiance1 * Response1
    + W     * Radiance2 * Response2;
```

Нельзя перемножать смесь radiance на смесь response.

### 8.3. Lumen/SSR limitation

Если lobe identity не проходит через stochastic reflection path, capability report обязан честно показывать:

```text
Lumen indirect specular cone pairing: unsupported/approximate
```

Нельзя заявлять полное CoD-style indirect specular coverage, пока radiance и response не спарены по лобу.

---

## 9. Patcher safety

### 9.1. Exact anchors

Каждая requested feature имеет exact expected count.

```text
count != expected → feature unavailable → build overlay fails transactionally
```

Никаких `warning + сохранить частично изменённый файл`.

### 9.2. Transactional overlay

1. собрать новый versioned overlay directory;
2. применить все patches;
3. проверить counts;
4. записать config/LUT/manifest;
5. выполнить smoke compile;
6. только затем remap `/Engine`;
7. предыдущий overlay сохранить до успешного перехода.

### 9.3. Capability manifest

Пример:

```json
{
  "GenericVNDFDirect_DefaultLit": true,
  "GenericVNDFDirect_MegaLights": false,
  "GenericVNDF_RectLight": false,
  "IndirectDiffuse_LumenRGBInterreflection": true,
  "IndirectDiffuse_SkylightRGBInterreflection": true,
  "IndirectSpecular_ConeEnvBRDF": true,
  "DualLobe_PerLobeMicroShadow": true,
  "Lumen_PerLobeConePairing": false
}
```

UI должен показывать этот status, а не только выбранный enum.

---

## 10. Validation harness

### 10.1. Deliverable

В репозитории должны быть реальные исходники и runnable command, а не только текстовый отчёт:

```text
Tools/MLSMicroShadowValidation/
    README.md
    generator/test source
    reference CSV
    generated report JSON
```

### 10.2. Analytical invariants

Для Monte Carlo integrator и runtime LUT:

```text
M(Visibility=0) = 0 exactly
M(Visibility=1) = 1 exactly
0 <= M <= 1
```

Reference integrator при `Roughness → 0` должен сходиться к:

```hlsl
step(sqrt(1 - Visibility), NoL)
```

Runtime LUT может clamp-ить roughness к 0.1, поэтому отдельно тестируются:

```text
integrator smooth limit
runtime clamped LUT behavior
```

Нельзя выдавать тест `r=0.1` за доказательство `r→0` limit.

### 10.3. Error metrics

Random sweep минимум 10000 точек против fresh MC reference.

Acceptance:

```text
mean absolute error <= 0.012
p95 absolute error  <= 0.035
p99 absolute error  <= 0.070
max absolute error  <= 0.12
```

Отдельный near-boundary set:

```text
Visibility ∈ [0.01, 0.20]
Roughness  ∈ [0.10, 0.20]
NoL        ∈ [0.75, 1.00]
```

Для него:

```text
p95 <= 0.06
max <= 0.15
```

`max error = 0.42` является release blocker.

### 10.4. Quantization tests

Сравнить:

```text
float LUT vs MC
RGBA8 LUT vs float LUT
runtime trilinear vs CPU trilinear
```

RGBA8 добавочная mean error должна быть:

```text
<= 0.003
```

### 10.5. Monotonicity

Не принудительно монотонизировать данные до понимания причины.

Проверять по Visibility и NoL с tolerance, но сохранять raw report. Если обнаружены реальные non-monotonic regions reference integral, не «чинить» их пост-фильтром без обоснования.

### 10.6. Azimuth tests

Минимум:

```text
Phi = 0°, 45°, 90°, 135°, 180°
```

и continuous random sweep.

Report должен показать ошибку canonical 4D LUT относительно полного 5D reference.

### 10.7. Renderer tests

Матрица:

| Path | Tests |
|---|---|
| Directional | all modes, moving light |
| Point | moving through crevice |
| Spot/IES | cone sweep |
| Rect | must remain unmodified |
| Anisotropy | explicit fallback |
| Dual lobe | W=0, W=1, intermediate |
| Lumen GI | direct-only vs RGB interreflection |
| Skylight | no black-hole burn |
| Reflection Capture | cone-aware EnvBRDF |
| SSR/Lumen reflections | capability limitations visible |
| MegaLights | patched or explicit warning |

### 10.8. Image test scenes

1. flat plane, constant normal, synthetic visibility ramp;
2. sinusoidal heightfield;
3. brick wall material;
4. rubble/cracked brick;
5. skin pores;
6. roughness split: left 0.1, right 0.8;
7. dual-lobe split;
8. white furnace indirect diffuse;
9. saturated red brick interreflection;
10. glossy grazing reflection halo test.

Exposure and tonemapper fixed across A/B.

---

## 11. Performance budgets

### Direct runtime

Per punctual light/pixel:

```text
one early-out
one packed 3D trilinear lookup per required roughness
```

Dual-lobe specular requires two LUT evaluations unless `R1 == R2` or `W` reaches endpoint.

Optimizations:

```hlsl
if (W <= 1e-4) evaluate only R1;
if (W >= 1 - 1e-4) evaluate only R2;
if (abs(R2 - R1) < epsilon) reuse M1;
```

Compile-time mode selection must eliminate unused algorithms.

### LUT generation

Use `ParallelFor` over cells and deterministic QMC sequence. UI must show progress/cancel.

### Shader size

Report before/after:

```text
compile time
DDC size
shader binary size
instruction count
read-only data size
```

---

## 12. Documentation requirements

Обновить:

```text
README_RU.md
CHANGELOG.md
MICROVISIBILITY_ARCHITECTURE_RU.md
SUPPORT_MATRIX_RU.md
VALIDATION_RU.md
```

README содержит только текущее состояние.

Обязательные честные формулировки:

```text
Generic VNDF-Based Micro-Shadowing is micro-BRDF-agnostic and may under-occlude.
The current LUT is isotropic.
Rect LTC is excluded.
Anisotropic materials use an explicit fallback.
Direct-only AO is diagnostic, not WWII indirect semantics.
Cone-aware indirect specular is a separate implementation from direct VNDF micro-shadowing.
```

---

## 13. Task cards для внешнего AI-агента

### CARD-01 — Terminology and enum split

**Input:** v0.12.3/v0.13 source.
**Output:** six explicit modes; real VNDF mode named correctly.
**Blocker:** no mode may claim VNDF without LUT/integrator path.

### CARD-02 — Shared VNDF reference math

**Output:** deterministic CPU implementation of isotropic GGX VNDF, correlated Smith weight, cone/hemisphere integral.
**Tests:** endpoints, smooth limit, independent sampler cross-check.

### CARD-03 — LUT generator and manifest

**Output:** generated packed RGBA8 `.ush`, manifest, CSV/JSON report.
**Blocker:** error thresholds, especially no `max=0.42`.

### CARD-04 — Runtime LUT sampler

**Output:** exact manual trilinear and roughness interpolation matching CPU.
**Tests:** CPU/GPU sample parity.

### CARD-05 — Direct DefaultLit integration

**Output:** Generic VNDF applied to diffuse and exact per-lobe specular.
**Blocker:** no shared multiplier based only on R1; no hidden side-channel state in final path.

### CARD-06 — Light-path exclusions/fallbacks

**Output:** Rect excluded; anisotropy explicit fallback; MegaLights capability status.

### CARD-07 — Lumen indirect RGB interreflection

**Output:** per-pixel diffuse albedo correction, exact anchor counts, geometric AO retained.

### CARD-08 — Skylight/non-Lumen indirect RGB interreflection

**Output:** material visibility separated from Screen/DFAO; no scalar colorless approximation.

### CARD-09 — Cone-aware indirect specular

**Output:** 32×32×8-style cone EnvBRDF, per-lobe pairing for captures/skylight, limitations for Lumen explicitly reported.

### CARD-10 — Transactional patcher and capability report

**Output:** no partial files; exact counts; previous overlay preserved.

### CARD-11 — Validation suite

**Output:** runnable harness, reports, image matrix, performance numbers.

### CARD-12 — Documentation cleanup

**Output:** no contradictions, exact distinction among Reference, WWII, PZ Analytical, Generic VNDF and Cone Overlap.

---

## 14. Definition of Done

Релиз нельзя назвать complete, пока одновременно не выполнено:

- [ ] UI содержит отдельный настоящий `Generic VNDF-Based (Isotropic LUT)`.
- [ ] Старый ConeOverlap не называется VNDF.
- [ ] PZ Analytical реализован отдельно от Reference Step.
- [ ] LUT создан настоящим VNDF sampling с `G2/G1` и normalization.
- [ ] `cosθ = sqrt(1-Visibility)` используется во всех cosine-weighted paths.
- [ ] LUT error проходит thresholds; max 0.42 устранён.
- [ ] Relative azimuth convention задокументирована и проверена.
- [ ] Generic VNDF term применяется к diffuse в exact mode.
- [ ] Specular micro-shadowing применяется отдельно к R1/R2 до смешивания.
- [ ] Rect LTC исключён.
- [ ] Anisotropy имеет честный fallback.
- [ ] Lumen patch требует exact count и не оставляет partial shader.
- [ ] Indirect diffuse использует RGB per-pixel albedo interreflection.
- [ ] Skylight и non-Lumen paths не используют material visibility как black scalar multiplier.
- [ ] Cone-aware indirect specular реализован и спарен с dual-lobe IBL хотя бы для captures/skylight.
- [ ] Capability report честно показывает неподдержанные Lumen/MegaLights paths.
- [ ] Validation harness находится в архиве и запускается одной командой.
- [ ] Документация не выдаёт diagnostic Direct Only за WWII semantics.
- [ ] Overlay build транзакционен.

---

## 15. Материалы, которые агент обязан считать source of truth

1. Pablo Zurita — *Improving Direct Lighting Material Occlusion — Part 1*.
2. Pablo Zurita — *Improving Direct Lighting Material Occlusion — Part 2*.
3. Danny Chan / Sledgehammer Games — *Material Advances in Call of Duty: WWII*.
4. Dupuy & Benyoub — *Sampling Visible GGX Normals with Spherical Caps*.
5. Текущие UE 5.7 shader sources, фактически находящиеся в проекте пользователя; не полагаться на память о другой версии Unreal.

При конфликте между README плагина и фактическим кодом приоритет имеет код + numerical validation. При конфликте между названием режима и математикой приоритет имеет математика, а название исправляется.
