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
hud.Draw(f, scoreboardHeld);            // Tab / Back зажаты: затемнение, табло и таблица игроков
// в паузе: hud.Draw(f, false, true) — только табло, приглушённое; в итогах HUD не рисуется, а таблицу берёт экран: Hud::DrawScoreTable(...)
```

Что делает `Hud` сам: отсчёт 3 · 2 · 1 по `phaseTimer` (140 % → 100 % за 0.15 с, гаснет в последние 0.15 с секунды) и GO! 0.5 с после перехода в `Playing`; баннеры GOAL! / OWN GOAL (цвет команды, имя и км/ч) с въездом 0.25 с и выездом 0.2 с, OVERTIME! 1.5 с, DEMOLITION / DEMOLISHED BY (по событию `Demo`, когда игрок — автор или жертва) и «Respawning 2.4»; табло с миганием последних 30 с (белый ↔ красный раз в секунду) и пульсом 8 % в последние 10 с, овертайм `+0:42` с подписью OVERTIME, тренировка (`mode 0`) — только время; буст-кольцо (дуга 270° с градиентом, пульс 6 Гц при расходе, вспышка при подборе пада, красный ноль); лента до 5 строк по 5 с; подсказка ball cam 2 с после переключения и постоянно первые 30 с; пинг (зелёный < 60, жёлтый < 120, красный) и FPS. Данные таблицы и ленты берутся из G1.0 (`points`, `assists`, `saves`, `shots`, `demos`, `events`, `names`, `lastGoalSpeed`); до него HUD работает на тех же структурах с нулями.

## Галерея

```
set MYENGINE_UI_GALLERY=<имя>     имена: main, play, private, loading, pause, results, controls, rebind, camera, video, hud-kickoff, hud-goal, hud-scoreboard; all — по кругу, 4 с на экран
myengine.exe --scene assets/scenes/scene.json --screenshot out.tga --screenshot-frame 70 --exit-after-frames 75
```

Переменная окружения, а не флаг: разбор аргументов `main.cpp` принадлежит режиму `--game` (G1-flow); регистрация одна — блок в `Application::Initialize` (`MYENGINE_UI_GALLERY`). Галерея живая: фокус, вкладки, слайдеры, переключатели, поле адреса работают на фейковых данных. Фон — `paint::MenuBackdrop` и нарисованная «сцена» (машина, поле) вместо 3D. Окно 1920×1080 на экране 1080p система обрезает по рабочей области, поэтому скриншоты сняты на 1600×900 (масштаб 0.83; пропорции те же).

## Шрифты и иконки

`assets/fonts/exo2/`: `Exo2-BlackItalic.ttf` (900i), `Exo2-ExtraBoldItalic.ttf` (800i), `Exo2-Bold.ttf` (700), `Exo2-SemiBold.ttf` (600) и `OFL.txt` — статические начертания, нарезанные из variable-шрифтов `google/fonts` (`ofl/exo2`) скриптом `tools/make_exo2.py` (fontTools `varLib.instancer`; скрипт сам скачивает исходники и убирает их; variable-файлы в репозитории не хранятся). Латиница и кириллица (имена игроков). Иконки — Lucide (`assets/fonts/lucide.ttf`, ISC): play, target, settings, log-out, refresh-cw, house, star, user, bot, wifi, wifi-off, shield, zap, circle-dot, server, log-in, eye, info, chevrons, стрелки. Лицензия Exo 2 добавлена в `tools/package_manifest.json` и `THIRD_PARTY_NOTICES.txt` дистрибутива.

## Тесты и замеры

Тест `rl_ui` (`tests/ui/RlUiTests.cpp`, `ctest -R rl_ui`, без окна и GPU: ImGui работает без экрана, черновые списки отрисовки читаются после кадра):
- ввод: повтор удержанной стрелки (≈ 8 срабатываний за секунду), WASD как буквы в поле, устройство последнего ввода, `CaptureToken` (клавиша, мышь, триггер и стик пада, Esc / Del не возвращаются);
- фокус: первый кадр, ↓ / ↑ по геометрии, конец списка не зацикливается, Enter один раз за нажатие, наведение и клик мыши, выключенный элемент пропускается, `ResetFocus`;
- виджеты: `OptionRow`, `ToggleRow`, `SliderRow` (шаг, Shift ×5, края, мышь), `StepperRow`, `TextRow` (разрешённые символы, длина, Backspace, буквы не двигают фокус), `BindRow` (плашки, переназначить, очистить, сброс), `Tabs` (Q/E, LB/RB), `ConfirmDialog` (область, фокус на Cancel, Back = Cancel, возврат фокуса), `ScrollView` (прокрутка к фокусу, колесо);
- логика `Hud`: цифры отсчёта, GO!, часы (`4:32`, `0:01`, `+0:42`), баннеры (цель, автогол, овертайм) и их срок, лента (5 строк, 5 с), баннер демолиша;
- раскладка на 1280×720, 1920×1080, 2560×1440, 3840×2160, 2560×1080 и 3440×1440: табло по центру окна, 22 px сверху, 466 px шириной на 1080p; буст-кольцо в 60 px от правого и 50 px от нижнего края; кнопка меню в колонке 16:9;
- отрисовка всех 13 экранов галереи на четырёх размерах окна (вершин больше 400 и меньше 150 000), все шрифты спеки запечены (в том числе кириллица в текстовых начертаниях и цифры с GO в кегле отсчёта).

**Стоимость:** кадр HUD матча 4v4 (табло, кольцо, баннер, лента из 5 строк, таблица в половине кадров) — медиана 0.025 мс CPU на кадр (построение списка отрисовки, 1100–3500 вершин). GPU, полный кадр галереи вместе с пустой сценой редактора на RTX 3050 Laptop, 1600×900: HUD с таблицей 0.49 мс, HUD с баннером 0.35 мс, главное меню 0.33 мс (`--frame-stats`); бюджет HUD для 144 FPS (6.9 мс на кадр) занят меньше чем на 1/10.

## Ограничения

- Экранов (меню, настройки, пауза, итоги, лобби) как логики здесь нет: галерея показывает их компоновку, не переходы; это G1-flow.
- Живой превью камеры (RT из R2) — только заглушка в галерее: окно «LIVE PREVIEW» с нарисованной сценой; кит даёт рамку и подпись, 3D рисует экран.
- 3D-сцена за меню (машина на подиуме) — не часть кита.
- Размер окна 1080p и выше на экране 1080p система обрезает: для скриншотов 1:1 с мокапами нужен экран больше.
- `Host::Install` пересоздаёт ImGui-контекст окна при смене размерного класса шрифтов (редактор в этот момент недоступен: вместе с игровым UI он и не показывается).
