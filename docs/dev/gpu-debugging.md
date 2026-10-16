# QA1: отладка GPU — маркеры, захват кадра, отчёт, DRED

Как понять, что движок рисует не так: посмотреть кадр в RenderDoc или PIX, получить текстовый отчёт без GUI, а если GPU упал — прочитать breadcrumbs.

## Что есть

| Что | Как включить | Нужен ли внешний инструмент |
| --- | --- | --- |
| Маркеры (группы) на всех проходах | всегда | нет (видны в RenderDoc/PIX) |
| Статистика кадра (draws, треугольники, смены PSO/текстур, барьеры, время GPU) | `--frame-stats N` | нет |
| Захват кадра `.rdc` | `F12`, меню Help > Developer > Capture GPU Frame, `--gpu-capture-frame N` | RenderDoc |
| Текстовый отчёт по захвату | `tools/gpu/frame_report.py` | RenderDoc |
| DRED (breadcrumbs, page fault) | `--dx12-debug` или `--dx12-dred` | нет |
| GPU-based validation | `--dx12-gbv` | Graphics Tools (Windows optional feature) |

## Установка инструментов

```bash
winget install BaldurKarlsson.RenderDoc
winget install Microsoft.PIX
```

Без них всё, кроме захвата `.rdc` и отчёта по нему, работает. Захват без RenderDoc не падает молча: F12 и меню показывают окно «Cannot capture a GPU frame» с подсказкой по установке, CLI печатает то же сообщение и завершается с кодом 3.

Движок ищет `renderdoc.dll` в `RENDERDOC_DLL` (файл или папка), затем в `Program Files\RenderDoc`, затем в `PATH`. PIX: `WinPixGpuCapturer.dll` из самой новой папки `Program Files\Microsoft PIX\<версия>`.

## Маркеры

Каждый проход обёрнут в `BeginEvent/EndEvent` (имена читаются в RenderDoc и PIX):

```
Frame
  Clear Back Buffer
  Scene
  Debug Lines
  UI
  Present Transition
Thumbnail: <файл ассета>      // превью Content Browser
Live view: <id>                // живые превью (Asset Viewers и т.п.)
```

`Scene`, `Debug Lines`, `UI` — группы по виду draw-вызовов: адаптер переключает группу, когда вид отрисовки меняется. Проходы в render target (миниатюры, превью) подписываются через `IRenderAdapter::SetRenderTargetLabel` перед `BeginRenderTarget`; без подписи получат имя `Render Target #<номер>`.

## Захват кадра

### Из редактора

1. Запустить движок так, чтобы RenderDoc подгрузился **до** создания устройства: `myengine.exe --renderdoc` (или `MYENGINE_RENDERDOC=1`).
2. `F12` или Help > Developer > Capture GPU Frame.
3. Файл появится в `<проект>\Saved\Captures\<карта>_<время>_frame<N>.rdc`, путь пишется в `logs/myengine.log`.

Если RenderDoc найден только в момент нажатия F12, движок скажет, что нужен перезапуск с `--renderdoc`: подключиться к уже созданному устройству RenderDoc не может.

### Из командной строки

```bash
myengine.exe --scene assets/scenes/coin_guard_demo.json --play --gpu-capture-frame 120 --exit-after-capture
```

| Код выхода | Значение |
| --- | --- |
| 0 | захват записан (путь напечатан в консоль и в лог) |
| 3 | RenderDoc не найден (сообщение с командой установки) |
| 4 | файл не появился за 60 кадров (окно свёрнуто?) |
| 5 | нет статистики кадра (`--frame-stats`) |
| 6 | устройство DX12 потеряно (см. DRED ниже) |

`--gpu-capture-frame N` захватывает N-й нарисованный кадр: запрос ставится после кадра N-1, RenderDoc снимает следующий. При `N = 1` будет снят кадр 2. Карта грузится синхронно (без splash), поэтому счёт кадров детерминирован.

### PIX

Движок грузит capturer по `--pix` (или `MYENGINE_PIX=1`), дальше захват делается кнопкой в самом PIX (attach к процессу). Программный захват из движка потребует NuGet-пакет WinPixEventRuntime и не сделан; маркеры в PIX видны и без него.

## Текстовый отчёт по захвату

```bash
python tools/gpu/frame_report.py Saved/Captures/coin_guard_demo_20261016_120000_frame120.rdc
python tools/gpu/frame_report.py capture.rdc --json report.json --markdown report.md
python tools/gpu/frame_report.py --self-test    # проверка разбора без RenderDoc
```

Отчёт содержит дерево маркеров, draws/dispatches/clears/copies/треугольники по группам, уникальные PSO, размер viewport группы, счётчики вызовов API (барьеры и т.д.) и предупреждения. Источники, по порядку:

1. `import renderdoc` в этом интерпретаторе (модуль RenderDoc подходит только по версии Python): полный replay-отчёт, с render targets и шейдерами.
2. `renderdoccmd.exe convert -c xml`: вызовы API из захвата по порядку, группы восстанавливаются по `BeginEvent/EndEvent`. Не нужен ни GPU, ни Python-модуль, окно не открывается. Именно этот путь работает с обычным установленным RenderDoc; в нём нет шейдеров и размеров текстур.

Если RenderDoc стоит не в Program Files, задайте `RENDERDOC_DIR`.

Пример (`coin_guard_demo`, кадр 60): Frame > Clear Back Buffer (2 clear), Scene (14 draws, 13308 треугольников), Debug Lines (1), UI (28), Present Transition; 43 draws, 3 PSO, 2 барьера. Совпадает с `--frame-stats`.

## Статистика кадра без RenderDoc

```bash
myengine.exe --scene assets/scenes/rocket_league.json --play --frame-stats 120
myengine.exe --scene assets/scenes/coin_guard_demo.json --play --frame-stats 120 --frame-stats-out out.json
```

После кадра N движок печатает таблицу в консоль, пишет JSON в `Saved/FrameStats/frame_stats_<карта>_<N>.json` (или в `--frame-stats-out`) и завершается. Время GPU берётся из timestamp-запросов и читается после того, как fence кадра прошёл; нули в `GPU ms` у групп короче разрешения таймера — нормально. Пример (RelWithDebInfo, `coin_guard_demo`, кадр 120): 43 draws, ~16k треугольников, 3 смены PSO, 14 смен текстур, 2 барьера.

В режиме Debug значения времени GPU завышены слоем отладки, для сравнения берите RelWithDebInfo.

## Потеря устройства: DRED и GBV

| Переключатель | Эффект |
| --- | --- |
| `--dx12-debug` / `MYENGINE_DX12_DEBUG=1` | debug layer (в любой конфигурации) + DRED |
| `--dx12-dred` / `MYENGINE_DX12_DRED=1` | только DRED: auto-breadcrumbs и page fault |
| `--dx12-gbv` / `MYENGINE_DX12_GBV=1` | GPU-based validation (кадр в разы медленнее; нужен Graphics Tools) |

Когда `GetDeviceRemovedReason` вернул ошибку (на `Present` или в начале кадра), в лог пишется причина, номер кадра и данные DRED:

```
[ERROR] DX12 device removed (Present), reason 0x887a0005 at frame 40
[ERROR] DRED breadcrumbs #0: commands 213 of 240 completed, list "..." queue "..."
[ERROR]   around the last completed command: 211:DrawIndexedInstanced >>213:ResourceBarrier<< 214:...
[ERROR] DRED page fault at GPU address 0x...
```

Движок завершается с кодом 6 и показывает сообщение. Чтобы проверить тракт, можно удалить устройство нарочно: `MYENGINE_DX12_TEST_REMOVE_DEVICE=<кадр>` (вместе с `--dx12-dred`). Для синтетического удаления DRED обычно сообщает «no breadcrumbs were recorded»: узлы breadcrumbs живут, пока список команд выполняется, и у быстрого кадра они успевают освободиться. Содержательные breadcrumbs ожидаются при настоящем зависании или page fault.

## Частые проблемы

- `--gpu-capture-frame` завершается с кодом 3: RenderDoc не установлен; `winget install BaldurKarlsson.RenderDoc` или `RENDERDOC_DLL`.
- Код 4: окно свёрнуто или закрыто диалогом, Present не вызывался.
- `--dx12-gbv` пишет в лог «GPU-based validation is not available on this system»: включите optional feature «Graphics Tools» в Windows.
- Консольный вывод от GUI-процесса: запускайте из терминала (движок присоединяется к консоли родителя) или перенаправляйте stdout (`Start-Process -RedirectStandardOutput`).
