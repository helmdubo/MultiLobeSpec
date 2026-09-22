# Receiver reconstruction: отдельный явный запуск

Требуется Package6 с `r.FogMS.Transport.TestReconstruction` и диагностическим проходом, вызывающим тот же `FogMS_TransportFieldIncident`, что штатный приёмник. Открываются уже существующие FogMS_Box и Pillar 1; подготовка/восстановление и шесть случаев совпадают с [Wall](gpu_wall_protocol.md). Ничего не запускается автоматически, новые уровни/акторы не создаются, карта не сохраняется.

Отдельный конфиг `gpu_wall_reconstruction_config.json` содержит `reconstruction_test=true`; задайте новый id, пример — `wall-reconstruction-repro-001`. В консоли/Output Log UE укажите **абсолютный путь своего checkout**:

```text
py "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/gpu_wall_reconstruction_probe.py"
```

Скрипт сохраняет исходный CVar, задаёт его 1 и принимает только `domain=transport_reconstruction`, `reconstructionTest=true`, `fluxDiagnosticsValid=false` и соответствующий layout. При отсутствии CVar диагностика прекращается до изменений сцены. Обычный capture не используется как fallback.

Ждите `gpu_wall-wall-reconstruction-repro-001.json`: `status=COMPLETED`, `restoration_ok=true`, шесть случаев с завершёнными barrier/measurement. Анализ вне UE:

```powershell
python "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/gpu_wall_reconstruction_analyze.py" "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/gpu_wall-wall-reconstruction-repro-001.json"
```

Результат записывается в `gpu_wall-wall-reconstruction-repro-001-reconstruction-analysis.json`, код выхода 0/2 — PASS/FAIL. Первые три slabs сохраняют TotalJ/residual, primaryJ/face bits, sigma_s/sigma_t. Четвёртый содержит максимум отдельно по каждому RGB-каналу среди восьми sample-позиций (`Offset={0.02,0.98}³`, координата `CellCoord+Offset-0.5`) и alpha=1 только при валидности всех восьми результатов. Это максимум нескольких samples, а не одна исходная RGB-выборка. Дампа потоков и flux-acceptance здесь нет.

Проверяются finite/positive значения, alpha=1 всех ячеек, свежесть кадров, hashes, коэффициенты и независимое совпадение face topology с положением стены. Для центра и центра+0.25dx в тени проверяются ячейки X≥N/2, для крайней половины — весь домен. Максимум reconstructed RGB должен быть ≤1e-6 пика соответствующего кадра без стены; контроль без стены обязан оставаться ярким. Source TotalJ проверяется отдельно, чтобы отличить утечку исходной сетки от утечки реконструкции. Это проверка барьера дискретной сетки; субъячеечную геометрическую точность или угловую точность сцены она не подтверждает.

Для остановки измените в **этом** конфиге `action=stop` и повторите entry-команду. Для восстановления после потери Python state используйте `action=restore`, `restore_receipt=gpu_wall-wall-reconstruction-repro-001.json` и ту же команду в той же карте. Проверяйте восстановление CVar, режимов, preferences и всех StaticMeshActor. Обычные numeric/wall анализаторы сохраняют отдельную численную и flux-проверку.
