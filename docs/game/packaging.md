# Упаковка игры (G2)

Как собрать `RocketArena.exe` для игрока и положить его с ассетами в папку или zip, который можно «скинуть другу». Архитектурное решение — [rl-online.md](rl-online.md), раздел 8. Скрипты: `tools/package_game.ps1` (сборка и упаковка), `tools/check_package.ps1` (проверка на чистой папке), манифест игры `games/<id>/game.json` (для Rocket Arena `games/rocketarena/game.json`). То же кнопкой в редакторе: окно Window > Build, [../editor/build-panel.md](../editor/build-panel.md).

## Быстрый старт

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_game.ps1      # build-shipping/ -> dist/RocketArena + dist/RocketArena-<версия>-win64-steam.zip (или -nosteam.zip)
powershell -ExecutionPolicy Bypass -File tools\check_package.ps1     # запуск из %TEMP%\rocket_clean вне репозитория
```

Без ключей скрипт **собирает пакет со Steam**, если нашёл Steamworks SDK (см. «Steam»), а иначе с предупреждением собирает без Steam. Первый запуск конфигурирует `build-shipping/` и собирает только цель `myengine` (около 20 минут, Release без тестов). Версия берётся из `project(myengine VERSION x.y.z)` в корневом `CMakeLists.txt` и больше нигде не пишется: оттуда её читают меню игры, ресурсы exe, отчёт о падении и имя zip (`games/rocketarena/game.json` версии не хранит). Дальше сборка инкрементальная. Параметры `package_game.ps1`:

| Параметр | Что |
| --- | --- |
| `-Game <id>` | какая игра: папка `games/<id>/game.json` (по умолчанию `rocketarena`); неизвестная — остановка со списком известных |
| `-Config Shipping` (по умолчанию) / `Release` / `RelWithDebInfo` | первые две — конфигурация Release в CMake; `RelWithDebInfo` — тот же shipping-пакет с символами (zip с суффиксом `-rwdi`) |
| `-Out dist` | куда класть `RocketArena/` и zip: относительно репозитория или полный путь (можно на другом диске) |
| `-BuildDir build-shipping` | папка сборки shipping: относительно репозитория или полный путь (можно на другом диске); с этим ключом суффиксы `-nosteam` и `-nowwise` не добавляются |
| `-SkipBuild` | упаковать уже собранный exe |
| `-NoZip` | без zip |
| `-SteamSdk <папка SDK>` | где искать Steamworks SDK (иначе см. «Steam»); без ключей Steam включается сам, если SDK найден |
| `-WithSteam` | Steam обязателен: нет SDK — остановиться (без ключа это только предупреждение) |
| `-NoSteam` | не искать SDK, собрать без Steam (папка сборки `build-shipping-nosteam/`) |
| `-NoWwise` | звук на XAudio2, как до Wwise, даже если SDK установлен: WAV вместо банков, папка сборки с суффиксом `-nowwise`, zip `…-nowwise.zip` (ключ уйдёт вместе с XAudio2, см. «Звук: Wwise») |
| `-WwiseSdk <папка SDK>` | где искать установленный SDK Wwise (папка `SDK` установки; рядом с ней `Licenses`). Иначе `WWISE_SDK_DIR` из кэша папки сборки, затем переменная `WWISESDK`. Нужен для сборки с Wwise и для лицензий кодеков. SDK не найден — предупреждение и пакет с XAudio2, как с `-NoWwise` (см. «Звук: Wwise»); названная здесь папка, где нет SDK, — ошибка |
| `-CopyZipTo <папка>` | скопировать готовый zip ещё и сюда (папка создаётся) |

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
| Отчёт и дамп падения | `%APPDATA%\myengine\crashes\` | `%LOCALAPPDATA%\RocketArena\crashes\crash-<дата>-<время>.dmp` и `.txt`, маркер `pending.txt` до следующего запуска |
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
  assets\                      сцены, модели, материалы, текстуры, шейдеры (.hlsl: компилируются при запуске), data, audio (с Wwise: банки audio/wwise/*.bnk, без WAV), fonts, fx
  licenses\                    лицензии сторонних компонентов
  README.txt, version.txt      README на русском и английском (запуск, управление, игра с другом через Steam, LAN, IP и VPN, брандмауэр, где логи и дампы); версия, коммит, есть ли Steam
  steam_api64.dll, steam_appid.txt   только с -WithSteam
```

Не нужны и не кладутся: `d3dcompiler_47.dll`, XAudio2, XInput, DLL и файлы SDK Wwise (библиотеки статические), универсальный CRT (всё это часть Windows 10 и 11), PDB, кэши `.myemesh` / `.myetex`, тестовые и редакторские ассеты (`stress_5000.json`, демо-сцены, скрипты, префабы, ассистент).

**Размер:** папка 42.6 МБ (ассеты 24.5, Python 10.4, exe 5.6, CRT 2), zip 19 МБ при цели ≤ 300 МБ.

### Манифест ассетов

`games/<id>/game.json` (для Rocket Arena `games/rocketarena/game.json`; поля `id`, `displayName`, `build`, `options`, `externalData` описаны в [../editor/build-panel.md](../editor/build-panel.md)): список корней (`assets`: файл или папка), `exclude` (маски, сильнее всего), `runtime` (Python), `config` и `project` (то, что скрипт пишет в дистрибутив), `licenses`, `steam`. При `followReferences` скрипт идёт дальше по ссылкам: всё, что названо в взятых `.json` (карты, материалы, `.shader.json`, данные) как путь к `.obj`, `.material.json`, `.shader.json`, `.tga`, `.dds`, `.bmp`, `.png`, `.hlsl`, `.wav`, `.ttf`, относительно репозитория или самого файла, попадает в пакет сам. Поэтому, когда G1 переведёт игровую карту на ассеты `rl2`, старые `assets/*/rl/` и `rl_*.obj` уйдут из дистрибутива без правок манифеста. Ссылка на файл, которого нет, выводится предупреждением. Новый ассет игры достаточно положить в одну из перечисленных папок или назвать в карте.

## FlyDoom (D2)

`package_game.ps1 -Game flydoom` (или окно Window > Build, игра FlyDoom) собирает цель `FlyDoom` и кладёт в `dist/FlyDoom` пакет, который можно отдать любому: распаковал, `FlyDoom.exe`, играешь. Манифест — `games/flydoom/game.json`, README — `tools/package_flydoom/README.txt` (русский и английский), проверка — `tools/check_flydoom_package.ps1` (кнопка Check package). У игры нет Steam и Wwise (звук XAudio2), поэтому в имени zip нет суффиксов: `FlyDoom-<версия>-win64.zip`. Всё остальное работает как у Rocket Arena: те же параметры, `##progress`, `-BuildDir`/`-Out` на другом диске.

```
FlyDoom/
  FlyDoom.exe                  Release, /MD, без PDB (PDB в dist/symbols)
  python314.dll, python314.zip, vcruntime140*.dll ...   как у Rocket Arena (движок линкуется с Python)
  config/app.json, myengine.myproject
  assets/shaders, assets/fonts   то, что нужно движку (DOOM своих ассетов не имеет)
  assets/doom/freedoom1.wad    Freedoom Phase 1: пакет играет и без чужих данных
  wads/doom1.wad               shareware DOOM 1.9 (id Software разрешила раздавать свободно) + wads/doom1.wad.license.txt
  assets/flybrain/malecns_v1.fbrain   мозг мухи, 154 МБ (если файл есть на машине сборки)
  COPYING, SOURCE.txt, THIRD_PARTY.txt, README.txt, version.txt
  licenses/                    тексты лицензий данных и библиотек
```

**Данные.** Freedoom обязателен: нет `assets/doom/freedoom1.wad` — скрипт сам запускает `tools/doom/fetch_freedoom.ps1` до сборки (скачивает релиз 0.13.0 и сверяет SHA-256), не получилось — остановка. Shareware `doom1.wad` берётся из `%LOCALAPPDATA%/FlyDoom/wads/` (или `assets/doom/`) и кладётся только если его md5 `f0cefca49926d00903cf57551d901abe` (v1.9, 4 196 020 байт): другой файл под этим именем не едет, нет файла — предупреждение, пакет без него (только Freedoom). Текст лицензии shareware (`tools/package_flydoom/doom1.wad.license.txt`, из пакета Debian `doom-wad-shareware`: заявление Джона Кармака 1999 года) лежит рядом с WAD и едет, только если едет WAD. WAD полной игры (`DOOM.WAD`, `DOOM2.WAD`, ...) в пакет не попадает никогда: он собственность игрока, игра сама находит Steam-копию. Игра ищет WAD и в `wads/` рядом с exe ([doom.md](doom.md)), полная игра берётся раньше shareware, а он раньше Freedoom.

**Мозг мухи.** `assets/flybrain/malecns_v1.fbrain` (≈154 МБ, коннектом MaleCNS v1.0, CC BY 4.0) в git не лежит: его делает `tools/flybrain/fetch_malecns.ps1` (гигабайты; поэтому скрипт упаковки его сам не запускает: `autoFetch: false`). Нет файла — предупреждение, пакет без мухи. Атрибуция (Berg et al. 2026, Cell 189(18), doi 10.1016/j.cell.2026.08.015, зеркало QuixiAI/MaleCNS, что изменено нами) — в `THIRD_PARTY.txt` и внутри самого файла (секция `ATTRIB`). Пока игрок-мозг (N1) не в базе, FlyDoom файл не читает.

**GPL.** `FlyDoom.exe` содержит код DOOM, поэтому пакет несёт `COPYING` (GPL v2, из `game/doom/third_party/doomgeneric`) и `SOURCE.txt`: репозиторий, **полный хэш коммита** (`git rev-parse HEAD`), версия, «чисто ли дерево», где лежит код DOOM (`game/doom`) и как собрать то же самое. Если при упаковке в дереве были незакоммиченные изменения, `SOURCE.txt` так и пишет, а скрипт предупреждает: пакет из такого дерева не воспроизводится по названному коммиту. `THIRD_PARTY.txt` перечисляет всё, что в пакете не наше, с лицензиями: doomgeneric и id (GPL v2), модуль OPL Chocolate Doom (GPL), Nuked OPL3 (LGPL 2.1), Freedoom (модифицированная BSD), doom1.wad (лицензия id для shareware), MaleCNS (CC BY 4.0), библиотеки движка.

**База лиц в пакет не кладётся** (решение Артёма: лица реальных людей не едут в игры): `check_flydoom_package.ps1` считает ошибкой `faceid/allowed_faces.fdb` в пакете.

**Проверка.** `tools/check_flydoom_package.ps1 [-Frames 400] [-Keep]` (то же, что кнопка Check package) копирует пакет в `%TEMP%\flydoom_clean` и проверяет:

1. файлы: `COPYING` (GPL v2), `SOURCE.txt` с 40-значным хэшем без неподставленных `{...}`, `THIRD_PARTY.txt` с названиями всех данных и атрибуцией мухи, `licenses/`, `version.txt` перечисляет файлы данных; нет WAD полной игры, PDB, файлов SDK; `doom1.wad` — тот самый (md5) и с лицензией рядом; `.fbrain` — файл формата (`FLYBRAIN`, версия 1, не менее 100 000 нейронов, секция `ATTRIB`);
2. иконка и поля exe (`FlyDoom`, версия как в `version.txt`);
3. запуск из чистой папки без аргументов (`--exit-after-frames`): код 0, рядом с exe ничего не изменилось, лог в `<LOCALAPPDATA>/FlyDoom/logs` без путей репозитория и без `[ERROR]`, строка `DOOM: the game data is ...`, в `APPDATA` ничего, скриншот кадра — картинка из многих цветов, а не чёрный экран;
4. каждый WAD пакета отдельно через `--iwad` с `-warp 1 1`: игра берёт именно его и доходит до первого уровня (на машине с DOOM из Steam запуск без аргументов может выбрать его, поэтому WAD пакета проверяются по `--iwad`);
5. падение (`MYENGINE_CRASH_TEST=1`): `.dmp` (больше 10 КБ) и `.txt` (с «access violation», версией и местом `FlyDoom.exe+0x…`) в `crashes` и маркер `pending.txt`; следующий запуск пишет в лог «the previous run crashed» с именем отчёта, показывает красную плашку на 12 секунд («The game crashed last time. Send crash-….txt from %LOCALAPPDATA%\FlyDoom\crashes») и снимает маркер.

Мозг мухи проверяется как файл; когда N1 научит игру читать `.fbrain`, к проверке добавится строка лога о загрузке.

## Иконка, версия и ресурсы exe

`app/resources/RocketArena.ico` (7 размеров, 16..256 пикселей, PNG внутри) рисует `tools/gen_rl_icon.py` (машина Pulse и мяч на тёмно-синем фоне меню; `python tools/gen_rl_icon.py --preview out.png` показывает картинку). Иконка закоммичена, сборка скрипт не запускает. `app/resources/RocketArena.rc.in` превращается CMake в `RocketArena.rc` в папке сборки: иконка с id 1 (Проводник и панель задач, а `Window.cpp` берёт её же для окна), `VERSIONINFO` (название «Rocket Arena», описание, версия файла и продукта `x.y.z`, коммит в `ProductVersion`, оригинальное имя файла). Оба exe (`RocketArena.exe` и `myengine.exe`) получают ресурсы со своими названиями. `package_game.ps1` останавливается, если версия в exe не совпала с версией проекта (папку сборки надо переконфигурировать), а `check_package.ps1` проверяет иконку и поля.

Папка с ресурсами: `app/resources/` (иконка и шаблон `.rc`); версия и коммит в `Version.h` (`cmake/Version.h.in`, генерируется в `<build>/generated/myengine/core/`).

## Падения: minidump и тост

`engine/include/myengine/core/CrashHandler.h`, подключён первой строкой `wWinMain` (`main.cpp`). Что ловится: необработанные исключения (`SetUnhandledExceptionFilter`: обращение по null, деление на ноль, необработанный C++ `throw`), `std::terminate`, `abort()`, чисто виртуальный вызов, неверный параметр CRT. Что пишется в `%LOCALAPPDATA%\RocketArena\crashes\`:

- `crash-<дата>-<время>.dmp`: minidump (стеки всех потоков, список модулей, исключение; 70-250 КБ), через `dbghelp.dll` из System32, который подгружается только при падении. С сегментами данных библиотек дамп этой игры весит около 40 МБ: для присылки другом это много, поэтому их нет; если для разбора понадобится больше, добавьте `MiniDumpWithDataSegs` в `CrashHandler.cpp`;
- `crash-<дата>-<время>.txt`: версия и коммит, время, исключение (код, `RocketArena.exe+0x…`), последние 40 строк лога (лог при следующем запуске перезаписывается, поэтому его конец копируется сюда; при падении в первые миллисекунды лога ещё нет);
- `pending.txt`: маркер, пишется последним; в нём путь к `.txt` и одна строка о причине.

Папка `crashes` создаётся вместе с родителями (первый же запуск, упавший до создания `%LOCALAPPDATA%\RocketArena`, тоже оставляет отчёт). Игрок видит окно «Rocket Arena has stopped working» с папкой отчёта. При следующем запуске, на первом главном меню, игра находит маркер, удаляет его и показывает красный тост: «The game crashed last time. Send crash-….txt from %LOCALAPPDATA%\RocketArena\crashes» (12 с); в лог идёт строка `Game UI: the previous run crashed: …`. Тост показывается один раз на падение. Отчёт и дамп лежат, пока игрок их не удалит.

**Steam забирает обработчики себе.** `SteamAPI_Init` (его вызывает экран Private match, когда ему нужен Steam) пишет в консоль «Setting breakpad minidump AppID = 480» и ставит свой обработчик необработанных исключений (на машине разработки порядок не проверялся без `Reassert`: он дешёвый и страхует от замены). Поэтому экраны игры каждый кадр зовут `crash::Reassert()` (`RlGameFlow::Draw`): один атомарный обмен фильтра, и если на месте оказался чужой, все обработчики (фильтр, `std::terminate`, `abort`, чисто виртуальный вызов, неверный параметр CRT) возвращаются. Проверяется в `check_package.ps1` падением через 10 с после открытия экрана Steam (`MYENGINE_CRASH_TEST=2`).

Чтобы дамп можно было читать, shipping-сборка линкуется с `/DEBUG` (PDB с именами функций, без исходных строк) и `/PDBALTPATH:%_PDB%` (в exe имя файла PDB без пути репозитория). PDB не входит в пакет: скрипт кладёт его в `dist/symbols/RocketArena-<версия>-<steam|nosteam>.pdb`. Открыть дамп: Visual Studio (File > Open > `.dmp`, «Debug with Native Only», путь к символам = папка с PDB) или WinDbg `.sympath`.

Для проверки есть переменные окружения (работают в любой сборке): `MYENGINE_CRASH_TEST=1` роняет процесс в самом начале `wWinMain` (чтение по нулевому адресу), `MYENGINE_CRASH_TEST=2` роняет его через 10 с после первого кадра экранов игры (уже после старта Steam), `MYENGINE_CRASH_NO_DIALOG=1` не показывает окно (для скриптов). Не покрыто: переполнение стека (фильтр запускается на почти исчерпанном стеке и может не успеть), убийство процесса извне, зависание, сбой драйвера GPU (драйвер сам перезапускается, игра пишет «device removed» в лог).

## Проверка на чистой папке

`tools/check_package.ps1 [-WithoutScripting] [-Frames 600] [-Keep]` копирует `dist/RocketArena` в `%TEMP%\rocket_clean`, запускает `RocketArena.exe --exit-after-frames 600` из другой рабочей папки с подменёнными `LOCALAPPDATA` и `APPDATA` и проверяет:

1. процесс завершился сам с кодом 0;
2. рядом с exe ничего не появилось, не изменилось и не исчезло (снимок размеров и времени до и после);
3. лог лежит в `<LOCALAPPDATA>\RocketArena\logs`, в нём нет путей репозитория (`Documents\GitHub`, `.team`, `build-shipping`, корень репозитория), нет строк `[ERROR]`, есть загрузка банка звуков и эффектов;
4. в `APPDATA` ничего не записано;
5. с `-WithoutScripting` то же без `python314.zip`: игра работает без скриптинга (`ScriptRuntime` предупреждает и продолжает).

Дальше (G2b) скрипт проверяет ещё:

6. иконку (`RT_GROUP_ICON` 1) и поля версии exe: «Rocket Arena», описание, версия как в `version.txt`;
7. падение: запуск с `MYENGINE_CRASH_TEST=1` заканчивается кодом `0xC0000005`, в `crashes` появляются `.dmp` (больше 20 КБ) и `.txt` (с «access violation», версией и местом `RocketArena.exe+0x…`) и маркер; следующий обычный запуск пишет в лог «the previous run crashed» и снимает маркер;
8. Steam (только в пакете со Steam): рядом лежат `steam_api64.dll` и `steam_appid.txt`; запуск копии без `steam_appid.txt` (Steam в таком запуске не стартует, как при выключенном клиенте) проходит экраны выбора Steam: игра не падает, а в логе написано, почему Steam недоступен.

Результат последней проверки — в отчёте задачи `.team/tasks/g2.md`, G2b — `.team/tasks/g2b.md`. Проверка на «машине без Visual Studio и без репозитория» — запуск того же zip на другом компьютере; здесь она заменена чистой папкой и независимыми `LOCALAPPDATA` и `APPDATA`.

## Steam

SDK в репозиторий не кладётся ([steam-setup.md](steam-setup.md), [rl-online.md](rl-online.md) раздел 7.4). `package_game.ps1` ищет его до долгой сборки в таком порядке: `-SteamSdk <папка>`, переменная окружения `STEAMWORKS_SDK_DIR` (или `STEAMWORKS_SDK`), первая строка локального файла `tools\steam_sdk.txt` (в репозиторий его не коммитим), папки `sdk` и `steamworks_sdk` рядом с репозиторием и выше по дереву (так находится `C:\Users\пк\Documents\GitHub\sdk`). Папка считается SDK, если в ней есть `public\steam\steam_api.h` и `redistributable_bin\win64\steam_api64.dll`. Явно заданный и неверный `-SteamSdk` останавливает скрипт.

- **SDK найден:** exe собирается с `MYENGINE_STEAM=ON`, рядом кладутся `steam_api64.dll` (из SDK) и `steam_appid.txt` со значением 480 (тестовый Spacewar), в zip добавляется `-steam`.
- **SDK не найден:** предупреждение и сборка без Steam в `build-shipping-nosteam/` (отдельная папка: сборка настроена на одно из двух), zip `-nosteam`. Игра говорит «в этой сборке Steam нет», LAN и IP работают.
- `-WithSteam` превращает отсутствие SDK в ошибку, `-NoSteam` отключает поиск.

Что получает игрок: Steam работает у обоих, если запущен клиент Steam, оба в друзьях и оба запускают игру из папки со `steam_appid.txt` (в Steam пишется «Играет в Spacewar»). Без клиента Steam (или без `steam_appid.txt`) экраны выбора Steam показывают причину («Steam is not available: …»), всё остальное (боты, LAN, IP) работает. Условия распространения SDK подтверждает владелец аккаунта: в zip попадает только `steam_api64.dll` из `redistributable_bin`, ничего другого из SDK.

## Звук: Wwise

По умолчанию пакет собирается с `-DMYENGINE_WWISE=ON`: игра играет через Wwise ([../engine/wwise.md](../engine/wwise.md)), библиотеки SDK линкуются в `RocketArena.exe` статически (конфигурация Release: библиотеки Wwise Release и `AK_OPTIMIZED`, без связи с Wwise Authoring и профайлера). SDK Wwise в репозитории нет, его ставит сам разработчик ([../engine/wwise.md](../engine/wwise.md), раздел «Установка SDK»; шаги Launcher — в корневом README.md, раздел «Wwise SDK»). Поэтому пакет собирается по-разному, в зависимости от того, есть ли SDK на машине (скрипт ищет его в самом начале, до долгой сборки: `-WwiseSdk`, затем `WWISE_SDK_DIR` в кэше папки сборки, затем переменная `WWISESDK`):

| Что на машине | Что делает скрипт |
| --- | --- |
| SDK найден, `-NoWwise` не задан | пакет с Wwise (как описано ниже); в консоли строка `Wwise SDK: <папка>` |
| SDK не найден, `-NoWwise` не задан | заметное предупреждение (чего не нашёл, что собирается без Wwise, куда смотреть: README.md, раздел «Wwise SDK») и пакет с XAudio2, как с `-NoWwise`: WAV вместо банков, папка сборки с суффиксом `-nowwise`, zip `…-nowwise.zip`, `wwise no` в `version.txt` |
| задан `-NoWwise` | пакет с XAudio2, SDK не ищется и не нужен |
| `-WwiseSdk <папка>` указывает не на SDK | ошибка: в папке нет `include\AK\SoundEngine\Common\AkSoundEngine.h` |

`check_package.ps1` принимает оба варианта пакета (по `version.txt`). Версия SDK скриптом не проверяется: её проверяет CMake при настройке сборки (другая серия — ошибка, другой выпуск 2025.1 — предупреждение).

В пакете с Wwise рядом с exe **нет** ни одной DLL Wwise (библиотеки линкуются статически), а в пакете нет ни `.lib`, ни `.h`, ни другого файла SDK: скрипт проверяет это и останавливается, если такой файл попал в `dist`.

Что кладётся и что нет (раздел `wwise` в `games/rocketarena/game.json`):

| Что | С Wwise (по умолчанию) | С `-NoWwise` |
| --- | --- | --- |
| `assets/audio/wwise/Init.bnk`, `RocketArena.bnk` | да (скрипт останавливается, если банка нет: `tools\wwise\build_wwise_project.ps1`) | нет |
| `assets/audio/*.wav` | нет: `AudioSystem` на Wwise их не открывает (`LoadSound` ничего не делает, звуки лежат в банках) | да |
| `assets/data/audio_bank.json` | да: по нему движок берёт имена событий и шины | да |
| `assets/audio/manifest.json` | нет (описание генератора WAV, в игре не читается) | нет |
| `licenses/` | + `Wwise` в `THIRD_PARTY_NOTICES.txt` (© Audiokinetic, используется по лицензии), `Ogg-Vorbis-Xiph-LICENSE.txt`, `Opus-LICENSE.txt`, `OpusFile-LICENSE.txt` | как раньше |
| `version.txt` | строка `wwise yes` | `wwise no` |

Декодеры Vorbis и Opus входят в статические библиотеки Wwise, поэтому их лицензии (Xiph.Org, BSD) лежат в `licenses/`; тексты скрипт копирует из папки `Licenses` установленного Wwise (рядом с папкой `SDK`, это файлы `OggLicense.txt`, `OpusLicense.txt`, `OpusFileLicense.txt`), в репозитории их копий нет; строка о Wwise — из `tools/package_licenses_wwise/Wwise-NOTICE.txt`. Строка «XAudio2 and XInput come with Windows» в пакете с Wwise говорит только про XInput.

Папка сборки настроена на одно из двух: без `-BuildDir` у пакета с `-NoWwise` своя папка с суффиксом `-nowwise` (`build-shipping-nosteam-nowwise/`). Явный `-BuildDir`, настроенный с другим значением `MYENGINE_WWISE`, даёт ошибку «use another folder or delete it». `check_package.ps1` по `version.txt` понимает, какой это пакет: у пакета с Wwise он ждёт в логе `Audio: Wwise …` и `Wwise banks from …: N events` (N — число событий `audio_bank.json` из самого пакета, сейчас 22), не ждёт банка XAudio2, считает ошибкой любую строку `WARN`/`ERROR` про банк или Wwise, и проверяет файлы: банки есть, WAV и файлов SDK нет, лицензии кодеков на месте.

**Размер** (пакет без Steam, версия 0.1.0): с Wwise папка 56.1 МБ, zip 22.3 МБ; с `-NoWwise` папка 54.3 МБ, zip 21.4 МБ. Банки Wwise (5.7 МБ, PCM без сжатия) почти равны WAV, которые они заменяют, плюс библиотеки Wwise в exe. Размер банков зависит от настроек сжатия проекта Wwise ([../engine/wwise-project.md](../engine/wwise-project.md)).

## Лицензии

`licenses/` собирается из `tools/package_licenses/` (`THIRD_PARTY_NOTICES.txt`, `OFL-1.1.txt` для шрифтов Inter и JetBrains Mono) и из лицензий в репозитории (Python, Jolt, FreeType, pybind11, Lucide); с Wwise добавляются лицензии кодеков и строка об Audiokinetic (раздел «Звук: Wwise»). Тексты MIT для RocketSim, Dear ImGui, ImGuizmo, nlohmann/json, DirectXTK и DirectXTex приведены в `THIRD_PARTY_NOTICES.txt` общим текстом со ссылками на источники: авторские строки по годам нужно сверить с репозиториями проектов.

## Ограничения

- Игровой режим (`startInGame`) идёт без панелей редактора (G1).
- Личные клавиши, настройки и свой `rl_tuning.json` в shipping лежат в `UserDataDirectory()` (`%LOCALAPPDATA%\RocketArena`): их пишут экраны игры и окно RL Tuning (G1).
- `PrefabLibrary` пишет в лог предупреждение, что `assets/prefabs` нет: префабы редактора в дистрибутив не входят, на игру это не влияет.
- PDB shipping-сборки содержит только имена функций (компилятор не получает `/Zi`), без исходных строк; для строк нужны `/Zi` у всех целей и пересборка. PDB хранится в `dist/symbols` и подходит только к той сборке, из которой он взят: храните его вместе с отправленным zip.
- Подпись кода и установщик не делались: zip распаковывается и запускается; SmartScreen может предупредить о неподписанном exe.
