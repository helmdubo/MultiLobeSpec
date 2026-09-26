# Material Surface Visibility / Microshadowing — audit v0.12.1

## Семантика карты

Запекаемый файл `_tex_ao` фактически является **Material Surface Visibility**:

- `1.0 / белый` — верхняя полусфера полностью видима;
- `0.0 / чёрный` — полностью закрыта;
- linear grayscale, `sRGB=false`;
- albedo/interreflection в карту не запекаются.

Для direct microshadowing используется raw visibility. Для indirect diffuse raw visibility нельзя просто умножать на irradiance: это создаёт «чёрные дыры». Модель CoD:WWII поднимает её с учётом diffuse albedo:

```hlsl
float3 InterreflectionVisibility(float Visibility, float3 DiffuseAlbedo)
{
    return Visibility / max(1.0 - DiffuseAlbedo * (1.0 - Visibility), 1e-3);
}
```

## Рекомендуемый 2K профиль: мягкая масса + crisp crevices

- Reconstruction: Auto / Periodic FFT для Wrap;
- Boundary: Wrap;
- DirectX normal: On;
- Reconstruct Z from RG: On;
- Remove mean slope: On;
- Use source RG normal for AO: On;
- Relief Scale: `0.60–0.80`, старт `0.65`;
- Max Slope: `6–8`;
- Radius: `24–32 px`, старт `28`;
- Slices: `12–16`, старт `16`;
- Steps per side: `10–12`, старт `10`;
- Distribution Power: `2.2–2.6`, старт `2.35`;
- Falloff Start: `0.55–0.65`, старт `0.60`;
- Strength: `1.0`;
- Output Power: `1.0`.

Силу physical map не следует подбирать через Strength/Gamma. Для более слабого рельефа уменьшайте `Relief Scale` или `Radius`.

## One-click assignment

Кнопка **Find + Assign AO From Current Selection**:

1. читает выбранные actors, Static Mesh assets или material instances;
2. фильтрует material instances по master filter;
3. для каждой normal texture с суффиксом `_tex_n` ищет рядом точный asset `<base>_tex_ao`;
4. сопоставляет слоты по числовому суффиксу parameter name (`Normal2 -> AO2`), иначе использует стабильный порядок;
5. предпочитает AO parameter с тем же Material Layer association/index;
6. модифицирует, сохраняет и логирует каждый material instance.

## Runtime audit

Текущий `GGX-aware heuristic` не является VNDF-методом из irradiance.ca Part 2. Он использует один half-vector и roughness ramp; в нём отсутствуют VNDF integration, `G2/G1`, нормализация cone/hemisphere и 3D LUT по Visibility/Roughness/NoL/NoV. До отдельной LUT реализации режим следует считать experimental.

Raw `_tex_ao` одновременно попадает в стандартные indirect AO paths UE и в direct microshadowing плагина. Это создаёт double/over-occlusion. Production fix: direct использует raw visibility, indirect diffuse — albedo-aware interreflection visibility, indirect specular — отдельную cone/BRDF integration.
