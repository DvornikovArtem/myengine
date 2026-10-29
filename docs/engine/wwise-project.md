# W2: проект Wwise и банки звуков

Пошаговые рецепты (добавить звук, поменять звук, разобрать ошибку скрипта): [wwise-guide.md](wwise-guide.md). Этот документ описывает, как устроены проект и скрипт.

Wwise 2025.1.12.9287 заменит XAudio2 как звуковой движок игры. Эта задача превращает `assets/data/audio_bank.json` в проект Wwise и банки. В сборке с `-DMYENGINE_WWISE=ON` игра играет через эти банки (раздел «AudioSystem на Wwise» в [wwise.md](wwise.md#audiosystem-на-wwise)); без опции она играет через XAudio2 ([audio.md](audio.md)).

## Зачем это нужно

События игры описаны в `audio_bank.json`: модуль `audio` играет их по имени. Чтобы перейти на Wwise, те же события, шины и параметры нужны в виде проекта Wwise и готовых банков (`.bnk`). Руками это не делается: скрипт строит проект из того же json через WAAPI, поэтому проект и банк игры всегда можно собрать заново из одного источника.

## Что сделано

| Что | Где | За что отвечает |
| --- | --- | --- |
| Скрипт сборки | [tools/wwise/build_wwise_project.ps1](../../tools/wwise/build_wwise_project.ps1) | создаёт проект из `audio_bank.json`, генерирует банки, проверяет результат |
| Проект Wwise | [wwise/RocketArena/](../../wwise/RocketArena/) (`RocketArena.wproj`, Work Unit `.wwu`) | события, звуки, шины, затухания, Game Parameters, SoundBank. Лежит в корне репозитория, не в `assets` |
| Копии звуков | [wwise/RocketArena/Originals/SFX/](../../wwise/RocketArena/Originals/SFX/) | wav, которые Wwise скопировал при импорте |
| Банки для игры | [assets/audio/wwise/Init.bnk](../../assets/audio/wwise/Init.bnk), [assets/audio/wwise/RocketArena.bnk](../../assets/audio/wwise/RocketArena.bnk) | то, что загрузит игра |
| Правила git проекта | [wwise/RocketArena/.gitignore](../../wwise/RocketArena/.gitignore) | шаблон Wwise плюс `GeneratedSoundBanks/` и `.p4ignore` |

В git лежат: `.wproj`, Work Unit (`.wwu`), `Originals/SFX`, банки. SDK и Authoring в git нет. Локальные файлы Wwise (`.cache`, `*.validationcache`, `*.wsettings`, `GeneratedSoundBanks/`) в `.gitignore` проекта и в git не попадают.

Размеры: `RocketArena.bnk` 5 994 995 байт, `Init.bnk` 2 734 байта. 22 события, 24 media (24 wav; `manifest.json` не звук).

## Как запустить

Нужен Windows PowerShell 5.1 и установленный Wwise. Пакеты ставить не нужно.

```
powershell -ExecutionPolicy Bypass -File tools\wwise\build_wwise_project.ps1 -Rebuild
```

| Режим | Что делает |
| --- | --- |
| `-Rebuild` | удаляет `wwise/RocketArena`, создаёт проект заново, наполняет, генерирует банки, проверяет |
| `-BanksOnly` | только пересобирает банки существующего проекта (после правок руками в Authoring) и проверяет |
| `-Verify` | проверка без изменений; код возврата 1 при расхождении |

Без режима скрипт печатает подсказку и выходит с кодом 2. Режимы нельзя совмещать.

Параметры:

- `-WwiseRoot` — папка, в которой есть `Authoring\x64\Release\bin\WwiseConsole.exe`. Если не задан, берётся `$env:WWISEROOT`, затем `HKCU\Environment\WWISEROOT` (сессия, запущенная до установки Wwise, переменную не видит, а реестр её хранит).
- `-HttpPort` — порт сервера WAAPI, по умолчанию 8090; нужен только режиму `-Rebuild`. Если на порту уже кто-то отвечает (например, открыт Authoring), скрипт останавливается с ошибкой.

## Как это устроено

Шаги `-Rebuild`:

1. `WwiseConsole create-new-project` создаёт пустой проект (платформа Windows). В `.gitignore` проекта дописываются `GeneratedSoundBanks/` и `.p4ignore`.
2. `WwiseConsole waapi-server` запускается в фоне. Скрипт вызывает WAAPI по HTTP: POST на `127.0.0.1:<порт>/waapi`, в каждом запросе нужны поля `uri`, `options` и `args`.
3. Через WAAPI создаются шины, Game Parameters, затухания, звуки (импорт wav), свойства звуков, события, SoundBank. В конце `project.save`, сервер останавливается.
4. Нормализация идентификаторов (ниже).
5. `WwiseConsole generate-soundbank` пишет банки во временную папку. В `assets/audio/wwise` копируются только два `.bnk`. Метаданные Wwise (`.json`, `.txt`) не копируются: в них пути машины, и игре они не нужны. Перед копированием папка `assets/audio/wwise` очищается.
6. Проверка (как `-Verify`).

### Поля audio_bank.json и объекты Wwise

| Поле `audio_bank.json` | Объект или свойство Wwise |
| --- | --- |
| событие `x` | Event `Play_x` с одним действием Play; цель — верхний объект события в `Containers\Default Work Unit` |
| `file` / `files` из одного элемента | Sound `x` с источником-файлом. Имя звука — имя события, не имя файла |
| `files` из нескольких элементов | Random Container `x`, внутри Sound с именами файлов. Режим Standard (`NormalOrShuffle` 0), Random, «не повторять последний» (`RandomAvoidRepeatingCount` 1) |
| `bus`: `sfx` / `music` / `ui` | Output Bus (с `OverrideOutput`) на шину `Sfx` / `Music` / `Ui` под `Main Audio Bus`. Все три шины создаются всегда. В Wwise 2025.1 корневая шина называется Main Audio Bus (раньше Master Audio Bus) |
| `volume`, `volumeMin`, `volumeMax` | Volume в дБ: `20·log10` от среднего `volumeMin` и `volumeMax`. Если `volumeMax` > `volumeMin` — Randomizer на Volume |
| `pitch`, `pitchMin`, `pitchMax` | Pitch в центах: `1200·log2` от середины диапазона (не задаётся, если середина 0). Если `pitchMax` > `pitchMin` — Randomizer на Pitch на разброс ± от середины |
| `loop` | у Sound: `IsLoopingEnabled` и бесконечный цикл. У Random Container: `PlayMechanismLoop`, бесконечно |
| `maxInstances` > 0 | Playback Limit: `UseMaxSoundPerInstance`, `MaxSoundPerInstance` = N, `IsGlobalLimit` = 1 (глобально по всей игре, а не на Game Object). Поведение при заполнении (`MaxReachedBehavior`, `OverLimitBehavior`) скрипт не задаёт: остаются значения Wwise по умолчанию (0 у обоих). Совпадает ли это с поведением движка («при заполнении уходит самый старый звук»), не проверялось: это видно в Authoring в блоке Playback Limit; как AudioSystem освобождает слоты голосов, которые Wwise завершил по лимиту, описано в разделе «AudioSystem на Wwise» ([wwise.md](wwise.md#audiosystem-на-wwise)) |
| `priority` | `OverridePriority` и `Priority` = то же число. У события без `priority` — 0 |
| `spatial`, `minDistance`, `maxDistance`, `rolloff` | `OverridePositioning`, `3DSpatialization` = Position, `3DPosition` = Emitter, `EnableAttenuation` и Attenuation ShareSet `Att_<min>_<max>` |
| `doppler` | не переносится |

Подробности:

- **Pitch.** Базовое значение `pitch` отдельно не переносится: оно только подставляется как значение по умолчанию и в `pitchMin`, и в `pitchMax`, то есть базовый pitch входит в диапазон Randomizer.
- **Затухания.** Один ShareSet на каждую уникальную пару `minDistance`/`maxDistance`; сейчас их 7: `Att_3_50`, `Att_3_60`, `Att_3_70`, `Att_4_70`, `Att_4_90`, `Att_4_100`, `Att_6_120`. Кривая `VolumeDry`: 0 дБ до `min`, затем закон `(min / distance)^rolloff` дискретно точками (шаг по расстоянию ×1.6), на последней пятой пути к `max` закон умножается на `1 - smoothstep`, в точке `max` — тишина (−200 дБ). `RadiusMax` = `max`. Единицы Wwise — метры, масштаб 1:1. Это та же формула, что в `AudioSpatial.h` движка.
- **События без `spatial`** (`goal_horn`, `crowd_roar`, `crowd_ambience`, `countdown_*`, `ui_*`, `whistle`, `menu_music`): скрипт позиционирование не задаёт. Эти события в Wwise двумерные (3DSpatialization по умолчанию None). Обратный случай: событие с `spatial`, которое движок играет без позиции (`hasPosition = false`, например свой мотор и свои пады), в Wwise остаётся трёхмерным, поэтому AudioSystem на Wwise играет такие события на Game Object в позиции слушателя ([wwise.md](wwise.md#audiosystem-на-wwise)).

### Game Parameters

Шесть параметров, у всех начальное значение 1:

| Параметр | Диапазон | На что действует |
| --- | --- | --- |
| `Pitch` | 0.25 .. 4 | RTPC на свойство Pitch трёх шин Sfx, Music, Ui. Кривая: отношение частот → центы `1200·log2`, от −2400 до +2400 центов (предел Wwise) |
| `Volume` | 0 .. 4 | RTPC на свойство Volume трёх шин. Кривая `20·log10`: 0 → −200 дБ, 4 → +12.04 дБ |
| `Bus_Master` | 0 .. 1 | RTPC на `BusVolume` у Main Audio Bus |
| `Bus_Sfx`, `Bus_Music`, `Bus_Ui` | 0 .. 1 | RTPC на `BusVolume` своей шины |

Кривые — ломаные, все точки Linear. Смысл: `Pitch` и `Volume` действуют на все звуки (AudioSystem ставит их на Game Object каждого голоса), `Bus_*` — настройки громкости из меню.

### SoundBank

Один банк `RocketArena` со всеми событиями: включение `\Events\Default Work Unit` с фильтрами `events`, `structures`, `media`. Медиа лежат внутри банка, потоковых `.wem` нет. `Init.bnk` генерирует сам Wwise.

Медиа не сжаты: проект оставляет PCM (Default Conversion Settings). Поэтому банк 5.99 МБ при 5.8 МБ wav, и движку не нужен кодек. Чтобы уменьшить банк, можно выбрать Vorbis в Authoring, но тогда в SDK понадобится декодер Vorbis.

### Одинаковый результат при пересборке

Wwise даёт каждому объекту случайные GUID, ShortID и MediaID. Из-за этого два `-Rebuild` различались бы во всех Work Unit и в банках. После наполнения скрипт заменяет эти идентификаторы на производные от пути имён объекта (MD5):

- фабричные Work Unit (`Factory *.wwu`) сохраняют свои объекты; меняется только ID самого Work Unit;
- встроенная шина Main Audio Bus остаётся как есть;
- `ObjectRef` и `ActiveSource` следуют за определением;
- папка `.cache` удаляется (она знает старые идентификаторы и создаётся заново при следующей загрузке).

Результат проверен: два `-Rebuild` подряд дают побайтно одинаковые файлы проекта и банки; `-BanksOnly` даёт тот же банк. Различий в проекте и банках не остаётся.

## Лицензия

Без ключа Wwise работает как Trial: некоммерческий проект, до 200 media в банках (у нас 24). Ключи не вводим.

## Проверка

`-Verify` (и конец `-Rebuild`, `-BanksOnly`) проверяет без изменений:

- есть проект и оба банка; в `assets/audio/wwise` нет ничего кроме `.bnk`;
- каждое из 22 событий `Play_<имя>` есть в `RocketArena.bnk`: в HIRC банка ищется объект Event с FNV-1 32-бит от имени в нижнем регистре, как у `AK::SoundEngine::GetIDFromString`;
- число media в банке равно числу уникальных файлов в `audio_bank.json` (24), в банке есть данные (чанк DATA);
- все 6 Game Parameters есть в `Game Parameters\Default Work Unit.wwu`, и их идентификаторы есть в банках (кроме звуковых данных).

```
powershell -ExecutionPolicy Bypass -File tools\wwise\build_wwise_project.ps1 -Verify
```

Проект открывается командой `WwiseConsole verify --abort-on-load-issues` без миграции и без изменения файлов.

## Правки руками в Authoring

1. Открыть `wwise/RocketArena/RocketArena.wproj`.
2. После правок сохранить проект (Ctrl+S) и лучше закрыть Authoring, чтобы банк собрался ровно из сохранённых файлов. Порт WAAPI здесь не нужен: `-BanksOnly` и `-Verify` сервер не поднимают, порт 8090 нужен только `-Rebuild`.
3. Запустить `-BanksOnly`: банки в `assets/audio/wwise` обновятся.
4. Закоммитить изменения `.wwu` и `.bnk` вместе.

`-Rebuild` стирает ручные правки: проект создаётся заново из `audio_bank.json`.

## Чего скрипт не делает

- Не подключает Wwise к игре (это делает AudioSystem, см. [wwise.md](wwise.md#audiosystem-на-wwise)) и не копирует SDK.
- Не создаёт Stop-события: цикл останавливается через `StopPlayingID`/`StopAll` в движке.
- Не переносит доплер, не создаёт реверберации и шины эффектов, не делает потоковых файлов.
- Не генерирует `SoundbanksInfo.json` и заголовок `Wwise_IDs.h`: имена хэшируются в движке (`AK::SoundEngine::PostEvent("Play_x")` считает FNV сам).
- Только платформа Windows, язык SFX (без озвучки).
- События с `spatial`, которые движок играет без позиции, остаются в Wwise трёхмерными: AudioSystem на Wwise играет их на Game Object в позиции слушателя (см. выше).

Среди файлов проекта нет путей машины: единственное совпадение при поиске по имени разработчика Wwise — шаблон встроенного запроса в `Queries/Factory Queries.wwu`, это не путь.
