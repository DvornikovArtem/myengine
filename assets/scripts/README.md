# Скрипты игры

Здесь лежат `Python`-скрипты механики (`*.py`). Движок добавляет эту папку в `sys.path` и читает скрипты отсюда (а не из копии рядом с exe), поэтому правки подхватываются `hot reload`.

Архитектура и правила - в `docs/scripting/architecture.md`.

Механика T6: `game_manager.py`, `coin_spawner.py`, `coin.py`, `enemy_spawner.py`, `chaser.py`, `firework.py`.
Готовая сцена - `assets/scenes/coin_guard_demo.json`. Правила написаны на Python; баланс хранится в `props` сцены и prefab.
Запуск, управление и проверки описаны в [T6: сбор монет под охраной](../../docs/scripting/t6-gameplay.md).

Поля шаблонов можно менять и сохранять через окно `Prefabs` (`View → Prefabs`): [T8: редактор prefab](../../docs/scripting/t8-prefab-editor.md).
После `Save` новые значения получают следующие заспавненные объекты; существующие экземпляры не меняются.

Rocket League (RL1): `rocket_league.py`, шаблоны `assets/prefabs/rl_*.prefab.json`, карта `assets/scenes/rocket_league.json`. Play, затем `1`–`4`:
`W`/`S` газ и тормоз, `A`/`D` руль, `Space` прыжок (второй `Space` с направлением — рывок), `Shift` буст, `Ctrl` powerslide,
`Q`/`E` air roll, `C` камера на мяч, `M`/`Esc` меню. Текстуры и сетки генерирует `tools/gen_rl_assets.py`; подробности — в [RL1](../../docs/scripting/rl1-rocket-league.md).
