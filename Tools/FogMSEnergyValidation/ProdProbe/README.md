# ProdProbe — измерения FogMS через UE-MCP мост

Все скрипты работают против запущенного редактора с плагином `UE_MCP_Bridge` (ws://127.0.0.1:9877). Зависимости: Python 3 + numpy + Pillow, без WebSocket-библиотек (`uemcp.py` реализует протокол сам).

| Скрипт | Что делает |
|---|---|
| `uemcp.py` | JSON-RPC клиент: `cmd "<console>"`, `py "<code>"`, `pyfile <path>`, любой метод моста |
| `gpuprofile.py [label]` | `ProfileGPU` → разбор последнего блока лога, суммы по группам FogMS / native fog / Lumen |
| `measure.py <label> quality=.. iterations=.. cvar=..` | выставляет актор «FogMS - Live Box», профиль GPU, `FogMS.DumpSpatial`, скриншот → `measure/<label>/` |
| `resid_stats.py [glob]` | взвешенная по яркости невязка из `*.rgba32f` (метрика сходимости, устойчивее «худшей ячейки») |
| `lagtest2.py <label> <light> <off> <quality> <it> <warm>` | отклик решателя на выключение/включение источника по дампам |
| `runall.sh` | серия конфигураций warm/cold × итерации × направления |
| `compare.py <ref>` | PSNR скриншотов относительно эталонной конфигурации |
| `stoptest.ps1` / `hitchparse.py` | воспроизведение остановки камеры мышью и разбор `stat dumphitches` |
| `gen_perlin_worley.py N out.png` | бесшовный 3D Perlin-Worley атлас для Volume Texture (Nubis-раскладка каналов) |
| `build_plugin.sh <round>` | staging worktree → `RunUAT BuildPlugin -StrictIncludes` (пути и worktree захардкожены; править под себя) |

Переменная `FOGMS_LOG` указывает на активный лог редактора (при запуске с `-abslog`).
Редактор должен быть окном на переднем плане, иначе level viewport не рендерится и профили пустые.
