# FogMSEnergyValidation — инструменты проверки FogMS

Статус на 2026-09-27. Текущий набор проверок — [`ProdProbe/`](ProdProbe/README.md) (этап C, раунды 1–49: проверки в
редакторе через мост UE-MCP и сборщики ассетов `matedit_*.py`). Остальное в этой папке — инструменты этапов B1–B3 и
пакетов 4–6 (21–22.09.2026). Они сохранены рядом с кодом как доказательная база архивных отчётов
([`docs/archive/`](../../docs/archive/README.md)); с текущим кодом не перепроверялись. GPU-пробы этих этапов рассчитаны на
тогдашние пакеты и состояние тестовой карты; CPU-эталоны самостоятельны (Python + NumPy, без Unreal).

| Путь | Что это | Этап | Статус |
|---|---|---|---|
| `ProdProbe/` | проверки в редакторе, A/B, драйверы раундов, сборка ассетов | C | текущий |
| `reference.py` (ниже) | CPU-эталон энергии однородного слоя, варианты B1 | B1 | исторический (режим `World (Current Frame)` в коде остался как legacy overlay) |
| `transport_reference.py` + [`.md`](transport_reference.md) | CPU-эталон переноса по шести осям | B2 | эталон режима `Transport (B2)`; исторический документ |
| `angular_transport_reference.py` + [`.md`](angular_transport_reference.md) | CPU-эталон угловой конечнообъёмной схемы (квадратуры `gauss2x12`, `gauss3x16`, `cube1..4`) | B3 | исторический: 16/24 направления и квадратура по солнцу этапа C им не покрыты (не проверено) |
| `density_animation_contract.*`, `directional_motion_contract.*`, `directional_motion_probe.py`, `directional_motion_recovery.py` | контракты анимации плотности и направленного ветра | B3 | механизм в коде тот же (свойства FogMS\|Density Animation); упоминание пакета из 24 строк устарело: с S0 пакет 32 строки (`FogMSRender::BoxRowCount`) |
| `edge_flow_*.py` | Edge Flow (движение детали относительно ветра) | пакет 5 | исторический |
| `reconstruction_contract.py`, `test_view_integration.py`, `test_coherent_view_integration.py`, `test_anchored_view_integration.py`, `screen_scattering_contract.py` | реконструкция во фрокселях, View Integration, FSSS/SSFS | пакеты 4–6 | исторический; `r.FogMS.ViewIntegration` и `r.FogMS.SSFS` остались в коде как legacy (по умолчанию 0) |
| `AnimationProbe/`, `B2Probe/`, `B3Probe/`, `MotionProbe/`, `ContinuousProbe/`, `ScreenScattering/` | GPU-пробы в редакторе | B2 – пакет 6 | исторические |
| `Results/*.summary.json` | сводки GPU-проб B2/B3 (21.09) | B2/B3 | доказательства архивных отчётов |

## FogMS energy reference (B1)

`reference.py` is an independent **CPU reference**, not a GPU test and not a
measurement of the authored UE scene. Requires Python 3 and NumPy.

```powershell
python reference.py --output energy-reference.json
```

The slab has homogeneous extinction, isotropic scattering, no emission or
geometry, and unit isotropic radiance incident from both boundaries. Its width
is one; extinction equals total optical thickness. The discrete-ordinates
operator integrates the formal solution analytically over each cell, retaining
cell-average radiance. Each hemisphere uses Gauss-Legendre quadrature.

Assertions check the operator row balance, constant white-furnace solution at
albedo 1, and escaped plus absorbed flux against incident flux (2*pi). An albedo
0.9 case is repeated at 32/64/128 cells. These validate the independent reference;
they do not certify FogMS.

The output also evaluates B1-like variants: primary plus three added orders,
per-order damping .35/.5/1, and indirect-shadow blend 1/.5. These retain full
path coverage, accurate segment integration and common angular quadrature, so
they isolate the algebraic loss from order truncation/damping. They do not
reproduce B1's 5 m cutoff, 32-cubed sampling, geometry, Lumen, native fog mixture
or shader compositing. No result should be presented as the percentage error in
the user's screenshot.

For albedo 1 and optical thickness 1, reference center radiance is 1. The .35 / 3
added-order variant gives about .4362; even damping 1 with three orders gives
about .83344. Additional orders, domain coverage, conservative discretization and
native-source calibration require separate GPU validation before a physical
mode can be accepted. See [`docs/archive/FogMS_Energy_Audit.md`](../../docs/archive/FogMS_Energy_Audit.md).

Derivation background: [PBRT, Equation of Transfer](https://www.pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/The_Equation_of_Transfer).
