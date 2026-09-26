# ProdProbe — проверки FogMS в редакторе (этап C, раунды 1–49)

Статус на 2026-09-27: рабочий набор инструментов текущего кода. Хронология и результаты каждого прогона —
[`docs/history/FogMS_Prod_Report.md`](../../../docs/history/FogMS_Prod_Report.md); архитектура —
[`FogMS_HANDOVER.md`](../../../FogMS_HANDOVER.md); свойства, cvar и политика ассетов —
[`FogMS_UserGuide.md`](../../../FogMS_UserGuide.md).

## Что нужно

- Запущенный редактор UE 5.8.2 с установленным плагином и плагином проекта `UE_MCP_Bridge` (JSON-RPC 2.0 по WebSocket,
  `ws://127.0.0.1:9877`, методы `execute_command`, `execute_python`, `get_output_log` и др.). Мост в этот репозиторий
  не входит: он живёт в проекте заказчика.
- Python 3 + numpy + Pillow. `uemcp.py` реализует протокол WebSocket сам, без сторонних библиотек:
  `python uemcp.py cmd "stat unit"`, `python uemcp.py py "print(1)"`, `python uemcp.py pyfile <файл>`.
- `FOGMS_LOG` — путь к логу запущенного редактора: скрипты читают из него вывод консольных команд и ищут ошибки.
- Тестовая карта заказчика `/Game/FogMS_Test/FogMS_Box` с актёром `FogMS - Live Box`; в скриптах раундов 46–49 путь
  проекта задан константой `PROJECT` (правьте под себя). Карта и проект в репозиторий не входят.

## Правила прогона

- Редактор — общий с владельцем: прогон только в свободное окно. Скрипты раундов 34+ сначала снимают состояние
  (`snapshot` → `measure/*.json`), в конце восстанавливают его (`restore`) и никогда не сохраняют карту.
- Старые A/B-скрипты раундов 5–17 (`abfrozen.sh`, `abinject.sh`, `asyncab.sh`, `sweepab.sh`, `tierab.sh`, `refdump.sh`,
  `nobindless_test.sh`, `pie_test.py`, `soak.py`, `runall.sh`) возвращают Box в жёстко заданное состояние того раунда, а не
  в снимок; `boxstate.py save|restore` делает снимок свойств Box. Перед повторным использованием проверьте их восстановление.
- Вьюпорт должен рендериться: новые скрипты на время прогона ставят `bThrottleCPUWhenNotForeground = False`
  (старые требовали окна на переднем плане, `fg.ps1`). `HighResShot` и `FogMS.DumpSpatial` надёжнее в раскладке одного
  вьюпорта: орто-панели четырёхпанельной раскладки сбрасывают флаг последней сборки поля (раунд 47).
- Для A/B поля замораживают небо (`r.SkyLight.RealTimeReflectionCapture 0`) и сравнивают с шумовым полом повтора
  той же конфигурации.
- Кадры пишутся в `measure/` (не в git; на машине разработчика — junction на `D:/FogMS_ProbeFrames`), итоги — в
  `results/diagNN/*.json` и листы `*.png` (в git — только то, на что ссылается отчёт).

## Группы скриптов

| Группа | Скрипты | Назначение |
|---|---|---|
| Инфраструктура | `uemcp.py`, `gpuprofile.py`, `measure.py`, `boxstate.py`, `diag34lib.py`, `diag35lib.py`, `cloudproto_session.py` (`run_file`: запуск скрипта в редакторе через `runpy`), `setmode.py`, `hitchparse.py`, `stoptest.ps1`, `fg.ps1` | мост, профиль GPU, дамп поля (`FogMS.DumpSpatial`), снимок/восстановление, захват кадров |
| Ассеты (политика — `FogMS_UserGuide.md` §9) | `matedit_injection.py`, `matedit_density.py`, `matedit_cloud.py`, `matedit_weather.py`, `texgen/gen_weather_textures.py`, `gen_perlin_worley.py`; прототип P1 `cloudproto_material.py` | создают или правят материалы, текстуры и пресеты `/MultiLobeSpec/FogMS/...` в запущенном редакторе; идемпотентны, связи проверяют по T3D-экспорту, при ошибке не сохраняют |
| A/B раундов 5–32 | `abfrozen.sh`, `abinject.sh`, `asyncab.sh`, `sweepab.sh`, `tierab.sh`, `hybridab.sh`, `intervalab.sh`, `skyab.sh`, `lumenab.sh`, `trimsab.sh`, `refdump.sh`, `nobindless_test.sh`, `identity_check.sh`, `runall.sh` | квадратуры, доставка, async, интервал решения, источники неба, Lumen Bounce, проход 2, тождество поля |
| Сравнение и сценарии | `compare.py`, `fielddiff.py`, `resid_stats.py`, `lagtest.py`, `lagtest2.py`, `delivab.py`, `fieldcheck.py`, `fieldcheck2.py`, `mediumcheck.py`, `nightcheck.py`, `nightcmp.py`, `soak.py`, `pie_test.py`, `multibox_test.py`, `viseval.py`, `d1_blocks.py`, `d1_metrics.py`, `d2_move.py`, `d3_spot.py`, `d4_density.py` | PSNR, разница полей J, сходимость, ночь, стабильность, несколько Box, диагностика раунда 34 |
| Проверки раундов 35–49 | `d35_*.py`, `d36_*.py`, `d37_lobe.py`, `d38_*.py`, `d39_cloud.py`, `d40_fix.py`, `d41_sun.py`, `d45_p2.py`, `d46_host.py`, `d47_shadow.py`, `d48_weather.py`, `d49_sky.py` | по одному драйверу на раунд; критерии и подкоманды (`snapshot`, `matedit`, `copyback`, `check`, `cost`, `all`, `restore`, `summary`) — в docstring каждого файла |
| Без редактора | `fwd_lobe_check.py` | CPU-проверка узла прямого лепестка (среднее по сфере = 1, пол, тождества) |
| Сборка | `build_plugin.sh` | копия `build.sh` из внешней папки `.codex-build`: `RunUAT BuildPlugin -StrictIncludes`; путь к worktree захардкожен (старый worktree), правьте под себя |

Подкоманда `copyback` в `d46_host.py`, `d48_weather.py`, `d49_sky.py` копирует сохранённые в проекте `.uasset` в
локальную (игнорируемую git) папку `Content/` рабочей копии, чтобы следующая сборка плагина их содержала. С коммита
`083341c` эти файлы в репозиторий не коммитятся; указания «→ коммит» в старых docstring устарели.
