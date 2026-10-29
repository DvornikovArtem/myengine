# Wwise в сборке и AudioSystem на Wwise (W1, W3)

Пошаговые рецепты (включить Wwise, добавить или поменять звук, разобрать ошибку): [wwise-guide.md](wwise-guide.md). Этот документ описывает, как всё устроено.

Звуковой движок Audiokinetic Wwise 2025.1.12 собирается вместе с движком (W1), запускается и останавливается в нужном порядке, а при `-DMYENGINE_WWISE=ON` через него играет `AudioSystem` (W3, раздел [AudioSystem на Wwise](#audiosystem-на-wwise)). Код: `cmake/Wwise.cmake`, `engine/src/audio/wwise/`, тесты `tests/audio/WwiseRuntimeTests.cpp` и `tests/audio/WwiseAudioTests.cpp`, SDK Wwise в репозитории нет: его ставит сам разработчик (раздел [Установка SDK](#установка-sdk)).

## Зачем это нужно

По решению проекта Wwise заменит звук на XAudio2 ([audio.md](audio.md)): шины, затухание, события и микс будут настраиваться в проекте Wwise, а не в коде и JSON-банке. W1 — первый шаг: движок собирается с установленным SDK, звуковое ядро стартует и гаснет без падений и утечек, и это проверяет тест.

Что не тронуто: код игры и `audio_bank.json`. При `MYENGINE_WWISE=OFF` весь игровой звук работает, как раньше (XAudio2). Проект Wwise (папка `wwise/`) и скрипт сборки банков — отдельная задача, описана в [wwise-project.md](wwise-project.md). Подключение `AudioSystem` к Wwise (W3) описано ниже.

## Что сделано

| Что | Где | За что отвечает |
| --- | --- | --- |
| Модуль CMake | [cmake/Wwise.cmake](../../cmake/Wwise.cmake) | опция `MYENGINE_WWISE`, поиск установленного SDK, проверка версии и платформы, цели `myengine_wwise` и `myengine_wwise_io` |
| Подключение к сборке | [CMakeLists.txt](../../CMakeLists.txt), [engine/CMakeLists.txt](../../engine/CMakeLists.txt) | `include(Wwise)`; при `ON` в движок добавляются исходники рантайма, линковка (PRIVATE) и макрос `MYENGINE_WWISE=1` |
| Рантайм | [engine/src/audio/wwise/WwiseRuntime.h](../../engine/src/audio/wwise/WwiseRuntime.h), [WwiseRuntime.cpp](../../engine/src/audio/wwise/WwiseRuntime.cpp) | запуск и остановка звукового ядра, `RenderAudio(seconds)`, `SetBankDirectory`, `LoadBank`, офлайн-режим без устройства, ошибки SDK в `core::Logger` |
| AudioSystem на Wwise (W3) | [engine/src/audio/wwise/WwiseAudioSystem.cpp](../../engine/src/audio/wwise/WwiseAudioSystem.cpp) | реализация `audio::AudioSystem` на Wwise; при `ON` вместо `AudioSystem.cpp` и бэкендов XAudio2 |
| Заголовок AudioSystem | [engine/include/myengine/audio/AudioSystem.h](../../engine/include/myengine/audio/AudioSystem.h), [engine/src/audio/AudioSystem.cpp](../../engine/src/audio/AudioSystem.cpp) | один заголовок для обеих сборок: `VoiceCount()` не inline, закрытое состояние `WwiseState` (в XAudio2-сборке пустое) |
| Тесты | [tests/audio/WwiseRuntimeTests.cpp](../../tests/audio/WwiseRuntimeTests.cpp), [tests/audio/WwiseAudioTests.cpp](../../tests/audio/WwiseAudioTests.cpp), [tests/audio/AudioTests.cpp](../../tests/audio/AudioTests.cpp), [tests/CMakeLists.txt](../../tests/CMakeLists.txt) | `audio_wwise_runtime` (рантайм) и `audio_wwise` (AudioSystem); оба регистрируются только при `ON`; в `audio` часть про микшер XAudio2 за `#ifndef MYENGINE_WWISE` |
| SDK | установленный Wwise (Audiokinetic Launcher), не в репозитории | заголовки, библиотеки и файлы низкоуровневого IO из примеров SDK читаются там, где установлены; CMake их не копирует и не распаковывает |

## Сборка

Опция `MYENGINE_WWISE` по умолчанию `OFF`, пока игра не играет через Wwise: тогда CMake ничего не ищет, сборка такая же, как была.

```bash
cmake -S . -B build -DMYENGINE_WWISE=ON
cmake --build build --config RelWithDebInfo
```

Нужен установленный SDK Wwise (раздел ниже). Без него `MYENGINE_WWISE=ON` не соберётся: CMake остановится с сообщением, чего не нашёл и что делать. Wwise Authoring нужен только тому, кто правит звуки. В проекте отключена автоперегенерация CMake, поэтому после правки CMake-файлов папку сборки надо переконфигурировать вручную (повторить первую команду).

### Установка SDK

SDK принадлежит Audiokinetic и лежит не в репозитории, а на машине каждого разработчика (как Steamworks SDK для `-SteamSdk`). Короткая инструкция с шагами Audiokinetic Launcher — в [корневом README.md](../../README.md), раздел «Wwise SDK»; здесь главное.

1. Аккаунт Audiokinetic и Audiokinetic Launcher; в Launcher на странице *Wwise* выбрать версию **2025.1.12.9287** (*Install New Version*), нажать *Install*.
2. Пакеты и платформы: **SDK (C++)** обязательно; платформа **Microsoft / Windows / Visual Studio 2026** (в SDK это папка `x64_vc180`) для сборки Visual Studio 2026 и/или **Microsoft / Windows / Visual Studio 2022** (`x64_vc170`) для Visual Studio 2022: отметить ту, что соответствует вашей Visual Studio, или обе; пакет **Authoring** — только чтобы править проект `wwise/RocketArena` (для сборки и игры не нужен); плагины не нужны.
3. В Launcher у установленной версии в меню выбрать *Set Environment Variables* (он предлагает `WWISEROOT` и `WWISESDK`) и перезапустить терминал и IDE, иначе процессы не увидят `WWISESDK`.

Версия **ровно 2025.1.12.9287**: банки репозитория собраны ею. Другой выпуск 2025.1 — предупреждение CMake (банки одного выпуска могут не загрузиться в другом), другая серия — ошибка.

Лицензия: условия Audiokinetic, каждый разработчик принимает их сам при установке. Пробный режим без ключа — только для некоммерческих проектов и до 200 медиафайлов в SoundBank (у нас 24); бесплатная некоммерческая лицензия без этого предела выдаётся при регистрации проекта (подробности и ссылки — в README).

### Какой SDK берёт CMake

Порядок поиска:

1. `-DWWISE_SDK_DIR=<папка установки>/SDK` — подойдёт и папка установки без `SDK` (CMake добавит его сам);
2. переменная окружения `WWISESDK` (её ставит Launcher, шаг 3);
3. иначе `FATAL_ERROR`: что не найдено, какая версия и платформа нужны, ссылка на этот раздел и README.

Проверяется:

- **Версия** из `include/AK/AkWwiseSDKVersion.h` сверяется с `MYENGINE_WWISE_VERSION_*` в `cmake/Wwise.cmake`. Серия не 2025.1 — ошибка. 2025.1, но не ровно 2025.1.12.9287 — предупреждение.
- **Платформа выбирается по компилятору:** `MSVC_VERSION` 1950 и выше (v145, Visual Studio 2026) — `x64_vc180`; 1930–1949 (v143, Visual Studio 2022) — `x64_vc170`; другой компилятор — ошибка с перечнем поддерживаемых Visual Studio. Библиотеки читаются прямо из `<SDK>/<платформа>/{Debug,Profile,Release}/lib`, без копирования и распаковки. Если папки нет, ошибка называет именно нужную платформу (Microsoft / Windows / Visual Studio 2026 или 2022), которую надо отметить в Launcher. Выбранная платформа печатается в строке `Wwise …` при настройке.
- **Файлы IO** (`AkDefaultIOHookDeferred` и остальные) собираются из `<SDK>/samples/SoundEngine/{Win32,Common}`.

Путь к SDK остаётся в папке сборки (кэш CMake и `.vcxproj`) и в репозиторий не попадает.

## Какие библиотеки в какой конфигурации

| Конфигурация проекта | Библиотеки Wwise | Замечание |
| --- | --- | --- |
| Debug | Debug | с профилировщиком и связью с Authoring |
| RelWithDebInfo | Profile | оптимизированные, с профилировщиком и связью |
| Release, MinSizeRel | Release | `AK_OPTIMIZED`: без связи и монитора; такая и shipping-сборка `tools/package_game.ps1` |

Линкуются `AkSoundEngine`, `AkMemoryMgr`, `AkStreamMgr`, `AkVorbisDecoder`, `AkOpusDecoder` и `CommunicationCentral` (только Debug и Profile), а из системных — `ws2_32`, `Msacm32`, `Dbghelp`, `Winmm`. Папка платформы (`x64_vc180` для v145, `x64_vc170` для v143) с динамической CRT; варианты «(StaticCRT)» не берём. `AkMusicEngine.lib` в этой версии нет: музыкальный движок внутри `AkSoundEngine`. Spatial Audio не используется.

Целей две:

- `myengine_wwise` — SDK: заголовки как `SYSTEM` (к ним не применяется `/W4`), библиотеки конфигурации, системные библиотеки, определение `AK_OPTIMIZED` для Release и MinSizeRel;
- `myengine_wwise_io` — исходники низкоуровневого IO из примеров SDK (`samples/SoundEngine`: файловый хук для Stream Manager, расположение файлов банков, рабочие потоки). Они не наши, поэтому компилируются без `/W4 /WX` и с `UNICODE`: файлы примеров вызывают общие имена Windows API с `wchar_t`.

## WwiseRuntime

Внутренний класс `myengine::audio::wwise::WwiseRuntime`. Типы SDK из `engine/include/` не видны: рантайм подключается из `engine/src/audio`, а линковка с Wwise у движка `PRIVATE`. Макрос `MYENGINE_WWISE=1` движок определяет публично, чтобы остальной код (тесты) знал, что рантайм собран.

Порядок `Initialize`:

1. Memory Manager.
2. Stream Manager.
3. Устройство Stream Manager и низкоуровневое IO (читает файлы банков из папки банков).
4. Job workers (2 потока).
5. Sound Engine; кодеки Vorbis и Opus регистрируются через `AK_STATIC_LINK_PLUGIN`.
6. Communications (связь с Authoring: Profiler, Remote Connect) — только Debug и Profile; сбой запуска — предупреждение в лог, игра продолжает работать.

`Shutdown` идёт в обратном порядке; деструктор вызывает его сам.

| Метод | Что делает |
| --- | --- |
| `Initialize(папка банков, withDevice, withCommunication = true)` | запускает ядро; `false`, если что-то не стартовало (уже запущенное останавливается). Несуществующая папка банков не ошибка: об этом скажет первый `LoadBank` (в лог идёт предупреждение) |
| `Shutdown()` | останавливает всё, что запущено; повторный вызов ничего не делает |
| `RenderAudio(seconds = 0)` | один звуковой кадр: события и вызовы из очереди обрабатываются. С устройством `seconds` игнорируется; без устройства (офлайн) вызов рендерит `seconds` звука (0 — один буфер) |
| `SetBankDirectory(папка)` | ещё одна папка банков (прежние остаются в поиске, новая ищется первой); `false`, если ядро не запущено. Несуществующая папка — предупреждение в лог |
| `IsOffline()` | запущено ли ядро без устройства, то есть время идёт только в `RenderAudio(seconds)` |
| `LoadBank(имя)` | банк из папки по имени (`Init`, `Main`, с `.bnk` или без); `false` и строка в лог, если не загрузился |
| `IsInitialized()` | запущено ли ядро |

Подробности:

- Без устройства (`withDevice = false`) ядро работает офлайн: `AkInitSettings::bOfflineRendering`, потока движка Wwise нет, ничего не играет. Звуковое время идёт только внутри `RenderAudio(seconds)` и на потоке, который его вызвал (`SetOfflineRenderingFrameTime`), поэтому конец события приходит внутри этого вызова, а тесты не ждут реальных часов (тесты, `MYENGINE_AUDIO=0`, машины без звука).
- Звуковое ядро одно на процесс (состояние Wwise глобальное): второй `Initialize` при работающем первом возвращает `false` и пишет ошибку, первый не страдает.
- Ошибки SDK идут в `core::Logger` (с именем результата `AK_...`), исключений нет. В Debug и Profile сообщения монитора Wwise тоже попадают в лог: ошибки как `Error`, остальное как `Debug`.
- Связь с Authoring слушает сеть. По умолчанию параметр `withCommunication` включён, но `AudioSystem` запускает рантайм с выключенной связью и включает её только по `MYENGINE_WWISE_PROFILER=1`; тесты её тоже выключают.

## AudioSystem на Wwise

`audio::AudioSystem` — тот же класс и те же вызовы, что в XAudio2-сборке, поэтому `RlFeedback`, `FxSystem`, `RlGameSystem` и `Application` не меняются. При `MYENGINE_WWISE=ON` его реализация лежит в [engine/src/audio/wwise/WwiseAudioSystem.cpp](../../engine/src/audio/wwise/WwiseAudioSystem.cpp) и заменяет `AudioSystem.cpp`, `NullAudioBackend.cpp` и `XAudio2Backend.cpp` (это перечислено в [engine/CMakeLists.txt](../../engine/CMakeLists.txt)). Событие из `audio_bank.json` — это событие `Play_<имя>` проекта Wwise ([wwise-project.md](wwise-project.md)).

### Как включить

```bash
cmake -S . -B build -DMYENGINE_WWISE=ON
cmake --build build --config RelWithDebInfo
```

Код игры не меняется. При `OFF` всё как раньше: играет XAudio2. Замеры (W4) сделаны ([ниже](#стабильность-и-замеры)); XAudio2 остаётся в `OFF`-сборке, а полный переход на Wwise (W6) будет отдельной задачей позже.

### Вызов AudioSystem и что делает Wwise

| Вызов `AudioSystem` | Что происходит в Wwise |
| --- | --- |
| `Initialize` | запускается `WwiseRuntime` (`withDevice` = `config.useDevice`); слушатель — game object 1 с `SetDefaultListeners`; `SetRandomSeed(config.randomSeed)`; создаётся пул слотов голосов |
| `LoadBank(audio_bank.json)` | из JSON берутся имена событий и их шина, `loop`, `spatial`, `priority`, `doppler`; банки `Init` и `RocketArena` грузятся из папки `wwise` рядом со звуками проекта: `<directory из json>/wwise`; поиск такой же, как у файлов XAudio2 (как задано, от рабочей папки, затем от папки файла и трёх уровней выше) |
| `HasEvent` | есть ли имя в банке из JSON |
| `PlayEvent` | слот голоса, свой game object, позиция, RTPC `Volume` и `Pitch`, `PostEvent("Play_<имя>")` с `AK_EndOfEvent` (подробнее ниже) |
| конец события | колбэк из потока Wwise кладёт номер объекта в очередь под мьютексом; `Update` освобождает слот и снимает регистрацию game object. Всё остальное звуковое ядро вызывается только из игрового потока |
| `Stop(fade)`, `StopAll`, `StopBus` | `StopPlayingID` с затуханием. Во время затухания `IsPlaying` уже `false`, а слот освобождается по таймеру затухания (не ждёт колбэка) |
| `IsPlaying` | слот жив и не затухает |
| `SetVoiceVolume`, `SetVoicePitch` | RTPC `Pitch` на game object голоса; RTPC `Volume` = `v` / громкость события (см. «Громкость голосов») |
| `SetVoicePosition` | `SetPosition` объекта голоса |
| `SetBusVolume`, `SetBusMuted` | глобальные RTPC `Bus_Master`, `Bus_Sfx`, `Bus_Music`, `Bus_Ui`; `mute` ставит 0. Значение выше 1 хранится для `GetBusVolume`, а в Wwise ограничено 1 |
| `GetBusVolume`, `IsBusMuted` | сохранённые значения; настройки, сделанные до загрузки банков, применяются при `LoadBank` |
| `SetListener` | запоминает слушателя; применяется в `Update`: `SetPosition` слушателя |
| `Update(dt)` | порядок: слушатель → позиции голосов «за слушателем» и доплер → `RenderAudio(dt)` → очередь завершений |
| `GetStats` | `voicesActive`, `voicesPeak`, `played`, `stolen`, `dropped` с теми же смыслами, что у XAudio2; `voicesTotal` — размер пула слотов, то есть `monoVoices + stereoVoices`, но не меньше 8; `soundsLoaded` — число загруженных банков (2); `soundBytes` — размер файлов `.bnk` на диске; `deviceName` — «Wwise: the default device» или «Wwise: offline, no device» |

`PlayEvent` по шагам:

1. Находится слот. Если пул полон, уступает слабейший голос (см. «Пул и лимиты»).
2. Слот получает свой game object. Позиция: `hasPosition ? position : позиция слушателя`; такой голос без позиции следует за слушателем.
3. RTPC `Volume` = `volumeScale`, `Pitch` = `pitchScale` × доплер.
4. `PostEvent` события `Play_<имя>` с колбэком `AK_EndOfEvent`.
5. Если Wwise не принял событие, слот свободен, `dropped++`, предупреждение в лог один раз на имя.

### Вызовы, которые при Wwise не имеют смысла

`LoadSound`, `CreateSound`, `UnloadSound`, `GetSoundInfo`, `Play(SoundHandle)`, `GetVoiceGain` явно отказывают: невалидный handle, `false` или `0` и одно предупреждение в лог на вызов. Альтернатива была в том, чтобы не объявлять их при Wwise. Выбран отказ, потому что заголовок `AudioSystem.h` один и тот же при `ON` и `OFF`: нет проблем с ODR, тесты XAudio2 и игра собираются одинаково. Игра этими вызовами не пользуется (её звуки — события).

### Пул и лимиты

- Приоритеты берутся из JSON. При полном пуле уступает слабейший голос: с низшим `priority`, при равенстве — самый старый, но только если его приоритет не выше, чем у нового звука. Иначе новый звук отбрасывается, `dropped++`.
- `maxInstances`, случайные `pitch` и `volume`, затухания теперь живут в проекте Wwise (Playback Limit, Randomizer). Код их не дублирует.

### Доплер

Доплер считает код (`ComputeSpatial` из [AudioSpatial.h](../../engine/include/myengine/audio/AudioSpatial.h)), а не Wwise: в проекте Wwise доплера нет. Он применяется к голосам с позицией: `hasPosition` и 3D-событие, либо вызван `SetVoicePosition`. RTPC `Pitch` = заданный pitch × доплер, значение пересчитывается в `Update` по движению слушателя.

### Слушатель и оси

В заголовках SDK сказано, что система координат левосторонняя, слушатель по умолчанию смотрит по +Z, верх +Y, а (1, 0, 0) — вправо. Это совпадает с миром игры (`right = up × forward`, как в `AudioSpatial.cpp`), поэтому оси не переворачиваются. `forward` и `up` слушателя ортонормируются перед `SetPosition`, иначе Wwise отклонит позицию.

Что звук справа звучит справа, численно не проверено: захват выхода в офлайн-режиме не заработал. Проверять на слух.

### Офлайн-время

Без устройства (тесты, `MYENGINE_AUDIO=0`) `Update(dt)` рендерит `dt` секунд звука синхронно (`SetOfflineRenderingFrameTime`). Одноразовый звук заканчивается через столько вызовов `Update(dt)`, сколько он длится; тесты не ждут реальных часов.

### Профайлер Wwise Authoring

Связь с Wwise Authoring (Profiler, Remote Connect) открывает сетевой порт. Поэтому по умолчанию она выключена, а включается переменной окружения `MYENGINE_WWISE_PROFILER=1`. Связь есть только в библиотеках Debug и Profile (Debug и RelWithDebInfo; Release её не имеет). Иначе брандмауэр Windows спрашивал бы разрешение при каждом запуске.

Что проверено без человека: `Comm::Init` и `Comm::Term` исполняются в Debug и в RelWithDebInfo (тест `audio_wwise_runtime` с `MYENGINE_WWISE_TEST_COMM=1` печатает `communication with the authoring: up`, а `audio_wwise` с `MYENGINE_WWISE_PROFILER=1` пишет `profiler link ...: up`; в логе строка `Wwise: the communication with the authoring started`). Что НЕ проверено: подключение самого Wwise Authoring к игре (его проверяет человек, шаги ниже), и окно брандмауэра Windows при первом запуске нового exe: показывается ли оно, из консоли не видно.

#### Как подключить Wwise Authoring к игре (шаги для человека)

1. Собрать игру с Wwise (`cmake -S . -B build -DMYENGINE_WWISE=ON`, затем `--config RelWithDebInfo`) и запустить с переменной:

   ```powershell
   $env:MYENGINE_WWISE_PROFILER = "1"
   build\app\RelWithDebInfo\myengine.exe --game
   ```

   Windows может спросить разрешение для `myengine.exe` в сети: разрешить для частной сети.
2. В Wwise Authoring открыть проект `wwise\RocketArena\RocketArena.wproj`.
3. Переключить раскладку на **Profiler** (меню `Layouts`, клавиша F6).
4. В панели `Remote Connections` (кнопка соединения в верхней части Profiler или `Layouts` / `Remote Connections`) выбрать компьютер с игрой: имя приложения `myengine` на этой же машине (`localhost`) и нажать `Connect`.
5. Начать матч в игре. В Authoring должны появиться: **Capture Log** (события `Play_*`, их остановки, предупреждения), **Voices Graph / Voice Monitor** (голоса матча: моторы, буст, мяч, трибуны; число совпадает с `AudioStats::voicesActive` игры), **Performance Monitor** (CPU звука, память), **Game Object Profiler** (по одному game object на голос) и шкалы шин `Bus_Master`, `Bus_Sfx`, `Bus_Music`, `Bus_Ui`.
6. Меню Settings → Audio меняет громкости шин: в Authoring видно, как двигаются эти шкалы и значения RTPC `Bus_*` (`Game Parameter Monitor`).

Если игра не видна в списке: проверить, что переменная задана в том же окне, из которого запущена игра, и что брандмауэр не блокирует `myengine.exe`; сеть между компьютерами не нужна, подключение идёт на тот же компьютер.

### Громкость голосов

`SetVoiceVolume(v)` задаёт итоговую громкость голоса, как в XAudio2-сборке (там он заменяет громкость события): RTPC `Volume` = `v` / громкость события. Громкость события — поле `volume` из `audio_bank.json`, её же даёт проект Wwise (`engine` 0,5, `crowd_ambience` 0,35), а RTPC `Volume` умножается на неё; делить нужно только на неё, потому что разброса громкости у событий нет. Поэтому `RlFeedback`, который ставит абсолютные значения (мотор 0,7 и 1,0, буст 0,8 и 1,0, занос до 0,8, трибуны 0,35 и 1,0), звучит при Wwise так же, как при XAudio2. `PlayEvent` не делит: `Volume` = `volumeScale`, а громкость события, как и раньше, даёт проект.

Предел: кривая `Volume` в проекте идёт от 0 до 4 (+12 дБ), самое большое отношение у `crowd_ambience` (1 / 0,35 ≈ 2,86). Положительные дБ Wwise применяет, а не обрезает: пик шины Sfx у стационарной петли мотора растёт вместе с параметром до его значения 4 (это проверяет тест `audio_wwise`).

### Тест audio_wwise

Регистрируется только при `ON`, работает без устройства и без окна ([tests/audio/WwiseAudioTests.cpp](../../tests/audio/WwiseAudioTests.cpp)). Таймаут в ctest — 300 секунд.

```bash
cmake --build build --config RelWithDebInfo --target myengine_wwise_audio_tests
ctest --test-dir build -C RelWithDebInfo -R audio_wwise$ --output-on-failure
```

Что проверяет:

- банки грузятся;
- каждое событие из JSON: `HasEvent` и валидный `PlayEvent`;
- одноразовый звук завершается примерно за длину своего WAV и освобождает слот;
- петля продолжается и останавливается `Stop` с затуханием;
- неизвестное событие даёт невалидный handle; устаревший handle не трогает новый голос;
- `StopBus(Music)` трогает только музыку;
- `SetBusVolume` и `SetBusMuted` меняют глобальные RTPC (через `AK::SoundEngine::Query`);
- `SetVoicePosition`, `SetVoicePitch` и доплер доходят до объекта и его RTPC; `SetVoiceVolume(v)` даёт `Volume` = `v` / громкость события (1 / 0,35 у `crowd_ambience`, 1,4 у `engine` при 0,7), и пик шины Sfx у петли мотора растёт вместе с этим значением, до +12 дБ без обрезания;
- событие без позиции стоит у слушателя и следует за ним;
- пул и приоритеты;
- ограничение `maxInstances` проекта: слоты возвращаются;
- два цикла `Initialize` и `Shutdown`.

Переменные окружения:

| Переменная | Что делает |
| --- | --- |
| `MYENGINE_WWISE_TEST_DEVICE=1` | добавляет проверку с настоящим устройством: тихо (мастер 0), реальное время, колбэки из потока Wwise |
| `MYENGINE_WWISE_TEST_LOG=1` | печатает лог теста |

Тест `audio` (G7c) при `ON` проверяет только чистые части (WAV, `AudioSpatial`): часть про микшер XAudio2 закрыта `#ifndef MYENGINE_WWISE`. Остальные тесты (`rl_feedback`, `rl_game` и другие) идут без изменений.

## Стабильность и замеры

Коротко; цифры и сценарии в документах рядом с замерами.

- **Стабильность:** тест `audio_wwise_stress` (только при `ON`, без устройства, офлайн, в ctest около 15 секунд): 14 700 событий за 4.5 минуты звука с полным пулом, устаревшие и чужие handle, двойной `Stop`, `Stop` во время затухания, `LoadBank` дважды, пустые и обрезанные банки, 20 циклов запуска и остановки. Память Wwise и число game object после каждого сценария возвращаются к исходным. Найдено и исправлено: обрезанный банк вешал синхронный `LoadBank` (без своего потока движка Wwise ждёт `RenderAudio` вызвавшего потока): `LoadBank` теперь асинхронный с ожиданием, которое сам рендерит, а файл банка, который не цел по размерам кусков, отвергается до Wwise. Сценарии и числа: [stability.md](../measurements/wwise/stability.md).
- **Замеры XAudio2 и Wwise** на одной сцене (матч 2v2, настоящее устройство, три прогона на сторону, Tracy): [README.md](../measurements/wwise/README.md). Главное: звук в игровом потоке 0.2-0.4 % кадра у обеих реализаций, время кадра одинаково в пределах шума; Wwise тратит на 3 % CPU процесса больше (свои потоки), рабочий набор на 8 МБ меньше.
- **Память Wwise** видна в `AudioStats::engineMemoryBytes` и в Tracy (`Wwise memory, bytes`, `Wwise voices`); `engineObjects` показывает зарегистрированные game object голосов (в Release-библиотеках SDK память не считается и равна 0). Tracy-зоны: `Audio::Update`, `Audio::PlayEvent`, `Wwise::RenderAudio`, `Audio::BackendUpdate` (у XAudio2).

## Проверка

Тест `audio_wwise_runtime` (`tests/audio/WwiseRuntimeTests.cpp`) регистрируется только при `-DMYENGINE_WWISE=ON`:

```bash
cmake --build build --config RelWithDebInfo --target myengine_wwise_runtime_tests
ctest --test-dir build -C RelWithDebInfo -R audio_wwise_runtime --output-on-failure
```

Что проверяет:

- до запуска вызовы безвредны: `RenderAudio`, `Shutdown` ничего не делают, `LoadBank` возвращает `false`;
- запуск без устройства, 200 звуковых кадров, остановка; повторный `Shutdown` — пустая операция;
- два цикла подряд у одного рантайма и новый рантайм после остановки старого;
- второй рантайм при работающем первом не стартует и не мешает первому, а после остановки первого стартует;
- несуществующая папка банков, пустая папка, несуществующий банк, пустое имя, имя с `.bnk`: ошибки без падений, `LoadBank` после остановки возвращает `false`;
- в логе есть строки запуска, остановки и ошибок загрузки банков.

`MYENGINE_WWISE_TEST_COMM=1` добавляет цикл с Communications. Она слушает сеть, поэтому Windows может спросить про файрвол; по умолчанию цикл не идёт. Тест не открывает окно и ничего не играет. Таймаут в ctest — 120 секунд.

Рекомендуется прогонять Debug и RelWithDebInfo (разные библиотеки), а при обновлении SDK — обязательно оба.

## Что в репозитории и почему

- **SDK нет.** Заголовки, библиотеки и исходники примеров Wwise — собственность Audiokinetic под их лицензией, поэтому в репозитории лежит только наш код, который использует Wwise (`cmake/Wwise.cmake`, `engine/src/audio/wwise/`, тесты), проект `wwise/RocketArena` и собранные банки `assets/audio/wwise/*.bnk` (это наш контент). Установка — раздел [Установка SDK](#установка-sdk).
- **Лицензии кодеков в пакете.** `tools/package_game.ps1` при упаковке копирует тексты Ogg, Opus и OpusFile из папки `Licenses` установленного Wwise (рядом с папкой `SDK`), а не из репозитория. Путь к SDK берётся из `-WwiseSdk`, из `WWISE_SDK_DIR` в кэше папки сборки или из `WWISESDK`. Если SDK не найден, скрипт предупреждает и собирает пакет с XAudio2, как с `-NoWwise` ([packaging.md](../game/packaging.md), раздел «Звук: Wwise»).
- **Обновление SDK.** Поставить новую версию Wwise в Launcher (SDK (C++), платформа Windows для вашей Visual Studio: 2026 и/или 2022), поправить `MYENGINE_WWISE_VERSION_*` в `cmake/Wwise.cmake`, прогнать `audio_wwise_runtime`, `audio_wwise` и `audio_wwise_stress` в Debug и RelWithDebInfo и пересобрать банки той же версией Wwise.

## Ограничения и что дальше

- Проект Wwise (папка `wwise/`) и банки собираются скриптом, описанным в [wwise-project.md](wwise-project.md).
- Опция `MYENGINE_WWISE` остаётся `OFF` по умолчанию, пока игра не играет через Wwise.
- Направление звука справа/слева проверено только по заголовкам SDK, не по выходу: это надо оценить на слух.
- XAudio2 остаётся в `OFF`-сборке. Замеры (W4) сделаны; полный переход на Wwise (W6) будет отдельной задачей позже.
