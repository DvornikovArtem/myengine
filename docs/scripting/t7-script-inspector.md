# T7: панель Script в инспекторе

## Зачем это нужно

Баланс игры (очки за монету, скорость врага, длительность раунда) хранится в **данных**, а не в коде: в сцене и prefab как `props`. T7 даёт редактору способ эти данные увидеть и править: у выбранной сущности со скриптом в инспекторе появляется панель **Script** с полями Python-класса. В Edit значения редактируются и сохраняются вместе со сценой, в Play показываются текущие значения запущенного объекта.

PR [#23](https://github.com/DvornikovArtem/myengine/pull/23) `scripting/t7-script-inspector`, влит 08.10.2026.

## Что сделано

| Что | Где | За что отвечает |
| --- | --- | --- |
| `DrawScriptFields`, `DrawScriptFieldValues` | [ScriptInspector.h](../../engine/include/myengine/ui/ScriptInspector.h), [ScriptInspector.cpp](../../engine/src/ui/ScriptInspector.cpp) | Общие виджеты полей: редактирование `props` и режим «только чтение» |
| Секция Script в инспекторе | [SceneEditor.cpp](../../engine/src/ui/SceneEditor.cpp) | Заголовок, статус, поля, подсказки, undo |
| Три сервиса редактора | [Application.cpp](../../engine/src/core/Application.cpp) | `describeScriptFields`, `scriptStatus`, `liveScriptFields` — мост редактора к скриптовой подсистеме |
| `GetLiveFields`, `GetInstanceStatus` | [ScriptSystem.cpp](../../engine/src/scripting/ScriptSystem.cpp) | Текущие значения полей и статус экземпляра |
| `live_fields` | [ScriptHelpers.cpp](../../engine/src/scripting/ScriptHelpers.cpp) | Чтение полей живого объекта как JSON |

`DrawScriptFields(fields, props, undo)` вынесена отдельной функцией, чтобы её же использовала панель Prefabs ([T8](t8-prefab-editor.md)) без undo. Сам редактор Python не знает: описание полей он получает от `ScriptRuntime::DescribeFields` ([T5](t5-hot-reload.md)) в виде обычных C++-структур.

## Что видно в панели

Для каждой записи `ScriptComponent` выбранной сущности — строка `модуль.Класс` и статус:

| Режим | Статус | Поля |
| --- | --- | --- |
| Edit | `(applied on Play)` | Редактируемые виджеты |
| Play | `Active`, `Starting`, красное `Faulted - see the log` или `not created` | Текущие значения живого объекта, только чтение |

Если у класса нет редактируемых полей или скрипт не загрузился, показывается подсказка: «No editable fields, or the script failed to load (see the log)» и напоминание, что поле — это атрибут класса с типом (`speed: float = 3.0`).

### Виджеты по типу

| Тип поля | Виджет |
| --- | --- |
| `float` | `DragFloat` |
| `int` | `DragInt` |
| `bool` | `Checkbox` |
| `str` | `InputText` |

### Откуда значение и что с ним происходит (Edit)

- Если ключ есть в `props` и его тип подходит полю, показывается он.
- Если ключа нет, показывается значение по умолчанию из кода класса, **серым цветом**, с подсказкой «Default from the script code». В сцену оно не записывается.
- Правка записывает ключ в `props` сущности. Значение сохранится вместе со сценой и применится при следующем Play (в Play панель только показывает).
- Рядом с сохранённым значением есть кнопка **Reset**: она удаляет ключ из `props`, и снова действует значение по умолчанию из кода.
- Значение неподходящего типа (например, строка в `int`-поле) панель не показывает: вместо него серое значение по умолчанию. Рантайм такое значение тоже игнорирует и пишет предупреждение в лог.
- Правка и Reset попадают в историю отмены: `Edit Script Field <имя>` и `Reset Script Field <имя>`. Ctrl+Z / Ctrl+Y работают как для остальных свойств сущности.

### Play

В Play значения берутся у запущенного объекта (`live_fields`: `getattr` по каждому объявленному полю, значение приводится к типу поля, иначе показывается значение по умолчанию). Виджеты отключены: правка живого объекта пропала бы при Stop. Если свойство в скрипте выбрасывает исключение, панель показывает значения по умолчанию и не падает каждый кадр.

После горячей перезагрузки кэш полей сбрасывается: список полей и значения по умолчанию перечитываются из нового кода. Подробнее — в [T5](t5-hot-reload.md) и [D1](d1-play-reload.md).

## Как проверить

1. `run.bat` (по умолчанию откроется сцена `coin_guard_demo.json` в Edit). В иерархии выбрать `GameManager`: в инспекторе секция Script с `game_manager.GameManager` и полями `target_score`, `round_duration` и другими; значения по умолчанию серые, пока не записаны в сцену.
2. Поставить `target_score` на 3: цвет станет обычным, появится Reset. Нажать Ctrl+Z — значение вернётся и снова станет серым.
3. Play: те же поля, но отключены; статус `Active`. Правка из шага 2 остаётся в сцене после Stop.
4. Вписать в скрипт ошибку в `OnUpdate`: статус станет красным `Faulted - see the log`.

## Проверка

Автотест `script_api` ([ScriptApiTests.cpp](../../tests/scripting/ScriptApiTests.cpp)) проверяет описание полей (`ReloadProbe` — 3 поля), сброс кэша после перезагрузки (значение по умолчанию 8.0 вместо 3.0), статусы `Active` / `Faulted` и чтение живых значений через `GetLiveFields`. Автотест `prefab_inspector` (T8) проверяет общую функцию виджетов на `props`. Сохранённые прогоны: [T8, Debug](../measurements/lab2/tests/ivan/t8_debug_ctest.log) и [RelWithDebInfo](../measurements/lab2/tests/ivan/t8_relwithdebinfo_ctest.log) — CTest 7 из 7. Визуальную часть (серый цвет, Reset, undo) автотесты не проверяют — только вручную, по списку выше.
