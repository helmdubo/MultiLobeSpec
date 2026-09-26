# FogMS — подготовленные мягкие тени и пространственное рассеяние A2

2026-09-21. Новый срез в существующем MultiLobeSpec, UE 5.8.2 CL56702186, Windows D3D12/SM6/BindlessAll. Полевая карта прежняя: `/Game/FogMS_Test/FogMS_Box`. Заказчик разрешил оба направления и автономные перезапуски. Визуальная приёмка владельцем ожидается.

## Что изменилось

**Filtered Sun Shadow** подготавливает пропускание света через локальную плотность в координатах солнца. В кэше 128×128 лучей и 65 границ глубины (64 интервала); хранится T, а не только суммарная тень на земле. Поэтому поверхность перед объёмом, внутри него и за ним получает соответствующий пройденному пути свет. Два прохода Gaussian усредняют T в поперечных направлениях. **Shadow Filter Sigma** задаётся в сантиметрах мира. Это художественный радиус мягкости, не физическая модель углового размера солнца. Геометрические RT/VSM-тени остаются штатными.

Кэш использует тот же world-locked Perlin, порог, detail-октавы, плотность и OBB, что авторский FogMS. Он перестраивается при изменении этих данных, направления основного Atmosphere Sun index 0 или Sigma; движение камеры не является причиной перестроения. Постоянный atlas R32F занимает 4.0625 MiB. Старый march остаётся отдельным выключаемым путём и fallback для несовпадающего directional source.

**Scattering Mode → Spatial (Experimental)** интегрирует свет от соседних участков тумана в мировой OBB-сетке 32³. Двенадцать фиксированных направлений, по 12 шагов; HWRT-запрос ограничивает длину каждой трассы ближайшим opaque-треугольником. Источник — предыдущий native fog source `q`, с native extinction в alpha. Для постоянного сегмента используется `q * (1-exp(-sigma_t*ds))/sigma_t` с устойчивым пределом при sigma_t=0. Поле хранит de-exposed incoming radiance J; сила эффекта, sigma_s получателя и pre-exposure применяются в native fog ровно по одному разу.

Spatial заменяет Octaves. Он может переносить уже попавшие в native fog вклады directional/local/sky/emissive; отдельного нового решения полного Lumen GI на поверхностях здесь нет. Spatial Strength ограничен 0…0.5, рабочее значение 0.35; Spatial Distance — 500 cm. Это затухающий итерационный preview с историей, не отдельно выделенный строго второй порядок.

Добавлен ранний **FogMSRender** внутри того же плагина, с двумя глобальными compute shaders. Исходники Engine не менялись. Данные передаются через resident bindless atlases. В UE 5.8 callback PostTLAS идёт **после** callback base pass: shadow metadata публикуется после base pass, spatial metadata — после PostTLAS и перед native volumetric fog. Публикации защищены переходами между Graphics и AsyncCompute. Принудительный RDG async для Spatial запрещён явной проверкой.

Стресс-тест `r.RDG.AsyncCompute=2` в Build5 привёл к RHI assertion `GraphicsContext` на втором кадре после переключения, несмотря на bypass preview. Причина общего сбоя отдельно не изолирована; безопасный fallback всего рендера в этом режиме не заявляется. Для установленного варианта оставлено исходное `r.RDG.AsyncCompute=1`; финальный рабочий запуск — `MainGPUFinal.log`. Принудительный режим не включать при полевой проверке.

## Проверка

Доказательства: `E:/GITHUB/MultiLobeSpec/.codex-build/FogMS_Fields_20260921/`.

- StrictIncludes без unity/PCH: Build4 и финальный Build5 прошли без C++ warnings. Build5 добавляет только guard forced async, статус Strength=0 и кэш указателя CVar. Пакет 70 файлов сверён SHA256; 6 Engine shader hashes неизменны; authored scene settings восстановлены. Финальная установка фиксируется в `delivery-receipt.json`.
- GPU на RTX 3070, viewport 1479×954: шейдеры и реальные compute/RT dispatch прошли. Сравнения `Spatial4/Off4/Octaves4`, `ShadowReference4/ShadowFiltered4/ShadowWide4/ShadowOff4` просмотрены. У мягкой тени исчезла грубая мелкая граница, тени столбов сохранились.
- HDR readback RGBA32F: все проверенные поля конечны и неотрицательны. При солнце 80→160 сумма J выросла в **2.00319** раза; relative L2 error к 2×J80 **0.7013%**. В тестах нулевого источника и нулевого scattering albedo все **98 304 RGB-компонента строго нулевые**.
- Static A/B: относительное RMS temporal std поля **0.9756%**; PNG MAE на колонне за туманом **0.565/255**. После возврата камеры MAE на той же области **0.503/255**. Это два статичных измерения и отдельное перемещение, не доказательство отсутствия артефактов при любом движении.
- Spatial compute **0.258 ms**, copy **0.005 ms** в одном GPU profile. Это стоимость этих проходов, не всего FogMS. В steady-state filtered shadow profile нет Prefix/Filter/cache-copy: кэш переиспользуется. Цена перестроения пока не измерена.
- Проверены переключатели, g=0.5 bypass с понятным статусом, скрытие Box, нулевой gain. 27 CPU-проверок теневого кэша и 10 математических тестов пространственного оператора также прошли; они не подменяют GPU-проверки.

Первый запуск выявил недопустимый RDG Copy flag у parameterless pass; исправлен. Build3 считал Spatial, но не публиковал его перед fog: его изображению и полю не приписывается эффект рассеяния. Проверки эффекта относятся к Build4 и далее.

В первой GPU4 серии native VolumetricCloud был видим после перезапуска: временный глазик не сериализуется. Внутри A/B его состояние одинаковое. Перед финальным кадром восстановлены исходные hidden states, Sky Light Captured Scene / intensity 3 / RTC=false; выполнен один recapture после восстановления. Тестовый Python restore первоначально сохранял ссылку на USTRUCT цвета; это исправлено копированием значения, авторские цвета восстановлены из дискового snapshot. Пять промежуточных restore/visibility/density кадров после black-albedo теста исключены из доказательств возврата к исходному виду. Финальные `Owner*` относятся к восстановленной авторской сцене.

Owner-серия после восстановления: Spatial по сравнению с Off увеличивает среднюю яркость fog ROI примерно на 9.88/255; Off↔Strength=0 MAE на колонне за туманом 0.53/255 при static A/B 0.49/255. Static HDR relative RMS std 1.53% по двум снимкам. Финальный Build5 сохранён и запущен; его последний `profilegpu` захватил кадр Slate без Spatial event, поэтому он не используется для новой оценки производительности. Измерения 0.241–0.258 ms относятся к действительным Build4 dispatch и не являются FPS-прогнозом.

В `MainGPUFinal.log` нет FogMS/shader/RHI ошибок рабочего режима. Есть два startup diagnostics `FDataflowToolNodeSnapshot::Date` от native DataflowNodes. Исходник `Engine/Plugins/Dataflow/Source/DataflowNodes/Public/Dataflow/DataflowToolNode.h:47` задаёт `Date=FDateTime::Now()`, что не проходит проверку детерминированности default struct. Это отдельно записано в receipt; Engine и этот модуль не исправлялись.

## Пределы результата

Это **экспериментальный A2**, не завершённый camera-independent этап B. J живёт в мировой сетке, но источник доступен только в предыдущем camera frustum. Пропущенный участок history обрывает интегрирование луча; cut/reset требует прогрева. Native temporal accumulation также добавляет задержку и шум.

Поддержан один realtime perspective view, g=0, native history, deferred D3D12 SM6 и inline HWRT. Captures, stereo, forward, forced RDG async и прочие неподдерживаемые случаи обходятся или остаются за границей этого preview. Editor-модуль не заявляется как проверенная Shipping-реализация.

HWRT ограничивает трассы геометрией; masked triangles трактуются как opaque, procedural primitives исключены. Native source interpolation и trilinear 32³ могут смешивать свет через тонкую стену в пределах шага сетки. Изолированный GPU-тест непроницаемой стены с независимо фиксированным источником **ещё не выполнен**; отсутствие всех утечек не заявляется. Следующий этап должен отделить источник от camera history, уточнить границы геометрии и измерить непрерывное движение/сходимость.

Не реализованы новые направленные sky-transmittance/sky-irradiance поля для всех поверхностей, согласование полного diffuse/specular GI и SSFS. Этот срез смягчает солнечную тень и добавляет пространственный перенос внутри fog; он не затемняет весь SceneColor общей маской.

## Как сравнить

В акторе **FogMS - Live Box**:

1. **Filtered Sun Shadow**: выключено — прежний march; включено — подготовленная мягкая тень. **Shadow Filter Sigma**: 50/100/200 cm. **Cast Sun Shadow** выключает тень FogMS на поверхностях целиком.
2. **Scattering Mode**: Off (A1 Only) / Octaves / Spatial (Experimental). **Spatial Strength=0** убирает новый перенос; вернуть **0.35** и **Spatial Distance=500 cm** для установленного варианта.
3. После переключения или движения подождать несколько секунд. Плотность, Perlin, свет и экспозицию между A/B не менять.

Подробные признаки провала: `FogMS_Fields_Test_Protocol.md`. Backup карты и прежнего плагина: `Saved/FogMS_Backups/FogMS_Fields_20260921_021821` основного проекта. Commit/push/merge не выполнялись.
