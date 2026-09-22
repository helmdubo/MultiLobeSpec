# Native viewport motion probe

Сравнивает следы native temporal reconstruction при повторяемом повороте камеры, dolly, боковом pan и изменении FOV. B2/B3 solver, свет, density и exposure не перенастраивает. Использует только текущую сцену FogMS_Box, не создаёт акторов и не сохраняет map. Анимация временно замораживается в текущей фазе; камера, FOV, animation controls и editor preferences восстанавливаются также при исключении.

`dolly` моделирует W/S вдоль направления взгляда при постоянном FOV; `pan` — A/D по горизонтальному right-вектору камеры, дистанция задаётся `pan_cm`. У каждого case свой путь до общей конечной камеры. Снимки снимаются **после остановки**, поэтому показывают успокоение изображения, а не измеряют мерцание на каждом кадре непрерывного движения. CVars, изменённые внешним A/B-скриптом, этот harness не восстанавливает: это обязанность внешнего владельца замера.

Текущий guard читает `Wind Speed` и `Edge Flow Speed`: нужен установленный Motion Package5 или новее. Старые receipts остаются читаемыми анализатором; отсутствие новых authored-полей может сделать сравнение старого и нового снимка неполным.

## Запуск

1. В `motion_config.json` укажите новый `id` для каждого прогона и абсолютный `evidence_root`. Для сравнения до/после сохраните одинаковые параметры и исходную камеру. Существующий id никогда не перезаписывается.
   Необязательный `reference_camera_file` указывает абсолютный путь к `original.json` выбранного baseline: все движения завершатся именно в его `camera`, а после прогона всё равно восстановится свежая исходная камера текущего запуска.
2. Запустите **скрытым внешним процессом** `node capture_worker.mjs <absolute-motion_config.json>`. Он подключается только к локальному UE_MCP_Bridge `ws://127.0.0.1:9877` и ждёт probe.
3. Через текущий bridge выполните `motion_probe.py`. Например JSON-RPC `run_python_file` с абсолютным `script_path` согласно контракту установленного bridge. Сам probe не запускает процессы.
4. До завершения не двигайте viewport. Отклонение от последней установленной probe камеры/FOV завершает прогон `FAILED / CAMERA_STATE_DIVERGED` и восстанавливает исходное состояние. Этот guard сам по себе не доказывает ручное вмешательство: причиной может быть также редактор или ошибка camera API. Переключение viewport/pilot отслеживается отдельно.
5. Дождитесь `receipt.json` со статусом `COMPLETED` и `restoration.ok=true`, затем выполните `python -P analyze_motion.py <receipt.json>`.

Для A/B: `python -P compare_motion.py <before-receipt.json> <after-receipt.json> --output <comparison.json>`. Проверяются camera, resolution, frozen phase и одинаковый motion protocol; интервалы capture timing показаны явно. Численное отношение ошибок не считается пользовательской визуальной приёмкой.

`force_game_view` по умолчанию `true`: probe фиксирует native Game View, чтобы editor bounds, grid и icons не меняли screenshot между прогонами. Исходный режим сохраняется до первой мутации, setter сразу проверяется через `editor_get_game_view`, каждый tick проверяет тот же режим, каждый capture записывает его в metadata. После прогона исходный bool восстанавливается и проверяется точно; несовпадение даёт явный `GAME_VIEW_RESTORE_MISMATCH` и `FAILED`. API проверяет Game View целиком, но не перечисляет индивидуальные пользовательские show flags. Старый receipt без game-view metadata допускается лишь как `LIMITED` A/B с ручной проверкой PNG; известное различие режима даёт `INVALID`. Для аварийного `action=restore` snapshot должен содержать game-view state: неизвестное исходное значение не угадывается.

Проверка exposure требует существующий `FogMS - Fixed Exposure` с manual exposure либо одинаковыми override min/max. Если сцена отличается, скрипт останавливается до изменений. Исходный снимок сохраняется в `original.json` до первой мутации. После аварии редактора можно выбрать исходный id и `action=restore`; worker для восстановления не нужен. Для штатной остановки используйте `action=stop` и повторно выполните entry point.

Повторное восстановление пишет отдельный `restore-<timestamp>.json`, сохраняя исходный failure receipt и изображения. Camera restoration проверяется с допуском 0.002 см/градуса на round-trip редакторского API; оба фактических значения записываются в receipt.

Все вызовы UE `Rotator` используют **именованные** `pitch=`, `yaw=`, `roll=`: positional порядок Unreal Python отличается от JSON `[pitch,yaw,roll]`. После каждого camera setter немедленно проверяется фактическое значение против запрошенного, чтобы ошибка конструктора не стала новой «ожидаемой» камерой.

## Что именно измеряется

Для каждого delay 1/2/4/8/32 probe независимо повторяет: стартовая позиция → 32 тёплых кадра → 24 кадра движения → исходная камера → capture request. Затем строит reference с 80 кадрами ожидания. Все изображения сделаны в одной конечной камере с одной замороженной фазой.

Захват `UE_MCP_Bridge.capture_screenshot(target=editor)` вызывает `FScreenshotRequest::RequestScreenshot` и обычный viewport Invalidate. **Не используются HighResShot, SceneCapture или automation screenshot**, меняющие условия рендера. Окно редактирования остаётся realtime. Worker не задаёт разрешение; изменение размера viewport определяется по PNG dimensions.

Bridge не экспортирует render-frame screenshot callback. Поэтому запись `capture_offset_interval=[request_offset,file_observed_offset]` — честный интервал в editor engine frames. Это **не доказательство screenshot ровно на GPU frame 1/2/4/8**. Engine может иметь render-thread lag. Скрипт никогда не помечает timing точным. При анализе до/после сопоставляйте интервалы, особенно ранние кадры. PNG/ack не заменяются следующим запросом: каждый motion повторяется отдельно. Evidence сохраняется и при failure.

Requests/acks передаются через уникальные неизменяемые `capture_requests/000001.json` и `capture_acks/000001.json`. Worker не читает обновляемый `receipt.json`: он следит за неизменяемым `finished.json`. Это устраняет Windows sharing violation при замене файла, который другой процесс читает. При новом запуске после завершения entry point перезагружает controller module, поэтому исправления harness не требуют перезапуска UE.

Анализатор проверяет hashes, completeness, одинаковую конечную камеру/фазу, restoration, размеры и timing records. Общий нормализованный ROI настраивается в config или `--roi x0 y0 x1 y1`. Он сообщает RGB relative RMS, ошибку средней яркости, contrast ratio и spatial-gradient ratio против warmed reference; сравнивает также warmed references разных серий как baseline drift. Это LDR screenshot metrics с приблизительным sRGB decoding, а не energy audit и не сегментация только тумана. Выбирайте ROI вручную так, чтобы он содержал исследуемую структуру, и сохраняйте его для A/B.

Без заранее заданных `acceptance_thresholds` результат называется `MEASURED`, **не PASS**. При необходимости можно задать `settled_requested_delay`, `rgb_relative_rms_max`, `mean_luma_relative_error_max`, `contrast_relative_error_max`; PASS тогда означает только выполнение этих численных бюджетов. Не используйте одинаковые threshold для разных камер/ROI без обоснования.

Проверка constant-density + moving-light в этом минимальном harness **не реализована**: authored lights остаются неизменны. Неизвестный case завершает preflight ошибкой вместо молчаливого пропуска. Её следует проводить отдельно, сохранив собственный baseline и restore receipt.

Ожидаемое время default прогона около 40–60 секунд при 30 FPS, плюс задержки сохранения PNG. Deadline 180 секунд — страховка, а не целевое время.
