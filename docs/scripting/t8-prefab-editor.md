# T8: редактор полей prefab-шаблонов

## Зачем нужна задача

T4 умеет создавать объект из `.prefab.json`, T7 умеет показывать поля Python-поведения в инспекторе сущности. T8 соединяет эти возможности: баланс шаблона можно менять в редакторе, не открывая JSON вручную.

Например, `coin.score_value` задаёт очки за монету, `chaser.speed` — скорость врага, `firework.lifetime` — время жизни частицы. Правила игры остаются в Python, числа сохраняются в данных.

## Что сделано

- Окно `Prefabs` в нижней области редактора и пункт `View → Prefabs`.
- Список файлов `assets/prefabs/*.prefab.json`: `chaser`, `coin`, `firework` и любые новые шаблоны.
- Для всех сущностей шаблона, включая детей, показываются их скрипты и поля из `ScriptRuntime::DescribeFields`.
- Используется общий `DrawScriptFields` Артёма из T7: `float` → перетаскивание числа, `int` → целое число, `bool` → переключатель, `str` → текст. Ctrl+клик по числу позволяет ввести точное значение.
- Отсутствующее в `props` поле показывает серое значение по умолчанию из Python; `Reset` удаляет сохранённый ключ. Простое открытие окна ничего не записывает.
- `Save` записывает весь шаблон в его `.prefab.json`, сохраняя остальные компоненты, локальные ID и иерархию.
- `Reload` отменяет несохранённые правки и заново читает файл с диска. Пока есть правки, выбор другого шаблона заблокирован: сначала `Save` или `Reload`.
- Окно работает и в Edit, и в Play. Это редактор шаблона, а не живого экземпляра скрипта.

## Когда применяются изменения

В панели хранится отдельная JSON-копия — черновик. Правки не попадают ни в файл, ни в кэш спавна до нажатия `Save`.

После успешного сохранения **следующий** `world.spawn` получает новые `props`, а его Python-поведение — эти значения при создании. Уже существующие сущности и их состояния не меняются. `Stop` восстанавливает снимок сцены, но не отменяет отдельно сохранённые изменения файла шаблона.

Поэтому можно прямо в Play изменить очки монеты и подбирать новые монеты от спавнера, либо изменить скорость врага и дождаться следующей волны. Если хочется заменить сразу все экземпляры, нажать `Stop`, затем `Play`.

Это не изменение выбранной сущности: правки prefab не помечают сцену как изменённую и не попадают в историю Undo сцены. Для отмены поля есть `Reset`, для отмены всего несохранённого черновика — `Reload`. Закрытие окна сохраняет черновик до завершения сессии; выход не сохраняет его автоматически.

## Сохранение и внешние правки

В `PrefabLibrary` добавлены `ReloadPrefab(name)` и `SavePrefab(name, draft)`. Прежний `SavePrefab(name)` для клиентов T4 сохранён.

Сохранение проверяет JSON во временном мире, пишет временный соседний файл и атомарно заменяет исходный файл. Кэш спавна обновляется после успешной записи. Неудача, например read-only файл, оставляет старый файл и черновик для повторной попытки. В Windows сбрасываются также старые записи того же файла под другим регистром имени (`coin` / `COIN`).

Панель не хранит указатели в кэш между кадрами: `FileWatcher` может удалить запись. Если внешний файл изменился и черновик чистый, после доставки изменения панель обновляется. Если черновик изменён, показывается конфликт; `Save` блокируется до `Reload`. Перед сохранением файл дополнительно перечитывается, чтобы заметить внешнюю правку даже до очередного результата вотчера. Это не система совместного редактирования: после `Reload` несохранённые правки нужно внести заново.

Ошибочный / удалённый шаблон и отсутствующий Python-модуль дают сообщение вместо падения редактора. После исправления файла нажать `Reload`. Если в шаблоне нет скриптов, окно сообщает об этом и ничего не меняет.

## Где код

- [PrefabInspector.h](../../engine/include/myengine/ui/PrefabInspector.h), [PrefabInspector.cpp](../../engine/src/ui/PrefabInspector.cpp): черновик, выбор, сохранение, внешние изменения и отрисовка полей всех сущностей.
- [SceneEditor.cpp](../../engine/src/ui/SceneEditor.cpp): окно, меню View и docking; [Application.cpp](../../engine/src/core/Application.cpp) передаёт существующую библиотеку через `SceneEditorServices`.
- [ScriptInspector.cpp](../../engine/src/ui/ScriptInspector.cpp): общий виджет T7, только уточнены подсказки для сцены и prefab.
- [PrefabLibrary.cpp](../../engine/src/scene/PrefabLibrary.cpp): перечитывание и сохранение отдельного черновика.
- [PrefabInspectorTests.cpp](../../tests/ui/PrefabInspectorTests.cpp): автоматическая проверка реальных ImGui-виджетов, сохранения и Python-экземпляров.

## Как проверить вручную

Закрыть запущенный движок перед сборкой. Команды для PowerShell вводить по отдельности:

```powershell
cmake --build "C:\Users\ivanm\source\repos\myengine\build" --config RelWithDebInfo --target myengine
```

```powershell
& "C:\Users\ivanm\source\repos\myengine\build\app\RelWithDebInfo\myengine.exe" --scene "C:\Users\ivanm\source\repos\myengine\assets\scenes\coin_guard_demo.json"
```

1. Открыть вкладку `Prefabs` снизу; если скрыта, включить `View → Prefabs`.
2. Выбрать `coin`, изменить `score_value` с `1` на `3`. До `Save` новые монеты по-прежнему дают одно очко.
3. Нажать `Save`. Новые монеты спавнера будут давать три очка, прежние сохранят одно. Для простого опыта нажать `Stop → Play`: все монеты заново создадутся из сохранённого шаблона.
4. Выбрать `chaser`, изменить `speed`, `damage`, `aggressive` или `target_name`, сохранить и проверить следующую волну либо `Stop → Play`.
5. Нажать `Reset` у поля и `Save`: на следующем спавне применяется значение по умолчанию из Python, а ключ исчезает из `props` в файле.
6. Изменить поле без сохранения, нажать `Reload`: правка отменяется. Закрыть и открыть окно без Reload — черновик остаётся.

Эта ручная проверка намеренно меняет шаблоны в `assets/prefabs`, а не временные копии. Перед PR проверить их diff и оставить только желаемый баланс.

## Автоматическая проверка

```powershell
cmake -S "C:\Users\ivanm\source\repos\myengine" -B "C:\Users\ivanm\source\repos\myengine\build" -DMYENGINE_BUILD_TESTS=ON
```

```powershell
cmake --build "C:\Users\ivanm\source\repos\myengine\build" --config Debug --target myengine_prefab_inspector_tests
```

```powershell
ctest --test-dir "C:\Users\ivanm\source\repos\myengine\build" -C Debug -R "^prefab_inspector$" --output-on-failure
```

```powershell
cmake --build "C:\Users\ivanm\source\repos\myengine\build" --config RelWithDebInfo --target myengine_prefab_inspector_tests
```

```powershell
ctest --test-dir "C:\Users\ivanm\source\repos\myengine\build" -C RelWithDebInfo -R "^prefab_inspector$" --output-on-failure
```

Тест создаёт временный каталог с кириллицей, копирует туда шаблоны и скрипты и использует встроенный Python. В ImGui без окна ОС проверяются реальные перетаскивания float/int, ввод строки, переключатель и Reset. Далее проверяются черновики, все четыре типа в новом живом Python-экземпляре, неизменность старого экземпляра, поля детей / нескольких скриптов, отсутствие ненужных `props`, внешние правки, неверный JSON, read-only файл и повторное сохранение. Исходные шаблоны и сцены тестом не редактируются.

Оконный `myengine_gameplay_smoke` из T6 дополнительно проверяет настоящий цикл DirectX 12 со всеми панелями редактора на временной сцене. Логи результатов T8 сохраняются в `docs/measurements/lab2/tests/ivan/`.

## Результаты — 08.10.2026

Сборки движка и тестов успешны в Debug и RelWithDebInfo. Полный CTest: **7 из 7** в обеих конфигурациях, включая новый `prefab_inspector`. Оконный тест мини-игры прошёл по 120 кадров в каждой конфигурации, без ошибок скриптов и с нормальным завершением интерпретатора. Тесты использовали временные копии; исходные prefab и сцены не менялись.

- [Debug CTest](../measurements/lab2/tests/ivan/t8_debug_ctest.log), [RelWithDebInfo CTest](../measurements/lab2/tests/ivan/t8_relwithdebinfo_ctest.log).
- [Debug оконный тест](../measurements/lab2/tests/ivan/t8_debug_smoke.log), [RelWithDebInfo оконный тест](../measurements/lab2/tests/ivan/t8_relwithdebinfo_smoke.log).
- [Debug лог движка](../measurements/lab2/tests/ivan/t8_debug_engine.log), [RelWithDebInfo лог движка](../measurements/lab2/tests/ivan/t8_relwithdebinfo_engine.log).

Прежние предупреждения LNK4099 об отсутствующих PDB сторонних DirectXTK12 / DirectXTex остаются; новых предупреждений в нашем C++-коде нет. Новый Tracy-трейс для T8 не записывался: это задача редактора, а не отдельный замер производительности.

## Git

Ветка `scripting/t8-prefab-editor` создана от `origin/feature/scripting` (`e23dd4f`), где уже влиты T6 и D4. PR направлять в `feature/scripting`. Пользовательские локальные изменения `benchmark.json` и `scripting_demo.json` не относятся к T8.
