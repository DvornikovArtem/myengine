# Разбор краш-репорта (ЛР5, для разработчика)

Тестировщик прислал три файла `crash-<дата>-<время>-<reportId>.dmp / .txt / .log` ([tester-guide.md](tester-guide.md)). Задача: по `buildId` найти релиз, взять его бинарники и символы, открыть дамп, показать исключение, аварийный поток и стек с функцией, файлом и строкой, затем повторить сбой.

## 1. Что видно без отладчика: `.txt`

```
Rocket Arena crash report
reportId: 3f1c9a0d52b7e844
time: 2026-11-27 18:40:12 (local), 2026-11-27T15:40:12Z (UTC)
buildId: 19787654321.1
version: 0.1.0
commit: 5d4c…(полный SHA)
scene: rocket_league.json
last action: crash test: integer divide by zero
exception: integer division by zero 0xC0000094 at RocketArena.exe+0x2b1f3a
thread: 21344 (the main thread)
...
```

- `buildId` — какая сборка упала. `reportId` — какой это отчёт: у каждого сбоя свой.
- `last action` и `scene` — что повторять. `exception` — что ожидать в отладчике.
- `.log` — весь лог упавшего запуска. Его последняя строка `[CRASH] reportId=… buildId=…` привязывает лог к отчёту, а в начале есть строка `Build: version …, buildId …, commit …`.

## 2. Найти релиз по `buildId`

`buildId` — это `GITHUB_RUN_ID.GITHUB_RUN_ATTEMPT` прогона Actions, который собрал релиз. Найти его можно тремя способами:

- **по названию релиза:** оно заканчивается на `(build <buildId>)`. На странице <https://github.com/DvornikovArtem/myengine/releases> есть поиск, либо из консоли:

  ```powershell
  gh release list --repo DvornikovArtem/myengine
  ```

- **по прогону:** `https://github.com/DvornikovArtem/myengine/actions/runs/<число до точки>`. Там видно, какой тег собран.
- **по содержимому:** `build-info.txt` в обоих архивах релиза и строка `buildId` в его описании.

## 3. Скачать бинарники, символы и исходники этого релиза

```powershell
$tag = 'v0.1.0-lr5.1'                     # релиз, найденный по buildId
$dir = "C:\crash\$tag"
gh release download $tag --repo DvornikovArtem/myengine --dir $dir
Get-ChildItem $dir -Filter *.zip | ForEach-Object { Expand-Archive $_.FullName -DestinationPath $dir -Force }
# исходники ровно этого коммита (SHA — в описании релиза и в .txt отчёта), отдельной папкой рядом с репозиторием
git -C C:\Users\<вы>\Documents\GitHub\myengine fetch --tags
git -C C:\Users\<вы>\Documents\GitHub\myengine worktree add "$dir\src" $tag
```

Чтобы отладчик сам нашёл и exe, и pdb, положите их в одну папку с дампом:

```powershell
Copy-Item "$dir\RocketArena\RocketArena.exe", "$dir\RocketArena-symbols\RocketArena.pdb" C:\crash\report\   # туда же .dmp, .txt, .log отчёта
```

**Символы — только своего релиза.** Компоновщик записывает в exe GUID его pdb, а отладчик принимает только pdb с тем же GUID. Скрипт релиза проверяет это совпадение при сборке. С `RocketArena.pdb` другого релиза стек не раскроется: Visual Studio пишет «No matching symbols», WinDbg — «symbols could not be loaded», а `!sym noisy` показывает несовпадение GUID. Поэтому дамп раннего релиза разбирается символами раннего релиза и после публикации нового: архивы каждого релиза лежат в его Assets неизменными.

## 4. Открыть дамп в Visual Studio

1. **File → Open → File…** → `crash-….dmp`. Откроется **Minidump File Summary**:
   - *Process Name* — `RocketArena.exe`;
   - *Exception Code* — `0xC0000094` / `0xC0000005` / `0xE0000003`;
   - *Exception Information* — текст исключения;
   - список модулей с версией `0.1.0`.
2. Справа **Set symbol paths** → добавьте папку с `RocketArena.pdb` (если exe и pdb лежат рядом с дампом, это уже сделано).
3. **Debug with Native Only.** Отладчик остановится на исключении, например «Unhandled exception … 0xC0000094: Integer division by zero».
4. **Call Stack** (Debug → Windows → Call Stack, `Ctrl+Alt+C`) показывает функцию, файл и строку:

   ```
   RocketArena.exe!myengine::core::crash::test::DivideByZero() Line 14
   RocketArena.exe!myengine::game::`anonymous namespace'::...::DrawCrashTest(...) Line ...
   RocketArena.exe!...::DrawMain(...)
   ...
   ```

   Если VS попросит исходник (в pdb записаны пути машины сборки `D:\a\myengine\myengine\…`), нажмите **Browse and find** и укажите файл из `$dir\src`, то есть из worktree на коммите релиза.
5. **Threads** (`Ctrl+Alt+H`) — аварийный поток отмечен жёлтой стрелкой. Для кнопок CRASH TEST это `Main Thread`, совпадает с `thread: … (the main thread)` в `.txt`.

## 5. То же в WinDbg (по желанию)

WinDbg ставится командой `winget install Microsoft.WinDbg`.

```
windbg -z C:\crash\report\crash-….dmp -y "C:\crash\report;srv*" -i C:\crash\report -srcpath "$dir\src"
```

| Команда | Что показывает |
| --- | --- |
| `.exr -1` | запись исключения: код, адрес, для access violation — адрес и чтение/запись |
| `~` | потоки; точкой отмечен аварийный |
| `.ecxr` | переключиться на контекст исключения (регистры в момент сбоя) |
| `kn` | стек аварийного потока: `RocketArena!myengine::core::crash::test::DivideByZero+0x… [D:\a\…\CrashTests.cpp @ 14]` |
| `lm v m RocketArena` | какой pdb загружен и совпал ли он |
| `!analyze -v` | автоматический разбор |

## 6. Повторить сбой и сверить

1. Распакуйте пакет **того же** релиза в пустую папку и запустите его.
2. Сделайте действие из `last action` — нажмите ту же кнопку CRASH TEST.
3. Появится второй отчёт с **другим** `reportId`. Первый остаётся на месте.
4. Откройте новый дамп так же, как в §4. Код исключения и верхний кадр стека (функция, файл, строка) совпадают с первым отчётом. В `.txt` смещение `RocketArena.exe+0x…` тоже то же самое: exe тот же.
