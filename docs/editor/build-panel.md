# Панель Build: сборка и упаковка игры кнопкой (B0)

В редакторе `myengine` есть окно **Window > Build**: выбрал игру, конфигурацию и параметры, нажал **Build & Package**, смотришь живой лог и прогресс, в конце «Открыть папку», «Запустить», «Проверить пакет». Без консоли. Окно запускает тот же `tools/package_game.ps1`, что и руками ([../game/packaging.md](../game/packaging.md)), как дочерний процесс: редактор не блокируется, сборку можно отменить.

Код: `engine/include/myengine/editor/BuildPanelModel.h` (манифесты игр, разбор строк скрипта, состояние запуска, командная строка; без окна и без процессов), `BuildRunner.h` (дочерний процесс, пайп, Job Object), `engine/src/ui/editor/SceneEditorBuild.cpp` (окно). Тест без окна — `ctest -R build_panel_model` (`tests/core/BuildPanelModelTests.cpp`).

## Манифест игры: `games/<id>/game.json`

Каждая игра — папка `games/<id>/` с `game.json`. Окно показывает список по этим файлам, скрипт читает тот же файл (`-Game <id>`; без ключа `rocketarena`). Старый `tools/package_manifest.json` переехал в `games/rocketarena/game.json`; к нему добавлены новые поля, остальное (ассеты, исключения, лицензии, Wwise, Steam) не менялось, список файлов пакета Rocket Arena тот же (см. «Проверка» ниже).

| Поле | Что |
| --- | --- |
| `id` | имя папки (`a-z`, `0-9`, `-`, `_`); должно совпадать с папкой |
| `displayName` | название в окне: «Rocket Arena» |
| `name` | имя папки пакета и префикс zip: `RocketArena` (`<вывод>/RocketArena`) |
| `exe` | имя exe: `RocketArena.exe` |
| `build.cmakeTarget` | цель CMake shipping-сборки; окно проверяет, что она есть в `CMakeLists.txt` репозитория (`add_executable` / `add_library`), и без неё не даёт собирать («target not found») |
| `build.exePath` | где exe этой цели лежит в папке сборки: `app/Release/RocketArena.exe` (для RelWithDebInfo скрипт подставляет `RelWithDebInfo`) |
| `build.userDataFolder` | папка файлов игрока под `%LOCALAPPDATA%` |
| `build.icon` | иконка (путь от корня репозитория), может быть пустой |
| `build.readme` | шаблон `README.txt` пакета (`{VERSION}`, `{STEAM}`), по умолчанию `tools/package_readme.txt` |
| `build.checkScript` | скрипт проверки пакета (`tools/check_package.ps1`); пусто: у игры нет проверки, кнопка Check package выключена |
| `options.steam`, `options.wwise` | `optional`: переключатель есть, скрипт ищет SDK и без него собирает без него, с предупреждением; `no`: у игры этого нет, переключателя нет и SDK не ищется |
| `options.zip` | включён ли zip по умолчанию |
| `externalData[]` | данные, которые не лежат в репозитории: `name`, `check` (файл, по которому видно, что данные есть: путь от репозитория или с переменной окружения, `%LOCALAPPDATA%/FlyDoom/wads/doom1.wad`), `fetch` (скрипт, который их скачивает; `package_game.ps1` сам запускает его до сборки, если файла нет, а `"autoFetch": false` оставляет запуск человеку: мозг мухи качает гигабайты), `optional` (нет данных — предупреждение, пакет собирается без них). Окно показывает «found» или «not found», «the package goes without it» и название скрипта |
| `files[]` | файлы пакета, которые не ассеты: данные игры и файлы машины. `from` (путь или список: берётся первый, который есть; можно `%ИМЯ%`), `to` (место в пакете), `required` (нет файла — остановка; иначе предупреждение с текстом `ifMissing`, а `"warnIfMissing": false` — обычная строка), `md5` (какой именно файл имеется в виду: чужой не кладётся), `requires` (класть, только если положен другой файл списка: лицензия рядом с WAD). Положенные файлы `package_game.ps1` перечисляет в `version.txt` строками `file <путь>` |
| `licenses.copy[]` | `from`, `to`; `"root": true` кладёт файл рядом с exe (`COPYING`, `THIRD_PARTY.txt`), а не в `licenses/`; `"optional": true` — нет файла только предупреждение |
| `licenses.generated[]` | тексты из шаблона репозитория (`template`, `to`, `root`) с подстановкой `{VERSION}`, `{COMMIT}`, `{COMMIT_FULL}`, `{TREE}` (чисто ли дерево исходников: если нет, скрипт предупреждает, что коммит не содержит изменений пакета): `SOURCE.txt` игры под GPL |
| `assets`, `exclude`, `followReferences`, `runtime`, `config`, `project`, `licenses`, `wwise`, `steam` | как в [packaging.md](../game/packaging.md): что и откуда кладётся в пакет |

Ошибки манифеста понятны и называют поле: `game.json is not valid JSON: ...`, `game.json: "id" is missing`, `game.json: "id" is "x", but the folder is games/y`, `options: "steam" is "maybe", it must be "optional" or "no"`, `externalData[0]: "check" is missing`. Сломанный манифест не прячет игру: она остаётся в списке («broken game.json») с причиной под выбором.

`games/flydoom/game.json` — FlyDoom (серия FLYDOOM, D2): цель `FlyDoom` (`app/CMakeLists.txt`), без Steam и Wwise (переключателей в окне нет), данные игры и лицензии GPL; что именно кладётся и как проверяется — [../game/packaging.md](../game/packaging.md), раздел «FlyDoom». Окно показывает три внешних данных: Freedoom (нет — `package_game.ps1` скачивает его сам), shareware `doom1.wad` с этой машины и мозг мухи (оба необязательные: без них пакет собирается с предупреждением).

## Окно

1. **Game** — игры из `games/*/game.json`.
2. **Configuration** — `Release (shipping)` (по умолчанию) или `RelWithDebInfo` (тот же shipping-пакет с символами: `-Config RelWithDebInfo`, zip с суффиксом `-rwdi`, PDB в `<вывод>/symbols`).
3. **Steam** — Auto / On / Off (`-WithSteam` / `-NoSteam`), только если `options.steam` = `optional`. **Wwise sound** — Auto / Off (`-NoWwise`, звук XAudio2).
4. **Zip** — писать ли zip для игроков (`-NoZip` без него). **Output folder** — по умолчанию `dist`, относительно репозитория или полный путь (можно на другом диске); пакет ляжет в `<вывод>/<name>`. **Build folder** — папка shipping-сборки: пусто (по умолчанию) — `build-shipping` в репозитории (с суффиксами `-nosteam` / `-nowwise`, как у скрипта), иначе указанная папка (`-BuildDir`), относительно репозитория или полный путь, тоже можно на другом диске: сборка занимает около гигабайта, ей не обязательно лежать рядом с исходниками.
5. Предупреждение о месте: если на диске репозитория свободно меньше 3 ГБ, окно пишет это до запуска (сборка shipping оставляет папку сборки), кнопка называется «Build anyway».
6. **Build & Package** — запускает `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\package_game.ps1 -Game <id> -Config ... -Out ...` через `CreateProcess` (вывод и ошибки в пайп, окна нет). Пока идёт запуск, второй запрещён (кнопка выключена).
7. **Cancel** — убивает дерево процессов (powershell, cmake, MSBuild, компиляторы): процесс создаётся приостановленным и кладётся в Job Object, отмена завершает Job. Редактор закрывается: Job закрывается вместе с ним, сборка не остаётся сиротой.
8. Прогресс-бар по строкам `##progress` и этап, время, число предупреждений и ошибок; итог **Done** / **Failed** / **Cancelled**; при провале сообщение скрипта.
9. Лог: строки по мере появления (поток читает пайп, главный поток забирает очередь раз в кадр), автопрокрутка, подсветка: ошибки красным (`error C...`, `LNK`, `CMake Error`, `fatal error`, «ошибка C...», `##result fail`), предупреждения жёлтым (`WARNING:`, `warning C...`), служебные `##progress` приглушены; итоговые строки MSBuild («0 Error(s)», «Ошибок: 0») не красятся. Лог ограничен 20 000 строк (старые уходят), «Copy log» кладёт его в буфер обмена, «Clear log» чистит.
10. После сборки: **Open folder** (проводник), **Run** (`<вывод>/<name>/<exe>`), **Check package** (`check_package.ps1 -Dist <пакет>` тем же способом, лог в том же окне). Кнопки включены, когда в папке вывода есть пакет и ничего не идёт.

Окружение дочернего процесса — окружение редактора без `MYENGINE_UI_SCRIPT`: скрипт, которым ведётся сам редактор, не должен запускаться в игре, которую стартуют проверки пакета (иначе его путь попал бы в лог пакета).

Кодировка вывода: консольные программы (cmake, MSBuild) пишут в OEM-кодировке консоли, окно читает пайп как OEM и показывает UTF-8, поэтому русские сообщения MSBuild читаются.

## Машиночитаемый прогресс скрипта

`tools/package_game.ps1` пишет для программы строки (для человека всё остальное остаётся как было):

```
##progress <0-100> <этап>        manifest 1, steam 3, wwise 5, configure 8, build 12 (до 70), assets 72, assemble 80, licenses 88, size 94, zip 96, done 100
##zip <путь к zip>               если zip записан
##result ok <папка пакета>       в конце, код выхода 0
##result fail <что случилось>    при любой ошибке (trap в скрипте), код выхода 1
```

Сборка CMake не даёт процентов, поэтому на этапе `build` бар стоит на 12 %, пока идёт `cmake --build` (лог при этом идёт); конец сборки — 70 %.

## Скриптовый ввод (проверки без человека)

Строка `UiScript` (`MYENGINE_UI_SCRIPT`, [../game/rl-ui.md](../game/rl-ui.md)) `<сек> editor <команда>`:

| Команда | Что |
| --- | --- |
| `build-open` | открыть окно Build |
| `build <id> [rwdi] [nosteam] [nowwise] [nozip] [out=<папка>] [builddir=<папка>]` | выбрать игру, поставить параметры и нажать Build & Package |
| `check <id> [out=<папка>]` | нажать Check package |
| `build-finish <файл.tga> <секунд>` | когда запуск закончится: снять экран и через столько секунд закрыть редактор (чтобы скрипт не гадал, сколько идёт сборка) |
| `build-cancel` | нажать Cancel |

Вместе с командой `shot <файл>` так снимается окно в работе (запуск на скрытом рабочем столе: `.team/tools/run_hidden_desktop.ps1`).

## Тест `build_panel_model`

Без окна и без настоящей сборки: разбор манифестов (все ошибки с понятным текстом, список игр папки, манифесты репозитория), разбор строк `##progress` / `##result` / `##zip` и ошибок и предупреждений среди вывода MSBuild (в том числе русских), параметры и командная строка (кавычки при пробелах, лишние переключатели не попадают), порог свободного места, состояние запуска (Idle → Running → Done / Failed / Cancelled, второй запуск запрещён, ограничение лога), поиск цели CMake и внешних данных во временной папке; `BuildRunner` на коротком дочернем процессе (вывод по строкам, код выхода, повторный запуск, программа, которой нет, Cancel убивает дерево).

## Проверка (B0)

Rocket Arena собран и упакован из панели (запуск на скрытом рабочем столе скриптом `editor build rocketarena`), `check_package.ps1` по пакету прошёл. Список файлов пакета после переноса манифеста сравнён с тем, что даёт прежний скрипт (`git show <до B0>:tools/package_game.ps1` на том же exe): файлы, размеры и хэши совпадают; отчёт — `.team/tasks/b0.md`.
