# Стабильность звука на Wwise: сценарии и результаты

Протокол проверки `AudioSystem` на Wwise под нагрузкой и при порче данных: что ломаем, что должна делать игра, чем это подтверждено. Автоматическая часть - тест `audio_wwise_stress` (только при `-DMYENGINE_WWISE=ON`, без устройства и окна: Wwise рендерит офлайн, время идёт по `Update(dt)`; в ctest 14 с в Debug и 4 с в RelWithDebInfo, лимит 60 с). Показ вживую (с настоящим устройством) - в разделе «Ручной прогон».

| Параметр | Значение |
| --- | --- |
| Дата | 26.10.2026 |
| Машина | Windows 11, AMD Ryzen 7 5800H; устройство звука не нужно (офлайн) |
| Код | ветка `feature/wwise` с задачей W4 (тест [WwiseStressTests.cpp](../../../tests/audio/WwiseStressTests.cpp)) |
| Конфигурации | Debug и RelWithDebInfo (библиотеки Wwise Debug и Profile), `MYENGINE_WWISE=ON`; Release (библиотеки Release, без счёта памяти) собирается и проходит `audio_wwise` |

## Что добавлено для проверки

| Что | Где | За что отвечает |
| --- | --- | --- |
| Тест `audio_wwise_stress` | [WwiseStressTests.cpp](../../../tests/audio/WwiseStressTests.cpp), [CMakeLists.txt](../../../tests/CMakeLists.txt) | Сценарии 1-9 ниже. Таймаут ctest 60 с, на деле 4-15 с |
| `AudioStats::engineMemoryBytes`, `engineObjects` | [AudioTypes.h](../../../engine/include/myengine/audio/AudioTypes.h), [WwiseAudioSystem.cpp](../../../engine/src/audio/wwise/WwiseAudioSystem.cpp) | Память менеджера памяти Wwise (`AK::MemoryMgr::GetGlobalStats`) и число game object, зарегистрированных для голосов (играющие и остановленные, ждущие конца своего события). В Release-библиотеках SDK память не считается: 0 |
| `WwiseRuntime::MemoryUsed`, `CommunicationRunning` | [WwiseRuntime.h](../../../engine/src/audio/wwise/WwiseRuntime.h) | Те же числа для теста рантайма; журнал `Wwise: memory in use at the stop` |
| Асинхронный `LoadBank` с ожиданием | [WwiseRuntime.cpp](../../../engine/src/audio/wwise/WwiseRuntime.cpp) | Синхронный `LoadBank` не возвращался на обрезанном банке (сценарий 6). Теперь запрос асинхронный, а ожидание ответа само вызывает `RenderAudio` и `ProcessBanks`, с пределом 20 с |
| Проверка банка до Wwise | [WwiseAudioSystem.cpp](../../../engine/src/audio/wwise/WwiseAudioSystem.cpp) (`BankFileIsWhole`) | Файл `.bnk` - ряд кусков (имя 4 байта, размер 4 байта, содержимое), который должен кончаться ровно в конце файла и начинаться с `BKHD`. Обрезанный файл отвергается в логе, до Wwise не доходит |
| Идемпотентный `LoadBank` | [WwiseAudioSystem.cpp](../../../engine/src/audio/wwise/WwiseAudioSystem.cpp) | Один и тот же файл банка во второй раз не грузится и не считается; события из json обновляются на месте (слоты держат индексы). Ответ Wwise `AK_BankAlreadyLoaded` не ошибка |

## Таблица сценариев

В колонках записаны числа последнего прогона (Debug и RelWithDebInfo) и результат.

| № | Сценарий | Что делаем | Что ожидаем | Результат |
| --- | --- | --- | --- | --- |
| 1 | Десять тысяч событий за минуты | 14 722 вызова `PlayEvent` за 9000 шагов по 30 мс (270 с звука): смесь 22 событий, петли, случайные позиция, скорость, громкость и pitch, залпы по 60 событий каждые 400 шагов (пул забивается) | Пул заполняется, слабые голоса уступают сильным (`stolen`) или звук отбрасывается (`dropped`); ничего не падает | прошло. Принято 14 521 (`played` 16 007 вместе с прогревом), отброшено 215, вытеснено 1 084; пик 64 голоса из 64, пик game object 60 |
| 2 | Слоты и game object не текут | После сценария 1: `StopAll(0.1)`, ждём конца | `voicesActive` 0 и `engineObjects` 0; память Wwise не больше, чем после прогрева, плюс 0.5 МБ | прошло: 0 и 0; память Wwise после прогрева 11.23 МБ, пик 12.09 МБ (Debug) и 11.64 МБ (RelWithDebInfo), в конце 10.57 МБ |
| 3 | Память процесса | Private bytes процесса до и после сценария 1 | Рост не больше 24 МБ | прошло: Debug 20.7 -> 22.7 МБ, RelWithDebInfo 173.1 -> 180.9 МБ (прирост 2 и 8 МБ; предел теста 24 МБ; вторая сторона цикла (сценарий 8) показывает, что на повторах процесс не растёт) |
| 4 | Старые и чужие handle | `Stop` дважды; `Stop` с затуханием во время затухания; вызовы со старым handle после `Stop`; handle, который никто не выдавал (0, 1, 0xFFFF, 0x10000, 0xFFFFFFFF и другие); `Stop`, `SetVoiceVolume`, `StopAll`, `StopBus` и `Update` после `Shutdown` | Ничего не делают; старый handle не трогает голос, занявший его слот | прошло |
| 5 | `LoadBank` дважды | Второй вызов с тем же json; третий при играющем голосе | Банки считаются один раз (`soundsLoaded` и `soundBytes` те же, что после первого вызова), память не растёт (до 64 КБ), голос не трогается | прошло |
| 6 | Папка банков пуста, банков нет или они обрезаны | Копия проекта во временной папке: пустая папка; нет `Init.bnk`; `RocketArena.bnk` обрезан до 3 000 000 байт, до 200 байт (после заголовка), до 1 байта; нет `RocketArena.bnk` рядом с `Init.bnk` | `LoadBank` возвращает false и пишет ошибку в журнал; `HasEvent` и `PlayEvent` ничего не дают; игра идёт без звука (`Update`, настройки шин, слушатель); `Shutdown` не зависает; настоящие банки потом загружаются и играют | прошло для всех шести случаев. До правки (синхронный `LoadBank`) случай «после заголовка» зависал навсегда в `LoadBank`, а потом и в `Shutdown` (поток менеджера банков не отвечал) |
| 7 | Банк целый по кускам, но без событий | `RocketArena.bnk` обрезан перед куском `HIRC` (события и структура): Wwise грузит такой файл | `LoadBank` true; Wwise не знает событий и отказывает в `PostEvent`: `PlayEvent` возвращает невалидный handle, `dropped` растёт на 1, голос не остаётся, game object не копятся | прошло |
| 8 | Двадцать циклов запуска и остановки | `Initialize`, `LoadBank`, 40 событий, `Shutdown` - двадцать раз подряд | Память Wwise в каждом цикле та же (в пределах 1 МБ), процесс не растёт (до 12 МБ) | прошло: Wwise 11.21 МБ в первом цикле и в самом большом; private bytes после третьего и последнего цикла 2.2 и 2.5 МБ (Debug), 160.4 и 160.4 МБ (RelWithDebInfo) |
| 9 | Стоимость операций | Время `PlayEvent` и `Update` в сценарии 1 | Не взрывается | `PlayEvent` 5.5 мкс (Debug) и 1.3 мкс (RelWithDebInfo) в среднем; `Update` (рендер 30 мс звука) 1.27 и 0.245 мс в среднем, 5.66 и 1.05 мс в худшем шаге |

Счётчики сценария 1 одинаковы в Debug и RelWithDebInfo (14 722 / 14 521 / 215 / 1 084 / пик 64): случайность задана зерном, поведение детерминировано.

## Профайлер и связь с Authoring

`Comm::Init` и `Comm::Term` исполняются (Debug и RelWithDebInfo, библиотека `CommunicationCentral`): `MYENGINE_WWISE_TEST_COMM=1` у теста `audio_wwise_runtime` и `MYENGINE_WWISE_PROFILER=1` у `audio_wwise` печатают `up`. Что не проверено: подключение самого Wwise Authoring к игре и окно брандмауэра Windows при первом запуске. Шаги для человека - [docs/engine/wwise.md](../../engine/wwise.md), раздел «Профайлер Wwise Authoring».

## Ручной прогон (с настоящим устройством)

В автотесте устройство не используется. Вживую (не требует Authoring): собрать с `-DMYENGINE_WWISE=ON`, запустить `myengine.exe --game`, сыграть матч (моторы, буст, удары, трибуны, музыка меню), в Settings → Audio подвигать громкости, выйти из матча в меню и начать новый, закрыть игру. Ожидание: звук не пропадает и не залипает, после закрытия процесс завершается (поток Wwise не удерживает его).

Проверено без человека: ночной замер ([README.md](README.md)) - 12 прогонов по 90 секунд матча с настоящим устройством (6 на Wwise): ни зависаний, ни ошибок звука в журнале, процесс завершался сам по команде `quit` сценария; и тест `audio_wwise` с `MYENGINE_WWISE_TEST_DEVICE=1` (устройство, реальное время, колбэки из потока Wwise).

## Как запустить

```powershell
cmake -S . -B build -DMYENGINE_WWISE=ON
cmake --build build --config RelWithDebInfo --parallel 2 --target myengine_wwise_stress_tests
ctest --test-dir build -C RelWithDebInfo -R audio_wwise_stress -V    # строки "stat:" печатают числа таблицы
```
