# Numeric: явный запуск

Конфиг: `gpu_probe_config.json`. В нём задайте новый id; пример — `numeric-repro-001`. Существующая FogMS_Box должна быть открыта и активна; карту скрипт не создаёт и не сохраняет. Общие условия и восстановление — в [README](README.md).

В командной строке консоли/Output Log UE выполните **абсолютный путь** к entry. Замените путь примера на путь своего checkout:

```text
py "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/gpu_numerical_probe.py"
```

По умолчанию захватываются 11 случаев с TransportIterations=24 и TestGeometry=0: Test1 tau=0.1/1/4/8/16 с albedo=1; tau=4 с albedo=0/0.9; вакуум tau=0, albedo=0; Test2 tau=4 с вакуумом в средней половине X, albedo=0/0.9/1 и входящей яркостью только с отрицательной X-границы. Камера, свет, материалы и transform не изменяются.

Дождитесь рядом со скриптом `gpu_numerical-numeric-repro-001.json`: `status=COMPLETED`, `restoration_ok=true`, все 11 случаев содержат `barrier` и `measurement`. Между readback должны пройти минимум два новых render/producer frame; возраст источника ≤2 кадров. Наличие `ACCEPTED` в логе ещё не означает завершение.

В отдельном терминале, вне UE:

```powershell
python "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/analyze_gpu.py" "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/gpu_numerical-numeric-repro-001.json"
```

Выходной код 0 — PASS, 2 — FAIL; подробности в `gpu_numerical-numeric-repro-001-analysis.json`. Проверяются конечность/неотрицательность всех полей, коэффициенты, взаимность граней, J и primary Beer, остаток уравнения, четыре компонента потока и white-furnace J=1. CPU решается независимо до сходимости, максимум 128 итераций. Ошибка J/остатка/баланса ≤1%; это точность относительно шестинаправленного дискретного оператора.

При отсутствии необязательных `testBoundary/testGeometry/cellSizeCm` анализатор отмечает `PARTIAL_RECEIPT_INPUTS` и использует свежие live CVars и размер Box. Биты 0..5 primary alpha — открытые грани; биты 6..11 — missing surface-card flags, они отдельно учитываются и не превращаются в topology.

Для остановки: `action=stop` в `gpu_probe_config.json`, затем та же entry-команда `py`. Для восстановления: `action=restore`, `restore_receipt=gpu_numerical-numeric-repro-001.json`, затем та же команда. Требуются та же карта и тот же актор; snapshot берётся из указанного прогона.
