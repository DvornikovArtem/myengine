# Тренировка (G4g)

Отдельный модуль без рендера и без окна (`game/rl`): свободная игра и наборы упражнений поверх `RlSim` (кик-оффы против бота — поверх `RlMatch`). `RlGameSystem` подключает его несколькими вызовами (раздел «Подключение»); `RlMatch`, `RlGameSystem`, `RlNet*` модуль не меняет. Плашка тренировки, таймер, баннер SUCCESS / MISS и подсказки клавиш — компонент UI-кита ([rl-ui.md](rl-ui.md)), результаты игрока — файл `training.json` в пользовательской папке (`core::apppaths`).

| Файл | Что |
| --- | --- |
| `game/rl/include/rl/RlTraining.h`, `src/RlTraining.cpp` | `RlTraining` (свободная игра, раннер упражнений), `RlTrainingPack` / `RlTrainingExercise` (данные), `RlLoadTrainingPacks`, `RlTrainingResults` (результаты), `RlTrainingInfo` (данные для HUD) |
| `assets/data/training/*.json` | наборы упражнений: `striker`, `goalie`, `aerial`, `kickoff` |
| `engine/include/myengine/game/RlTrainingStore.h` | `TrainingResultsPath()` (`core::apppaths::UserDataDirectory() / "training.json"`), `TrainingPacksDirectory()`, `LoadTrainingPacks()`, `AttachTrainingResults()` |
| `engine/include/myengine/ui/rl/RlUiHud.h` | `HudState::training` и `Hud::DrawTraining` (плашка, таймер, баннер, подсказки), экран галереи `hud-training` |
| `tests/rl/RlTrainingTests.cpp` | тест `rl_training` |

## Свободная игра

`RlTraining training(tuning);` — сразу свободная игра: одна машина на месте кик-оффа и мяч в центре, без часов, счёта и соперников.

| Вызов | Что делает |
| --- | --- |
| `SetInfiniteBoost(bool)` | бесконечный буст: перед каждым тиком в баке 100 |
| `ResetBallToCar(distance = 6)` | мяч на полу перед машиной (по направлению взгляда), в покое |
| `BallInAir(distance = 14, height = 7)` | мяч в воздухе перед машиной на высоте `height`, ему дан толчок вверх 6 м/с: он взлетает на 2–3 м выше и падает, времени на воздушный удар хватает (для тренировки аэриалов) |
| `ResetCar()` | машина на место кик-оффа с начальным бустом |
| `ResetCounters()` | обнулить счётчики |

**Счётчики** (`Info()`): `hits` — касания мяча машиной, `lastHitSpeed` — скорость мяча сразу после касания (м/с), `bestHitSpeed` — лучшая, `maxTouchHeight` — самая большая высота центра мяча в момент касания (м). HUD показывает скорость в км/ч (`× 3.6`), высоту в метрах.

## Наборы упражнений (данные)

`assets/data/training/*.json` — один файл на набор. Папка целиком уходит в пакет игры (`assets/data` в `tools/package_manifest.json`). Набор читается `RlLoadTrainingPackFromFile` / `RlLoadTrainingPacks(папка)` (результат — по полю `order`, затем по имени); файл с ошибкой пропускается, сообщение попадает в `errors`.

```json
{
  "id": "striker",
  "name": "Striker",
  "order": 1,
  "description": "...",
  "exercises": [
    {
      "id": "rolling-ball",
      "name": "Rolling ball",
      "hint": "Catch up with the ball and shoot.",
      "car": { "x": 0, "z": -30, "yaw": 0, "boost": 33.3 },
      "ball": { "position": [0, 0.9125, -10], "velocity": [0, 0, 5], "spin": [0, 0, 0] },
      "timeLimit": 10,
      "attempts": 3,
      "success": { "type": "goal" }
    }
  ]
}
```

| Поле упражнения | Смысл |
| --- | --- |
| `id` | ключ в файле результатов, уникален в наборе (не задан — `ex<номер>`) |
| `name`, `hint` | название и строка подсказки для HUD |
| `car` | `x`, `z`, `yaw` (рад от +Z к +X, как в `RlSim::ResetCar`), `boost` 0..100; машина стоит на полу в покое |
| `ball` | `position`, `velocity`, `spin` — `[x, y, z]` |
| `timeLimit` | секунд игры (0 < t ≤ 120); у кик-оффа отсчёт идёт после обратного отсчёта матча |
| `attempts` | сколько промахов можно сделать (1..20, по умолчанию 3) |
| `success.type` | условие успеха, см. ниже |
| `kickoff` | только для `kickoffWin`: `{ "spot": 0..4, "botLevel": 0..2 }` вместо `car` и `ball` |

Координаты: синие атакуют ворота `+Z`, машина смотрит вдоль своей `+Z`, `yaw` 0 — на ворота соперника. Загрузчик проверяет типы, диапазоны (положения в пределах арены с запасом, буст 0..100), уникальность `id` и отказывает с понятным сообщением («exercise 3 (corner): …»).

### Условия успеха

| `success.type` | Успех | Промах (кроме «время вышло») |
| --- | --- | --- |
| `goal` | мяч в воротах соперника (+Z), правило гола как в матче (центр мяча за линией на радиус, между штангами, ниже перекладины) | мяч в своих воротах |
| `saved` | машина коснулась мяча, и он либо идёт от своих ворот (скорость по Z больше 0.5 м/с), либо 1.5 с после касания не залетел | мяч залетел в свои ворота |
| `touchAbove` (`height`, м) | касание машиной мяча, пока центр мяча не ниже `height` | — |
| `kickoffWin` (`distance`, м) | кик-офф против бота: машина коснулась мяча первой и мяч дальше `distance` м от центра на половине соперника (или гол) | первым коснулся бот, мяч ушёл на `-distance` или гол в свои ворота |

Причина промаха (`RlTrainingMiss`): `TimeUp`, `OwnGoal`, `LostKickoff`.

### Что входит в наборы

| Набор | Условие | Упражнения (`id`) |
| --- | --- | --- |
| `striker` (8) | гол | `open-net`, `rolling-ball`, `turn-and-shoot`, `bouncing-ball`, `cross`, `far-shot`, `corner`, `falling-ball` |
| `goalie` (8) | сейв | `straight-on`, `left-post`, `right-post`, `bouncing-shot`, `far-corner`, `hard-drive`, `cross-and-shoot`, `lob` |
| `aerial` (7) | касание выше 4–10 м | `easy-lift`, `higher`, `moving-ball`, `from-distance`, `crossing`, `near-the-wall`, `sky-high` |
| `kickoff` (7) | выигранный кик-офф против бота уровня 2 | `right-corner`, `left-corner`, `back-right`, `back-left`, `far-back`, `push-deep`, `quick-win` |

Удары вратарского набора рассчитаны так, что без касания мяч точно влетает в ворота: тест проверяет это для всех восьми.

## Как идёт упражнение

`StartPack(pack, exercise = 0)` — набор копируется, начинается упражнение. Состояния (`RlTrainingPhase`):

| Фаза | Что происходит |
| --- | --- |
| `Ready` | упражнение расставлено, мир стоит 1 с (надпись GET READY); у кик-оффа — обратный отсчёт матча 3 с, бот уже на месте |
| `Running` | идёт время игры (`elapsed`, `timeLeft`), проверяется условие успеха |
| `Success` / `Miss` | баннер 1.8 с, мир живёт дальше. Успех → следующее упражнение. Промах тратит попытку: остались попытки — то же упражнение сначала, не остались — следующее (`outOfAttempts`) |
| `PackDone` | после последнего упражнения: сводка «выполнено N из M» |

Кнопки HUD: `Retry()` — упражнение сначала без траты попытки (после «попытки кончились» — со свежими попытками; в сводке — весь набор с начала), `Next()` — следующее (после последнего — сводка), `Previous()` — предыдущее (в сводке — последнее; у первого остаётся на нём), `RestartPack()`. Переход вручную сбрасывает попытки упражнения. Кик-офф каждую попытку начинается новым `RlMatch` с зерном от номера упражнения и попытки, поэтому один и тот же ввод даёт один и тот же результат.

`Info()` отдаёт плоскую структуру для HUD: набор, номер упражнения («3 / 8»), попытка, время, баннер, причина промаха, `newBest`, счётчики, рекорд упражнения.

## Результаты

`RlTrainingResults` — по каждому упражнению: `successes`, `attempts` (все завершённые попытки), `bestTime` (самый быстрый успех, с), `bestHitSpeed` (самое быстрое касание в успехе, м/с). Формат `training.json`:

```json
{
  "version": 1,
  "packs": {
    "striker": {
      "open-net": { "successes": 2, "attempts": 3, "bestTime": 4.2, "bestHitSpeed": 31.1 }
    }
  }
}
```

Файл лежит в `core::apppaths::UserDataDirectory()`: `%APPDATA%\myengine\training.json` в разработке и `%LOCALAPPDATA%\RocketArena\training.json` в собранной игре. `game::AttachTrainingResults(training)` читает его (нет файла — результатов нет, сломанный файл — тоже нет, без падения) и заставляет `RlTraining` писать файл **после каждой завершённой попытки** (сначала во временный `.tmp`, затем подмена: обрыв посередине не оставит половину файла). `training.Results()` отдаёт те же данные меню (`Get(pack, exercise)`, `PackSuccesses(pack)`).

## HUD и экран галереи

`HudState::training` (`HudTraining`, `active = true`) — компонент кита: пока он активен, `Hud::Draw` вместо табло рисует плашку сверху слева (набор, «3 / 8», название упражнения, подсказка, точки попыток и «BEST 4.2 s · 112 km/h»), таймер сверху посередине (обратный отсчёт с полосой, последние 3 с мигает красным), баннер SUCCESS / MISS / PACK COMPLETE (подзаголовок: время и скорость, NEW BEST, причина, «TRY AGAIN» / «NO ATTEMPTS LEFT»), GET READY и ряд подсказок клавиш внизу (R / Y — Retry, N / RB — Next, P / LB — Previous, Esc / Start — Menu; в свободной игре R — мяч к машине, T — мяч в воздух, B — бесконечный буст, плюс счётчики касаний и метка ENDLESS BOOST). Подсказки можно заменить списком `HudTraining::hints`. Подробности полей — в [rl-ui.md](rl-ui.md). Экран `hud-training` (`MYENGINE_UI_GALLERY=hud-training`) крутит цикл 10 с: упражнение идёт → SUCCESS → MISS → свободная игра → сводка набора. Скриншоты (1600×900) — `.team/tasks/g4g-evidence/rl-hud-training-*.png`.

## Подключение к `RlGameSystem` и меню G1

Это делает владелец `RlGameSystem` (coder-2, G1-flow): модуль ничего в нём не меняет. Названия по `RlGameSystem.cpp` ветки `game-rl-online`.

**1. Экран Training в меню G1.** Кнопка Training (иконка `Icon::Target`) ведёт на экран выбора: строка «FREE PLAY» и по строке на набор из `game::LoadTrainingPacks(&errors)`; в строке набора — `results.PackSuccesses(pack)` из `pack.exercises.size()`. Выбор запускает тренировку (пункт 2).

**2. Запуск.** Поле `std::unique_ptr<rl::RlTraining> training;` и новый режим `Mode::Training` рядом с `Mode::Match`. Старт:

```cpp
training = std::make_unique<rl::RlTraining>(tuning);
game::AttachTrainingResults(*training);        // читает training.json, пишет после каждой попытки
if (pack) training->StartPack(*pack);          // иначе остаётся свободная игра
mode = Mode::Training;
accumulator = 0.0f;
cameraState = rl::RlCameraState{};
BeginTrainingVisuals(world);                   // как BeginMatchVisuals: машина 0 (синяя), при кик-оффе машина 1 (оранжевая), мяч, пады
```

**3. Тик.** В `Update` вместо `match->Step(inputs)`: `training->Step(latch.Take(player));` (внутри цикла фиксированных тиков, ввод один — игрок). Мир для рисования: `training->World()` / `PreviousWorld()` / `Tuning()`.

**4. Отрисовка.** `ApplyToScene` читает матч через `View()`; для тренировки в неё передаются `before = PreviousWorld()`, `now = World()`, `alpha`, число машин `World().carCount` и `Tuning()`; команда машины `slot` — это `slot` (0 синяя, 1 оранжевая бот). Машины: `cars[team][index]` как в матче. Сдвиг при расстановке упражнения не интерполируется (мир стоит, `PreviousWorld() == World()`). Камера — обычная `UpdateCamera` по `now.cars[0]`.

**5. Клавиши** (проверять на кадре, до тиков): `R` / pad Y — `training->Retry()` в упражнении, в свободной игре — `ResetBallToCar()`; `N` / RB — `Next()`; `P` / LB — `Previous()` (в свободной игре `P` занят меню практики, там — `T` / X → `BallInAir()` и `B` / LB → `SetInfiniteBoost(!training->InfiniteBoost())`); `Esc` / Start — в меню (`ToMenu`: `training.reset()`). Если клавиши заняты, пишите свои подписи в `HudTraining::hints`.

**6. HUD.** Каждый кадр из `training->Info()` заполняется `hud.training`:

```cpp
const rl::RlTrainingInfo info = training->Info();
rlui::HudTraining& t = hudState.training;
t.active = true;
t.phase = info.mode == rl::RlTrainingMode::FreePlay ? rlui::HudTrainingPhase::FreePlay : static_cast<rlui::HudTrainingPhase>(info.phase);
t.pack = info.packName;  t.exercise = info.exerciseName;  t.hint = info.hint;
t.index = info.exerciseIndex;  t.count = info.exerciseCount;
t.attempt = info.attempt;  t.attempts = info.attempts;
t.timeLeft = info.timeLeft;  t.timeLimit = info.timeLimit;  t.elapsed = info.elapsed;
t.bannerTime = info.bannerTime;
t.missReason = info.missReason == rl::RlTrainingMiss::TimeUp ? "TIME UP" : info.missReason == rl::RlTrainingMiss::OwnGoal ? "OWN GOAL" : info.missReason == rl::RlTrainingMiss::LostKickoff ? "LOST KICK-OFF" : "";
t.outOfAttempts = info.outOfAttempts;  t.newBest = info.newBest;
t.hits = info.hits;  t.lastHitKmh = info.lastHitSpeed * 3.6f;  t.bestHitKmh = info.bestHitSpeed * 3.6f;  t.maxTouchHeight = info.maxTouchHeight;
t.infiniteBoost = info.infiniteBoost;
t.recordTime = info.best.bestTime;  t.recordHitKmh = info.best.bestHitSpeed * 3.6f;
t.done = info.packDoneSuccesses;
```

Остальное в `HudState` — как у практики: `mode = 0`, `players` из одного игрока, `boost = World().cars[0].boost`, `phase` — из `World().match.phase` (у кик-оффа это обратный отсчёт 3-2-1-GO, HUD показывает его сам).

**7. Звук и эффекты.** `RlFeedback::OnTick(PreviousWorld, World)` вызывается так же, как для матча: тренировка отдаёт обычные `RlWorldState` (касания, прыжки, пады кик-оффа). Свободная игра и упражнения не вызывают гол-баннер матча и повтор: `RlDirector` в тренировке не создаётся.

**8. Выход.** `training.reset()` в `ToMenu`; результаты уже на диске.

## Тест `rl_training` (`ctest -R rl_training`, без окна)

- **Наборы:** четыре набора читаются и идут по `order`; в каждом 5–10 упражнений с названием, подсказкой, временем и попытками, уникальные `id`; машина и мяч внутри арены и не друг в друге; кик-оффы — против бота уровня 2; разбор: плохие тексты (не JSON, пустой список, нет `id`, неизвестное условие, неверный вектор, `timeLimit 0`, позиция вне арены, нет `car` / `ball`, `kickoffWin` без `kickoff`, повторный `id`) отказывают с причиной, значения по умолчанию.
- **Свободная игра:** бесконечный буст держит бак полным и без него бак тратится; мяч возвращается к машине на 6 м перед ней; мяч в воздухе поднимается; касание считается (скорость и высота), счётчики сбрасываются.
- **Упражнения на сценарном вводе:** `open-net` — газ с бустом прямо в мяч: гол, время и скорость удара, рекорд; баннер 1.8 с и автопереход; Previous / Retry; лучшее время не растёт. Ничего не делать: во всех упражнениях `striker`, `aerial` — промах «время вышло» ровно на `timeLimit`, во всех восьми `goalie` — удар долетает в свои ворота раньше конца времени (удары на цель). Сейв: машина на линии удара — успех; мяч в своих воротах — промах. Касание ниже порога не успех, на нужной высоте — успех. Кик-офф: обратный отсчёт матча, часы идут после него, бот едет к мячу, без ввода игрока бот выигрывает во всех пяти точках, один и тот же прогон даёт один и тот же конец.
- **Таймер и попытки:** таймер идёт, промах приходит на `timeLimit`, три промаха — следующее упражнение, ручной Retry ничего не тратит, промах с оставшимися попытками — то же упражнение сначала с мячом на месте, Next проходит набор до сводки, Previous / Retry в сводке.
- **Результаты:** файл пишется после попытки, а не в конце сессии; следующая сессия его читает и продолжает; промах считает только попытку; лучшее время сохраняется; текстовый круг; сломанный файл — пустые результаты без падения, затем файл перезаписывается; `TrainingResultsPath()` — `apppaths::UserDataDirectory() / "training.json"`.

## Ограничения

- Модуль не подключён к `RlGameSystem` и меню G1 (это делает владелец G1-flow по инструкции выше); живьём в движке тренировку я не смотрел — проверено тестом на настоящей симуляции и экраном галереи.
- Без касаний выполнимость `striker` и `aerial` тестом не проверяется (нужен автопилот); проверены сами условия, «не сделать ничего = промах» и положение объектов. Значения (скорости, высоты, время) стоит подкрутить после живой игры — это данные в `assets/data/training/*.json`.
- Свободная игра — одна машина и мяч, без бустовых падов и соперников; бесконечный буст в упражнениях набора не действует (там `car.boost` упражнения). Пады в кик-оффе работают как в матче (его ведёт `RlMatch`).
- Бот кик-оффа — `RlBots` v2 (мозг матча), уровень берётся из `kickoff.botLevel`.
