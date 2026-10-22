# G8: замеры кадра матча 4v4

Описание, таблицы и выводы: [../../game/rl-performance.md](../../game/rl-performance.md).

- `run_perf.ps1`, `analyze_perf.py`, `compare_perf.py`: как снять и разобрать (шапки скриптов).
- `scenario-*.txt`, `settings-*.json`: сценарии `MYENGINE_UI_SCRIPT` и настройки игры.
- `results/<набор>/results_perf.md|json`: разбор каждого набора прогонов (`before` до исправлений; `final-unlimited` без лимита и VSync; `final-default` с VSync; `final-client`, `final-clientrb` гость с VSync, хост без лимита; `limit-*` ограничитель 60/144/240 и VSync до и после; `quality-*` настройки качества; `ui-limit` настройки через экраны), `*.load.txt` загрузка процессора перед прогоном, %.
- `traces/`: по одному Tracy-трейсу «до» и «после» (офлайн 4v4, прогон 1; открывать `tracy-profiler`); остальные трейсы не хранятся (по 5-15 МБ).
