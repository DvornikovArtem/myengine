# RL1: Rocket League — камера из Python, ввод в Play, текстуры

Игру `assets/scripts/rocket_league.py` написал встроенный ассистент (AI1 / AI2). Эта задача довела её до играбельного состояния: камера от третьего лица, фокус ввода в Play, процедурные текстуры и отдельная карта.

## Как играть

1. Открыть карту `assets/scenes/rocket_league.json` (`File > Open Map...` или Project Browser).
2. Нажать Play. Ввод сразу идет в игру: кликать по окну сцены не нужно.
3. Нажать `1`, `2`, `3` или `4`: матч 1v1 … 4v4. Вы играете за синюю машину с золотой крышей.

| Клавиша | Действие |
| --- | --- |
| `W` / `S` | газ / тормоз и задний ход (в воздухе — наклон носа) |
| `A` / `D` | поворот (в воздухе — рыскание) |
| `Space` | прыжок; в воздухе второй `Space` — второй прыжок, а с `W`/`S`/`A`/`D` — рывок (dodge) |
| `Shift` | буст |
| `Ctrl` | powerslide (в воздухе — свободный air roll) |
| `Q` / `E` | air roll |
| `C` | камера на мяч (Ball Cam) |
| `M` или `Esc` | выйти в меню; на экране итогов — `Enter` |

Арена создается скриптом, поэтому в Edit карта пустая; Stop возвращает сцену к снимку, взятому при Play.

## me.camera

`me.camera.set(position, rotation_deg, fov=None)` двигает основную камеру сцены (`CameraComponent.isPrimary`); viewport в Play рисуется именно через нее. `rotation_deg` — `(pitch, yaw, roll)`, как в компоненте. `fov` — вертикальный угол обзора, от 0 до 180, рендер дополнительно ограничивает его 20–120. Без камеры `set` возвращает `False`.

- Когда скрипт взял камеру, `CameraControllerComponent.scriptControlled` становится `True`, и free-fly контроллер (WASD, мышь) в Play ее не двигает, иначе `W` ехал бы одновременно и машиной, и камерой. Поле не сохраняется в сцену; в Edit оно сбрасывается.
- Изменение камеры — часть сцены: Stop восстанавливает снимок и возвращает камеру на место.
- `me.debug.show_colliders(False)` убирает линии коллайдеров на время игры; `OnDestroy` возвращает их.

## Фокус ввода в Play

Пока в панели редактора активно текстовое поле (чат ассистента, консоль скриптов), нажатия клавиш не доходят до игры (`Application.cpp`, `uiWantsKeyboardCapture`). Поэтому игра «не реагировала», если Play запускали из чата или после ввода текста. Теперь при переходе Edit → Play `SceneEditor` снимает активный виджет ImGui (`ClearActiveID`) и ставит фокус на viewport. Вернуть клавиатуру редактору можно кликом в текстовое поле любой панели; `Esc` в поле снимает фокус обратно.

## Текстуры и материалы

Генератор: `tools/gen_rl_assets.py` (Python + Pillow, `python tools/gen_rl_assets.py`). Он пишет:

- `assets/textures/rl/*.tga` — RLE TGA, формат, который читает `ResourceManager` через DirectXTex;
- `assets/materials/rl/*.material.json` — по материалу на текстуру (`textured_lit`, tint белый);
- `assets/models/rl_box.obj`, `rl_sphere.obj` — куб и UV-сфера с правильными UV. У стандартного `crate.obj` UV повернуты по граням, а `sphere.obj` — икосаэдр без UV.

Поле (газон, разметка, штрафные, цвет ворот), стены, ворота, мяч, машины, пады и трибуны есть в трех темах: `classic`, `neon`, `desert`. Тема выбирается случайно при старте матча. Размеры текстур подобраны под растянутый `rl_box` (поле 42×74 м — 756×1332 px). `ResourceManager` зеркалит OBJ по Z, как assimp `ConvertToLeftHanded`, поэтому генератор пишет `z` с обратным знаком.

## Проверка

- `tests/scripting/ScriptApiTests.cpp` (`TestCameraAndDebug`): `camera.set/get/get_fov`, ошибки аргументов, флаг контроллера, `debug.show_colliders`.
- `tests/controls/ControlSystemsTests.cpp` (`TestScriptControlledCamera`): в Play камера под скриптом не двигается клавишами, в Edit флаг сбрасывается.
- Вручную: карта `rocket_league.json` → Play → `1` → езда, прыжок, буст, гол.
