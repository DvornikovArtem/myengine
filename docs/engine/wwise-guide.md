# Звук на Wwise: практическое руководство

Пошаговые рецепты для того, кто пришёл работать со звуком: включить Wwise, добавить звук, поменять звук, найти причину сбоя. Здесь нет описания устройства (оно в [wwise.md](wwise.md) и [wwise-project.md](wwise-project.md)), только что делать и в каком порядке. Все команды выполняются из корня репозитория в PowerShell. Рецепты проверены на копии репозитория вне рабочей папки (раздел «Что проверено»).

## 1. С чего начать

Документы читать в таком порядке:

| Документ | О чём | Когда читать |
| --- | --- | --- |
| [README.md](../../README.md), раздел 4.1 | как поставить SDK Wwise (набор библиотек и заголовков) через Audiokinetic Launcher и собрать с ним | первым, если Wwise ещё не стоит |
| этот файл | рецепты: включить, добавить звук, поменять звук, разобрать ошибку | вторым и потом как шпаргалка |
| [wwise-project.md](wwise-project.md) | как `audio_bank.json` превращается в проект Wwise и банки; режимы скрипта; что скрипт не переносит | перед тем как менять звуки |
| [wwise.md](wwise.md) | сборка, поиск SDK, `AudioSystem` на Wwise, тесты, профайлер, стабильность | перед тем как менять код звука |
| [audio.md](audio.md) | общий интерфейс `AudioSystem`, таблица полей `audio_bank.json`, сборка на XAudio2 | всегда, когда пишете вызов звука |
| [packaging.md](../game/packaging.md), раздел «Звук: Wwise» | что кладётся в zip игры и какие лицензии | перед сборкой пакета |
| [measurements/wwise/](../measurements/wwise/README.md) | замеры Wwise против XAudio2 и стабильность | по желанию |

Как устроен звук в одной картинке:

```
assets/audio/*.wav  ──┐
                      ├─ assets/data/audio_bank.json   одно описание событий (имя, файлы, шина, громкость, 3D, лимиты)
                      │
   XAudio2-сборка ────┘  читает json и wav, играет сама
   Wwise-сборка          читает из json только имена, шину, цикл, приоритет;
                         сам звук берёт из банков assets/audio/wwise/*.bnk,
                         а банки собирает tools/wwise/build_wwise_project.ps1 из того же json
```

`audio_bank.json` — единственный источник правды: из него получаются и проект Wwise (`wwise/RocketArena`), и банки. Игровой код об этом не знает: он зовёт `AudioSystem::PlayEvent("имя")` и в той, и в другой сборке.

## 2. Включить Wwise и вернуться на XAudio2

Установленный Wwise сам по себе ничего не меняет: пока опция `MYENGINE_WWISE` выключена (`OFF`, это значение по умолчанию), CMake не ищет SDK, а сборка и звук такие же, как до Wwise. Единственное исключение — скрипт упаковки `tools\package_game.ps1`: он сам ищет SDK и, если нашёл, собирает пакет с Wwise (раздел 5).

### Две папки сборки (удобнее всего)

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DMYENGINE_COPY_RUNTIME_ASSETS=ON
cmake -S . -B build-wwise -G "Visual Studio 18 2026" -A x64 -DMYENGINE_WWISE=ON -DMYENGINE_COPY_RUNTIME_ASSETS=ON
cmake --build build --config Debug --target myengine
cmake --build build-wwise --config Debug --target myengine
```

(`"Visual Studio 17 2022"`, если Visual Studio 2026 не установлена.) `build` играет через XAudio2, `build-wwise` через Wwise. Папки не мешают друг другу, переключаться между ними не нужно. SDK CMake ищет в `-DWWISE_SDK_DIR=<папка SDK>`, затем в переменной окружения `WWISESDK` (её ставит Launcher). Какую папку SDK ставить и какие платформы отметить в Launcher — в README, раздел 4.1.

### Одна папка: переключить туда и обратно

```powershell
cmake -S . -B build -DMYENGINE_WWISE=ON     # на Wwise
cmake -S . -B build -DMYENGINE_WWISE=OFF    # назад на XAudio2
```

Значение остаётся в кэше папки сборки, поэтому остальные ключи повторять не нужно. После переключения пересобирается почти весь движок (макрос `MYENGINE_WWISE` виден всему коду, а реализация `AudioSystem` подменяется целиком), то есть это полная пересборка. Поэтому две папки удобнее. В проекте отключена автоперегенерация CMake: после любой правки файлов CMake команду настройки надо повторить вручную.

### Как понять по логу, что играет

Лог игры лежит в `<папка сборки>/app/<конфигурация>/logs/myengine.log`, например `build-wwise/app/RelWithDebInfo/logs/myengine.log`. Строки про звук:

| Сборка | Что в логе |
| --- | --- |
| Wwise | `Wwise: started (audio device, banks: )`, затем `Audio: Wwise: the default device, 64 voices`, затем `Audio: Wwise banks from assets/audio/wwise: 22 events` |
| XAudio2 | `Audio: device default output, 8 channels, 48000 Hz, 64 voices` (число каналов и частота зависят от устройства), затем `Audio: sound bank <путь>/audio_bank.json: 22 events, 24 sounds` |
| любая, без звуковой карты или `MYENGINE_AUDIO=0` | XAudio2: `Audio: simulated, …`; Wwise: `Audio: Wwise: offline, no device …`; звука нет, остальное работает |

Число после `banks from` — число событий в `audio_bank.json`. Если строки `Audio: Wwise …` нет, а есть `Audio: device …`, вы запустили exe из папки, собранной без Wwise.

## 3. Добавить новый звук

Пример: звук `test_blip`, который играет на шине интерфейса. Формат файла ограничивает загрузчик XAudio2-сборки (`engine/src/audio/WavFile.cpp`): моно или стерео, частота от 8 до 192 кГц, PCM 8, 16, 24 или 32 бита либо float 32. Файл вне этих границ в сборке с `OFF` не загрузится (в логе будет причина вроде `the sample rate … is out of range`). Wwise при импорте строже в одном: **8-битный wav он не принимает** (скрипт падает на шаге `properties of the events` с `WAAPI ak.wwise.core.object.set failed: … Object \Containers\Default Work Unit\<имя> is unknown`, потому что звук не импортировался); 16, 24 и 32 бита PCM, float 32 и стерео Wwise принимает (проверено запусками `-Rebuild`). Берите 16-битный PCM, 44.1 или 48 кГц, как у всех звуков проекта. Звук с `spatial` делайте моно: стерео-файл играет на стерео-голосе без панорамы, только с затуханием. Банк хранит звук несжатым, поэтому длинный или 24-битный стерео-файл раздувает `RocketArena.bnk` (и каждый коммит банка) на свой размер.

1. **Положить wav** в `assets/audio/`, например `assets/audio/test_blip.wav`.
2. **Добавить событие** в `assets/data/audio_bank.json`, в объект `events` (запятая после предыдущего события обязательна):

   ```json
   "test_blip": { "file": "test_blip.wav", "bus": "ui", "volume": 0.6 },
   ```

   Минимум: `file` (или `files` для нескольких вариантов, из них выбирается случайный) и `bus` (`sfx`, `music` или `ui`). Остальные поля (`loop`, `spatial`, `minDistance`, `maxDistance`, `rolloff`, `priority`, `maxInstances`, разброс `volumeMin/Max` и `pitchMin/Max`) и их значения по умолчанию — в [audio.md](audio.md), раздел «Банк звуков». Что из них Wwise берёт в проект — в разделе 4 ниже.
3. **Собрать проект и банки.** Нужен установленный Wwise (пакет Authoring, WwiseConsole без окна):

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\wwise\build_wwise_project.ps1 -Rebuild
   ```

   Скрипт пересоздаёт `wwise/RocketArena` из json (около 15 секунд), генерирует `assets/audio/wwise/Init.bnk` и `RocketArena.bnk` и сам проверяет результат. В конце должно быть `verify OK: 23 events, 6 Game Parameters, banks in place` (число событий — на одно больше прежнего). Ошибки входа проверяются до удаления проекта: нет wav (`event 'test_blip': no file …`) или шина не из трёх (`unknown bus`) — скрипт остановится, ничего не тронув.
4. **Вызвать из кода** `PlayEvent` по имени. Образец — `RlFeedback` (`engine/src/game/RlFeedback.cpp`) и `FxSystem`:

   ```cpp
   auto& audio = myengine::audio::GetAudioSystem();

   audio.PlayEvent("test_blip");                      // событие без позиции: меню, интерфейс, общий звук

   myengine::audio::EventParams params;               // событие в мире
   params.hasPosition = true;
   params.position = {x, y, z};
   params.volumeScale = 0.8f;                         // умножается на громкость события
   params.pitchScale = 1.0f;
   const myengine::audio::VoiceHandle voice = audio.PlayEvent("test_blip", params);
   // для цикла сохраните voice и позже вызовите audio.Stop(voice, 0.1f)
   ```

   Без кода событие можно проиграть и со сцены: компонент `AudioSource` с полем `event` ([audio.md](audio.md), «Компонент сцены»).
5. **Проверить.**
   - Лог (раздел 2): строка `Audio: Wwise banks from …: 23 events`, нет строк `[WARN]` и `[ERROR]` про звук.
   - Скрипт ещё раз без изменений: `tools\wwise\build_wwise_project.ps1 -Verify` (код возврата 1, если события нет в банке).
   - Тест: он берёт каждое событие из json, проверяет `HasEvent` и что `PlayEvent` вернул живой голос, который доиграл и освободил слот. Если банк забыли пересобрать, тест падает строкой `FAIL the event test_blip plays (a valid handle)`.

     ```powershell
     cmake --build build-wwise --config Debug --target myengine_wwise_audio_tests
     ctest --test-dir build-wwise -C Debug -R audio_wwise$ --output-on-failure
     ```

   - Профайлер Wwise Authoring, если нужно увидеть голоса и громкости: раздел 5.
6. **Закоммитить вместе** (если коммитить только часть, игра и банк разойдутся):
   - `assets/audio/test_blip.wav`;
   - `assets/data/audio_bank.json`;
   - `wwise/RocketArena/**`: изменённые `Containers/Default Work Unit.wwu` и `Events/Default Work Unit.wwu` и копия wav `wwise/RocketArena/Originals/SFX/test_blip.wav`, которую сделал Wwise;
   - `assets/audio/wwise/RocketArena.bnk` (`Init.bnk` не меняется, пока не меняется состав Game Parameters и шин).

   Локальные файлы Wwise (`.cache`, `*.validationcache`, `*.wsettings`, `GeneratedSoundBanks/`) в `.gitignore` проекта, в коммит не идут. Каждый новый банк добавляет в историю около 6 МБ: не коммитьте банк после каждой мелкой правки, а только итоговую версию.

Без Wwise (`MYENGINE_WWISE=OFF`) тот же звук играет XAudio2: он читает тот же `audio_bank.json` и тот же wav. Пересобирать банки для него не нужно, но коммитить банк всё равно надо, иначе сборка с Wwise не услышит новое событие.

Zip игры (`tools/package_game.ps1`) менять не нужно: он берёт `audio_bank.json` и банки сам, а wav из пакета с Wwise исключает.

## 4. Поменять существующий звук

Два пути. Не смешивайте их.

> **Осторожно.** `-Rebuild` создаёт проект Wwise заново из `audio_bank.json` и **стирает любые правки, сделанные руками в Authoring**. Если вы подкрутили звук в Authoring, а потом кто-то (или вы сами) запустил `-Rebuild`, правки пропадут без предупреждения. Поэтому после ручных правок `-Rebuild` запускать нельзя, а у `-BanksOnly` такой опасности нет.

### Путь 1: через json и `-Rebuild` (по умолчанию)

Правите поле в `audio_bank.json`, запускаете `build_wwise_project.ps1 -Rebuild`, коммитите json, `.wwu` и `.bnk`. Один источник правды: XAudio2-сборка и Wwise-сборка получают одно и то же, и два человека не расходятся. Выбирайте его всегда, когда нужное поле есть в таблице.

| Поле json | В Wwise | Замечание |
| --- | --- | --- |
| `file` / `files` | Sound или Random Container с вариантами | один вариант не повторяется подряд |
| `bus` | выходная шина `Sfx`, `Music` или `Ui` | |
| `volume`, `volumeMin`, `volumeMax` | свойство Volume в дБ; при разных Min и Max ещё и Randomizer (разброс на каждый запуск) | |
| `pitch`, `pitchMin`, `pitchMax` | свойство Pitch в центах; при разных Min и Max ещё и Randomizer | базовый `pitch` отдельно не переносится: он входит в диапазон разброса |
| `loop` | бесконечный цикл | остановить можно `Stop`; Stop-событий в проекте нет |
| `spatial`, `minDistance`, `maxDistance`, `rolloff` | включённое 3D-позиционирование и затухание по расстоянию (по одному общему затуханию на пару Min/Max) | формула та же, что в `AudioSpatial.h` |
| `priority` | приоритет голоса | при нехватке голосов уступает слабейший |
| `maxInstances` | Playback Limit (предел одновременных звуков события, общий на всю игру) | что делать при переполнении, скрипт не задаёт: остаётся умолчание Wwise, совпадение с поведением XAudio2 не проверялось |
| `doppler` | **не переносится** | эффект Доплера считает код движка и меняет Pitch сам; поле json по-прежнему читается |
| `directory` | не переносится | банки ищутся в `<directory>/wwise` |

Чего в json нет и поэтому скрипт не создаёт: Stop-события, реверберация и шины эффектов, потоковые файлы, состояния и переключатели, сжатие звука (банк хранит несжатый PCM). Полный список — в [wwise-project.md](wwise-project.md), раздел «Чего скрипт не делает».

### Путь 2: вручную в Authoring и `-BanksOnly`

Нужен только для того, чего нет в json (эффекты, свои кривые, сжатие). Пакет Authoring должен быть установлен.

1. Открыть `wwise/RocketArena/RocketArena.wproj` и внести правки.
2. Сохранить проект (Ctrl+S) и лучше закрыть Authoring, чтобы банк собрался ровно из сохранённых файлов. Порт 8090 здесь не нужен: он нужен только `-Rebuild`.
3. `powershell -ExecutionPolicy Bypass -File tools\wwise\build_wwise_project.ps1 -BanksOnly` пересоберёт банки существующего проекта и проверит их.
4. Закоммитить `.wwu` и `.bnk` вместе.

Что при этом нужно знать:

- `audio_bank.json` не меняется, и XAudio2-сборка правки не услышит: сборки Wwise и XAudio2 начнут звучать по-разному.
- Следующий `-Rebuild` (у вас или у соседа) вернёт проект к json. Поэтому если ручная правка должна жить, договоритесь с командой и либо научите скрипт её делать, либо не запускайте `-Rebuild`.
- Если правка укладывается в поле json, делайте её путём 1.

## 5. Профайлер и пакет игры

- **Профайлер Wwise Authoring** (голоса, громкости шин, события в реальном времени). Связь с Authoring открывает сетевой порт, поэтому по умолчанию выключена. Включается переменной `MYENGINE_WWISE_PROFILER=1` при запуске Debug или RelWithDebInfo; в Release связи нет. Шаги подключения — в [wwise.md](wwise.md), «Профайлер Wwise Authoring». Профайлер Tracy и замеры времени кадра — там же и в [measurements/wwise/](../measurements/wwise/README.md).
- **Zip игры.** `tools\package_game.ps1` по умолчанию собирает пакет с Wwise, если SDK найден; без SDK — с предупреждением и пакет с XAudio2. `-NoWwise` принудительно оставляет XAudio2. Что кладётся, какие лицензии, размеры — в [packaging.md](../game/packaging.md), раздел «Звук: Wwise».

## 6. Частые ошибки

Тексты ниже взяты из кода (`cmake/Wwise.cmake`, `engine/src/audio/wwise/`, `tools/wwise/build_wwise_project.ps1`) и из реальных запусков.

| Что видите | Причина | Что делать |
| --- | --- | --- |
| CMake: `MYENGINE_WWISE is ON, but the Wwise SDK is not found: WWISE_SDK_DIR is empty and the environment variable WWISESDK is not set.` | SDK не установлен или процесс не видит переменную | Поставить SDK (README, 4.1) и открыть новый терминал; или указать `-DWWISE_SDK_DIR=<папка>/Wwise_2025.1.12.9287/SDK` |
| CMake: `The Wwise SDK is not found: WWISE_SDK_DIR is '…', and it has no include/AK/SoundEngine/Common/AkSoundEngine.h` | указана не та папка | Нужна папка `SDK` внутри установки (папка установки тоже подойдёт, CMake допишет `SDK` сам) |
| CMake: `The Wwise SDK in … is 2024.1.x…, the project needs 2025.1 (exactly 2025.1.12.9287)` (это ошибка) | другая серия Wwise | Поставить в Launcher ровно 2025.1.12.9287 |
| CMake: предупреждение `The Wwise SDK in … is 2025.1.x.y, the SoundBanks of the project are made by 2025.1.12.9287: banks of one build may not load in another` | тот же 2025.1, но другой выпуск | Поставить 2025.1.12.9287. С чужим выпуском сборка идёт, но банки могут не загрузиться (признак: раздел про банки ниже) |
| CMake: `The Wwise SDK in … has no x64_vc170/Debug/lib/AkSoundEngine.lib: the platform Windows (Microsoft Visual Studio 2022 / x64_vc170, x64) … is not installed` (или `x64_vc180`, Visual Studio 2026) | в Launcher не отмечена платформа вашей Visual Studio | Launcher: Modify установки Wwise, отметить Microsoft / Windows / Visual Studio 2022 или 2026 (какая у вас), можно обе |
| В логе игры: `Audio: Wwise did not take the event Play_<имя> (the bank has no such event?)` (одно предупреждение на имя) | событие есть в `audio_bank.json`, а в банке его нет: банки не пересобрали или не закоммитили; либо опечатка в имени | `build_wwise_project.ps1 -Verify` покажет расхождение; затем `-Rebuild` (или `-BanksOnly`) и закоммитить `.bnk`. Событие не играет, игра работает дальше |
| В логе: `Audio: the folder of the Wwise banks (assets/audio/wwise) is not found next to …/audio_bank.json` | рядом с exe нет папки `assets/audio/wwise` | Пересобрать с копированием ассетов (`-DMYENGINE_COPY_RUNTIME_ASSETS=ON`) или проверить, что банки на месте; в пакете их кладёт `package_game.ps1` |
| В логе: `Wwise: the bank file …/RocketArena.bnk is not whole: …` (причина после двоеточия: файл не открывается, нет чанка `BKHD`, обрезан), затем `Audio: the Wwise banks did not load from …` | банка нет, файл обрезан или испорчен (например, прерванная распаковка) | Вернуть банк из git или пересобрать (`-BanksOnly`). Целостность проверяет сама игра, до передачи файла Wwise |
| В логе: `Wwise: the bank 'RocketArena' did not load: AK_… (число)` | файл целый, но Wwise его отвергает: обычно банк другого выпуска SDK | Установить SDK 2025.1.12.9287 или пересобрать банки той же версией |
| Скрипт: `WAAPI ak.wwise.core.object.set failed: … Object \Containers\Default Work Unit\<имя> is unknown` | Wwise не импортировал wav события (шаг `import` не сообщает об этом): чаще всего 8-битный файл | Пересохранить wav как 16-битный PCM (раздел 3, шаг 1) |
| Скрипт: `[wwise] ERROR: something already answers WAAPI on port 8090 (Wwise Authoring is open?). Close it or pass -HttpPort <other port>` | порт 8090 занят: чаще всего открыт Wwise Authoring | Закрыть Authoring или передать `-HttpPort 8091`. Порт нужен только режиму `-Rebuild` (он поднимает сервер); `-BanksOnly` и `-Verify` его не открывают |
| Скрипт: `[wwise] ERROR: waapi-server stopped at once (exit code …)` или `did not answer in three minutes` | порт занят программой, которая не WAAPI, либо Wwise не запустился | Прочитать лог по пути из сообщения (`rocketarena_waapi.out` во временной папке), сменить `-HttpPort` |
| Скрипт: `Wwise is not found: pass -WwiseRoot <dir> … or set WWISEROOT` | не найден WwiseConsole (нет пакета Authoring или не задан `WWISEROOT`) | Установить Authoring или передать `-WwiseRoot <папка установки Wwise>`. Скрипт смотрит ещё в реестр (`HKCU\Environment`), поэтому старому терминалу переменная нужна не всегда |
| Переменная `WWISESDK` пуста в терминале, хотя Launcher её поставил | процесс запущен до установки и получил старое окружение | Открыть **новый** терминал (и перезапустить IDE или приложение, из которого запускаете). Проверка: `$env:WWISESDK` в PowerShell и `[Environment]::GetEnvironmentVariable('WWISESDK','User')`: если вторая команда выводит путь, а первая пуста, дело в старом окружении. Либо передать `-DWWISE_SDK_DIR` |

## 7. Что проверено

Рецепт из раздела 3 пройден на копии репозитория вне рабочей папки, без окон Authoring и игры:

- добавлен wav (копия `ui_click.wav` под именем `test_blip.wav`) и событие `test_blip`, как в примере;
- `-Rebuild` за 13 секунд создал проект и банки, в конце `verify OK: 23 events, 6 Game Parameters`; `RocketArena.bnk` вырос с 5 994 995 до 6 000 520 байт, media стало 25 (копия того же файла считается отдельным media);
- отдельный `-Verify` прошёл; без wav скрипт останавливается до удаления проекта с `event 'test_blip': no file …`;
- от репозитория отличались только `Containers/Default Work Unit.wwu`, `Events/Default Work Unit.wwu`, `Originals/SFX/test_blip.wav`, `RocketArena.bnk` и локальные файлы Wwise из `.gitignore`; `Init.bnk` не менялся;
- на этой копии собрана Debug-сборка с `-DMYENGINE_WWISE=ON` и пройден тест `audio_wwise` (23 события в логе `Audio: Wwise banks from …: 23 events`). Обратная проверка: с прежним банком (без `test_blip`) тот же тест падает с `FAIL the event test_blip plays (a valid handle)` и в логе стоит `Audio: Wwise did not take the event Play_test_blip (the bank has no such event?)`. То есть тест видит и новое событие, и забытую пересборку банка;
- тексты ошибок CMake из таблицы раздела 6 получены настоящими запусками настройки с пустым `WWISESDK`, с несуществующей папкой, с папкой другой серии, без нужной платформы и с другим выпуском 2025.1 (поддельные папки SDK, только проверка настройки). Текст занятого порта 8090 взят из кода: воспроизвести его не удалось (тестовый ответчик на порту не перехватил запрос), зато проверено, что `-BanksOnly` порт не открывает.
