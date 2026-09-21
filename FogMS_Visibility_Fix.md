# FogMS Box — скрытие через Outliner

2026-09-20. Пользователь обнаружил: глазик Outliner скрывает рамку, но эффект остаётся. Причина подтверждена: runtime отбирал Box только по `bEnabled`/`IsActorBeingDestroyed`, игнорируя actor visibility.

## Исправление

В `FogMS_BoxRuntime.cpp` видимость проверяется **до подсчёта** активных Box:

- Editor world: `!Actor.IsHiddenEd()` — глазик, editor layer и скрытый level.
- PIE/Game: `!Actor.IsHidden()` — Actor Hidden In Game.
- Политика выбирается через `World.UsesGameHiddenFlags()`; флаг HiddenInGame компонента рамки не выключает эффект. Режим просмотра G тоже не выключает эффект сам по себе.

Скрытый дубликат не считается вторым активным Box. При hide/show меняется существующий packet/revision; сбрасывается история тумана, включая поздний persistent capture. Apply и перекомпиляция для изменения видимости не нужны. Математика A1 и shader overlay не изменены.

Проверка штатных API UE 5.8.2: `Source/Editor/SceneOutliner/Private/ActorTreeItem.cpp:377` (глазик → SetIsTemporarilyHiddenInEditor); `Source/Runtime/Engine/Private/ActorEditor.cpp:987` (IsHiddenEd); `Source/Runtime/Engine/Private/World.cpp:9328` (UsesGameHiddenFlags); `Source/Runtime/Engine/Private/PostProcessVolume.cpp:101` (аналогичная editor-проверка PPV). PPV наследует ABrush и игнорирует runtime hidden, FogMS — AActor с явной проверкой runtime hidden.

`IsHiddenEdAtStartup()` не добавляется: после загрузки пользователь вправе показать actor. Отдельная маска per-viewport `HiddenEditorViews` в это исправление не входит; разные hidden masks одновременно собранных views требуют отдельного контракта данных.

## Проверки и доставка

BuildPlugin Win64 StrictIncludes без PCH/unity **PASS**: `E:\GITHUB\MultiLobeSpec\.codex-build\FogMS_Visibility_20260920`. Фикс включён в следующий пакет TextureDensity; отдельно пользователю не устанавливался.

GPU receipt: `.codex-build/FogMS_Box_20260920_1810/Probe/visibility-result.json`, status `MEASURED`, cleanup errors отсутствуют. D3D12/SM6, persistent HDR SceneCapture 256×144, фактическая ROI 223×95 (21185 pixels) от (32,8), jitter 0. Имена MaxX/MaxY в UE Python вводят в заблуждение: native helper принимает Width/Height. Между состояниями Apply не выполнялся.

- Величина A1-эффекта, HDR RGB MAE: **0.02549054**; максимальный шум повторного capture: **0.000011713**.
- Eye hidden против Enabled Off: **0.000011713**, в пределах повторного шума.
- Show против исходного shown: **0.0000028494**; скрытый дубликат против shown: **0**.
- Поздние captures при HistoryWeight=1: остаток **0.047–0.139%** величины эффекта; намного ближе к новому состоянию, чем к прежнему. Абсолютный ноль истории не доказан.

Проверены временный глазик, повторное появление и скрытый дубликат. Layer/hidden-level/game policy сверена с исходниками, отдельные runtime тесты для неё не выполнялись.

## Граница задачи

Этот фикс устраняет ошибку управления видимостью. Он не реализует MS и не закрывает замечание об исчезновении дымки. Физическое поле света определяется средой, источниками и геометрией; каскады/марши камеры — дискретизация. Рассеянная энергия меняет направление, поглощённая уходит из светового переноса, часть света выходит из объёма. A1 учитывает ослабление прямого света и штатный первый порядок; последующие рассеяния пока не добавлены.
