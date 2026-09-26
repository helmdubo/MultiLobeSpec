# FogMS — установка для проверки в проекте

## Текущая установка — Crash compatibility, 2026-09-21

`E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Crash_20260921/Package3`, StrictIncludes без unity/PCH, 72 сверенных файла. В раннем FogMSRender добавлена защита конкретного native D3D12/BindlessAll RT heap-restore сбоя UE5.8.2: CPU RHI translation выполняется последовательно, default context подготавливается через публичный RHI. Свет, RT и алгоритм FogMS сохранены. Настройка действует автоматически для этого процесса, в том числе при FogMS Off. Engine не изменён. Backup: `Saved/FogMS_Backups/FogMS_Crash_20260921_103122`. Проверки и ограничения — `FogMS_Crash_Report.md`; итоговые runtime receipts в каталоге Crash.

## Предыдущая установка — Stability, 2026-09-21

`E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Stability_20260921/Package1`, StrictIncludes, 70 сверенных файлов. Стабильная выборка indirect Jitter0/N8, аналитический history clip и conservative-depth validity; прежняя карта, View Distance50000cm/Z208, TLV EndDistance50000cm. Пользовательские density=.6, SpatialStrength=.5/Distance2000cm, IndirectSteps32 сохранены. Launcher `FogMS_Box_Test.cmd` теперь вызывает `Saved/FogMS/start_box.py`, прогревая native view перед Apply. Первый immediate startup не прошёл, два отложенных запуска прошли; подробности и пределы — `FogMS_Stability_Report.md`. Backup: `Saved/FogMS_Backups/FogMS_Stability_20260921_050523`. Итоговая сверка — `delivery-receipt.json` каталога Stability. Commit/push не выполнялись.

## Предыдущая установка — Fields, 2026-09-21

Пакет `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Fields_20260921/Package5`, 70 файлов, установлен после StrictIncludes. В прежней FogMS_Box включены Filtered Sun Shadow / Sigma=100cm и Spatial (Experimental) / Strength=.35 / Distance=500cm. Отчёт и пределы: `FogMS_Fields_Report.md`; пошаговый A/B: `FogMS_Fields_Test_Protocol.md`. Резервная копия прежнего плагина и авторской сцены: `Saved/FogMS_Backups/FogMS_Fields_20260921_021821`. Финальная проверка запуска, Engine hashes, package hashes и сохранения авторских параметров записывается в `delivery-receipt.json` каталога Fields. Это первый A2 с источником из camera history, не завершённый B. Приёмка владельцем ожидается; commit/push не выполнялись.

## Предыдущая установка — A1e, 2026-09-21

Пакет `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_A1e_20260921/Package` установлен в основной MimirHead; 60 файлов сверены SHA256. StrictIncludes и D3D12/SM6 завершены. В прежней `FogMS_Box` включены World Aligned Texture / World Texture Size=2000cm и Cast Sun Shadow / Steps=64. Прежние density/Box/sun/scattering/indirect controls сохранены (`main-ready.json`: authored_controls_preserved=true, saved=true). Основной Editor открыт; финальный кадр просмотрен. Новых уровней нет.

Backup: `Saved/FogMS_Backups/FogMS_A1e_20260921_005731`. Инструкция в проекте — `FogMS_A1e_README.md`, launcher прежний. Протокол, GPU цена и пределы — `FogMS_A1e_Report.md`; native cloud shadow reuse — `FogMS_NativeCloudShadows_Research.md`. Engine shader hashes 6/6 неизменны. Визуальная приёмка заказчика ожидается; commit/push не выполнялись.

2026-09-20. Выполнено по прямой просьбе заказчика «Сделай чтобы я смог проверить в проекте новый fog».

## Предыдущая установка — TextureDensity + A1b + экспериментальный A1c

2026-09-20. Пакет `.codex-build/FogMS_A1c_20260920/Package`, StrictIncludes 26 actions PASS; **52 package-owned файла** проверены SHA256. Backup: `Saved/FogMS_Backups/FogMS_A1c_20260920_222110`. Прежний `/Game/FogMS_Test/FogMS_Box` открыт и сохранён с Indirect Shadowing=true, `main-ready.json`: ready/saved/accepted_controls_preserved=true. Параметры плотности, Perlin, трансформация и A1b сохранены. Launcher прежний `FogMS_Box_Test.cmd`, startup включает preview по сохранённому флагу. Новых рабочих уровней нет; Engine не менялся. Результаты, специальные renderer settings, ограничения и откат — `FogMS_A1c_Report.md`. Приёмка заказчиком ожидается; commit/push не выполнялись.

## Предыдущая установка — TextureDensity + A1b

2026-09-20. После визуального принятия Perlin-плотности установлен следующий срез A1b: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_A1b_20260920\Package`. StrictIncludes PASS; 49 package-owned файлов сверены SHA256. Backup: `Saved/FogMS_Backups/FogMS_A1b_20260920_202839` в основном проекте. Engine/Perlin/DefaultEngine.ini/старая карта A1 сохранены.

Новых уровней не создано. В существующем `/Game/FogMS_Test/FogMS_Box` сохранён Mode=Octaves, ExtraOctaves=2, a/b/c=.5; принятые density/fog/exposure/transform сохранены. Запущен основной Editor с D3D12/SM6/BindlessAll/HWRT; A/B кадры просмотрены, receipt ready/saved=true. Выбрать **FogMS - Live Box → FogMS → Scattering**; **Off (A1 Only)** возвращает предыдущий результат вживую. Полный протокол — `FogMS_A1b_Report.md` и проектный `FogMS_Box_README.md`. Окончательная визуальная приёмка A1b — за заказчиком.

## Предыдущая установка — live Box (исторический протокол)

После проверки A1 заказчик разрешил следующий срез с движением без Apply, затем подтвердил закрытие редактора для установки. Установлен свежий `FogMS_Box_20260920_1810/Package3`; StrictIncludes без PCH/unity: SUCCESS, 25 actions. Все **55** package-файлов сверены SHA-256. Предыдущий плагин сохранён в `Saved/FogMS_Backups/FogMS_Box_20260920_1810/MultiLobeSpec` внутри проекта.

Добавлены отдельная `/Game/FogMS_Test/FogMS_Box`, launcher `FogMS_Box_Test.cmd`, `FogMS_Box_README.md` и `Saved/FogMS/enable_box.py`. Launcher включает D3D12/SM6/`-BindlessAll` и активирует live Box. Существующая карта A1, DefaultEngine.ini и исходная Perlin не изменены — SHA-256 сверены. Новая тестовая карта содержит Sun=10 lux и Exposure Compensation=4; авторские изменения A1 не переносятся обратно. Probe использовал Exposure Compensation=2, но на основном проекте экспозиция новой карты повышена для читаемого сравнения.

Выбрать **FogMS - Live Box**: transform/Feather Distance/Enabled действуют без Apply. Enabled Off даёт мгновенное сравнение со штатным освещением; полный выключатель legacy и диагностика описаны в README. Пакет editor-only; обновление DLL требует обычного перезапуска. Основной проект открыт на новой карте; `MainProject_Final.log`/`main-runtime-receipt.json` подтверждают активный Box, один actor, enabled=1 и `r.RayTracing=1`. Target overlay совпадает с active, error=none; итоговый кадр `MainProject_Box_Final.png` просмотрен в build-каталоге. Окончательная визуальная приёмка — за заказчиком.

Отдельный GPU probe завершил 18 кадров, тест позднего persistent capture и 56 замеров GPU; результаты и ограничения — `FogMS_Box_Report.md`. Карта плотности ещё не подключена; план с существующей VolumeTexture 64³ — `FogMS_TextureDensity_Plan.md`. Commit/push не выполнялись.

## Предыдущая установка A1 — сохранённый протокол

- Engine: `D:\PersonalProjects\UE5\UE_5.8\Engine`, UE 5.8.2 CL 56702186; исходники не редактировались.
- Проект: `D:\PersonalProjects\UE5\MimirHead_portfolio 5.7 5.8 - 3`.
- Свежий staging/build: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_Install_20260920_170812`.
- `BuildPlugin -TargetPlatforms=Win64 -StrictIncludes`: SUCCESS, 23 actions, без PCH/unity. DLL обеих editor modules собраны.
- Установка: `Plugins/MultiLobeSpec`; все 49 файлов побайтово сверены через SHA-256 с Package. Receipt: `install-receipt.json` в build-каталоге.
- Прежний плагин полностью сохранён в `Saved/FogMS_Backups/FogMS_Install_20260920_170812/MultiLobeSpec` внутри проекта.
- Существующие карты и настройки MLS не редактировались. Добавлена отдельная `/Game/FogMS_Test/FogMS_A1`, `FogMS_Test.cmd`, `FogMS_README.md` и скрипт включения `Saved/FogMS/enable_fogms.py`.

## Проверено на D3D12/SM6

`Editor.log` в build-каталоге: после FogMS.Apply оверлей `Shaders_e6982b96619d70ae87f9f4b6`, requested/active совпадают, enabled=1, error=none.

Реальный UE compile включал FVolumetricFogLightScatteringCS (193 допущенные permutations из 1024), FinalIntegration (1), MaterialSetup (4), directional shadow RGS (1), local light RGS (16), local light PS (64 из 128) и bounding sphere VS (1). Это покрытие текущей конфигурации UE; отклонённые платформой permutations не считаются проверенными. Ошибок fog shader compile не обнаружено.

Сохранены 1280×720 кадры с фиксированной камерой: `Final_Off.png`, `Final_On.png`, `Debug_Density.png`, `Debug_Transmittance.png`, `Debug_Seam.png`. Первый набор — исходная слабая плотность; подкаталог `Dense` — более наглядная демонстрация. Выход из диагностики восстановил temporal reprojection=1, FogMS оставлен On.

Демонстрационная карта на момент первой установки: Fog Density=0.08, Height Falloff=0.1, Extinction Scale=1, g=0, солнце 10 lux/10°, фиксированная Exposure Compensation=4 с сохранённым тонемаппером проекта. Это тест готовности к сравнению, **не полная математическая приёмка A1**, не доказательство универсального порога 5% и не замер производительности. Box Volume/MS тогда ещё не были реализованы; позднее заказчик проверил A1 без замеченных дефектов и разрешил Box.

В стартовом журнале до включения FogMS есть handled ensure OIT.SampleData/SeparateTranslucency (13:14:20), а при настройке viewport — ensure снятия realtime override. Редактор продолжил работу. Поэтому запуск не обозначается как полностью чистый: эти сообщения сохранены в логе, причинность отдельно не исследована. Первая версия setup-скрипта также ошибочно передавала Rotator позиционно; исправлена на именованные pitch/yaw/roll до итоговых кадров.

## Обновление A1d — 2026-09-21

Актуальный пакет `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_HV_20260920/Package` установлен в тот же проект; 52 файла проверены SHA256. StrictIncludes/non-unity/no-PCH и D3D12/SM6 прошли. Новых карт нет. Основной редактор открыт, существующая FogMS_Box сохранена, authored controls preserved=true (`field-ready.json`).

Backup: `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7 5.8 - 3/Saved/FogMS_Backups/FogMS_HV_20260920_235251`. Содержит прежний плагин, начальную сохранённую карту, более позднюю карту перед установкой и старый launcher script. Старые user edits не заменены первоначальным snapshot.

Новые live controls: Authored Sun Shadow; Detail Strength/Scale/Second Octave. Подробный протокол и ограничения — `FogMS_A1d_Report.md`. Сохранили RT Shadows/HWRT/A1c; preset grid4/Z128, 64 sun steps, 36 indirect directions. Обновлён `Saved/FogMS/enable_box.py`, используемый прежним `FogMS_Box_Test.cmd`. Он не меняет камеру и авторские параметры.

Во время разработки произошёл crash редактора в UE DeleteAllMaterialExpressions при попытке изменить загруженный материал. Оригинальный asset на диске не был сохранён повреждённым; новый material построен в изоляции и затем установлен. Основной редактор восстановлен. Текущий `Main_A1d.log` не содержит Error/fatal/shader compile failure. Исторический crash оставлен в прежнем `FogMS_A1c_20260920/Main.log`, не скрыт и не выдан за успешную проверку.

## Команды сравнения

Включить: `r.FogMS.Enable 1`, затем `FogMS.Apply`.

Штатный туман UE при сохранённых MLS-эффектах: `FogMS.Debug 0`, `r.FogMS.Enable 0`, затем `FogMS.Apply`.

После Apply дождаться компиляции. После смены карты или Extinction Scale повторить Apply. Подробная инструкция находится в `FogMS_README.md` рядом с проектом. Commit/push не выполнялись.

## 2026-09-21 — сглаживание внутри объёма

Установлен FogMS_Banding_20260921/Package2: StrictIncludes, 72 файла SHA256 verified. Filter Sun Inside Volume включён в прежней FogMS_Box при Sigma100; переключается live. Авторские параметры и последний ракурс сохранены. Matched On/Off, HDR finite, 120s camera/sun/occluder soak (2321 callbacks) прошли; restoration_ok=true. Редактор PID41564 оставлен открытым. Engine и crash compatibility guard сохранены. Протокол: FogMS_Banding_Report.md, точная доставка — .codex-build/FogMS_Banding_20260921/delivery-receipt.json. Визуальная приёмка владельца pending.
