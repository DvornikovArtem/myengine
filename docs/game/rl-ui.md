# UI-кит игры (G1-kit)

Кит рисует и фокусирует: шрифт Exo 2, виджеты на `ImDrawList`, модель фокуса, HUD-компоненты и галерея всех экранов с фейковыми данными. Логики игры в нём нет: экраны (меню, настройки, пауза, итоги) и подключение HUD к матчу пишет G1-flow поверх кита. Дизайн — `.team/design/rl-ui-spec.md` и мокапы `.team/design/rl-ui/png`; скриншоты галереи — `.team/tasks/g1-kit-evidence/<имя мокапа>.png`.

Код: заголовки `engine/include/myengine/ui/rl/RlUi*.h` (с комментариями), реализация `engine/src/ui/rl/`, пространство имён `myengine::rlui` (не `ui::rl`: внутри него `rl::` перестало бы означать библиотеку игры). Публичные заголовки не включают ни ImGui, ни `game/rl`: данные идут простыми структурами.

| Заголовок | Что |
| --- | --- |
| `RlUiCommon.h` | `Id` / `HashId`, `Vec2`, `Rect`, `Color`, `Span`, `Align`, `Device`, `Icon` |
| `RlUiTheme.h` | токены §4: цвета (`style::kBlue`, `kFocus`, `kAccent` ...), размеры, скос 12°, времена анимаций |
| `RlUiFonts.h` | Exo 2 (4 начертания × размеры спеки) и иконки Lucide: запекание в атлас ImGui под масштаб |
| `RlUiInput.h` | `RawInput` (клавиши, мышь, пад, набранный текст), `InputMapper` → `Nav` (повтор 0.35 с, затем 12 Гц), `TokenDisplayName`, `CaptureToken` |
| `RlUiFrame.h` | `Frame`: метрики, фокус, области диалогов, переход экрана, тосты, состояние виджетов, прозрачность |
| `RlUiPaint.h` | примитивы: скошенные формы, градиенты, свечение, радиальный свет, дуги, текст, иконки |
| `RlUiWidgets.h` | кнопки, вкладки, карточки, строки настроек, привязки, подсказки, диалоги, список с прокруткой |
| `RlUiHud.h` | `Hud` и `HudState`: табло, отсчёт, буст-кольцо, баннеры, лента, таблица, подсказка ball cam, пинг |
| `RlUiHost.h` | `Host`: подключение к `UiManager` (шрифты, устройства, вызов экрана каждый кадр вместо редактора) |
| `RlUiGallery.h` | галерея: все экраны и состояния HUD мокапов |

## Подключение (три строки)

```cpp
rlui::Host host;                                           // живёт столько же, сколько приложение; уничтожить ДО UiManager
host.Install(uiManager, {});                               // после UiManager::Initialize, до RegisterWindow окон (иначе шрифты пересоберутся на следующем кадре)
host.SetScreen([&](rlui::Frame& f) { currentScreen(f); }); // что рисуется каждый кадр: меню, HUD ...
```

`UiManager` получил небольшой хук (`SetGameUi`, `SetGameUiCapturesInput`, `RequestFontRebuild`, `GameUiHooks`): пока он включён, вместо редактора вызывается экран кита; шрифты кита запекаются в атлас каждого окна. `Install` включает «захват ввода» (`WantsMouseCapture` / `WantsKeyboardCapture` — да: открыто меню). В матче игра сама вызывает `uiManager.SetGameUiCapturesInput(false)`, чтобы ввод шёл к машине, и `true` в паузе и меню. `Host` сам читает клавиатуру (`GetAsyncKeyState`, пока окно активно), мышь и набранный текст из ImGui и XInput (первый пад) в `RawInput`; `Host::LastRawInput()` — то, что он видел. Фона `Host` не рисует: за UI идёт 3D-кадр игры; экран без сцены вызывает `paint::MenuBackdrop(f)`.

## Координаты и масштаб

Все `Rect` и размеры — **проектные px 1920 × 1080** (как мокапы). Кит умножает их на `scale = min(H / 1080, W / 1920) × UI scale` и ставит экран в «колонку меню» — область 16:9 по центру окна (на 21:9 по бокам остаётся фон). `Frame::metrics()` — масштаб, окно, колонка, размер окна в проектных px. HUD рисуется в `Space::Screen` (`f.SetSpace(...)`): те же проектные px, но от настоящих краёв окна (`designWindow` — его размер): табло по центру сверху, буст в правом нижнем углу. Параллелограмм вписан в свой `Rect`: верхний край от `x + bevel` до `x + w`, нижний от `x` до `x + w - bevel`.

Шрифты запекаются под ближайший «класс размера» (0.5, 0.67, 0.8, 1, 1.25, 1.5, 2, 3) и масштабируются на малую разницу до настоящего масштаба; при смене класса (другое окно, UI scale) `Host` просит `UiManager` пересобрать атласы (не чаще двух раз в секунду). Для больших кеглей запекаются только нужные глифы (отсчёт — цифры и GO, баннеры — заглавные), чтобы атлас помещался в 4K.

## Модель фокуса

Своя, не ImGui Nav (§5). Каждый виджет — элемент фокуса: `Frame::Item(id, rect, enabled)`. Ровно один элемент в фокусе, он всегда виден (белая плашка, полоса accent слева, свечение за 0.08 с). Стрелки / D-pad / стик (порог 0.5, повтор 0.35 с + 12 Гц) переносят фокус на ближайший элемент в этом направлении по геометрии элементов кадра; наведение мыши переносит фокус, клик подтверждает; Enter / Space / A подтверждает, Esc / B — `Nav::Back` (экран решает сам). Строки, которым нужны ← / →, забирают их у фокуса (`Frame::TakeNav`): `OptionRow`, `SliderRow` (Shift — шаг ×5), `StepperRow`, `BindRow` (выбор плашки). Фокус вступает в силу в конце кадра: виджет «в фокусе» первый раз виден со второго кадра экрана.

- `SetDefaultFocus(id)` — кто получает фокус, когда его нет (вызывать каждый кадр экрана); `ResetFocus()` — новый экран: фокус сбрасывается, начинается переход 0.18 с (прозрачность и сдвиг на 40 px; применяется ко всем отрисовкам автоматически).
- **Области (диалоги):** `PushScope(id)` ... `PopScope()`: пока область есть, оживают только её элементы; нижний экран не реагирует, фокус сохраняется и возвращается после закрытия. `ConfirmDialog` и `RebindDialog` делают это сами.
- `SetTextEditing()` (вызывает `TextRow`): буквы W A S D Q E R в этот момент текст, а не навигация.
- `IntState(id)` / `FloatState(id)`: маленькое состояние виджета между кадрами (выбранная плашка привязки, смещение списка).
- `ScrollView`: список выше окна. Элементы раскладываются в координатах содержимого, смещение кит применяет сам; сфокусированный элемент прокручивается в окно, колесо мыши прокручивает, список обрезается.
- `ShowToast(text, color, seconds)`: тост внизу по центру (обмен привязок: «Boost: Left mouse ↔ Jump», жёлтый, 2 с).
- Подсказки (`HintBar`): клавиатурные и геймпадные глифы вместе, как на мокапах; глифы устройства, которым пользовались последним (`Frame::LastDevice()`), яркие, остальные приглушены.

## Как собрать экран

Экран — функция `void(rlui::Frame&)`, которая каждый кадр вызывает виджеты. Пример — вся галерея, `engine/src/ui/rl/RlUiGallery.cpp`, по функции на мокап. Главное меню:

```cpp
void MainMenu(rlui::Frame& f)
{
    using namespace rlui;
    const Id play = HashId("main/play"), exit = HashId("main/exit");
    f.SetDefaultFocus(play);
    ScreenTitle(f, "MYENGINE", "ROCKET\nARENA", {80, 76}, true);
    Column column({76, 370, style::kMenuButtonWidth, style::kMenuButtonHeight}, style::kMenuButtonStep);
    if (MenuButton(f, play, column.Next(), "PLAY", Icon::Play)) GoTo(Screen::Play);
    MenuButton(f, HashId("main/settings"), column.Next(), "SETTINGS", Icon::Settings);
    if (MenuButton(f, exit, column.Next(), "EXIT", Icon::Exit)) askQuit = true;
    if (f.input().Pressed(Nav::Back)) f.SetFocus(exit);              // Back на главном меню фокусирует Exit
    if (askQuit)
    {
        const DialogResult r = ConfirmDialog(f, HashId("main/quit"), {"QUIT TO DESKTOP?", "Your settings are saved."});
        if (r != DialogResult::None) askQuit = (r == DialogResult::Cancel) ? false : (Quit(), false);
    }
    const Hint hints[] = {{Glyph::UpDown, Glyph::PadA, "Select"}, {Glyph::Enter, Glyph::None, "Confirm"}, {Glyph::Esc, Glyph::PadB, "Back"}};
    HintBar(f, Span<Hint>(hints));
}
```

Строки настроек возвращают новое значение (`OptionRow`, `ToggleRow`, `SliderRow`, `StepperRow` → −1 / 0 / +1, `TextRow` → изменился ли текст, `ValueRow` — только показ). Привязка и переназначение:

```cpp
const BindRowResult r = BindRow(f, HashId("controls/jump"), rows.Next(), "Jump", TokenDisplayName(keyboardToken), TokenDisplayName(padToken));
if (r.rebind) { rebinding = {&action, r.plate}; timer = 5.0f; }       // r.plate: 0 клавиатура / мышь, 1 геймпад
if (r.clear) Clear(action, r.plate);
if (r.resetRow) ResetRow(action);
...
if (rebinding)
{
    RebindDialog(f, {"REBIND · KEYBOARD / MOUSE", "JUMP", "Press a key or a mouse button...", "Esc - cancel · Del - clear · the key is already used by ", conflictName, timer / 5.0f});
    const std::string token = CaptureToken(f.input().PreviousRaw(), f.input().Raw(), rebinding.plate == 1);   // токен rl::RlControls: "W", "MouseLeft", "PadRT", "-PadLeftY"
    // Esc отмена, Del очистить, token -> RlControls::SetButton / SetAxis; при конфликте обмен и f.ShowToast("Boost: Left mouse <-> Jump")
}
```

`RebindDialog` только рисует (и берёт область, чтобы строки под ним молчали); ловит ввод экран через `InputMapper::Raw()` и `CaptureToken` (Esc и Del он не возвращает: это отмена и очистка).

**Новый виджет:** функция в `RlUiWidgets.cpp`, которая берёт `Frame&`, `Id`, `Rect`, регистрирует элемент через `f.Item(id, rect, enabled)`, рисует состояние (`ItemState`: `focused`, `hovered`, `activated`, `glow`) примитивами `paint::*` и возвращает результат. Экран-пример — в галерее; тест фокуса и виджетов — `tests/ui/RlUiTests.cpp`.

## HUD

```cpp
rlui::Hud hud;                          // один на матч; hud.Reset() в новом матче
rlui::HudState s;                       // каждый кадр из мира игры:
s.phase = rlui::HudPhase(world.match.phase);  s.phaseTimer = world.match.phaseTimer;  s.clock = world.match.clock;  s.overtime = world.match.overtime != 0;
s.mode = world.match.mode;  s.score[0] = world.match.score[0];  s.score[1] = world.match.score[1];
s.boost = world.cars[local].boost;  s.boosting = /* заряд падает */;  s.demolished = world.cars[local].demolished;  s.respawnTimer = world.cars[local].respawnTimer;
s.lastGoalTeam = world.match.lastGoalTeam;  s.lastGoalScorer = world.match.lastGoalScorer;  s.lastGoalOwn = world.match.lastGoalOwn != 0;  s.lastGoalSpeedKmh = /* lastGoalSpeed * 3.6 (G1.0) */;
s.players = ...;  // имя, команда, бот, очки, голы, ассисты, сейвы, удары, демо (G1.0)
hud.Update(dt, s);                      // анимации и смены: фаза → баннер, флаг овертайма → OVERTIME!, рост буста → вспышка пада, отсчёт, GO!
for (const auto& e : newEventsOfTheRing) hud.PushEvent({rlui::HudEventType::Goal, scorerIndex, -1});
f.SetSpace(rlui::Space::Screen);
hud.Draw(f, scoreboardHeld);            // Tab / Back зажаты: затемнение, табло и таблица игроков; при state.replay.active — только REPLAY
// в паузе: hud.Draw(f, false, true) — только табло, приглушённое; в итогах HUD не рисуется, а таблицу берёт экран: Hud::DrawScoreTable(...)
```

Что делает `Hud` сам: отсчёт 3 · 2 · 1 по `phaseTimer` (140 % → 100 % за 0.15 с, гаснет в последние 0.15 с секунды) и GO! 0.5 с после перехода в `Playing`; баннеры GOAL! / OWN GOAL (цвет команды, имя и км/ч) с въездом 0.25 с и выездом 0.2 с, OVERTIME! 1.5 с, DEMOLITION / DEMOLISHED BY (по событию `Demo`, когда игрок — автор или жертва) и «Respawning 2.4»; табло с миганием последних 30 с (белый ↔ красный раз в секунду) и пульсом 8 % в последние 10 с, овертайм `+0:42` с подписью OVERTIME, тренировка (`mode 0`) — только время; буст-кольцо (дуга 270° с градиентом, пульс 6 Гц при расходе, вспышка при подборе пада, красный ноль); лента до 5 строк по 5 с; подсказка ball cam 2 с после переключения и постоянно первые 30 с; пинг (зелёный < 60, жёлтый < 120, красный) и FPS. Данные таблицы и ленты берутся из G1.0 (`points`, `assists`, `saves`, `shots`, `demos`, `events`, `names`, `lastGoalSpeed`); до него HUD работает на тех же структурах с нулями.

## Галерея

```
set MYENGINE_UI_GALLERY=<имя>     имена: main, play, private, loading, pause, results, controls, rebind, camera, video, hud-kickoff, hud-goal, hud-scoreboard, hud-replay, hud-training; all — по кругу, 4 с на экран
myengine.exe --scene assets/scenes/scene.json --screenshot out.tga --screenshot-frame 70 --exit-after-frames 75
```

Переменная окружения, а не флаг: разбор аргументов `main.cpp` принадлежит режиму `--game` (G1-flow); регистрация одна — блок в `Application::Initialize` (`MYENGINE_UI_GALLERY`). Галерея живая: фокус, вкладки, слайдеры, переключатели, поле адреса работают на фейковых данных. Фон — `paint::MenuBackdrop` и нарисованная «сцена» (машина, поле) вместо 3D. Окно 1920×1080 на экране 1080p система обрезает по рабочей области, поэтому скриншоты сняты на 1600×900 (масштаб 0.83; пропорции те же).

## Шрифты и иконки

`assets/fonts/exo2/`: `Exo2-BlackItalic.ttf` (900i), `Exo2-ExtraBoldItalic.ttf` (800i), `Exo2-Bold.ttf` (700), `Exo2-SemiBold.ttf` (600) и `OFL.txt` — статические начертания, нарезанные из variable-шрифтов `google/fonts` (`ofl/exo2`) скриптом `tools/make_exo2.py` (fontTools `varLib.instancer`; скрипт сам скачивает исходники и убирает их; variable-файлы в репозитории не хранятся). Латиница и кириллица (имена игроков). Курсивные начертания рисуют кириллицу курсивными формами («п» похожа на «n»), поэтому текст с кириллицей в `BlackItalic` / `ExtraBoldItalic` рисуется прямым `Bold` (`StyleFor` в `RlUiPaint.cpp`; латинские заголовки сохраняют наклон). Иконки — Lucide (`assets/fonts/lucide.ttf`, ISC): play, target, settings, log-out, refresh-cw, house, star, user, bot, wifi, wifi-off, shield, zap, circle-dot, server, log-in, eye, info, chevrons, стрелки. Лицензия Exo 2 добавлена в `tools/package_manifest.json` и `THIRD_PARTY_NOTICES.txt` дистрибутива.

## Тесты и замеры

Тест `rl_ui` (`tests/ui/RlUiTests.cpp`, `ctest -R rl_ui`, без окна и GPU: ImGui работает без экрана, черновые списки отрисовки читаются после кадра):
- ввод: повтор удержанной стрелки (≈ 8 срабатываний за секунду), WASD как буквы в поле, устройство последнего ввода, `CaptureToken` (клавиша, мышь, триггер и стик пада, Esc / Del не возвращаются);
- фокус: первый кадр, ↓ / ↑ по геометрии, конец списка не зацикливается, Enter один раз за нажатие, наведение и клик мыши, выключенный элемент пропускается, `ResetFocus`;
- виджеты: `OptionRow`, `ToggleRow`, `SliderRow` (шаг, Shift ×5, края, мышь), `StepperRow`, `TextRow` (разрешённые символы, длина, Backspace, буквы не двигают фокус), `BindRow` (плашки, переназначить, очистить, сброс), `Tabs` (Q/E, LB/RB), `ConfirmDialog` (область, фокус на Cancel, Back = Cancel, возврат фокуса), `ScrollView` (прокрутка к фокусу, колесо);
- логика `Hud`: цифры отсчёта, GO!, часы (`4:32`, `0:01`, `+0:42`), баннеры (цель, автогол, овертайм) и их срок, лента (5 строк, 5 с), баннер демолиша;
- раскладка на 1280×720, 1920×1080, 2560×1440, 3840×2160, 2560×1080 и 3440×1440: табло по центру окна, 22 px сверху, 466 px шириной на 1080p; буст-кольцо в 60 px от правого и 50 px от нижнего края; кнопка меню в колонке 16:9;
- отрисовка всех 13 экранов галереи на четырёх размерах окна (вершин больше 400 и меньше 150 000), все шрифты спеки запечены (в том числе кириллица в текстовых начертаниях и цифры с GO в кегле отсчёта).

**Стоимость:** кадр HUD матча 4v4 (табло, кольцо, баннер, лента из 5 строк, таблица в половине кадров) — медиана 0.025 мс CPU на кадр (построение списка отрисовки, 1100–3500 вершин). GPU, полный кадр галереи вместе с пустой сценой редактора на RTX 3050 Laptop, 1600×900: HUD с таблицей 0.49 мс, HUD с баннером 0.35 мс, главное меню 0.33 мс (`--frame-stats`); бюджет HUD для 144 FPS (6.9 мс на кадр) занят меньше чем на 1/10.

## Игровой режим (G1-flow)

Поверх кита сделан игровой режим: `myengine.exe --game` (или `"startInGame": true` в `config/app.json`, так стартует `RocketArena.exe`, см. [packaging.md](packaging.md)) открывает окно без панелей редактора, главное меню с машиной на подиуме и весь путь игрока от меню до итогов матча. Редактор в Play остаётся как был: старый текстовый оверлей и окно RL Tuning живут только в нём, в `--game` их нет. `--game --rl-host [порт]` / `--rl-join ip:порт` сразу ведут в лобби или подключение, минуя Play.

### Из чего состоит

| Что | Где | За что отвечает |
| --- | --- | --- |
| `RlGameSystem` | `engine/src/ecs/systems/RlGameSystem.cpp` | матч (`RlMatch` или сеть G5), сцена за экранами: арена, машины, мяч, пады, подиум меню и облёт камеры; HUD-данные; звук (`RlFeedback`); команды от экранов (`StartMatch`, `SetPaused`, `LeaveMatch`, `RestartMatch`, `StartHostedMatch`) и `Status()` с ответом: стадия, `HudState`, лента событий, лобби, ошибка, победитель и MVP |
| `RlGameFlow` | `engine/src/game/RlGameFlow.cpp` | экраны и переходы между ними, настройки игрока, переназначение клавиш, подтверждения; рисует кит и отдаёт команды системе |
| `RlGameSettings` | `engine/src/game/RlGameSettings.cpp` | файл `game_settings.json`: видео, камера, звук, последний выбор в Play, имя игрока |
| `Window::SetDisplayMode` | `engine/src/core/Window.cpp` | Windowed / Borderless / Fullscreen (смена режима дисплея, откат при выходе), список разрешений |
| Сцена | `assets/scenes/rocket_league.json` | собирается `tools/gen_rl_assets.py pack` (`tools/rl_assets/game_scene.py`): арена, трибуны, небо, прожекторы, табло из пака G7a, 8 машин (кузов, фары, пламя буста, 4 колеса), 34 пада и шары больших, мяч, подиум `RlPodium` (`podium.py`) |

`RlGameSystem` и `RlGameFlow` не знают друг о друге: система кладёт в `RlGameStatus` всё, что нужно показать, а экраны читают его раз в кадр и пишут команды, которые система выполняет в начале следующего `Update` (команды меняют сцену, поэтому не из середины кадра UI). `Application` собирает их вместе (`Initialize`, режим игры) и отдаёт флоу сервисы: выход, видео-настройки, список разрешений, прогресс загрузки, освещение, лог.

### Экраны и переходы

```
Boot ─▶ Main menu ─▶ Play (Offline · bots | Private match) ─▶ Loading ─▶ Match (HUD) ◀▶ Pause ─▶ Settings ─▶ Pause
          │  │                │ Create / Join ─▶ Room ─▶ Lobby ─────▲        │ Leave (подтверждение)
          │  └▶ Training ─────┘                                              ▼
          └▶ Settings (Controls · Camera · Video · Audio)             Results ─▶ Play again / Main menu
```

- **Boot** — чёрный экран с логотипом, пока грузится карта (`sceneReady`), не меньше 0.9 с. **Main menu**: Play, Garage (выбор машины, [rl-cars.md](rl-cars.md)), Training (экран тренировки, см. ниже), Settings, Exit (подтверждение «Quit to desktop?»); Back фокусирует Exit. За меню — 3D-сцена: синяя машина на подиуме, камера делает оборот за 40 с и качается на ±5°, машина стоит справа кадра; позади тёмное небо стадиона (свет меню мягче, чем в матче).
- **Play**: вкладки Offline · bots (карточки 1v1–4v4, сложность Rookie / Pro / All-star, Start match) и Private match (G1-lobby, [rl-lobby.md](rl-lobby.md), раздел «Экраны»): переключатель сети LAN / Steam (Steam — только в сборке с `MYENGINE_STEAM` и если `Available()`, иначе красная строка с причиной), слева Host (размер команды, боты, порт матча в LAN, Create lobby), справа Join (список лобби с режимом, числом игроков, пингом и пометкой «full / other version / match in progress» — Enter входит; в LAN ещё поле адреса `ip[:порт]` и Join address; Refresh). Выбор запоминается в `game_settings.json`.
- **Room** (Private match, после Create / Join / приглашения): состав лобби с готовностью (HOST / READY / NOT READY), у хоста панель Invite (LAN — адрес лобби и Copy address в буфер, Steam — Invite friends: оверлей) и Start match (доступен, когда все гости готовы; иначе подпись «<имя> is not ready»), у гостя «I am ready» / «Not ready». Leave и Esc выходят из лобби. Ошибки лобби (занятый порт, нет ответа хоста, лобби закрыто, другая версия, Steam не запущен) — панелью кита с причиной. Приглашение друга из Steam и `+connect_lobby <id>` в командной строке сразу открывают комнату в этом лобби.
- **Garage** (G4i, [rl-cars.md](rl-cars.md)): подиум меню с выбранной машиной, переход камеры за 0.4 с, вращение машины (оборот за 20 с, мышь или правый стик крутят руками), слева название, чипы (класс, «HITBOX ≈ …», EQUIPPED), описание, пять полосок (Top speed, Acceleration, Weight, Turning, Height) и габариты хитбокса, справа Preview team Blue | Orange (Q / E), внизу карусель из шести карточек с профилем кузова; ← → меняют кузов на подиуме сразу, Enter / A выбирает (пишется в `game_settings.json`, тост «… equipped»), на не выбранной машине полоски показывают разницу с выбранной. Входы: пункт главного меню, строка Car в Play, Change car (G / Y) в комнате Private match (панель «YOUR CAR» под приглашением) и в лобби матча; Back возвращает туда, откуда пришли. Виджеты кита: `CarCard`, `StatBar`, `Image`.
- **Training** (G1-training, [rl-training.md](rl-training.md)): слева Free play и наборы Striker / Goalie / Aerial / Kick-off, справа панель сфокусированного пункта (у набора таблица упражнений: сделано, попытки, лучшее время и скорость удара из `training.json`); Enter запускает тренировку через Loading, в игре HUD тренировки (плашка с упражнением, таймер, баннеры SUCCESS / MISS), клавиши R / N / P (D-pad на паде), пауза и выход возвращают на этот экран.
- **Loading**: строка конфига, «STANDARD ARENA», совет с кнопкой в рамке, бар. Матч строится после третьего кадра экрана (стройка занимает мгновение), бар — max(время, прогресс `LoadingService`), экран не короче 0.8 с.
- **Lobby** (Private match, лобби матча G5 после Start в комнате: хост поднимает `RlNetHost` на транспорте сервиса, гости подключаются к нему; хост стартует матч сам, когда подключились все игроки комнаты, не позже чем через 12 с): две команды, имена и пинги гостей, у хоста Start match (при «Fill with bots: Off» — только когда все слоты заняты людьми), у всех Leave / Cancel; клиент видит «Waiting for the host». Сбой сети где угодно → «CONNECTION FAILED» на главном меню с причиной (порт занят, нет ответа, хост закрыл соединение, другой `rl_tuning.json`).
- **Match**: HUD кита на реальных данных, см. ниже. Esc / Start / `M` — пауза (по кнопке `Menu` из `RlControls`).
- **Pause**: Resume, Settings (после неё возврат в паузу), Restart match (только офлайн, с подтверждением), Leave match (с подтверждением); в тренировке «Restart training» и «Leave training»; справа панель со счётом и временем. Офлайн матч стоит, онлайн идёт дальше (машина без ввода).
- **Results**: баннер победителя, счёт, чип OT, MVP (максимум очков в победившей команде, дальше голы, дальше меньший слот), таблица игроков, машина победившей команды на подиуме, Play again (офлайн — новый матч; онлайн только у хоста: он заново открывает лобби, клиентам нужно подключиться снова) и Main menu. Экран появляется через 1.6 с после конца матча.
- Курсор мыши прячется в матче и через 2 с без движения в меню. Каждый переход 0.18 с, фон-сцена не перезапускается.

### Настройки

Один файл `game_settings.json` в `core::apppaths::UserDataDirectory()` (`%APPDATA%\myengine` в разработке, `%LOCALAPPDATA%\RocketArena` в shipping), пишется при выходе из Settings, при старте матча и при выходе из игры; битый файл не ломает запуск (значения по умолчанию), значения вне диапазона зажимаются, незнакомые ключи игнорируются (тест `rl_game`). Раскладка клавиш — `rl_controls.json` там же; свой `rl_tuning.json` из окна RL Tuning в shipping тоже пишется туда и читается вместо файла дистрибутива.

| Вкладка | Что | Как применяется |
| --- | --- | --- |
| Controls | 11 строк (газ, тормоз, руль, тангаж, крен, прыжок, буст, powerslide, ball cam, таблица, пауза), две плашки: клавиатура / мышь и геймпад; Invert pitch; Pad deadzone | сразу в матче и в файл `rl_controls.json` |
| Camera | поле зрения, дистанция, высота, угол, жёсткость, swivel, transition (см, как в RL), Ball cam: Toggle / Hold | сразу; камера игрока заменяет числа камеры из `rl_tuning.json` только для картинки (в хэш сетевого матча она не входит) |
| Video | режим окна, разрешение, VSync, лимит кадров (60 / 120 / 144 / 240 / без лимита), тени, сглаживание (MSAA 4×), bloom, UI scale 75–125 %, Show FPS | `IRenderAdapter::SetVSync`, `SetRenderSettings`, `Window::SetDisplayMode`; ключ `--render-settings` в командной строке сильнее качества из настроек |
| Audio | Master, Effects, Music | `RlFeedback::SetVolumes` (шины) |

- **Video, смена окна.** Режим или разрешение применяются сразу и спрашивают «KEEP THESE SETTINGS?» с таймером 15 с: Keep оставляет, Revert или истёкший таймер возвращают прежнее. Borderless занимает рабочий стол и показывает его разрешение, Fullscreen ставит разрешение дисплея (`ChangeDisplaySettingsEx`, возвращается при смене режима и выходе).
- **Camera, живой просмотр.** Вместо маленького окна справа за экраном настроек идёт настоящая игра: практика сама ездит по арене, а камера считается теми же числами, что вы двигаете, поэтому поле зрения и дистанция видны в точности как в матче (по краю прозрачный градиент, а не рамка из мокапа). В паузе за экраном — застывший матч.
- **Controls, как переназначается.** `RlControls` хранит токены (`W`, `-S`, `MouseLeft`, `PadRT`, `-PadLT`). Строка из двух половин оси (руль, тангаж, крен) просит клавиши по очереди (сначала «влево / вниз», потом «вправо / вверх»); стик геймпада сразу занимает всю ось (двигать его надо в положительную сторону: вправо, на себя). Клавиша, которая уже занята на этом устройстве, меняется местами с прежней владелицей, внизу тост «Jump: F <-> Boost». Enter на плашке — переназначить (окно с таймером 5 с, Esc — отмена, Del — очистить), Del / X — очистить, R / Y — вернуть строку к значению по умолчанию, Reset to defaults — все привязки (подтверждение).

### HUD на реальных данных

Каждый кадр система переводит мир в `HudState` (фаза, таймер, часы, счёт, буст машины игрока и «тратится ли он», демолиш и таймер респауна, последний гол со скоростью в км/ч, таблица игроков с очками, голами, ассистами, сейвами, ударами, демолишами, имена, пинг) и достаёт из кольца событий матча новые записи (гол, автогол, ассист, сейв, удар, демолиш) — `Hud::PushEvent`. Событие помнится по тику, поэтому откат клиента не показывает одну строку дважды. Расчёт статистики — [rl-match.md](rl-match.md) (G1.0), передача по сети — [rl-netcode.md](rl-netcode.md). Таблица по Tab, ping и подсказка ball cam — как в ките; ping офлайн скрыт, у хоста 0, у клиента RTT.

Звук и эффекты (`RlFeedback`, [rl-feedback.md](rl-feedback.md)) подключены целиком: старт и конец матча, `OnTick` после каждого тика (у сети — через наблюдателей хоста и клиента, откат безопасен), `Frame` раз в кадр, музыка меню, громкости из Settings; клики меню — события банка `ui_click`, `ui_hover`, `ui_back`.

### Проверка без человека

- `MYENGINE_UI_SCRIPT=<файл>` — скрипт из строк `<секунды> <команда>`: `key <имя> [down|up|tap]`, `pad <кнопка>`, `axis <ось> <значение>`, `mouse x y`, `click [left|right]`, `wheel n`, `shot <файл.tga>`, `game <команда>`, `note`, `quit`. Клавиши и мышь попадают и в кит (через `Host::SetRawInputHook`), и в игровой `InputManager`. `game goal-blue | goal-orange | demolish | boost` кладёт мяч в ворота и т. п. в офлайн-матче.
- `MYENGINE_GAME_SETTINGS=<файл>` — другой файл настроек (чтобы тест не трогал настройки человека); `APPDATA` на тест подменяется отдельно. `MYENGINE_RL_MATCH_SECONDS=<n>` — короткий матч для экрана итогов.
- `.team/tasks/g1-evidence/run.sh <скрипт> [настройки.json]` запускает игру со своей папкой пользователя и превращает скриншоты в PNG; сценарии — `.team/tasks/g1-evidence/scripts/` (`mockups.txt` снимает все 13 экранов мокапов в 1920×1080: `rl-main.png`, `rl-play.png`, …).
- Тест `rl_game` (`tests/ui/RlGameTests.cpp`, `ctest -R rl_game`, без окна): файл настроек (умолчания, круг записи и чтения, зажим, битый файл, имя на границе символа), разбор и порядок команд скрипта, и `RlGameSystem` через команды экранов — матч от старта до итогов (часы, пауза стоит, гол, лента, победитель, MVP, повторный старт и рестарт), лобби хоста и занятый порт, переназначение и сброс клавиш, камера игрока.

### Замеры

4v4 с HUD, 1920×1080 borderless, RTX 3050 Laptop, VSync выключен, RelWithDebInfo: кадр целиком 2.2 мс (≈ 450 FPS), `UiManager::Update` 0.24 мс, из них экраны флоу (HUD) 0.04–0.07 мс. Чтение клавиш — один вызов `GetKeyboardState` на кадр: 256 вызовов `GetAsyncKeyState` в начале стоили на этой машине 20 мс на кадр (80 мкс на вызов), поэтому `Host` читает состояние клавиатуры разом.

## Ограничения

- Экраны и переходы — игровой режим выше; галерея (`MYENGINE_UI_GALLERY`) остаётся как справочник компоновки и стенд для дизайнера.
- Живой просмотр камеры в игре — вся сцена за экраном настроек, а не окно с рендером в текстуру (см. «Игровой режим»); рамка «LIVE PREVIEW» остаётся только в галерее.
- 3D-сцена за меню (машина на подиуме, облёт камеры) — часть `RlGameSystem`, не кита.
- Размер окна 1080p и выше на экране 1080p система обрезает: для скриншотов 1:1 с мокапами нужен экран больше.
- `Host::Install` пересоздаёт ImGui-контекст окна при смене размерного класса шрифтов (редактор в этот момент недоступен: вместе с игровым UI он и не показывается).

## Повтор гола (G7e)

`HudState::replay` (`HudReplay`: `active`, `author`, `team`, `goalSpeedKmh`, `progress`, `speed`): пока `active`, `Hud::Draw` рисует вместо баннеров, отсчёта, ленты и буст-кольца рамки letterbox (сверху 10.8 %, снизу 7.8 % высоты, выезжают за 0.35 с), метку REPLAY с мигающей красной точкой, автора и скорость мяча на голе цветом команды, метку скорости `0.5x` в замедлении, линию прогресса клипа и подсказку «Space / A — Skip replay»; табло и пинг остаются поверх рамок. В галерее — экран `hud-replay` (`MYENGINE_UI_GALLERY=hud-replay`). Заполняет структуру игра из `RlDirectorFrame::replay` ([rl-replay.md](rl-replay.md)).

## Тренировка (G4g)

`HudState::training` (`HudTraining`): пока `active`, `Hud::Draw` вместо табло рисует компонент тренировки (остаются пинг, обратный отсчёт кик-оффа, буст-кольцо и подсказка ball cam). Поля: `phase` (`HudTrainingPhase`: `Ready`, `Running`, `Success`, `Miss`, `PackDone` — те же числа, что у `rl::RlTrainingPhase`, и `FreePlay`), `pack`, `exercise`, `hint`, `index` / `count` («3 / 8»), `attempt` / `attempts`, `timeLeft` / `timeLimit` / `elapsed`, `bannerTime` / `bannerDuration`, `missReason` («TIME UP», «OWN GOAL», «LOST KICK-OFF»), `outOfAttempts`, `newBest`, счётчики (`hits`, `lastHitKmh`, `bestHitKmh`, `maxTouchHeight`), `infiniteBoost`, рекорд упражнения (`recordTime`, `recordHitKmh`), `done` (сводка набора) и `hints` (список `HudKeyHint {key, pad, label}`; пусто — обычные подсказки фазы).

Куски по отдельности (в `Space::Screen`, подсказки — в `Space::Menu` на строке подсказок меню): `DrawTrainingPlate` (плашка слева сверху: набор, «3 / 8» справа, название, подсказка, точки попыток — потраченные красные, текущая голубая, и «BEST 4.2 s · 112 km/h»; в свободной игре — четыре счётчика и метка ENDLESS BOOST), `DrawTrainingTimer` (таймер сверху посередине, обратный отсчёт с полосой, последние 3 с мигает красным; в свободной игре — время сессии), `DrawTrainingBanner` (SUCCESS зелёный, MISS красный, PACK COMPLETE синий и GET READY мигающим текстом; въезжает за 0.25 с, уезжает за 0.2 с до конца `bannerDuration`), `DrawTrainingHints` (ряд клавиш внизу слева; на геймпаде надписи кнопок). Экран галереи — `hud-training` (цикл 10 с). Заполняет структуру игра из `RlTrainingInfo` ([rl-training.md](rl-training.md)).

Что сделано не полностью и что не проверялось (G1-flow):

- Хост лобби не может перекинуть слот в другую команду (← / → на слоте из спеки §1.6): для этого нужна команда в протоколе G5. Игроки занимают слоты по порядку подключения.
- «Play again» онлайн — это заново открытое лобби у хоста: клиентам нужно подключиться снова (матч G5 нельзя начать второй раз на тех же соединениях).
- Геймпад: экраны читают XInput и подсказки переключаются на кнопки, но устройства на машине разработки не было — проверены только клавиатура и мышь и скриптовые события пада (`pad`, `axis` в `MYENGINE_UI_SCRIPT`), а не настоящий контроллер.
- Fullscreen с разрешением, отличным от рабочего стола, не запускался на мониторе разработчика (смена режима дисплея мигает экраном); проверены Windowed, Borderless и Fullscreen на разрешении рабочего стола, подтверждение и откат за 15 с.
- Раскладка проверена на 1280×720, 1600×900 и 1920×1080 (и кит — на 1440p, 4K, 21:9 в `rl_ui`); живой запуск на 21:9 и 4K не делался.
- Имя игрока — имя пользователя Windows (до 15 байт); поле имени в Settings нет, Steam — G6.
