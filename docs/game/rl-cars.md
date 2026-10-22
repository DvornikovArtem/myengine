# Машины гаража (G4i-assets+ui)

Шесть оригинальных кузовов под шесть хитбоксов Rocket League, данные машин, профильные превью и экран **Garage** (меню, строка Car в Play, Change car в лобби). Выбор хранится в `game_settings.json`, на подиуме меню, в Garage и в матче у локального игрока стоит выбранный кузов. **Физика у всех машин пока одна — Pulse** (`rl_tuning.json`): числа машины в данных лежат в объекте `physics`, который G4i-physics заменит по машинам; до тех пор кузов и хитбокс — только картинка и подпись, а полоски Garage считаются из `physics` и сравниваются между машинами набора.

Все модели, текстуры и превью нарисованы генератором «по мотивам»: ни один ассет Rocket League не используется. Размеры хитбоксов и колёс — числа из конфигов [RocketSim](https://github.com/ZealanL/RocketSim) (MIT, © 2022 ZealanL; `src/Sim/Car/CarConfig/CarConfig.cpp`, коммит `c2baacb8f4b441dd8505e63c2aeb5a1679b60b02`, тот же, что у дифф-теста `rl_rocketsim_diff`).

| Файл | Что |
| --- | --- |
| `tools/rl_assets/cars.py` | таблицы RocketSim (`ROCKETSIM`), шесть `Design` (профиль корпуса, кабина, детали, ливрея), `body_texture`, `build_body`, `build_lights` |
| `tools/rl_assets/cardata.py` | `assets/data/cars/<id>.json` и профильные PNG `assets/ui/cars/<id>_<team>.png` |
| `assets/models/rl2/car_<id>_body.obj`, `car_<id>_lights.obj`, `assets/textures/rl2/car_<id>_{blue,orange}.tga`, `assets/materials/rl2/car_<id>_{blue,orange}.material.json` | меш, лампы, краска двух команд (у Pulse имена прежние: `car_body`, `car_lights`, `car_blue`, `car_orange`); колёса `car_wheel` и пламя `car_boost` общие |
| `assets/data/cars/*.json` | данные машин |
| `assets/ui/cars/*.png` | 12 превью 512×256 RGBA в профиль, нос вправо |
| `engine/include/myengine/game/RlCars.h`, `src/game/RlCars.cpp` | `RlCarCatalog`: чтение данных, `FindOrDefault`, полоски `Segments` |
| `engine/src/game/RlGameFlow.cpp`, `engine/src/ecs/systems/RlGameSystem.cpp` | экран Garage и показ выбора: подиум, матч, лобби, итоги |
| `engine/include/myengine/ui/rl/RlUiWidgets.h` | виджеты кита `CarCard`, `StatBar`, `Image` (`RlUiPaint.h`), иконки `Car`, `Check`, `Ruler`, глиф `G` |
| `tests/rl/RlCarsTests.cpp`, `tests/ui/RlGameTests.cpp` | тесты `rl_cars` и `rl_game` |

## Машины

| Ключ | Название | Класс | Хитбокс RL | Размер хитбокса (длина × ширина × высота, uu) | Колёса: радиус перед / зад, uu | Характер |
| --- | --- | --- | --- | --- | --- | --- |
| `pulse` | Pulse | Balanced | Octane | 120.507 × 86.6994 × 38.6591 | 12.5 / 15 | клиновидный хэтчбек со спойлером, как был |
| `vector` | Vector | Long | Dominus | 130.427 × 85.7799 × 33.8 | 12 / 13.5 | низкое длинное купе, плоский нос, утиный хвост, белая полоса по центру |
| `slate` | Slate | Flat | Plank | 131.32 × 87.1704 × 31.8944 | 12.5 / 17 | плоский и широкий, лопата перед носом, большое низкое крыло, фартуки колёсных арок |
| `dart` | Dart | Low-long | Breakout | 133.992 × 83.021 × 32.8 | 13.5 / 15 | длинный узкий спидстер, острый нос, кабина сзади, киль и передние плавники |
| `fusion` | Fusion | Hybrid | Hybrid | 129.519 × 84.6879 × 36.6591 | 12.5 / 15 | купе между Pulse и Vector: двухцветный капот, лёгкий спойлер |
| `bulwark` | Bulwark | Tall | Merc | 123.22 × 79.2103 × 44.1591 | 15 / 15 | высокий коробчатый универсал с багажником на крыше и кенгурятником |

У каждой машины своя ливрея (общая раскладка атласа 1024×1024, цвет команды синий / оранжевый), свои лампы по габаритам носа и хвоста, свой профиль корпуса. Бюджет: тело + лампы + 4 колеса — Vector 2892, Slate 2952, Dart 2880, Fusion 2904, Bulwark 3024, Pulse 2512 треугольников при лимите 4000. Тело строится вокруг хитбокса: длина как у хитбокса (+ вынос носа и хвоста), крыша над хитбоксом, низ немного ниже его, арки под колёса по их радиусу и подвеске (`Design.belly`). Колёса на мешах машин тоже по числам RocketSim.

## Данные машины (`assets/data/cars/<id>.json`)

```json
{
  "id": "vector", "name": "Vector", "class": "Long", "hitboxClass": "Dominus",
  "description": "Long and low. Hits hard off the flat nose.", "order": 1,
  "hitbox": {
    "halfExtentX": 0.42890, "halfExtentY": 0.16900, "halfExtentZ": 0.65214, "comOffsetY": -0.1575, "comOffsetZ": -0.09,
    "wheelMountXFront": 0.311, "wheelMountXRear": 0.33, "wheelFrontZ": 0.413, "wheelRearZ": -0.4375, "wheelMountYOffset": 0.0,
    "wheelRadiusFront": 0.12, "wheelRadiusRear": 0.135, "suspensionRestFront": 0.2195, "suspensionRestRear": 0.2185,
    "rocketSim": { "size": [130.427, 85.7799, 33.8], "offset": [9.0, 0.0, 15.75] }
  },
  "mesh": { "body": "assets/models/rl2/car_vector_body.obj", "bodyScale": 1.07, "lights": "...", "lightsScale": 0.996,
            "materials": { "blue": "...", "orange": "..." }, "boostNozzle": [0.0, -0.05, -0.78] },
  "preview": { "blue": "assets/ui/cars/vector_blue.png", "orange": "assets/ui/cars/vector_orange.png" },
  "physics": { "mass": 180.0, "maxSpeed": 23.0, "throttleAccel": 16.0, "steerSpeed": [...], "steerAngle": [...], ... }
}
```

- **`hitbox`** — именно те ключи, что у секции `car` файла `rl_tuning.json` (`halfExtentX/Y/Z`, `comOffsetY/Z`, `wheelMountX*`, `wheel*Z`, `wheelMountYOffset`, `wheelRadius*`, `suspensionRest*`): G4i-physics может подставить их в `RlCarTuning` как есть. Пересчёт из RocketSim: полуразмеры `size / 2`, смещение центра масс `-offset`, колёса — точка подвеса минус смещение хитбокса, пружина = длина покоя − 12 uu хода. Тест проверяет все шесть хитбоксов против таблицы RocketSim.
- **`mesh`** — меши и краска двух команд; `bodyScale` / `lightsScale` — масштабы сущностей (меши нормированы под сферу отсечения, как у остального пака); `boostNozzle` — где начинается пламя буста.
- **`physics`** — подмножество секции `car` (`mass`, `inertiaMultiplier`, `friction`, `maxSpeed`, `maxAngularSpeed`, `supersonic*`, `throttleAccel`, `driveSpeed` / `driveFactor`, `brakeAccel`, `steerSpeed` / `steerAngle`, `powerslide*`, `latFriction*`, `handbrakeLatFriction`, прыжок, додж, воздух, буст): у всех шести **значения Pulse** на момент генерации (читаются из `rl_tuning.json`). G4i-physics меняет числа в этих файлах (и, если нужно, генератор `cardata.py`); читающий код не меняется.
- **Новая машина:** строка в `cars.ROCKETSIM` (если новый хитбокс) и `Design` в `cars.DESIGNS`, `python tools/gen_rl_assets.py pack --only car` — меши, текстуры, материалы, данные и превью появятся сами, экран Garage подхватит её из папки.

## Полоски Garage

Из данных (`RlCarCatalog::Segments`): сырые значения — `maxSpeed` (Top speed), `throttleAccel` (Acceleration), `mass` (Weight), угол колёс на 10 м/с из `steerAngle` (Turning), высота хитбокса (Height). Для каждой полоски минимум набора — 3 сегмента, максимум — 10, остальное по месту между ними; если машины не отличаются (физика у всех Pulse), полоска стоит посередине. Сейчас различается только Height (Slate 3, Dart 4, Vector 4, Fusion 6, Pulse 7, Bulwark 10), остальные — по 6 у всех.

## Экран Garage и показ выбора

- **Вход.** Главное меню: пункт Garage между Play и Training, справа внизу плашка «YOUR CAR · PULSE · BALANCED». Play → Offline: строка **Car** под Bot difficulty (Enter открывает Garage, Back возвращает с выбором). Комната Private match: панель «YOUR CAR» под приглашением с кнопкой **Change car** (G / Y). Лобби матча: кнопка **Change car** под командами и G / Y; Garage открывается поверх комнаты или лобби. В паузе матча Garage не открывается.
- **Garage.** Сцена — подиум меню, камера за 0.4 с переходит в кадр гаража (чуть ближе и ниже), машина вращается (оборот за 20 с), перетаскивание мышью или правый стик вращают её руками, через 3 с без ввода вращение возвращается. Слева: «GARAGE», название машины, чипы (класс, «HITBOX ≈ OCTANE», «EQUIPPED»), описание, пять полосок, строка «Hitbox 1.21 × 0.87 × 0.39 m». Справа сверху переключатель Preview team Blue | Orange (Q / E, LB / RB) красит только предпросмотр. Снизу карусель шести карточек (270×150, в фокусе 270×176 белая) с профилем кузова (синий вариант, в фокусе затемнён) и отметкой EQUIPPED; ← → (D-pad, стик) двигают фокус, **кузов на подиуме меняется сразу**, без затемнения. Enter / A — выбрать (тост «Slate equipped», `game_settings.json` пишется сразу), Esc / B — назад без изменений, мышь: наведение — фокус, двойной клик — выбрать, колесо листает карусель.
- **Сравнение.** Когда в фокусе не выбранная машина, на полосках видна разница с выбранной: общие сегменты синие; прибавка у Top speed / Acceleration / Turning зелёная, потеря — пустой сегмент с красной рамкой; у Weight и Height разница голубая (компромисс); справа «+2» / «−3», а в строке габаритов «compared with Pulse».
- **Матч.** Машина локального игрока — выбранная (меш, краска команды, лампы, пламя буста у её сопла), боты и сетевые игроки — Pulse (сеть выбор не передаёт: G4i-physics). Меши и текстуры всех машин и превью грузятся за экраном загрузки (`PreloadCars`), поэтому карусель не подтормаживает.
- **Лобби, итоги, таблица.** В строке каждого слота профиль, название и класс машины (свой слот — выбранная машина, остальные — Pulse); в таблице Tab и итогов под именем игрока название машины; на подиуме итогов — машина выбранной, если победила команда игрока, иначе Pulse; под плашкой MVP — «PULSE · BALANCED»; на экране загрузки «Your car: PULSE».
- **Края.** Первый запуск — Pulse. Машины из файла настроек нет в данных (убрали) — Pulse и тост «Your car is no longer available — Pulse selected» на главном меню. Ключ в `game_settings.json` очищается до букв, цифр и `-` `_`.

Скриншоты — `.team/tasks/g4i-evidence/` (`rl-main-garage`, `rl-garage`, `rl-garage-compare`, `rl-garage-orange`, `rl-play-car`, `rl-lobby-cars`, `rl-lobby-garage`, `rl-match-car`, `rl-hud-scoreboard-cars` в 1080p; `-720` и `-219` — 1280×720 и 21:9; `garage-carousel.gif`; `cars-previews.png`), сценарии — `scripts/*.txt`.

## Тест `rl_cars` и проверки

- `rl_cars` (`ctest -R rl_cars`, без окна): шесть файлов читаются; id, названия, классы и хитбокс-классы по таблице; **хитбокс, колёса, радиусы и пружины каждой машины равны числам RocketSim**; Pulse равен машине `rl_tuning.json` (хитбокс, колёса, физика); файлы, которые называет машина (меши, краска и её текстура, превью — PNG 512×256 RGBA), есть; кузов лежит вокруг хитбокса (длина, ширина, крыша над хитбоксом, низ чуть ниже) и укладывается в 4000 треугольников с лампами и колёсами; сопло за хвостом; полоски (Slate 3 / Bulwark 10 по высоте, рост по хитбоксу, середина при равных числах, пара машин с разной массой и скоростью); битые данные (не JSON, нет частей, хитбокс без размера, файл в папке не читается — назван и пропущен, неизвестный ключ — Pulse, без Pulse — первая машина); ключ машины в настройках (по умолчанию, круг записи, очистка, пустой и неверного типа).
- `rl_game`: `SetSelectedCar` (неизвестный ключ — Pulse), подиум меню с машиной игрока, Garage (машина в фокусе заменяет кузов на следующем кадре, краска выбранной команды, лампы, возврат к выбранной после просмотра), матч (машина игрока в матче, бот — Pulse, названия машин в таблице), подиум после матча.
- `rl_ui`: отрисовка экранов кита и шрифты (в наборе теперь «≈»).

## Ограничения

- Физика всех машин — Pulse: хитбокс Vector длиннее и ниже в данных, но матч считается по хитбоксу из `rl_tuning.json`; кузов Slate, Dart, Vector, Fusion, Bulwark может чуть торчать из хитбокса Pulse или не доставать до него. G4i-physics подставит `hitbox` и `physics` машины в симуляцию, и картинка совпадёт.
- Колёса в матче стоят там, где их считает симуляция (по `rl_tuning.json`): у машин с другими осями арки кузова и колёса расходятся на 1–4 см. На подиуме и в Garage колёса ставятся по данным машины.
- Выбор машины по сети не передаётся: у гостей и ботов Pulse, в лобби чужие слоты показывают Pulse; «смена видна второму игроку за 1 с» — работа G4i-physics (Join / Welcome / лобби).
- Цвет акцента (необязательный пункт спеки) не сделан: у кузовов нет отдельной маски акцента.
- Превью — статичные PNG из программного растеризатора (`raster.py`), без теней и бликов движка; синий вариант в карточках, оранжевый лежит рядом для лобби и будущих экранов.
