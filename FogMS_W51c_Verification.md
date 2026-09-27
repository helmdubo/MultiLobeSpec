# W51c — локальный бюджет солнечного марша погодного Host

27.09.2026. **Статус: код собран и вынесен в черновик PR; в открытый пользовательский проект ещё не установлен.** Его карта `/Game/FogMS_Test/FogMS_Box` остаётся несохранённой по указанию владельца. Новый уровень и правки Engine не создавались. Экспериментальный `Native Weather Preview` по-прежнему выключен по умолчанию.

## Что изменилось

При включённом `Native Weather Preview` подсистема FogMS ставит **только компоненту своего Volumetric Cloud Host** `Shadow View Sample Count Scale = 1.6` вместо сохранённого масштаба (обычно 3.2). В UE 5.8.2 фактический верхний предел равен `min(10 × scale, r.VolumetricCloud.Shadow.ViewRaySampleMaxCount)`: при штатном глобальном cap80 это 16 вместо 32 шагов. Другие облачные компоненты и глобальный cvar не меняются. Выключение Preview, Clear или прекращение подачи погоды возвращает масштаб Host. `r.FogMS.Weather.ShadowViewSampleScale 0` оставляет авторское значение; его ручная правка в Details во время Preview получает приоритет до следующего включения Preview. Изменение не требует пересборки шейдера.

## Проверка качества и цены до интеграции

Контролируемый непрерывный пролёт W/S через одну и ту же сохранённую карту в отдельном PIE: 140 м за 20 с, Broken, солнце 3°, ветер и ручное время Box заморожены. Парные кадры [у края 80](Tools/FogMSEnergyValidation/ProdProbe/results/w51b/motion_80_03.png) / [16](Tools/FogMSEnergyValidation/ProdProbe/results/w51b/motion_16_03.png), [внутри 80](Tools/FogMSEnergyValidation/ProdProbe/results/w51b/motion_80_05.png) / [16](Tools/FogMSEnergyValidation/ProdProbe/results/w51b/motion_16_05.png) сохраняют тёмную сердцевину и солнечный край. На пяти выровненных по положению кадрах средняя разница RGB облачной области 0.47–0.74/255. Это выборочные кадры, а не доказательство отсутствия мерцания между ними.

Пять валидных `ProfileGPU CloudView (CS) 571×321` на режим: глобальный cap80 / cap16 дал медианы **8.890 / 4.879 мс** (−45.1%). При глобальном cap80 изменение только масштаба Host 3.2 / 1.6 дало **9.100 / 5.016 мс** (−44.9%). Логи изолированных процессов: `E:/GITHUB/MultiLobeSpec/.codex-build/WeatherPIEProbe_20260927/ProbeShadowMedian.log` и `ProbeHostScaleMedian.log`. Это цена видового прохода в Broken/3°, не всей погодной системы.

## Сборка и предел проверки

Свежая staged-копия плагина собрана UE 5.8.2 `RunUAT BuildPlugin -TargetPlatforms=Win64 -StrictIncludes`: Editor, Game Development и Shipping **успешно**, без PCH и unity. `Build.log` и пакет: `E:/GITHUB/MultiLobeSpec/.codex-build/WeatherCoherence_20260927_W51c/`. Исходники `FogMS_CloudHost.cpp/.h` в staged-копии совпали по SHA-256 с рабочей веткой. Предупреждения C4701 для `Phase0/1/2` в неизменённом `FogMS_BoxVolume.cpp` остались; Shipping не запускался.

Отдельный временный проект с **копией того же FogMS_Box**, пакетом W51c и новой DLL загрузился, но его PIE-время оставалось 0.0 с при `game_paused=false`, `time_dilation=1` и разрешённом тике Weather. Погодный актёр поэтому не подавал данные Host; попытки с игровым view target и принудительным снимком не изменили это состояние. Протоколы `RuntimeTest/Editor2.log`, `Editor3.log`, `Editor4.log` и `w51c_runtime_result*.json` в том же staged-каталоге. Эти неуспешные пробы **не подтверждают и не опровергают** жизненный цикл нового C++ управления; его runtime-проверка остаётся открытой. Установленную DLL и открытый редактор владельца я не заменял и не перезапускал.

## Полевые ворота после безопасной установки

После завершения работы с несохранённой картой установить собранный пакет и запустить эту же карту. Сначала вернуть штатный глобальный лимит `r.VolumetricCloud.Shadow.ViewRaySampleMaxCount 80`. При Clear/Preview off Host должен иметь прежний масштаб 3.2; Broken + `Native Weather Preview` — 1.6; `r.FogMS.Weather.ShadowViewSampleScale 0` при том же Broken должен сразу вернуть 3.2, повторное `... 1.6` — снова 1.6, выключение Preview/Clear — 3.2. Затем сравнить непрерывный W/S, Overcast и Broken при дневном/низком солнце, наземную тень и SkyLight capture. Если что-то ухудшится, `r.FogMS.Weather.ShadowViewSampleScale 0` возвращает старую цену/качество без пересборки.
