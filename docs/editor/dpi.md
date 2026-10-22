# Масштаб экрана Windows в редакторе (RL-B1)

`myengine.exe` — Per-Monitor V2 DPI-aware ([`app/resources/myengine.manifest`](../../app/resources/myengine.manifest)): окно, мышь и swap chain в настоящих пикселях. Редактор при этом выглядит так же, как на экране 100 % (те же размеры панелей, шрифтов, отступов), но рисуется чётко. Для игрового UI всё устроено иначе, он подстраивается под размер окна: [../game/rl-ui.md](../game/rl-ui.md), «Масштаб экрана Windows».

## Зачем это нужно

Раньше процесс был DPI-unaware: при 125 % Windows давал ему виртуальный экран 1536×864 и растягивал кадр до 1920×1080, текст редактора мылился. Просто включить DPI-awareness недостаточно: ImGui считает в пикселях, и весь редактор стал бы в 1,25 раза мельче. Поэтому раскладка остаётся в прежних («логических») пикселях, а масштаб применяется на границах.

## Что сделано

| Что | Где | За что отвечает |
| --- | --- | --- |
| Арифметика масштаба | [`DpiScale.h`](../../engine/include/myengine/core/DpiScale.h), [`DpiScale.cpp`](../../engine/src/core/DpiScale.cpp) | DPI → масштаб, длины, логические ↔ настоящие пиксели, DPI из `WM_DPICHANGED`, «следовать ли предложенному прямоугольнику» |
| Окно | [`Window.h`](../../engine/include/myengine/core/Window.h), [`Window.cpp`](../../engine/src/core/Window.cpp) | размер по умолчанию × масштаб, `Dpi()` / `DpiScale()`, `OnDpiChanged`, координаты drag-and-drop файлов в логических пикселях |
| `WM_DPICHANGED`, область рендера | [`Application.cpp`](../../engine/src/core/Application.cpp) | передача сообщения окну и `UiManager`, перевод viewport редактора в пиксели (`TryBuildEditorRenderRegion`) |
| ImGui в логических пикселях | [`UiManager.h`](../../engine/include/myengine/ui/UiManager.h), [`UiManager.cpp`](../../engine/src/ui/UiManager.cpp) | `DisplaySize`, `DisplayFramebufferScale`, плотность шрифтов, вершины в рендерер, мышь, пересборка при смене масштаба |
| Манифест | [`myengine.manifest`](../../app/resources/myengine.manifest), [`app/CMakeLists.txt`](../../app/CMakeLists.txt) | Per-Monitor V2 для `myengine.exe` |
| Тест | [`DpiScaleTests.cpp`](../../tests/core/DpiScaleTests.cpp) | ctest `dpi_scale` |

## Как это устроено

Масштаб окна `s = DPI окна / 96` (1.25 при 125 %). `UiManager` хранит его в контексте каждого окна (`WindowUiContext::uiScale`).

- **Раскладка — в логических пикселях.** `io.DisplaySize = размер окна / s`: на экране 1920×1080 при 125 % ImGui видит 1536×864, ровно как раньше видел виртуальный экран. Поэтому все размеры панелей, шрифты в пикселях и раскладка доков в `.ini` остаются прежними и остаются валидными.
- **Шрифты — с плотностью `s`.** `io.DisplayFramebufferScale = s`, каждый `ImFontConfig` (текст, иконки, моно, шрифт по умолчанию) получает `RasterizerDensity = s`: размер глифа в логических пикселях прежний, но растр в `s` раз плотнее, поэтому текст чёткий, а не растянутый.
- **Вершины — в настоящие пиксели при передаче в рендерер.** Позиции вершин, смещение и перенос (`translationX/Y`) умножаются на `s`; границы scissor считаются из `ClipRect` с тем же `s` (`FramebufferScale`) и зажимаются в размер окна.
- **Мышь — в логические.** Позиция из `WM_MOUSEMOVE` и кнопок делится на `s` перед `AddMousePosEvent`; `CanStartSceneNavigation` тоже получает мышь в настоящих пикселях и переводит её сам.
- **Viewport редактора — в пиксели.** Прямоугольник сцены задаёт ImGui в логических пикселях; `Application::TryBuildEditorRenderRegion` умножает его на `UiManager::GetUiScale(windowId)` и зажимает в размер заднего буфера.
- **Drag-and-drop файлов.** Координаты клиентской области (из OLE) переводятся в логические в `Window.cpp`, потому что панели ловят их в раскладке ImGui.
- **Смена масштаба.** `WM_DPICHANGED` → `Window::OnDpiChanged` (Windowed берёт прямоугольник Windows, Borderless и Fullscreen нет) и `UiManager::OnWindowDpiChanged`: если эффективный масштаб изменился, взводится `fontRebuild_`, и на следующем `Update` контексты окон создаются заново с атласами под новый масштаб (раскладка доков сохраняется в `.ini`).
- **Игровой UI.** Пока включён игровой UI (`SetGameUi`), `EffectiveUiScale` равен 1: он масштабирует себя сам по размеру окна, второй множитель дал бы двойной масштаб. Его шрифты растеризует FreeType, а не stb_truetype, как у редактора, и слой DPI редактора к ним не относится: [../game/rl-ui.md](../game/rl-ui.md), «Резкость текста».

## Правила для нового UI-кода

- **Раскладывайте всегда в логических пикселях:** `ImGui::GetContentRegionAvail()`, `ImVec2(…)`, размеры окон, `ImGui::GetMousePos()` — это логические пиксели, ничего не умножайте на DPI.
- **Настоящими пикселями считаются только границы** с окном, рендером и мышью: размер окна и swap chain (`Window::Width()` / `Height()`), прямоугольник области рендера, координаты из Win32-сообщений и OLE. Если число приходит оттуда в ImGui — делите на `UiManager::GetUiScale(windowId)` (`dpi::ToLogical`); если уходит туда из ImGui — умножайте (`dpi::ToPhysical`).
- Новый шрифт запекайте с `RasterizerDensity = uiScale` (см. `addTextFont` в `UiManager::CreateWindowContext`), иначе он будет растянут и мыльный.
- Новый путь передачи вершин ImGui в рендерер должен применять тот же множитель, что и `RenderWindow`.

## Проверка

`ctest -R dpi_scale` (`tests/core/DpiScaleTests.cpp`): DPI → масштаб (96…384, нулевой и слишком большой), длины (1280×720 → 1600×900 при 125 %), логические ↔ настоящие пиксели, `WM_DPICHANGED` (DPI из `wParam`, Windowed следует предложенному прямоугольнику, Borderless и Fullscreen нет), масштаб и размерный класс шрифтов игрового UI, окно без показа (его DPI, размер клиентской области, `OnDpiChanged`, `scaleToDpi = false`; без рабочего стола эти проверки пропускаются). Сам редактор на 125 % и 150 % глазами и смену монитора проверять вручную.
