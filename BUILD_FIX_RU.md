# MultiLobeSpec 0.11.3 — сборка UE 5.7

## Что исправлено

Лог 0.11.2 падал не на компиляции C++, а на линковке `MultiLobeSpecEditor.dll`.
Все восемь unresolved external symbols были статическими клавишами `EKeys::*`,
инстанцированными inline-кодом `SListView`/`STableRow`.

Исправление двухуровневое:

1. `InputCore` объявлен явной public-зависимостью editor-модуля.
2. Display-only список нормалей заменён на пассивный `STextBlock` внутри `SScrollBox`;
   редактору больше не нужно инстанцировать клавиатурную навигацию `SListView`.

Шейдерная логика и математика Baker в этом hotfix не менялись.

## Чистая пересборка

Закройте Unreal Editor и Visual Studio. После замены папки плагина удалите:

```powershell
$Project = 'D:\PersonalProjects\UE5\MimirHead_portfolio 5.7'

Remove-Item "$Project\Plugins\MultiLobeSpec\Binaries"     -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$Project\Plugins\MultiLobeSpec\Intermediate" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$Project\Intermediate\Build\BuildRulesProjects\MimirHead_portfolioModuleRules" -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem "$Project\Binaries\Win64\UnrealEditor-MultiLobeSpec*" -ErrorAction SilentlyContinue | Remove-Item -Force
```

Затем заново сгенерируйте project files и соберите `MimirHead_portfolioEditor Win64 Development`.
Для прямого запуска UBT:

```powershell
$Engine  = 'D:\PersonalProjects\UE5\UE_5.7'
$Project = 'D:\PersonalProjects\UE5\MimirHead_portfolio 5.7'

& "$Engine\Engine\Build\BatchFiles\Build.bat" `
  MimirHead_portfolioEditor Win64 Development `
  -Project="$Project\MimirHead_portfolio.uproject" `
  -WaitMutex
```

Если снова появятся именно `EKeys::*`, проверьте, что компилируется эта копия плагина:
`Plugins/MultiLobeSpec/MultiLobeSpec.uplugin` должна иметь `VersionName: 0.11.3`.
