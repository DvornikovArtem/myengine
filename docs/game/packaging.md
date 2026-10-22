# Упаковка игры (G2)

Как собрать `RocketArena.exe` для игрока и положить его с ассетами в папку или zip, который можно «скинуть другу». Архитектурное решение — [rl-online.md](rl-online.md), раздел 8. Скрипты: `tools/package_game.ps1` (сборка и упаковка), `tools/check_package.ps1` (проверка на чистой папке), манифест `tools/package_manifest.json`.

## Быстрый старт

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_game.ps1      # build-shipping/ -> dist/RocketArena + dist/RocketArena-<версия>-win64.zip
powershell -ExecutionPolicy Bypass -File tools\check_package.ps1     # запуск из %TEMP%\rocket_clean вне репозитория
```

Первый запуск скрипта конфигурирует `build-shipping/` и собирает только цель `myengine` (около 15 минут, Release без PDB и тестов). Дальше сборка инкрементальная. Параметры `package_game.ps1`:

| Параметр | Что |
| --- | --- |
| `-Config Shipping` (по умолчанию) / `Release` | обе записи — конфигурация Release в CMake |
| `-Out dist` | куда класть `RocketArena/` и zip |
| `-BuildDir build-shipping` | папка сборки shipping |
| `-SkipBuild` | упаковать уже собранный exe |
| `-NoZip` | без zip |
| `-WithSteam -SteamSdk <папка SDK>` | положить `steam_api64.dll` и `steam_appid.txt` (480), см. ниже |

## Shipping-сборка

CMake-опция `MYENGINE_SHIPPING` (по умолчанию OFF). Её включает скрипт: `-DMYENGINE_SHIPPING=ON -DMYENGINE_BUILD_TESTS=OFF -DMYENGINE_COPY_RUNTIME_ASSETS=OFF`, отдельная папка `build-shipping/`, конфигурация Release (Tracy включён только в RelWithDebInfo, так что в Release его нет).

| Что | В обычной сборке | В shipping |
| --- | --- | --- |
| Имя exe | `myengine.exe` | `RocketArena.exe` (`OUTPUT_NAME` той же цели `myengine`) |
| `MYENGINE_SOURCE_DIR` | зашит в exe: ассеты, шрифты и шейдеры читаются из репозитория | не определён; `core::apppaths::SourceRoot()` пуст, всё ищется рядом с exe |
| Ассистент и MCP | панель ассистента, `myengine_mcp.exe` | `myengine_mcp` не собирается, `SceneEditor::InitializeAssistant()` ничего не делает (код ассистента остаётся в библиотеке, но не запускается) |
| Тесты | собираются | не собираются |
| Автосохранение карты при выходе, «Save Map» | пишут `.json` карты | выключены: карты лежат рядом с exe, туда писать нельзя |
| Профайлер | Tracy в RelWithDebInfo | нет |

## Куда пишет игра

Все пути ведёт один модуль `engine/include/myengine/core/AppPaths.h` (`core::apppaths`): `SourceRoot`, `UserDataDirectory`, `LogDirectory`, `SavedDirectory`, `AssetCacheDirectory`. В shipping ничего не пишется рядом с exe: он может лежать в `Program Files` или в только читаемой папке.

| Файл | Обычная сборка | Shipping |
| --- | --- | --- |
| Лог | `<exe>\logs\myengine.log` | `%LOCALAPPDATA%\RocketArena\logs\myengine.log` |
| Раскладка окон ImGui | `%APPDATA%\myengine\editor_layout.ini` | `%LOCALAPPDATA%\RocketArena\editor_layout.ini` |
| Недавние проекты | `%APPDATA%\myengine\recent_projects.json` | `%LOCALAPPDATA%\RocketArena\recent_projects.json` |
| Миниатюры, захваты GPU, статистика кадра | `<проект>\Saved\…` | `%LOCALAPPDATA%\RocketArena\Saved\…` |
| Бинарный кэш моделей и текстур | рядом с исходным файлом (`.myemesh`, `.myetex`) | `%LOCALAPPDATA%\RocketArena\Cache\<хэш пути>.myemesh` / `.myetex` |
| Настройки игры (видео, камера, звук, последний выбор в Play) | `%APPDATA%\myengine\game_settings.json` | `%LOCALAPPDATA%\RocketArena\game_settings.json` |
| Личные клавиши | `%APPDATA%\myengine\rl_controls.json` | `%LOCALAPPDATA%\RocketArena\rl_controls.json` (пишет Settings > Controls) |
| Свой `rl_tuning.json` (окно RL Tuning) | `assets/data/rl_tuning.json` проекта | `%LOCALAPPDATA%\RocketArena\rl_tuning.json` (читается вместо файла дистрибутива) |

Кэш в shipping не запекается заранее: он привязан ко времени изменения исходного файла, а распаковка zip меняет время. Первый запуск строит его сам (две-три секунды на арену), следующие читают из `Cache`. Чтобы начать с нуля, достаточно удалить `%LOCALAPPDATA%\RocketArena`.

Стартовая карта задаётся конфигурацией: в `AppConfig` появились поля `startScene`, `startInPlay` и `startInGame` (`config/app.json`). Командная строка сильнее конфигурации; `main.cpp` не менялся.

**Игровой режим (G1).** `RocketArena.exe` — тот же `myengine`, собранный в Release с `MYENGINE_SHIPPING`; вместо отдельной цели режим игры включается полем `"startInGame": true` в `config/app.json` дистрибутива (на разработческом `myengine.exe` — флагом `--game`). Так не нужен второй исполняемый файл и вторая сборка: редактор и игра делят один код, а упаковка остаётся прежней. В игровом режиме открывается `assets/scenes/rocket_league.json`, вместо редактора идут экраны игры (`docs/game/rl-ui.md`), настройки лежат в `%LOCALAPPDATA%\RocketArena\game_settings.json`.

## Что в дистрибутиве

```
RocketArena/
  RocketArena.exe              Release, /MD, без PDB
  python314.dll, python314.zip встроенный Python (python314.dll нужен при загрузке exe)
  vcruntime140*.dll, msvcp140*.dll, concrt140.dll, vccorlib140.dll   Visual C++ app-local (из VC\Redist\MSVC\<версия>\x64 Visual Studio)
  config\app.json              окно 1600x900, startScene = rocket_league.json, startInGame = true
  myengine.myproject           проект RocketArena, карта по умолчанию — rocket_league
  assets\                      сцены, модели, материалы, текстуры, шейдеры (.hlsl: компилируются при запуске), data, audio, fonts, fx
  licenses\                    лицензии сторонних компонентов
  README.txt, version.txt      как запустить и управление; версия и коммит
  steam_api64.dll, steam_appid.txt   только с -WithSteam
```

Не нужны и не кладутся: `d3dcompiler_47.dll`, XAudio2, XInput, универсальный CRT (всё это часть Windows 10 и 11), PDB, кэши `.myemesh` / `.myetex`, тестовые и редакторские ассеты (`stress_5000.json`, демо-сцены, скрипты, префабы, ассистент).

**Размер:** папка 42.6 МБ (ассеты 24.5, Python 10.4, exe 5.6, CRT 2), zip 19 МБ при цели ≤ 300 МБ.

### Манифест ассетов

`tools/package_manifest.json`: список корней (`assets`: файл или папка), `exclude` (маски, сильнее всего), `runtime` (Python), `config` и `project` (то, что скрипт пишет в дистрибутив), `licenses`, `steam`. При `followReferences` скрипт идёт дальше по ссылкам: всё, что названо в взятых `.json` (карты, материалы, `.shader.json`, данные) как путь к `.obj`, `.material.json`, `.shader.json`, `.tga`, `.dds`, `.bmp`, `.png`, `.hlsl`, `.wav`, `.ttf`, относительно репозитория или самого файла, попадает в пакет сам. Поэтому, когда G1 переведёт игровую карту на ассеты `rl2`, старые `assets/*/rl/` и `rl_*.obj` уйдут из дистрибутива без правок манифеста. Ссылка на файл, которого нет, выводится предупреждением. Новый ассет игры достаточно положить в одну из перечисленных папок или назвать в карте.

## Проверка на чистой папке

`tools/check_package.ps1 [-WithoutScripting] [-Frames 600] [-Keep]` копирует `dist/RocketArena` в `%TEMP%\rocket_clean`, запускает `RocketArena.exe --exit-after-frames 600` из другой рабочей папки с подменёнными `LOCALAPPDATA` и `APPDATA` и проверяет:

1. процесс завершился сам с кодом 0;
2. рядом с exe ничего не появилось, не изменилось и не исчезло (снимок размеров и времени до и после);
3. лог лежит в `<LOCALAPPDATA>\RocketArena\logs`, в нём нет путей репозитория (`Documents\GitHub`, `.team`, `build-shipping`, корень репозитория), нет строк `[ERROR]`, есть загрузка банка звуков и эффектов;
4. в `APPDATA` ничего не записано;
5. с `-WithoutScripting` то же без `python314.zip`: игра работает без скриптинга (`ScriptRuntime` предупреждает и продолжает).

Результат последней проверки — в отчёте задачи `.team/tasks/g2.md`. Проверка на «машине без Visual Studio и без репозитория» — запуск того же zip на другом компьютере; здесь она заменена чистой папкой и независимыми `LOCALAPPDATA` и `APPDATA`.

## Steam

Задел под Steamworks ([rl-online.md](rl-online.md), раздел 7.4): SDK в репозиторий не кладётся. `-WithSteam -SteamSdk <папка SDK>` собирает exe с `MYENGINE_STEAM=ON` (папка сборки должна быть настроена так же: иначе скрипт останавливается), копирует `redistributable_bin\win64\steam_api64.dll` рядом с exe и пишет `steam_appid.txt` со значением 480 (тестовый Spacewar). Путь к SDK проверяется до долгой сборки. Условия распространения SDK нужно подтвердить с Артёмом до того, как что-то из SDK попадёт в zip. Без `-WithSteam` в пакете нет ничего от Steam.

## Лицензии

`licenses/` собирается из `tools/package_licenses/` (`THIRD_PARTY_NOTICES.txt`, `OFL-1.1.txt` для шрифтов Inter и JetBrains Mono) и из лицензий в репозитории (Python, Jolt, FreeType, pybind11, Lucide). Тексты MIT для RocketSim, Dear ImGui, ImGuizmo, nlohmann/json, DirectXTK и DirectXTex приведены в `THIRD_PARTY_NOTICES.txt` общим текстом со ссылками на источники: авторские строки по годам нужно сверить с репозиториями проектов.

## Ограничения

- Игровой режим (`startInGame`) идёт без панелей редактора (G1).
- Личные клавиши, настройки и свой `rl_tuning.json` в shipping лежат в `UserDataDirectory()` (`%LOCALAPPDATA%\RocketArena`): их пишут экраны игры и окно RL Tuning (G1).
- `PrefabLibrary` пишет в лог предупреждение, что `assets/prefabs` нет: префабы редактора в дистрибутив не входят, на игру это не влияет.
- PDB не собираются: crash-дампы читать нечем. Если понадобится, `/Zi` и `/DEBUG` добавляются в shipping отдельно, а PDB в дистрибутив не кладётся.
- Подпись кода и установщик не делались: zip распаковывается и запускается; SmartScreen может предупредить о неподписанном exe.
