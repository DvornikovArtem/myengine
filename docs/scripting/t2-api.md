# T2: доступ Python к движку

## Зачем это нужно

Артем сделал запуск Python (T0), вызовы методов поведения движком (T3), hot reload (T5 + D3 / D1) и инспектор полей (T7). Все эти ветки, как и наш T1, влиты в `feature/scripting`; база этой задачи — `586385d`.

T2 дает обратную связь: теперь сам Python-скрипт может находить объекты, двигать их, читать клавиши, менять физику и обращаться к другим поведениям. Игровая механика и prefab-спавн остаются отдельными T6 и T4.

## Что доступно

- `entity.name`, `id`, `alive`, сравнение и хеширование; `get_script(MyBehaviour)` возвращает экземпляр поведения или `None`.
- `entity.transform`: `position`, `rotation` в градусах, `scale`.
- `entity.rigidbody`: `velocity`, `mass`, `use_gravity`, `is_kinematic`, `is_grounded` (только чтение), `add_impulse(vector)`.
- `entity.collider`: `is_trigger`, `radius`, `half_extents`.
- `entity.mesh`: `mesh`, `material`, `visible`.
- `world.find(name)` возвращает `Entity` или `None`; `find_all(prefix="")` — список по префиксу имени; `find_in_radius(center, radius, prefix="")` — ближайшие первыми, с учетом мировой позиции дочерних объектов.
- `input.is_down(action)`, `was_pressed(action)`, `is_key_down(key)`, `was_key_pressed(key)`. Названия клавиш: `"R"`, `"Space"`, `"F1"`, стрелки, Shift и другие стандартные клавиши. Регистр не важен; неизвестное имя дает `ValueError`.
- `time.dt`, `total`, `frame` обновляются каждый шаг ScriptSystem, в Edit тоже. `total` — сумма переданных dt, а не отдельные часы ОС.
- `send(name, method, *args)` вызывает метод у подходящих поведений сущности. Возвращает `False`, если ничего не доставлено. Ошибка метода отключает получателя через существующий `SafeCall`, а не роняет вызывающий скрипт и движок.
- `hud.set(key, text)` и `hud.clear(key)`: строки рисуются поверх viewport в Play и очищаются при Stop / загрузке сцены.
- `camera.set(position, rotation_deg, fov=None)` ставит основную камеру сцены (ту, через которую рисуется viewport) и возвращает `False`, если камеры нет; `camera.get()` возвращает `(position, rotation_deg)` или `None`, `camera.get_fov()` — вертикальный угол обзора. Подробности: [RL1](rl1-rocket-league.md).
- `debug.show_colliders(visible)` и `debug.colliders_visible()`: линии коллайдеров во viewport (`Show > Colliders`, F3). Флаг живет в редакторе, а не в сцене: игра, которой они мешают, выключает его в `OnStart` и возвращает в `OnDestroy`.

Удаление `entity.destroy()` и рекурсивное удаление детей уже реализованы Артемом в T3. T2 использует эту очередь, не создает вторую. До конца кадра объект еще `alive`, но новые запросы к миру не возвращают помеченный на удаление объект. Повторное удаление безопасно.

## Безопасные ссылки

Прокси компонента хранит только handle сущности, а не адрес компонента. Каждое чтение/изменение заново проверяет сущность и ищет компонент. Если объект удален, выбрасывается `myengine.EntityDeadError`; если живой объект не имеет компонента — `AttributeError`.

Кроме id хранится версия сцены: Stop / загрузка могут повторно использовать те же id, поэтому старая ссылка не должна внезапно указывать на новый объект. Hot reload поведения сам по себе версию сцены не меняет: сохраненные ссылки продолжают работать при переносе состояния L2.

Векторы возвращаются как копии:

```python
import myengine as me

class Mover(me.Behaviour):
    speed: float = 2.0

    def OnUpdate(self, dt):
        transform = self.entity.transform
        transform.position = transform.position + me.Vec3(self.speed * dt, 0, 0)
```

`transform.position.x += 1` меняет только временную копию. Нужно присваивать весь `Vec3`, как в примере. Векторы с NaN/бесконечностью, неположительная масса и размеры коллайдера отклоняются с `ValueError`.

## Проверка

Из корня репозитория, в PowerShell:

```powershell
cmake -S . -B build -DMYENGINE_BUILD_TESTS=ON
cmake --build build --config Debug --target myengine myengine_script_api_tests myengine_job_stress myengine_scene_serialization myengine_controls_tests myengine_scripting_smoke
ctest --test-dir build -C Debug --output-on-failure
& ".\build\tests\Debug\myengine_scripting_smoke.exe"
```

Для второй конфигурации заменить `Debug` на `RelWithDebInfo`. Установленный Python не используется: тест запускает тот же встроенный интерпретатор, что и движок.

`script_api` проверяет изменения C++-компонентов из Python, копии векторов, запросы по именам/радиусу/иерархии, ввод, время, сообщения и HUD, мертвые и сохраненные от другой сцены ссылки. Дополнительно проверяется интеграция с T3/T5/D1/T7: `props`, жизненный цикл, события обеим сторонам, удаление поддерева, ошибки с файлом и строкой, `Faulted`, hot reload L1/L2, сохранение старого кода после SyntaxError и обновление списка полей инспектора.

Скрипты тестов лежат в `tests/scripting`, копируются во временный каталог с кириллицей в пути. Исходные сцены и `assets/scripts` не меняются. При ошибке каталог и лог сохраняются для диагностики; после успешного теста временные файлы удаляются.

Это не заменяет T9: длинная игровая сессия, серия перезагрузок и ручная проверка undo/UI будут нужны после готовой механики.
