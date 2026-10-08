# ЛР 2: замеры скриптинга

## Иван — D4, проверка профилирования

Дата: 08.10.2026. Конфигурация трейса: `RelWithDebInfo`, Tracy 0.14.1.
Исходники: ветка `scripting/d4-profiling`, база `bd8f342` плюс локальные изменения D4, ещё не закоммиченные на момент записи.

Это технический тест D4: проверяем, что вызовы скриптов видны в Tracy и время скриптов попадает в Statistics. Это **не игровой benchmark, не замер FPS и не сравнение до/после**. Сцены пользователя для этой записи не изменялись.

### Сохранённые файлы

- [d4_1.tracy](traces/ivan/d4_1.tracy) — исходный трейс автотеста `myengine_script_api_tests --wait-for-tracy`.
- [d4_1_capture.log](traces/ivan/d4_1_capture.log) — вывод Tracy capture.
- [d4_1_scripts.csv](traces/ivan/csv/d4_1_scripts.csv) — отдельные события, фильтр `Scripts::`, экспорт с `-u`.
- [d4_1_summary.csv](traces/ivan/csv/d4_1_summary.csv) — сводка по именам зон, тот же фильтр, без `-u`.
- [d4_debug_ctest.log](traces/ivan/d4_debug_ctest.log) и [d4_relwithdebinfo_ctest.log](traces/ivan/d4_relwithdebinfo_ctest.log) — результаты повторного запуска CTest на том же коде.
- [d4_debug_smoke.log](traces/ivan/d4_debug_smoke.log) и [d4_relwithdebinfo_smoke.log](traces/ivan/d4_relwithdebinfo_smoke.log) — результаты оконного теста, по 120 кадров.

SHA-256 исходного трейса:

```text
dd9cf540bfc9706199bae59a6a7dad14d0d293347a757af0556c179035b2cdd1
```

### Условия записи

Capture запущен перед headless-тестом API. Флаг `--wait-for-tracy` ждёт подключение профайлера до начала тестовых сценариев. Python встроен в движок; установленный системный Python не нужен.

Тест последовательно проверяет API, безопасные ссылки на сущности, жизненный цикл, сообщения, события, prefabs, hot reload и профилирование. Поле `src_file` в CSV сохраняет путь на компьютере Ивана, а `src_line` — строку исходника на момент записи.

Во время записи также выполнялся оконный smoke-тест Debug. Запуск не изолирован для сравнения производительности. Кроме того, тестовое поведение `ProfileProbe` намеренно создаёт нагрузку: около 10 мс в `OnUpdate` и ещё около 10 мс во вложенном сообщении. В игровых скриптах этой нагрузки нет.

### Что получилось

По логу capture: длительность трейса 891.08 мс, 302 зоны, 2 маркера кадров. Это маркеры короткого headless-запуска, а не два отрисованных игровых кадра.

После фильтра `Scripts::` получено 286 событий и 38 разных имён зон. Из них 155 событий — отдельные вызовы через границу C++ → Python; остальные — общие фазы `ScriptSystem`. Эти 155 событий включают создание, reload, сообщения и попытки вызова методов, а не только методы жизненного цикла.

| Метод жизненного цикла | Число зон вызова |
| --- | ---: |
| `OnStart` | 22 |
| `OnUpdate` | 78 |
| `OnCollision` | 2 |
| `OnTrigger` | 2 |
| `OnDestroy` | 4 |
| `OnReload` | 6 |

Числа учитывают вызовы через `SafeCall`, в том числе пустые унаследованные методы. Наличие зоны не означает, что метод был переопределён в Python.

Также в записи есть `Counter.add`, `Counter.fail`, `Broken.OnUpdate`, `Counter.create`, `ReloadProbe.reload` и `ProfileProbe.work`. Зоны с исключениями завершены, отрицательных длительностей нет. У разных экземпляров `Counter` различаются ID в тексте зоны.

Все отфильтрованные зоны записаны на главном потоке, с индексом Tracy `thread=3`. Это индекс внутри трейса, не системный ID потока.

Пример искусственной нагрузки: `Scripts::api_behaviours.ProfileProbe.work` занимает 10 008 973 нс, то есть 10.008973 мс. Это подтверждает запись вложенного вызова, а не демонстрирует скорость игрового скрипта.

### Как читать CSV

CSV сохранены в UTF-8. Значения и числовая точность исходного экспорта Tracy сохранены; окончания строк нормализованы в LF.

В `d4_1_scripts.csv`:

- `name` — имя зоны, например `Scripts::api_behaviours.Counter.OnUpdate`.
- `src_file`, `src_line` — место создания зоны в C++.
- `ns_since_start` — временная отметка начала события на шкале Tracy, в наносекундах.
- `exec_time_ns` — длительность события, в наносекундах; для миллисекунд разделить на 1 000 000.
- `thread` — индекс потока в Tracy.
- `value` — текст зоны: `entity=<id>; script=<index>`. Для reload целого модуля `entity=0`; у общих фаз текст может отсутствовать.

В `d4_1_summary.csv`: `total_ns` — суммарная длительность одной группы, `counts` — число событий, `mean_ns` / `min_ns` / `max_ns` / `std_ns` — среднее, минимум, максимум и стандартное отклонение в наносекундах. `mean_ns` сохраняет целочисленное усечение оригинального экспорта. `total_perc` — процент от длительности трейса, не загрузка CPU и не FPS.

Длительности включают вложенные зоны. **Нельзя складывать время родителя и дочернего вызова:** например, `ProfileProbe.work` уже входит во время вызывающего `OnUpdate`, а тот — в `Scripts::Update`. По той же причине нельзя складывать проценты всех строк. Бакет Statistics `scripts` тоже уже входит в `world`.

Проверка экспорта: для всех 38 групп число, сумма, среднее, минимум и максимум согласуются с отдельными событиями. Проверены все шесть методов жизненного цикла и метаданные разных сущностей.

### Автотесты

CTest: 5 из 5 тестов успешно в Debug и 5 из 5 в RelWithDebInfo. Время повторного запуска: 8.76 с и 3.69 с соответственно; это время тестового набора, а не время кадра движка.

Оконный smoke-тест успешно прошёл по 120 кадров в обеих конфигурациях. Он проверяет публикацию `scripts` в статистику окон, корректность времени относительно `World update`, сохранение Play-снимка и штатное завершение.

Профильный сценарий API проверяет вложенное сообщение без двойного учёта времени, исключение, пропуск `Faulted`, режим Edit, сброс статистики и отсутствие рантайма. Исходники: [ScriptApiTests.cpp](../../../tests/scripting/ScriptApiTests.cpp), [api_behaviours.py](../../../tests/scripting/api_behaviours.py), [ScriptingSceneSmoke.cpp](../../../tests/scene/ScriptingSceneSmoke.cpp).

### Как повторить запись в PowerShell

Закрыть запущенный движок перед сборкой. Каждую команду вводить отдельно. Ниже используется новое имя `d4_2`, чтобы не перезаписать сохранённый `d4_1`; при повторных запусках выбрать следующий свободный номер.

```powershell
cmake --build "C:\Users\ivanm\source\repos\myengine\build" --config RelWithDebInfo --target myengine_script_api_tests
```

Сначала в отдельном окне PowerShell запустить Tracy:

```powershell
& "C:\Users\ivanm\source\repos\tracy\tracy-capture.exe" -o "C:\Users\ivanm\source\repos\myengine\docs\measurements\lab2\traces\ivan\d4_2.tracy" -s 5
```

Затем в другом окне запустить тест:

```powershell
& "C:\Users\ivanm\source\repos\myengine\build\tests\RelWithDebInfo\myengine_script_api_tests.exe" --wait-for-tracy
```

После завершения capture экспортировать отдельные события:

```powershell
& "C:\Users\ivanm\source\repos\tracy\tracy-csvexport.exe" -u -f "Scripts::" "C:\Users\ivanm\source\repos\myengine\docs\measurements\lab2\traces\ivan\d4_2.tracy" | Out-File -Encoding utf8 "C:\Users\ivanm\source\repos\myengine\docs\measurements\lab2\traces\ivan\csv\d4_2_scripts.csv"
```

Отдельно экспортировать сводку:

```powershell
& "C:\Users\ivanm\source\repos\tracy\tracy-csvexport.exe" -f "Scripts::" "C:\Users\ivanm\source\repos\myengine\docs\measurements\lab2\traces\ivan\d4_2.tracy" | Out-File -Encoding utf8 "C:\Users\ivanm\source\repos\myengine\docs\measurements\lab2\traces\ivan\csv\d4_2_summary.csv"
```

Описание реализации и ручной проверки Statistics: [D4: профилирование скриптов](../../scripting/d4-profiling.md).
