# Звук (G7c)

Модуль `engine/include/myengine/audio/`, реализация `engine/src/audio/`. XAudio2 2.9 (Windows 10+, без внешних DLL), но наружу не виден ни один тип DX и Windows. Всё, что нужно игре: шины, пул голосов, 3D, зацикленные звуки с pitch на лету, события из банка звуков. Работает и без звукового устройства: тогда голоса симулируются по времени (тесты, машины без звука).

Звуки процедурные: `tools/gen_rl_audio.py` (numpy + scipy) пишет `assets/audio/*.wav` и `manifest.json`; чужих сэмплов нет. Подключение к игре (`RlGameSystem`) — отдельная задача G7d; здесь движок, ассеты, компонент сцены, тесты и этот документ.

Звук игры переезжает на Wwise: проект и банки описаны в [wwise-project.md](wwise-project.md). При сборке с `-DMYENGINE_WWISE=ON` реализация `AudioSystem` — Wwise, подробности в [wwise.md](wwise.md). XAudio2 остаётся в сборке с `OFF` до замеров W4 и удаления (W6); всё описанное ниже относится к этой сборке.

## Состав

| Файл | Что |
| --- | --- |
| `AudioTypes.h` | `Bus`, `SoundHandle`, `VoiceHandle`, `PlayParams`, `EventParams`, `ListenerState`, `AudioConfig`, `AudioStats` |
| `AudioSystem.h` | `AudioSystem`: инициализация, звуки, банк, голоса, шины, слушатель, `Update` |
| `WavFile.h` | чтение WAV без устройства (PCM 8/16/24/32 и float 32, моно/стерео, любая частота → 16 бит) |
| `AudioSpatial.h` | 3D без устройства: затухание, панорама, доплер (чистые функции) |
| `src/audio/AudioBackend.h` | устройство за системой: `XAudio2Backend` и `NullBackend` |
| `ecs/components/FxComponents.h` | `AudioSourceComponent` (и `ParticleEmitterComponent`), играются `FxUpdateSystem` |

Глобальный экземпляр: `audio::GetAudioSystem()`. `Application` инициализирует его при старте (устройство, банк `assets/data/audio_bank.json`), останавливает при выходе; `MYENGINE_AUDIO=0` — без устройства. Сцена играет звук через компоненты, игра — через тот же экземпляр.

## API

```cpp
auto& audio = myengine::audio::GetAudioSystem();

// звуки и события
audio::SoundHandle click = audio.LoadSound("assets/audio/ui_click.wav");   // кэш по пути; невалидный хэндл, если файла нет
audio.LoadBank("assets/data/audio_bank.json");                             // события: имя -> варианты, шина, громкость, 3D, лимиты

// событие: случайный вариант, случайные pitch и громкость, шина и 3D из банка
audio::EventParams hit;
hit.hasPosition = true;  hit.position = {x, y, z};  hit.velocity = ballVelocity;
hit.volumeScale = std::clamp(speed / 40.0f, 0.2f, 1.0f);                   // сила удара
audio::VoiceHandle v = audio.PlayEvent("ball_hit", hit);

// цикл с изменением на лету: мотор
audio::VoiceHandle engine = audio.PlayEvent("engine", {true, carPosition});
audio.SetVoicePitch(engine, 0.7f + 1.1f * throttleRpm);                    // 0.05 .. 4
audio.SetVoicePosition(engine, carPosition, carVelocity);
audio.Stop(engine, 0.15f);                                                 // с затуханием

// шины и слушатель
audio.SetBusVolume(audio::Bus::Master, 0.8f);  audio.SetBusVolume(audio::Bus::Music, 0.5f);
audio.SetListener({cameraPosition, cameraForward, cameraUp, cameraVelocity});

audio.Update(deltaTime);                                                   // раз в кадр: освободить закончившиеся, применить 3D и затухание
```

`PlayParams` (для `Play(SoundHandle, params)` без банка): шина, громкость, `pitch`, `loop`, `priority`, `spatial`, `position`/`velocity`, `minDistance`/`maxDistance`/`rolloff`, `dopplerFactor`.

### Хэндлы голосов

`VoiceHandle` — слот пула и поколение: хэндл голоса, который закончился или был украден, недействителен, вызовы с ним ничего не делают (`IsPlaying` → false, `GetVoiceGain` → 0). Хранить хэндл цикла безопасно.

### Шины

`Master` умножает всё; голос играет на `Sfx`, `Music` или `Ui`. Громкость голоса = `volume × fade × 3D × bus × master`; `SetBusMuted` глушит шину (или всё через `Master`). `GetVoiceGain` показывает итог (для тестов и индикаторов). Шины реализованы как группы громкости, применяемые к голосу в `Update` и при изменении (без отдельных submix-голосов XAudio2).

### Пул голосов

При `Initialize` создаются `AudioConfig::monoVoices` (48) + `stereoVoices` (16) голосов — всего 64. Моно-звук берёт моно-голос, стерео (музыка, толпа) — стерео-голос. Если свободных нет, голос отдаёт самый слабый играющий того же формата, если его `priority` не выше, чем у нового звука: сначала меньший приоритет, потом тише, потом старше. Иначе новый звук отбрасывается. Счётчики `stolen` и `dropped` — в `GetStats()`. У события в банке есть `maxInstances`: при превышении новый звук забирает голос самого старого экземпляра этого события (удары мяча не копятся).

### 3D

Своя математика (`ComputeSpatial`), без X3DAudio, чтобы проверять без устройства:

- **Затухание:** полная громкость до `minDistance`, дальше `(minDistance / distance)^rolloff`, плавное затухание до нуля на последней пятой пути к `maxDistance`; дальше тишина.
- **Панорама:** проекция направления на «правый» вектор слушателя (`up × forward`), равная мощность (`cos`/`sin`); ближе `minDistance` панорама сужается.
- **Доплер:** `(c + v_слушателя_к_источнику) / (c + v_источника_от_слушателя)`, `c = 343 м/с`, предел 0.5…2, множитель `dopplerFactor` (0 — выключен).
- Моно-голоса панорамируются матрицей выхода XAudio2 (`SetOutputMatrix`); стерео только затухают.

### Зацикленные звуки и pitch

Цикл — бесшовный по построению (генератор делает целое число периодов, шум в частотной области, музыка заворачивает хвосты). Pitch — `SetFrequencyRatio` (0.05…4); частоты дискретизации, отличные от 44.1 кГц, проигрываются через то же отношение частот.

## Банк звуков

`assets/data/audio_bank.json`:

```json
{ "directory": "assets/audio",
  "events": {
    "ball_hit": { "files": ["ball_hit_1.wav", "ball_hit_2.wav", "ball_hit_3.wav"], "bus": "sfx", "volume": 0.9,
                  "pitchMin": 0.94, "pitchMax": 1.06, "spatial": true, "minDistance": 4, "maxDistance": 100, "priority": 8, "maxInstances": 4 } } }
```

| Поле | Значение по умолчанию | Что |
| --- | --- | --- |
| `file` / `files` | — | один файл или варианты (вариант не повторяется дважды подряд) |
| `bus` | `sfx` | `sfx`, `music`, `ui` |
| `volume`, `pitch` | 1 | базовые |
| `volumeMin/Max`, `pitchMin/Max` | = базовые | случайный разброс на каждый запуск |
| `loop` | false | цикл |
| `spatial`, `minDistance`, `maxDistance`, `rolloff`, `doppler` | false, 2, 80, 1, 1 | 3D; без `EventParams::hasPosition` событие играет не пространственно |
| `priority` | 0 | при нехватке голосов |
| `maxInstances` | 0 (без лимита) | одновременных голосов события |

События пакета (их играет модуль `RlFeedback`, см. [rl-feedback.md](../game/rl-feedback.md)): `engine`, `boost`, `skid` (циклы машины: мотор, буст, шины), `ball_hit` (3 варианта), `ball_bounce`, `jump`, `dodge`, `land`, `pad_small`, `pad_big`, `demolish`, `goal_horn`, `crowd_roar`, `crowd_ambience` (цикл), `bump`, `whistle`, `countdown_beep`, `countdown_go`, `ui_click`, `ui_hover`, `ui_back`, `menu_music` (цикл, шина `music`).

## Звуки пакета

`python tools/gen_rl_audio.py`, 44.1 кГц, 16 бит; всего **5.7 МБ** (лимит 15), 24 файла. Мотор — гармонический ряд 100 Гц с биением и гулом, буст — шум со свистом, удары — «глухой удар + щелчок + корпус» в трёх вариантах, прыжок и додж — свист с шумом, пады — аккорд-колокольчик (большой — арпеджио), демолиш — взрыв с закрывающимся фильтром и треском, гол — горн из пилообразных волн и рёв трибун (стерео), отсчёт — писк и «go», толпа — цикл (стерео), свисток судьи, шины (цикл шума со скрипом), удар машины о машину, UI — короткие клики, музыка меню — петля 16 секунд, 120 BPM (бас, пэд, арпеджио, ударные), стерео.

## Компонент сцены

`AudioSourceComponent` (`"AudioSource"` в JSON сцены): `event`, `playing`, `retriggerInterval` (0 — цикл играет, одиночный звук — один раз; > 0 — повтор каждые N секунд), `volume`, `pitch`, `spatial`. Позиция берётся из `Transform`; скорость (доплер) — по смещению. Слушатель — первичная камера сцены (`FxUpdateSystem`). Цикл, чей объект исчез (остановили Play и начали заново), останавливается сам.

## Без устройства

`AudioConfig::useDevice = false`, `MYENGINE_AUDIO=0` или отсутствие XAudio2/устройства: `HasDevice()` → false, голоса считают время (длина буфера / pitch), всё остальное одинаково — пул, шины, 3D, события, статистика. Игра не падает и не знает о разнице.

## Подключение к игре (G7d)

Готовый модуль — `engine/include/myengine/game/RlFeedback.h`: сам находит события в состоянии симуляции, играет звуки и эффекты, не повторяет их после отката в сети; точная инструкция подключения к `RlGameSystem` — в [rl-feedback.md](../game/rl-feedback.md). Таблица ниже — какой звук к какому событию:

| Событие игры | Звук |
| --- | --- |
| тик машины (газ, обороты) | цикл `engine`, `SetVoicePitch` от оборотов, позиция/скорость машины; для своей машины `hasPosition = false` |
| буст держится | цикл `boost` (громкость по доле нажатия), `Stop(…, 0.1)` при отпускании |
| удар по мячу | `ball_hit`, `volumeScale` от скорости удара; отскок — `ball_bounce` |
| прыжок / двойной / додж / приземление | `jump`, `dodge`, `land` в позиции машины |
| подбор пада | `pad_small` / `pad_big` (для своей машины без позиции) |
| демолиш | `demolish` в позиции машины |
| гол | `goal_horn` + `crowd_roar` без позиции; цикл `crowd_ambience` весь матч |
| отсчёт | `countdown_beep` ×3, `countdown_go` |
| меню | `ui_hover`, `ui_click`, `ui_back` на шине `ui`, цикл `menu_music` на шине `music`, `StopBus(Music, 0.5)` при старте матча |
| настройки | `SetBusVolume` для `Master`, `Sfx`, `Music`, `Ui` |

Слушателя игра ставит сама (`SetListener` по камере) либо оставляет `FxUpdateSystem`, если в сцене есть камера с `isPrimary`.

## Тесты

`audio` (без устройства, `tests/audio/AudioTests.cpp`): чтение WAV (16/8/24 бит, float, ошибки), 3D (затухание, панорама, доплер), пул (хэндлы, поколения, кража по приоритету и громкости), шины (произведение, mute, лимит), цикл и pitch, fade, банк (варианты, `maxInstances`, нет позиции — не 3D), все звуки пакета (формат, длина по манифесту, нет клиппинга, цикл бесшовный, ≤ 15 МБ), банк проекта (все события загружаются), дымовой тест на реальном устройстве (громкость 0).

## Ограничения

- Шины — группы громкости, без эффектов (реверберации, фильтров); X3DAudio не используется (своя математика).
- Музыка целиком в памяти (стерео 16 секунд ≈ 2.8 МБ), без потоковой подгрузки.
- Один поток: `AudioSystem` вызывают из потока игры.
