# Wall: явный запуск

Конфиг: `gpu_wall_config.json`; задайте новый id, пример — `wall-repro-001`. Требуются исправление крайней половины ячейки (Package5), существующая FogMS_Box и существующий `FogMS - Pillar 1` с нативным кубом. Общие условия и восстановление — в [README](README.md).

Для обычной проверки потока оставьте `reconstruction_test=false`. Если новый CVar реконструкции существует, его исходное значение сохраняется, на время этой серии задаётся 0 и затем восстанавливается. Старый Package5 без CVar допускается только для обычной серии. Диагностика sampler имеет [отдельный entry/config/analyzer](gpu_wall_reconstruction_protocol.md).

В консоли/Output Log UE укажите **абсолютный путь** своего checkout:

```text
py "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/gpu_wall_probe.py"
```

Скрипт временно скрывает остальные `StaticMeshActor`. Pillar 1 получает толщину 5 cm и размер, перекрывающий весь Box по YZ с запасом. Используются три координаты по X относительно Box: центр, центр+0.25dx, xMin+0.25dx. Каждой соответствует отдельный кадр с тем же кубом скрытым и видимым — всего шесть случаев. Настройки общие: Transport, 24 итерации, Test1, tau=4, albedo=0.9, отрицательная X-граница, TestGeometry=1. Новые акторы/материалы/уровни не создаются; карта не сохраняется.

Дождитесь `gpu_wall-wall-repro-001.json`: `status=COMPLETED`, `restoration_ok=true`, шесть случаев с barrier/measurement и ≥2 свежими render/producer frame между ними. Затем вне UE:

```powershell
python "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/gpu_wall_analyze.py" "C:/work/MultiLobeSpec/Tools/FogMSEnergyValidation/B2Probe/gpu_wall-wall-repro-001.json"
```

PASS требует ошибок J/остатка/баланса ≤1% относительно CPU и независимого совпадения low6 topology с ожидаемой геометрией стены. Яркость за стеной ≤1e-6 пика соответствующего кадра без стены. Стена внутри крайней половины ячейки должна полностью перекрыть отрицательную X-границу: нулевой входящий поток и чёрный домен. Открытость внешней грани не кодирует её освещённость, поэтому этот случай проверяется по заранее известному положению стены. Missing-card flags в этих диагностических случаях должны быть нулевыми. Тест ограничен шестинаправленным оператором и контролируемой чёрной стеной.

Перед изменениями сохраняются свежие transform/temporary-hidden всех `StaticMeshActor` и общие настройки. В нормальном процессе pose восстанавливается из исходного raw Transform; при отдельном recovery — через Actor setters. Для остановки задайте `action=stop` в `gpu_wall_config.json` и повторите entry-команду. Для recovery задайте `action=restore`, `restore_receipt=gpu_wall-wall-repro-001.json` и повторите её в той же карте. Проверяйте все restoration checks; восстановление не является численным PASS.
