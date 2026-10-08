# D4: профилирование скриптов

## Зачем это нужно

D4 не меняет игровую механику и не ускоряет Python. Она показывает, сколько времени занимают скрипты, чтобы при разработке T6 находить дорогие вызовы и сравнивать изменения.

## Что сделано

Все вызовы через `ScriptSystem::SafeCall` получили отдельную зону Tracy. Имя зоны состоит из модуля, класса и метода:

```text
Scripts::coin.Coin.OnUpdate
Scripts::chaser.Chaser.OnTrigger
Scripts::game_manager.GameManager.add_score
```

Измеряются `OnStart`, `OnUpdate`, `OnCollision`, `OnTrigger`, `OnDestroy`, `OnReload`, методы сообщений `me.send`, а также создание экземпляров (`create`) и перенос состояния при reload (`reload`). Пустая C++-реализация метода, который не переопределён в Python, тоже проходит через эту границу и может иметь короткую зону.

В тексте зоны — `entity=<id>; script=<index>`. ID не входит в имя: все копии одного поведения объединяются в одну группу в Tracy и CSV, а конкретную сущность можно посмотреть в подробностях вызова. Для перезагрузки модуля целиком ID равен 0 (нет конкретной сущности), имя — `Scripts::<module>.reload`. Имена ограничены 1024 байтами. Tracy копирует имя и текст при записи события; Python-объекты или указатели на компоненты в профайлер не передаются.

Общие зоны `Scripts::Update`, `Scripts::HotReload`, `Scripts::Start`, `Scripts::Events`, `Scripts::OnUpdate`, `Scripts::Flush` сохранены. Отдельные вызовы вложены в них. Вложенный `send` виден внутри вызывающего метода; исключение корректно закрывает зону, после чего прежний `SafeCall` пишет ошибку и переводит экземпляр в `Faulted`.

Tracy, как и раньше, включена только в `RelWithDebInfo`. В `Debug` код создания имён и записи отдельных зон исключается при компиляции.

## Бакет scripts в Statistics

В Statistics добавлена строка `Scripts: ... ms (part of World update)`. Это время **всего шага `ScriptSystem::Update` на главном потоке**: вызовы поведений, создание экземпляров, доставка событий, удаление, применение hot reload, обработка готовых результатов вотчера и обновление статистики.

Фоновое чтение файлов в Streaming job в этот бакет не входит. Оно имеет отдельную зону `FileWatcher::Scan`. Прямой вызов одного Python-метода из другого без перехода через C++ не создаёт новую отдельную зону: его время входит в зону внешнего метода.

Измерение выполняется один раз вокруг всего шага, а не сложением длительностей отдельных методов. Поэтому вложенные `send` не увеличивают результат дважды. При раннем выходе, отсутствии интерпретатора и обработке ошибки публикуется новый замер; `ResetInstances` / `Shutdown` сбрасывают старое значение. В Edit время может быть ненулевым: вотчер и статистика продолжают работать.

Время измеряется и в Debug обычными часами `steady_clock`, независимо от Tracy. В `FrameTimes avg_ms` в логе добавлено `scripts=...` — среднее за тот же интервал, что и другие поля.

**Не складывать `scripts` и `world`: `scripts` уже входит в `world`.** Statistics показывает опубликованные результаты предыдущего завершённого кадра, как и остальные времена кадра; счётчики экземпляров остаются в существующем разделе.

## Проверка в PowerShell

Перед сборкой закрыть работающий движок. Команды вводить по одной.

```powershell
cmake --build "C:\Users\ivanm\source\repos\myengine\build" --config Debug --target myengine myengine_script_api_tests
```

```powershell
& "C:\Users\ivanm\source\repos\myengine\build\tests\Debug\myengine_script_api_tests.exe"
```

```powershell
cmake --build "C:\Users\ivanm\source\repos\myengine\build" --config RelWithDebInfo --target myengine myengine_script_api_tests
```

```powershell
& "C:\Users\ivanm\source\repos\myengine\build\tests\RelWithDebInfo\myengine_script_api_tests.exe"
```

Ожидается `OK: ... profiling`. Тест проверяет измерение вложенного сообщения, кадра с исключением, пропуск `Faulted`, переход в Edit, сброс и отсутствие рантайма. Искусственная нагрузка есть только в тестовом модуле; реальные скрипты и сцены не меняются.

В редакторе открыть Statistics и посмотреть строку Scripts. Без игровых скриптов время будет небольшим. Оконный тест `myengine_scripting_smoke` дополнительно проверяет публикацию бакета в статистику окон.

Сохранённый трейс, CSV и логи проверки лежат в `docs/measurements/lab2/traces/ivan`; условия и результаты описаны в [README замеров](../measurements/lab2/README.md). Это техническая проверка D4, а не игровой benchmark.

Для повторной записи автотеста сначала запустить capture в отдельном окне PowerShell. Ниже новое имя `d4_2`, чтобы не перезаписать сохранённый `d4_1`; если оно уже занято, выбрать следующий свободный номер:

```powershell
& "C:\Users\ivanm\source\repos\tracy\tracy-capture.exe" -o "C:\Users\ivanm\source\repos\myengine\docs\measurements\lab2\traces\ivan\d4_2.tracy" -s 5
```

Затем в другом окне запустить:

```powershell
& "C:\Users\ivanm\source\repos\myengine\build\tests\RelWithDebInfo\myengine_script_api_tests.exe" --wait-for-tracy
```

Флаг ждёт подключения Tracy до 10 секунд: без него короткий автотест может завершиться до повторной попытки соединения. Обычные автотесты не ждут профайлер. В Debug флаг сообщает, что нужна RelWithDebInfo.

В Tracy искать `Scripts::api_behaviours.Counter.OnUpdate`, `Scripts::api_behaviours.Listener.OnCollision` или `Scripts::api_behaviours.ProfileProbe.work`. Для CSV фильтровать полное имя нужной зоны, как при прошлых замерах. После T6 можно записать обычный запуск игровой сцены; флаг ожидания нужен только автотесту.

## Результаты проверки

- Движок, headless-тесты и оконный smoke-тест собраны в Debug и RelWithDebInfo.
- CTest: все 5 тестов проходят в обеих конфигурациях.
- Оконный smoke-тест: 120 кадров, бакет scripts опубликован во всех окнах, его время не превышает World update, сохранение Play-снимка и выход работают.
- Записан и сохранён настоящий RelWithDebInfo-трейс автотеста вместе с двумя CSV-экспортами и логами проверок. Экспорт подтвердил зоны всех шести методов жизненного цикла, сообщений, создания и reload. Проверены ID разных экземпляров и завершение зон с исключениями. [Сохранённые результаты](../measurements/lab2/README.md).
- Исходные сцены не изменены. В сборке остаются прежние предупреждения LNK4099 о недостающих PDB сторонних DirectXTK12 / DirectXTex; ошибок и новых предупреждений C++ нет.

## Git

Ветка `scripting/d4-profiling` создана от `origin/feature/scripting` (`bd8f342`), куда уже влиты T2 и T4. PR направлять в `feature/scripting`. Локальные пользовательские изменения `benchmark.json` и `scripting_demo.json` не относятся к D4.
