# ЛР5. Краш-репорты и упаковка релиза

Что сделано по обязательной части ТЗ (полнота и стабильность), где это в коде и как проверить.

- Для тестировщика — [tester-guide.md](tester-guide.md).
- Для разработчика (разбор дампа) — [dump-analysis.md](dump-analysis.md).
- Устройство пакета игры и старый механизм падений — [../game/packaging.md](../game/packaging.md).

## Как это работает целиком

```
push в ветку ──► Actions: чистый checkout + подмодули ─► package_game.ps1 (RelWithDebInfo) ─► 2 zip ─► артефакты прогона
push тега v* ──► то же для коммита тега ───────────────────────────────────────────────────► 2 zip ─► GitHub Release тега

RocketArena.exe ──► CrashHandler (SetUnhandledExceptionFilter + DbgHelp) ──► crashes\crash-<дата>-<время>-<reportId>.dmp/.txt/.log
                         buildId вшит при сборке (Version.h) ─────────────────────► в меню, в логе, в .txt, в build-info.txt обоих zip
```

## 2.1. Сборка в GitHub Actions и публикация релиза

| Требование | Как сделано |
| --- | --- |
| Workflow в `.github/workflows/` | [`release.yml`](../../.github/workflows/release.yml): `push` в `main`, `lab5-crash`, `lab5/**` и теги `v*`; `windows-2025`; `actions/checkout` с `submodules: recursive` (FreeType, Tracy, Jolt); остальные зависимости лежат в `external/` |
| Штатный способ сборки, с отладочной информацией | [`tools/ci/build_release.ps1`](../../tools/ci/build_release.ps1) вызывает обычный [`tools/package_game.ps1`](../../tools/package_game.ps1) с `-Config RelWithDebInfo` (`/O2` + `/Zi`, pdb). Без Steam и Wwise: их SDK нет в репозитории |
| Тег → сборка коммита тега → два ZIP одной сборки → GitHub Release | один прогон собирает exe один раз; из него `RocketArena-<ver>-build<id>-win64.zip` (пакет) и `…-symbols.zip` (`RocketArena.pdb`); шаг `gh release create <tag> … --verify-tag` |
| В описании релиза `buildId`, SHA и ссылка на прогон | `release-notes.md` от `build_release.ps1`: таблица buildId / commit / built by (URL прогона с номером попытки) и краткая инструкция для тестировщика |
| Workflow падает при отсутствии обязательных файлов или сбое публикации | `build_release.ps1` проверяет exe, рантайм, конфиг, стартовую сцену, шейдеры, шрифты и pdb, а также что pdb принадлежит exe (GUID из записи RSDS exe ищется в pdb) и что в exe вшит этот buildId; после публикации workflow сверяет, что у релиза есть оба asset |
| Один `buildId` и SHA в обоих ZIP | `build-info.txt` в корне обоих архивов (buildId, commit, ref, run, время); в пакете ещё `version.txt` (`build`, `commit-full`) |
| `buildId` в краш-репорте | CMake-переменная `MYENGINE_BUILD_ID` → `Version.h` (`MYENGINE_BUILD_ID`), её передаёт `package_game.ps1 -BuildId`; в Actions это `GITHUB_RUN_ID.GITHUB_RUN_ATTEMPT`, локально — `local` |
| Пакет: exe, модули, ресурсы, конфиг одной рабочей сцены | состав по манифесту `games/rocketarena/game.json`: exe, `python314.dll/.zip`, Visual C++ runtime рядом с exe, ассеты сцены `rocket_league.json`, `config/app.json`, `myengine.myproject` |
| Два релиза из двух коммитов с изменением компилируемого кода | `v0.1.0-lr5.1` — кнопки INTEGER DIVIDE BY ZERO и ACCESS VIOLATION; `v0.1.0-lr5.2` — добавлена ASSERTION FAILURE (новый код в `CrashTests.cpp` и `RlGameFlow.cpp`) |
| Инструкция для тестировщика | [tester-guide.md](tester-guide.md); ссылка — в описании каждого релиза |

Чтобы выпустить релиз, достаточно поставить тег на коммит и запушить его:

```powershell
git tag v0.1.0-lr5.3
git push origin v0.1.0-lr5.3
```

## 2.2. Автоматический краш-репорт

| Требование | Как сделано |
| --- | --- |
| Механизм подключается при запуске | `crash::Install` — первая строка `wWinMain` ([`app/src/main.cpp`](../../app/src/main.cpp)): `SetUnhandledExceptionFilter`, `std::set_terminate`, `SIGABRT`, обработчики pure call и invalid parameter ([`CrashHandler.cpp`](../../engine/src/core/CrashHandler.cpp)) |
| Minidump с контекстом исключения и аварийного потока | `MiniDumpWriteDump` из `dbghelp.dll` (System32); `MINIDUMP_EXCEPTION_INFORMATION` с указателями на исключение и `ThreadId` упавшего потока; тип `MiniDumpNormal \| WithThreadInfo \| WithUnloadedModules` — стеки всех потоков, модули, исключение |
| Отчёт = дамп + связанный лог + метаданные | `crash-<дата>-<время>-<reportId>.dmp` / `.txt` / `.log`; `.log` — копия лога упавшего запуска с последней строкой `[CRASH] reportId=… buildId=…` |
| Метаданные до открытия дампа: `reportId`, время, `buildId` | `.txt`: reportId, время местное и UTC, buildId, версия, полный SHA, сцена, последнее действие, исключение и адрес `RocketArena.exe+0x…` (для access violation — чтение/запись и адрес), поток (главный или нет), PID, конец лога |
| Уникальный `reportId` | 64 случайных бита (`std::random_device`), создаются при `Install`. Один запуск даёт не больше одного сбоя, поэтому у двух аварийных запусков разные id и разные имена файлов; старые отчёты не перезаписываются |
| Лог хранит контекст: сцена и последнее действие | `crash::SetScene` при загрузке сцены (`Application.cpp`), `crash::SetLastAction` при смене экрана (`screen main`, `screen match` …) и при нажатии кнопки CRASH TEST; это же пишется в лог (`Game UI: …`); при старте в лог идут `Build: …` и `Crash handler: reportId …, reports go to …` |
| Где отчёт и что передавать | `%LOCALAPPDATA%\RocketArena\crashes\`, все три файла — [tester-guide.md](tester-guide.md); путь и reportId показывает и окно «has stopped working» |

Тестовые сбои — панель **CRASH TEST** в главном меню (`DrawCrashTest`, [`RlGameFlow.cpp`](../../engine/src/game/RlGameFlow.cpp)). Каждый сбой вызывается отдельной функцией `__declspec(noinline)` в [`CrashTests.cpp`](../../engine/src/core/CrashTests.cpp), поэтому верхний кадр стека — она, с файлом и строкой. Значения помечены `volatile`, чтобы оптимизатор `/O2` не убрал деление и запись. Assertion failure (lr5.2) — `MYENGINE_CHECK`: проверка, которая остаётся в релизной сборке (стандартный `assert` там выключен `NDEBUG`). При провале она пишет условие, файл и строку в «last action» и поднимает исключение `0xE0000003`, поэтому у дампа есть контекст.

## 2.3. Символы и разбор дампа

`RocketArena.pdb` лежит в отдельном архиве релиза. Exe ссылается на него только по имени, без путей машины сборки. Отладчик сопоставляет exe и pdb по GUID.

В ходе работы нашлась старая ошибка: ключ `/PDBALTPATH:%_PDB%` в `app/CMakeLists.txt` проходил через экранирование `%` в CMake и MSBuild, и exe называл свой pdb `%%%RocketArena.pdb%%%`. Такой файл не нашёл бы ни один отладчик. Теперь имя подставляет генератор-выражение `$<TARGET_PDB_FILE_NAME:…>`, а скрипт релиза проверяет, что запись RSDS в exe называет именно `RocketArena.pdb`. Поиск релиза по buildId, скачивание и разбор в Visual Studio или WinDbg описаны в [dump-analysis.md](dump-analysis.md).

## Стабильность

- **Штатный запуск и выход без отчёта:** фильтр срабатывает только на необработанное исключение. Обычный выход (`EXIT → Quit`, закрытие окна) ничего не пишет.
- **Два аварийных запуска — два отчёта:** у каждого запуска свой `reportId`, имена файлов не совпадают.
- **Дамп раннего релиза после выхода нового:** архивы каждого релиза неизменны, а pdb раннего релиза подходит только к его exe. См. [dump-analysis.md](dump-analysis.md) §3.

## Что проверено

См. раздел «Проверка» в описании PR ЛР5. Локально:
- сборка тем же скриптом, что в CI (`build_release.ps1`);
- распаковка zip в чистую папку и запуск;
- штатный выход;
- сбои всех кнопок с отчётами;
- разбор дампа по символам.
