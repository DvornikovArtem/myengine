# Скриптинг: архитектура и план разработки (ЛР2)

## Контекст

**Проект.** `myengine` - учебный игровой движок на `C++17` / `DirectX 12` под `Windows`, сборка `CMake + MSVC`. Движок - статическая библиотека `engine/`, приложение - `app/`. Из ЛР1 есть `job system` (пулы `High` на `fibers` и `Streaming`), профайлер `Tracy` (только `RelWithDebInfo`).

**Что требует ЛР2.**
- Скриптовая подсистема на `Python` (`pybind11`): явные инициализация и шатдаун интерпретатора, единая точка загрузки скриптов, понятное время жизни `state`, продуманная дистрибуция рантайма;
- Двусторонние `bindings`: движок -> скрипт через класс поведения с `trampoline`, скрипт -> движок - минимум 4 категории `API` (сущности/трансформ, ввод, спавн/уничтожение, запросы к миру), без висячих ссылок в обе стороны;
- `Hot reload` скриптов без перезапуска движка. Минимум - вне `Play`: поменяли число или условие в файле -> изменение видно при следующем `Play`;
- Одна механика целиком на скриптах: логика, состояние, реакция на события. Возможность поменять правило - правим скрипт на месте, и изменение видно в игре;
- `Prefab`: шаблон в `JSON` (компоненты + начальные значения, **включая поля скриптов** - баланс в данных, а не в коде), спавн из скрипта одной командой. Механика обязана им пользоваться;
- Правка полей скриптового поведения в редакторе с сохранением в сцену / шаблон;
- Стабильность: сломанный скрипт не роняет движок, в логе `файл:строка`, серия `reload` подряд, длинная сессия без утечек, корректный выход;
- Допфичи: `hot reload` в `Play`, `hot reload` через `job system`, зоны `Tracy` на вызовы скриптов, `REPL`-консоль.

## Список определений

Определения терминов, которые встречаются дальше:
- **`Embedding` (встраивание)** - `Python` работает внутри нашего `exe` как библиотека (хозяин процесса - движок);
- **`Binding`** - описание того, что из `C++` видно в `Python`: `.def("destroy", ...)` -> в скрипте работает `e.destroy()`;
- **`Behaviour`** - `C++`-класс "поведение сущности". Скрипт - это `Python`-класс, унаследованный от него;
- **`Trampoline`** - класс-прослойка: `C++` вызывает виртуальный `OnUpdate`, а выполняется одноимённый `Python`-метод;
- **`GIL (Global Interpreter Lock)`** - один глобальный замок интерпретатора: `Python`-код в каждый момент выполняет только один поток;
- **`Handle`** - ссылка на объект через `id`. Если объект удалён, это видно;
- **`Hot reload`** - подхватить изменённый файл скрипта без перезапуска движка;
- **`Prefab` (шаблон)** - `JSON`-файл с описанием объекта; из скрипта создаётся одной командой `world.spawn("coin")`;
- **Поле поведения** - атрибут скриптового класса с аннотацией типа (`speed: float = 3.0`); видно и правится в редакторе;
- **`props`** - значения полей поведения, сохранённые в сцене или шаблоне (`{"speed": 5.0}`);
- **`Faulted`** - экземпляр скрипта, который упал с ошибкой и отключён; остальное продолжает работать.

## Общая идея

Делаем **`Python`, встроенный в движок (`embedding`), через `pybind11` 3.x**:
- Движок - хозяин процесса. Интерпретатор создаётся при старте и живёт всю сессию **на главном потоке**;
- Поведение сущности - `Python`-класс, унаследованный от `C++`-класса `Behaviour`. Движок вызывает его методы (`OnStart`, `OnUpdate`, `OnTrigger`, ...) через `trampoline`;
- Скрипт работает с движком через модуль `myengine`: сущности, компоненты, спавн `prefab`-ов, ввод, запросы к миру, время, лог, `HUD`;
- Скрипт никогда не получает сырой указатель на данные движка. Сущность для скрипта - это **id + проверка "жива ли"** (идея `handles` из лекции);
- **Правила** механики (условия, порядок, формулы) - в коде скриптов, **баланс** (числа, флаги, строки) - в полях поведения, значения которых лежат в шаблонах и сцене и правятся в редакторе;
- Всё, что меняет структуру мира во время кадра (удаление сущностей, события физики), идёт через **очереди** и применяется в одной известной точке кадра;
- Ошибка в скрипте ловится, пишется в лог как `файл:строка` + `traceback` и отключает только этот экземпляр скрипта. Движок продолжает работать.

**Мини-пример "двусторонности":**

```python
# assets/scripts/coin.py
import myengine as me

class Coin(me.Behaviour):
    # Поля поведения: аннотация типа = "редактируемое поле".
    # Здесь значения по умолчанию, реальные - из шаблона coin.prefab.json / инспектора
    spin_speed: float = 90.0
    score_value: int = 1

    def OnStart(self): # движок -> скрипт
        self.collected = False # обычное состояние (не поле): в инспекторе не видно

    def OnUpdate(self, dt): # движок -> скрипт, каждый кадр
        t = self.entity.transform # скрипт -> движок
        t.rotation = t.rotation + me.Vec3(0, self.spin_speed * dt, 0)

    def OnTrigger(self, other): # движок -> скрипт, по событию физики
        if other.name == "Controlled_1" and not self.collected: # правило - в коде
            self.collected = True
            me.send("GameManager", "add_score", self.score_value) # число - из данных
            self.entity.destroy() # отложенное удаление, безопасно
```

## Структура репо

```
engine/include/myengine/scripting/
    ScriptRuntime.h - интерпретатор: Initialize/Shutdown, импорт, описание полей, безопасный вызов (без pybind11 в заголовке)
    ScriptSystem.h - IUpdateSystem: экземпляры скриптов, очередь событий, отложенный destroy, hot reload
    PrefabLibrary.h - загрузка prefab-файлов, спавн, сохранение props
engine/include/myengine/ecs/components/
    ScriptComponent.h - какие скрипты висят на сущности + значения их полей (только данные)
engine/include/myengine/core/
    FileWatcher.h - опрос mtime файлов с callback (вынесен из ResourceManager)
engine/src/scripting/
    ScriptRuntime.cpp - PyConfig, sys.path, перенаправление print в Logger, загрузка модулей
    Behaviour.h - C++-класс Behaviour + trampoline (приватный заголовок, тут pybind11)
    ScriptBindings.cpp - PYBIND11_EMBEDDED_MODULE(myengine, m): всё, что видно скрипту
    ScriptSystem.cpp - жизненный цикл, события, ошибки
    ScriptHotReload.cpp - перезагрузка модулей
engine/src/scene/
    SceneSerializer.cpp - рефакторинг: SerializeEntity / DeserializeEntity + компонент Script
    PrefabLibrary.cpp
engine/src/ui/
    ScriptInspector.cpp - панель полей скрипта в инспекторе + панель Prefabs (вызывается из SceneEditor)
assets/scripts/ - *.py скрипты игры
assets/prefabs/ - *.prefab.json
assets/scenes/scripting_demo.json - демо-сцена ЛР2
external/pybind11/include/ - pybind11 3.x (header-only)
external/python/include/, external/python/libs/python314.lib - заголовки и import-библиотека Python 3.14
external/runtime/ - python314.dll, python314.zip (стандартная библиотека), нужные *.pyd
```

Пространства имён: `myengine::scripting` для подсистемы, `myengine::scene` для `prefab`.

**Правило:** публичные заголовки (`engine/include/...`) **не подключают `pybind11` и `Python.h`**. Всё питоновское живёт в `.cpp` и приватных заголовках `engine/src/scripting/` (`pImpl`). Тогда остальной движок (в том числе редактор) не зависит от `Python` и не компилируется медленнее.

## Python в сборке и на диске (дистрибуция)

**Проблема.** Если движок ищет `Python` через `PATH` или `PYTHONHOME`, то на другой машине он найдёт другую версию или не найдёт вовсе. Кроме того, и репозиторий, и установленный `Python` могут лежать в путях с кириллицей и если путь передать как `char*` в кодировке `ANSI`, он сломается.

**Решение - рантайм поставляется рядом с билдом**:
1. `Python 3.14` лежит в репозитории, как и остальные зависимости:
   - `external/python/include` и `external/python/libs/python314.lib` берём из установленного 3.14;
   - `external/runtime/python314.dll`, `python314.zip` и `*.pyd` берём из "`Windows embeddable package (64-bit)`" той же версии `3.14.x`;
   - `Python` на другом пк тогда не нужен вообще, а установленный - не мешает: системные пути и переменные окружения не читаются.
2. `CMake`: `INTERFACE`-цель `myengine_python` (include `external/python/include` и `external/pybind11/include` как `SYSTEM`, чтобы `/W4` не ругался на чужой код) + `IMPORTED`-библиотека `python314.lib`. `find_package(pybind11)` не используем: он сам ищет `Python` в системе. Для `ScriptBindings.cpp` нужен `/bigobj`.
3. `Debug`: `pybind11` при подключении `Python.h` сам снимает `_DEBUG`, поэтому линкуется обычная `python314.lib`, а `python314_d.lib` не нужна.
4. Запуск интерпретатора через `PyConfig` в изолированном режиме, все пути как `wchar_t*` (`std::filesystem::path::c_str()` на `Windows` как раз `wchar_t`):

```cpp
PyConfig config;
PyConfig_InitIsolatedConfig(&config); // не смотреть на PYTHONHOME, PATH, user site
config.write_bytecode = 0; // не мусорить __pycache__ в assets/scripts
PyConfig_SetString(&config, &config.home, exeDir.c_str());
config.module_search_paths_set = 1;
PyWideStringList_Append(&config.module_search_paths, (exeDir / L"python314.zip").c_str());
PyWideStringList_Append(&config.module_search_paths, exeDir.c_str()); // *.pyd
PyWideStringList_Append(&config.module_search_paths, scriptsDir.c_str()); // assets/scripts
py::scoped_interpreter guard{&config}; // pybind11 умеет принимать PyConfig
```

Плюс `PyPreConfig.utf8_mode = 1`, чтобы `open()` в скриптах по умолчанию читал `UTF-8`.

5. **Скрипты читаются из исходников**: `MYENGINE_SOURCE_DIR/assets/scripts`, а если папки нет - `<exe>/assets/scripts` (так же, как `ResourceManager::ResolvePath`). Иначе правки в редакторе кода не попадут в `hot reload`: движок смотрел бы на копию рядом с `exe`.
6. **`print`.** `Exe` собран как `WIN32`, консоли нет, поэтому `print` молча ничего бы не выводил. При старте `sys.stdout` и `sys.stderr` заменяются `Python`-объектом, у которого `write()` копит строку и отдаёт её в `Logger` (`Info` и `Error` соответственно).

## Время жизни state

`State` - это интерпретатор и всё, что в нём лежит. Кто что создаёт и когда умирает:

| Что | Создаётся | Умирает | Владелец |
|---|---|---|---|
| Интерпретатор (`sys.modules`, модуль `myengine`) | `Application::Initialize`, после `Logger`, до систем | `Application::Shutdown`, после `ScriptSystem::Shutdown` | `ScriptRuntime` (член `Application`) |
| Модуль скрипта (например, `coin`) | первый `import` (из `ScriptSystem` или инспектора) | подменяется при `hot reload`, живёт до выхода | `ScriptRuntime` (кэш модулей) |
| Экземпляр поведения (например, `Coin()`) | шаг 1 `ScriptSystem::Update` в `Play` | `destroy()` сущности, `Stop`, `hot reload` (пересоздание), выход | `ScriptSystem` (`py::object`) |
| Данные поведения (`props`) | сцена / `prefab` / инспектор | вместе с сущностью | `ScriptComponent` (`C++`, `JSON`) |
| Очереди событий и удаления | каждый кадр | очищаются в конце шага скриптов и при `Stop` | `ScriptSystem` |

**Единая точка загрузки и выполнения:** все `import` идут через `ScriptRuntime` (внутренний метод `GetModule(name)` в `.cpp`, наружу не торчит, потому что возвращает `py::object`), все вызовы `Python`-методов - через `ScriptSystem::SafeCall`. Ни одна другая часть движка `Python` не трогает (инспектор спрашивает описание полей у `ScriptRuntime`).

**Что не должно утекать:** после `Stop` число экземпляров = 0, после `hot reload` старые модули и экземпляры отпущены. Для проверки `ScriptSystem` отдаёт счётчики (`instances`, `modules`, `len(gc.get_objects())`) в панель `Statistics` - на длинной сессии они не растут.

## Потоки

**Все вызовы `Python` - только на главном потоке.** Интерпретатор создаётся там же и держит `GIL` всю сессию.

**Почему не в задачах `job system`.** `GIL` - один замок на весь интерпретатор, и `Python` запоминает, какой поток его держит (в `thread-local` состоянии потока). Задача `High`-пула может уснуть в `Wait` и проснуться на другом потоке:

Кроме того:
- `GIL` всё равно пропускает только один поток `Python`-кода за раз, то есть ускорения не было бы;
- `Registry` нельзя менять из нескольких потоков, а `EventBus` по правилам ЛР1 только на главном потоке.

**Где `job system` всё-таки работает** (сквозное правило, допфича `D3`): опрос `mtime` и чтение текста изменённых скриптов и шаблонов - задачей `Streaming`-пула (там нет `Python`, только файлы). Проверка (`compile`) и применение - на главном потоке.

Правила:
- В `Debug` каждая точка входа в `Python` проверяет `assert(std::this_thread::get_id() == mainThreadId)`;
- `C++`-код `ScriptSystem` может сам вызывать `jobs::Dispatch`, но внутри задач `Python` не трогаем;
- Никаких `py::object` в `static`-переменных: они разрушатся после остановки интерпретатора и уронят выход.

## Публичный API (примерный вид)

### C++: подсистема

```cpp
namespace myengine::scripting
{
    struct ScriptRuntimeDesc
    {
        std::filesystem::path exeDir; // где лежат python314.dll / .zip
        std::filesystem::path scriptsDir; // assets/scripts (исходники)
        core::Logger* logger = nullptr;
    };

    // Описание поля поведения для инспектора (из аннотаций класса)
    struct ScriptFieldInfo
    {
        enum class Type { Float, Int, Bool, String };
        std::string name;
        Type type;
        nlohmann::json defaultValue; // значение по умолчанию из кода класса
    };

    // Владеет интерпретатором. Один на приложение, живёт в Application
    class ScriptRuntime
    {
    public:
        bool Initialize(const ScriptRuntimeDesc& desc); // PyConfig, sys.path, stdout -> Logger, import myengine
        void Shutdown(); // после ScriptSystem::Shutdown: все py::object уже отпущены
        bool IsInitialized() const;

        // Поля класса module.className - для инспектора (работает и в Edit, без экземпляров)
        std::vector<ScriptFieldInfo> DescribeFields(const std::string& module, const std::string& className);
    };

    // Последние ошибки скриптов (для лога, консоли, оверлея)
    struct ScriptError
    {
        std::string file; // "coin.py"
        int line = 0; // строка, где упало (последний кадр traceback в файле скрипта)
        std::string message; // "AttributeError: ..." + полный traceback
        double time = 0.0;
    };

    class ScriptSystem final : public ecs::IUpdateSystem
    {
    public:
        ScriptSystem(ScriptRuntime& runtime, input::InputManager& input, scene::PrefabLibrary& prefabs, core::Logger& logger);
        void Update(ecs::World& world, float deltaTime) override;

        void RequestReloadAll(); // явная команда (F5 / кнопка в редакторе)
        void ResetInstances(); // по SceneLoadedEvent и при Stop: выбросить все экземпляры
        void Shutdown(); // отпустить все py::object, вызывается до ScriptRuntime::Shutdown

        const std::deque<ScriptError>& GetRecentErrors() const; // последние 32
        const std::map<std::string, std::string>& GetHudLines() const; // hud.set(...)
        nlohmann::json GetLiveFields(ecs::EntityId entity, std::size_t scriptIndex) const; // текущие значения в Play
    };
}

namespace myengine::ecs::components
{
    struct ScriptComponent : Component
    {
        struct Entry
        {
            std::string module; // "coin" -> assets/scripts/coin.py
            std::string className; // "Coin"
            nlohmann::json props; // {"spin_speed": 120} - значения полей; нет ключа = значение по умолчанию из кода
        };
        std::vector<Entry> scripts;
    };
}

namespace myengine::scene
{
    class PrefabLibrary
    {
    public:
        // Загрузить (с кэшем) и создать копию. Возвращает id корневой сущности или kInvalidEntity
        ecs::EntityId Instantiate(ecs::World& world, std::string_view prefabName, const ecs::components::Vec3* position = nullptr);

        std::vector<std::string> ListPrefabs() const; // для панели Prefabs
        nlohmann::json* GetPrefabJson(std::string_view prefabName); // для правки props в редакторе
        bool SavePrefab(std::string_view prefabName); // записать .prefab.json
    };

    // Вынесено из SceneSerializer, используется и сценой, и prefab
    nlohmann::json SerializeEntity(const ecs::World& world, ecs::EntityId entity);
    void DeserializeEntity(ecs::World& world, ecs::EntityId entity, const nlohmann::json& json);
}
```

**Где лежит `Python`-объект скрипта.** Не в `ScriptComponent`, а в `ScriptSystem`: `std::unordered_map<EntityId, std::vector<Instance>>`, где `Instance = { py::object object; Behaviour* native; State state; }`. Компонент - это данные (что сериализуется, правится в инспекторе и копируется в снимок `Play`), экземпляр - состояние времени выполнения. Так `ECS` и сериализатор вообще не знают о `Python`.

### C++ <-> Python: `Behaviour` и `trampoline`

```cpp
// engine/src/scripting/Behaviour.h
class Behaviour
{
public:
    virtual ~Behaviour() = default;
    virtual void OnStart() {}
    virtual void OnUpdate(float dt) {}
    virtual void OnCollision(EntityRef other, Vec3 point, Vec3 normal, float impulse) {}
    virtual void OnTrigger(EntityRef other) {}
    virtual void OnDestroy() {}
    virtual void OnReload() {}

    EntityRef entity; // заполняет движок до OnStart
};

class PyBehaviour : public Behaviour, public py::trampoline_self_life_support
{
public:
    using Behaviour::Behaviour;
    void OnUpdate(float dt) override { PYBIND11_OVERRIDE(void, Behaviour, OnUpdate, dt); }
    // ... так же для остальных методов
};

// ScriptBindings.cpp
py::classh<Behaviour, PyBehaviour>(m, "Behaviour")
    .def(py::init<>())
    .def_readonly("entity", &Behaviour::entity)
    .def("OnUpdate", &Behaviour::OnUpdate)
    // ...
```

Как это работает: движок вызывает `native->OnUpdate(dt)` - обычный виртуальный вызов `C++`. Если объект на самом деле `Python`-класс, `PyBehaviour::OnUpdate` через `PYBIND11_OVERRIDE` находит `Python`-метод `OnUpdate` и вызывает его. Если в `Python`-классе метода нет, вызывается пустая `C++`-версия.

`py::classh` (бывший `smart_holder`, в `pybind11` 3.x уже в основной ветке) + `trampoline_self_life_support` нужны, чтобы `Python`-часть объекта не умерла раньше `C++`-части. Мы и так держим `py::object` в `ScriptSystem`, так что это вторая линия защиты.

Правило для скриптов: **не писать свой `__init__`** (используем `OnStart`). Если всё же пишем - обязательно `super().__init__()`, иначе `pybind11` бросит `TypeError` при создании, и экземпляр будет отключён.

### Что видно скрипту (модуль `myengine`)

| Объект | Что есть |
|---|---|
| `Vec3` | `x, y, z`, `+ - * /`, `length()`, `normalized()`, `dot`, `cross`, `__repr__` |
| `Entity` | `id`, `name`, `alive`, `transform`, `rigidbody`, `collider`, `mesh`, `get_script(cls)`, `==`, `hash` |
| `Transform` | `position`, `rotation` (градусы), `scale` |
| `Rigidbody` | `velocity`, `use_gravity`, `is_kinematic`, `mass`, `add_impulse(v)` (`velocity += v / mass`), `is_grounded` (только чтение) |
| `Collider` | `is_trigger`, `radius`, `half_extents` |
| `MeshRenderer` | `mesh`, `material`, `visible` |
| `input` | `is_down(action)`, `was_pressed(action)`, `is_key_down(key)`, `was_key_pressed(key)` (`key` - `"R"`, `"Space"`, `"F1"`, ...) |
| `world`, `Entity` | `world.spawn(prefab, position=None) -> Entity`, `entity.destroy()` |
| `world` | `find(name) -> Entity / None`, `find_all(prefix) -> list`, `find_in_radius(center, radius, prefix="") -> list` (ближние первыми) |
| `time`, `log`, `hud`, `send` | `time.dt / total / frame`; `log.info / warn / error` (и `print`); `hud.set(key, text)` / `clear(key)`; `send(name, method, *args)` - вызвать метод у скриптов сущности `name` |
| `EntityDeadError` | исключение при обращении к удалённой сущности |

Вызовы "движок -> скрипт": `OnStart()`, `OnUpdate(dt)`, `OnCollision(other, point, normal, impulse)`, `OnTrigger(other)`, `OnDestroy()`, `OnReload()`.

**Владение на границе - прокси.** `entity.transform` возвращает объект, который хранит только `id`. Каждое чтение и запись делает `world.TryGet<TransformComponent>(id)`:

```python
t = e.transform
e.destroy() # (отложенно) ... кадр закончился, сущность удалена
t.position # -> EntityDeadError: entity 42 is dead (а не чтение освобождённой памяти)
```

В обратную сторону: `C++` держит `Python`-объекты только в `ScriptSystem` (`py::object`, со счётчиком ссылок) и отпускает их на `destroy`, `Stop`, `reload` и выходе. `Id` сущностей не переиспользуются, поэтому для `handle` хватает самого id (поколение не нужно).

**Ловушка с копиями.** `t.position` возвращает **копию** `Vec3`. `t.position.x += 1` изменит копию и ничего не сделает. Правильно: `t.position = t.position + me.Vec3(1, 0, 0)`. Это пишем в `docstring` свойства `position` (`help(me.Transform)` покажет).

Как добавить новый компонент в `API`: прокси-класс `XxxRef { EntityId id; }` + `py::class_<XxxRef>` с `def_property` на каждое поле + свойство `xxx` у `Entity`. Реестра компонентов в движке нет, поэтому список открытых компонентов ограничен таблицей выше.

## Поля поведения и редактор

**Что такое поле.** Атрибут класса **с аннотацией типа** - редактируемое поле поведения. Без аннотации - обычное состояние скрипта, в редакторе не видно:

```python
class Chaser(me.Behaviour):
    speed: float = 3.0 # поле: float -> DragFloat
    damage: int = 1 # поле: int -> DragInt
    aggressive: bool = True # поле: bool -> Checkbox
    target_name: str = "Controlled_1" # поле: str -> InputText
    _cooldown = 0.0 # не поле (нет аннотации)
```

Поддерживаемые типы минимума: `float`, `int`, `bool`, `str`. Список полей `ScriptRuntime::DescribeFields` достаёт через `inspect.get_annotations(cls)` (по всему `MRO`). Работает и в `Edit`, когда экземпляров нет: модуль просто импортируется.

**Откуда берётся значение поля** (по приоритету):
1. `props` в `ScriptComponent` сущности (сцена или копия из шаблона) - если ключ есть;
2. значение по умолчанию из кода класса.

Экземпляр создаётся так: `obj = cls()` -> движок со стороны `C++` записывает `native->entity` (из `Python` поле только читается) -> для каждого ключа из `props` `setattr(obj, key, value)` (с проверкой: поле объявлено и тип подходит, иначе предупреждение в лог) -> `OnStart`.

**Баланс в данных, а не в коде**: в шаблоне `coin.prefab.json` лежат `"props": {"spin_speed": 120, "score_value": 1}`. Код скрипта задаёт только значения по умолчанию на случай, если в данных ключа нет.

**Редактор**:
- **Панель Script в инспекторе** (выделенная сущность):
  - для каждой записи `ScriptComponent` - заголовок `coin.Coin` и статус (`Active` / `Faulted` / `Не загружен`);
  - в `Edit`: поля с текущими значениями (`props` или значение по умолчанию, у значений по умолчанию серая метка), виджет по типу. Правка пишется в `props`, через `RecordSceneMutation...` (работает `undo`), сохраняется вместе со сценой, применяется на следующем `Play`. Кнопка "Сбросить" удаляет ключ из `props`;
  - в `Play`: живые значения полей экземпляра (`GetLiveFields`), только чтение;
- **Панель Prefabs**: список `assets/prefabs/*.prefab.json` -> выбрали шаблон -> те же виджеты для `props` его скриптов -> "Сохранить" пишет `.prefab.json`. Применяется при **следующем спавне** (кэш шаблона сбрасывается по `FileWatcher`), в том числе прямо в `Play`.

## Жизненный цикл скрипта

**Порядок систем** в `Application::Initialize`:

```
CameraControl -> PlayerControl -> Motion -> Physics -> ScriptSystem (потом render)
```

Скрипты идут **после физики**: события текущего кадра доходят до скриптов в этом же кадре, а то, что скрипт поставил в `velocity`, физика применит на следующем кадре. Это осознанная задержка в один кадр (`one-frame-off lag`).

**`ScriptSystem::Update(world, dt)`:**

```
0. Hot reload: FileWatcher раз в 500 мс / команда F5 (в любом режиме)
   если режим не Play -> выход
1. Синхронизация: у сущностей с ScriptComponent без экземпляров создаём экземпляры (модуль -> класс -> cls() -> entity -> props) и ставим в "ждут OnStart"
2. OnStart всем ждущим
3. События физики из очереди -> OnCollision / OnTrigger (пропуская мёртвых и помеченных на удаление)
4. OnUpdate(dt) всем активным (обход копии списка id, а не живого реестра)
5. Flush: для каждой сущности из очереди удаления -> OnDestroy у её скриптов -> удалить детей рекурсивно -> world.DestroyEntity
```

`Tracy`-зоны на шаги: `Scripts::Update`, `Scripts::HotReload`, `Scripts::Start`, `Scripts::Events`, `Scripts::OnUpdate`, `Scripts::Flush`. Зоны на каждый отдельный вызов скрипта - допфича `D4`.

**Состояния экземпляра:** `PendingStart -> Active -> (Faulted) -> Destroyed`.

**Спавн** (`world.spawn`) выполняется сразу: сущность и компоненты создаются, `Entity` возвращается скрипту, можно сразу двигать. Но экземпляры скриптов новой сущности создаются на шаге 1 **следующего** кадра, тогда же их `OnStart` (как в `Unity`). Это безопасно: шаг 4 обходит копию списка, а не реестр.

**Удаление** (`entity.destroy()`) всегда отложенное до шага 5:
- `World::DestroyEntity` немедленный, и удаление посреди обхода `unordered_map` ломает итераторы;
- Повторный `destroy()` той же сущности ничего не делает;
- После `destroy()` сущность до конца кадра ещё жива, но события ей уже не доставляются.

**События физики.** `ScriptSystem` подписывается на `CollisionEvent` и `TriggerEvent`, но обработчик подписки только кладёт событие в `std::vector`. Почему не вызывать скрипт прямо из подписки:
- События публикуются **внутри шага физики**: если скрипт там удалит или создаст тело, физика доработает с испорченными данными;
- `EventBus` не реентерабелен: подписка из обработчика ломает вектор подписчиков.

Доставка: `OnCollision(other, ...)` получают скрипты **обеих** сущностей пары, `OnTrigger(other)` - тоже обеих. Если оба коллайдера триггеры, физика присылает событие дважды - дубли одной пары за кадр отбрасываем.

**`EventBus` дополняется `Unsubscribe`** (подписка возвращает токен), чтобы `ScriptSystem` отписывалась при уничтожении, а не оставляла в шине лямбду с висячим `this`.

## Play / Stop и сцена

- Скрипты (шаги 1-5) работают **только в `Play`**;
- `Stop` восстанавливает мир из снимка (`LoadWorldFromString`): все компоненты по новым адресам, сущности, заспавненные скриптами, исчезают. После загрузки мира публикуется `SceneLoadedEvent`, по нему `ScriptSystem::ResetInstances()` выбрасывает все экземпляры (без `OnDestroy`) и очищает очереди. Следующий `Play` создаст их заново с нуля - **уже из нового кода**, если файлы менялись (минимум `hot reload`);
- Демо-сцена ЛР2 - `assets/scenes/scripting_demo.json`. Выбор сцены - аргумент `--scene <path>` или `config/`. **При выходе сохраняем в ту сцену, которая загружена**.

**Выход из приложения** (`Application::Shutdown`), порядок важен:
1. `ScriptSystem::Shutdown()` - отпускает все `py::object` (скрипты при выходе не вызываются);
2. Существующая логика: восстановить снимок `Play`, сохранить сцену;
3. `ScriptRuntime::Shutdown()` - `Py_Finalize`;
4. `jobs::Shutdown()` и остальное как было.

Если поменять 1 и 3 местами, деструктор `py::object` сработает после остановки интерпретатора, и выход упадёт (`World` и её системы уничтожаются только в деструкторе `Application`).

## Hot reload

**Как замечаем изменения:**
- **Файловый вотчер.** Вспомогательные `BuildDependencies` / `HasChanged` из `ResourceManager` выносим в `core::FileWatcher`:

```cpp
class FileWatcher
{
public:
    void Watch(const std::filesystem::path& file, std::function<void(const std::filesystem::path&)> onChanged);
    void Poll(); // раз в interval проверяет exists + last_write_time
};
```

`ScriptSystem` следит за всеми `*.py` в `assets/scripts`, `PrefabLibrary` - за `*.prefab.json` (сброс кэша шаблона);
- **Явная команда:** `F5` / кнопка "`Reload scripts`" в редакторе - перезагрузить все модули (на случай, если вотчер что-то пропустил, и для демо).

**Что делаем при изменении `coin.py`**:
1. Читаем текст и делаем `compile(src, path, "exec")`. **`SyntaxError`** -> в лог `coin.py:12: SyntaxError: ...` и в список ошибок, **старый модуль продолжает работать**. Файл мог быть сохранён наполовину - через 500 мс придёт полная версия;
2. Загружаем код в **новый объект модуля** (`importlib.util.spec_from_file_location` + `exec_module`), а не `importlib.reload`. Причина: `reload` выполняет код в старом объекте модуля, и если верхний уровень упадёт посередине (`NameError`), модуль останется наполовину новым. У нас при любой ошибке старый модуль не тронут;
3. Успех -> подменяем `sys.modules["coin"]` и кэш `ScriptRuntime`, в лог `hot reload: coin.py`;
4. Модуль-помощник (`utils.py`), который импортируют другие: `from utils import f` держит старую функцию. Поэтому при изменении модуля **без** наследников `Behaviour` перезагружаем все скриптовые модули.

**`Hot reload` в `Play` - допфича `D1`**:
- **L1 - сброс:** каждый экземпляр, у которого `type(obj).__module__ == "coin"`, пересоздаётся из нового класса: `cls()` -> `entity` -> `props` -> `OnStart`. Старый объект просто отпускаем (без `OnDestroy`). `Faulted`-экземпляры при этом оживают;
- **L2 - перенос состояния:** новый объект создаётся так же, но вместо `OnStart`:
  - всё состояние из `old.__dict__` переносится (`self.collected`, `self.score`, таймеры);
  - **кроме полей, у которых в коде поменялось значение по умолчанию** (старый класс говорил `speed: float = 3.0`, новый - `5.0`) - для них берётся новое значение. То есть переносятся те поля, которые не изменились;
  - потом `OnReload()`, если он есть;
  - если класса с таким именем больше нет - предупреждение, старый экземпляр работает дальше.

Пример для демо: враги бегут к игроку, меняем `speed: float = 3.0` на `8.0` или условие `if dist < 1.0` на `if dist < 3.0`, сохраняем - враги сразу бегут быстрее / ловят издалека, счёт и таймер раунда не сбрасываются.

## Prefab

**Формат:** отдельный файл `assets/prefabs/<name>.prefab.json`, компоненты в **том же формате, что в сцене** (через `SerializeEntity` / `DeserializeEntity`). `Id` внутри - локальные, только для связей родитель-ребёнок.

```json
{
  "name": "Coin",
  "entities": [
    {
      "id": 1,
      "Tag": { "name": "Coin" },
      "Transform": { "position": [0, 0.5, 0], "rotationDeg": [0, 0, 0], "scale": [0.4, 0.4, 0.4] },
      "MeshRenderer": { "meshPath": "assets/models/sphere.obj", "materialPath": "assets/materials/gold.material.json", "visible": true },
      "Collider": { "type": "sphere", "radius": 0.5, "isTrigger": true },
      "Script": { "scripts": [ { "module": "coin", "class": "Coin", "props": { "spin_speed": 120, "score_value": 1 } } ] }
    },
    {
      "id": 2,
      "Tag": { "name": "CoinGlow" },
      "Hierarchy": { "parent": 1 },
      "Transform": { "position": [0, 0, 0], "rotationDeg": [0, 0, 0], "scale": [1.2, 1.2, 1.2] },
      "MeshRenderer": { "meshPath": "assets/models/pyramid.obj", "materialPath": "assets/materials/cool.material.json", "visible": true }
    }
  ]
}
```

Меши берём из имеющихся (`sphere`, `crate`, `pyramid`). Материал `gold.material.json` - новый, по образцу `warm.material.json` (делается в `T4`).

**`Instantiate`** (из скрипта - одна команда `me.world.spawn("coin", pos)`):
1. Загрузить файл (кэш по имени, сброс кэша при изменении файла - тот же `FileWatcher`);
2. Для каждой записи `world.CreateEntity()` -> таблица `локальный id -> новый id`;
3. `DeserializeEntity` для каждой (в том числе `Script` с `props`), затем `SetParent` по таблице (ссылки на родителя переписываются на новые `id`);
4. Корень - сущность без родителя. Если передана позиция - пишется в его `Transform.position`;
5. Вернуть `id` корня.

Скрипты экземпляра заработают со следующего кадра (см. "Жизненный цикл").

**Рефакторинг `SceneSerializer`:** тело цикла по сущностям из `SerializeWorld` / `DeserializeWorld` выносится в `SerializeEntity` / `DeserializeEntity`, добавляется ключ `"Script"`. Формат `benchmark.json` не меняется.

## Ошибки

**Где ловим.** Каждый вызов из `C++` в `Python` обёрнут в одну функцию:

```cpp
template <class F>
bool ScriptSystem::SafeCall(Instance& instance, const char* what, F&& call)
{
    try { call(); return true; }
    catch (const py::error_already_set& e) { ReportError(instance, what, e); } // Python-исключение
    catch (const std::exception& e) { ReportError(instance, what, e.what()); } // C++ из биндинга, на всякий случай
    instance.state = State::Faulted;
    return false;
}
```

**Что происходит при ошибке:**
- В лог первой строкой пишется **`файл:строка`** и тип ошибки (`coin.py:17: AttributeError in Coin.OnUpdate: 'Entity' object has no attribute 'no_such'`), дальше полный `traceback`. Файл и строку берём из последнего кадра `traceback`, который лежит в `assets/scripts`;
- Ошибка кладётся в `GetRecentErrors()` (последние 32) - для консоли / оверлея;
- **Только этот экземпляр** переходит в `Faulted`: его методы больше не вызываются. Остальные скрипты и движок работают. Без этого одна опечатка в `OnUpdate` давала бы 60 одинаковых ошибок в секунду;
- `Faulted`-экземпляр оживает после `Stop` -> `Play` (минимум) или сразу после `hot reload` своего файла (`D1`).

**`C++`-исключения из биндингов** (`Registry::Get` бросает `runtime_error`, прокси бросает `EntityDeadError`) `pybind11` сам превращает в `Python`-исключения. Скрипт может их поймать (`try/except`), а если не поймал - срабатывает та же схема.

**Ошибки в данных:** в сцене указан несуществующий модуль или класс, в `props` поле, которого нет, или значение не того типа - ошибка в лог, сущность живёт без этого скрипта / поле берёт значение по умолчанию.

**Что не защищено:** бесконечный цикл (`while True:`) в скрипте повесит главный поток. Это известное ограничение. Как его снять, описано в `D10` (лимит времени на скрипт), но в наш план реализации она не входит.

## Механика

**"Сбор монет под охраной"** - всё на `Python`, движок даёт только ввод, физику, триггеры, рендер, спавн:

| Скрипт | Где висит | Что решает |
|---|---|---|
| `game_manager.py` / `GameManager` | сущность `GameManager` в сцене | счёт, таймер раунда, условие победы и поражения, рестарт по `R`, `HUD` |
| `coin_spawner.py` / `CoinSpawner` | сущность в сцене | когда и где спавнить монеты из шаблона `coin`, сколько одновременно |
| `coin.py` / `Coin` | шаблон `coin` | вращение, подбор по `OnTrigger`, сколько очков даёт |
| `enemy_spawner.py` / `EnemySpawner` | сущность в сцене | волны врагов из шаблона `chaser`: размер волны, интервал, рост сложности |
| `chaser.py` / `Chaser` | шаблон `chaser` | идти к игроку (`find_in_radius` / `find`), при касании - штраф, возврат игрока на старт |
| `firework.py` / `Firework` | шаблон `firework` | при победе: разлёт частиц-ящиков, самоуничтожение по таймеру |

**Что мы должны уметь поменять**:
- Число: очки за монету, сколько монет для победы, скорость врагов, длительность раунда, размер волны (в инспекторе / шаблоне или в коде);
- Условие: "монета даёт очки только если игрок в прыжке", "враг ловит с 3 м, а не с 1 м", "при касании врага теряешь половину очков, а не все";
- Порядок: "сначала волна врагов, потом монеты", "каждая 5-я монета спавнит врага".

## План реализации

| # | Задача | Кто | Зависит от | Обязательна | День |
|---|---|---|---|---|---|
| **T0** | Каркас: `external/pybind11`, `external/python`, `external/runtime`, цель `myengine_python` в `CMake`. `ScriptRuntime`: `PyConfig` с `wchar_t`-путями, `sys.path`, `print` -> `Logger`, `assert` главного потока. Модуль `myengine` с `log`, `Vec3` и пустым `Entity` (`id`, `alive`). Заголовки `ScriptSystem` / `PrefabLibrary` / `FileWatcher` с заглушками, `ScriptSystem` зарегистрирована после физики. Проверить сборку `Debug` + `RelWithDebInfo` **на обеих машинах** | A | - | Да | 05.10 |
| **T1** | Сериализация: `ScriptComponent.h` (**первым коммитом**, он нужен T3), `SerializeEntity` / `DeserializeEntity`, ключ `"Script"` с `props`. Демо-сцена `scripting_demo.json`, `--scene`, сохранение при выходе в текущую сцену, `SceneLoadedEvent` | B | - | Да | 05.10 |
| **T2** | `API` "скрипт -> движок": прокси `Transform` / `Rigidbody` / `Collider` / `MeshRenderer`, `Entity` целиком, `EntityDeadError`, `world.find` / `find_all` / `find_in_radius`, `input`, `time`, `send`, `hud` | B | T0 | Да | 06.10 |
| **T3** | "Движок -> скрипт": `Behaviour` + `trampoline` (`py::classh`), создание экземпляров по `ScriptComponent` + применение `props`, `OnStart` / `OnUpdate` / `OnDestroy`, очередь удаления + `entity.destroy()` + рекурсивное удаление детей, очередь событий -> `OnCollision` / `OnTrigger`, `EventBus::Unsubscribe`, `SafeCall` + `файл:строка` + `Faulted`, `ResetInstances`, порядок `Shutdown`, счётчики `state` в Statistics | A | T0, T1 (заголовок) | Да | 06.10 |
| **T4** | `Prefab`: `PrefabLibrary` (кэш, переназначение id, иерархия, позиция, сброс кэша по вотчеру), `world.spawn`, шаблоны `coin` / `chaser` / `firework`, материал `gold` | B | T1, T2 | Да | 07.10 |
| **T5 + D3** | `Hot reload` (минимум): `FileWatcher` **сразу на `Streaming`-пуле** (опрос `mtime` и чтение текста - задачей, результат в очередь готовых; это и есть D3), `compile` -> новый модуль -> подмена на главном, перезагрузка всех при изменении помощника, команда `F5`, `DescribeFields` (аннотации) | A | T3 | Да + допфича D3 | 07.10 |
| **D4** | Зоны `Tracy` на entry points скриптов + бакет `scripts` в Statistics | B | T3 | Допфича | 07.10 |
| **D1** | `Hot reload` в `Play`: L1 сброс, L2 перенос неизменившихся полей, `OnReload`, оживление `Faulted` | A | T5 | Допфича | 08.10 (утро) |
| **T6** | Механика целиком: `game_manager.py`, `coin_spawner.py`, `coin.py`, `enemy_spawner.py`, `chaser.py`, `firework.py` | B | T2, T3, T4 | Да | 08.10 (утро) |
| **T7** | Редактор: панель Script в инспекторе - поля по `DescribeFields`, общий `DrawScriptFields(fields, props)`, виджет по типу, правка `props` с `undo`, "Сбросить", живые значения в `Play`, статус экземпляра | A | T5, T1 | Да | 08.10 (день) |
| **T8** | Редактор: панель Prefabs - список шаблонов, `DrawScriptFields` из T7 для `props`, "Сохранить" в `.prefab.json` | B | T4, T7 | Да | 08.10 (день) |
| **D6** | `REPL`-консоль + список ошибок скриптов (`ImGui`) | B | T3 | Допфича | 08.10 (день) |
| **T9** | Стабильность: прогон всех сценариев в `Debug` и `RelWithDebInfo`, фиксы | A + B | всё | Да | 08.10 (вечер) |

## Допфичи

Сквозное правило: где возможно, использовать `job system` из ЛР1.

| # | Допфича | Кто | Зависит от |
|---|---|---|---|
| **D1** | `Hot reload` в `Play`: L1 сброс, L2 перенос неизменившихся полей | A | T5 |
| **D2** | События движка в скрипте: подписка / отписка на ходу (`me.events.subscribe("Collision", fn)`), таймеры и постинг "в будущее" (`me.events.post_delayed(2.0, "wave")`), `OnTriggerExit` / `OnCollisionExit` | B | T3 |
| **D3** | `Hot reload` через `job system`: опрос `mtime` и чтение файлов задачей `Streaming`-пула, `compile` и применение на главном | A | T5 |
| **D4** | Профилирование: зоны `Tracy` на entry points | B | T3 |
| **D5** | Поддержка `IDE`: `stubs` `myengine.pyi` | B | T2 |
| **D6** | `REPL`-консоль в игре + лог ошибок | B | T3 |
| **D7** | Инспектор L1: типы и ограничения полей (`speed: float = me.field(3.0, min=0, max=20)`), слайдеры, выпадающие списки, валидация | A | T7 |
| **D8** | Инспектор L2: поля-ссылки на сущности (`door: me.Entity = None`), выбор в инспекторе, сериализация как id, удалённая цель -> `None` | B | T7 |
| **D9** | Запросы к миру: `raycast` | B | T2 |
| **D10** | `Sandbox`: лимит времени на вызов скрипта | A | T3 |
| D11 | Отладчик `debugpy` | A / B | T3 |
| D12 | Загрузка уровня в рантайме (парсинг сцены задачей `job system`) / `save game` со скриптовыми состояниями | - | - |

## Сценарии стабильности для живого демо

| # | Сценарий | Что делаем | Ожидаем |
|---|---|---|---|
| 1 | Исключение в `OnUpdate` | Вписать `self.entity.no_such()` в `coin.py` | В логе `coin.py:17: AttributeError ...`, монеты этого типа замирают (`Faulted`), игра идёт, враги ходят |
| 2 | Ошибка при создании | `__init__` без `super().__init__()`, ошибка в `OnStart` | Экземпляр `Faulted`, остальное работает |
| 3 | Сломанный скрипт + `reload` | Удалить двоеточие, сохранить | `coin.py:9: SyntaxError` в логе, **старый код продолжает работать**. Вернуть двоеточие - подхватилось |
| 4 | Ошибка на верхнем уровне модуля | `x = undefined_name` в начале модуля | Старый модуль цел, `файл:строка` в логе |
| 5 | **Серия `reload` подряд** | 10-20 сохранений подряд с разными правками (в том числе `F5` много раз), часть с ошибками | Каждый раз либо новый код, либо старый + ошибка в логе. Ни одного краша, счётчики модулей не растут |
| 6 | Переименованный класс | `class Coin` -> `class Coin2` | Предупреждение, сущности с `Coin` в данных без поведения (ошибка в логе) |
| 7 | Удаление себя | `self.entity.destroy()` в `OnTrigger` и в `OnUpdate`, дважды подряд | Удаляется один раз в конце кадра, без краша |
| 8 | Ссылка на удалённую сущность | Спавнер хранит список монет, монету собрали | `coin.alive == False`, обращение -> `EntityDeadError`, не краш |
| 9 | Массовый спавн | Клавиша: 1000 монет за кадр; спавн внутри `OnTrigger` | FPS проседает, но без краша. В `Tracy` видна стоимость |
| 10 | `Play` / `Stop` много раз | В том числе сразу после `reload` и с `Faulted`-скриптами | Сцена возвращается в исходное, после `Stop` экземпляров 0 |
| 11 | Ошибка в данных | Несуществующий модуль / класс в сцене, лишнее поле или строка вместо числа в `props` | Ошибка в логе, сущность без скрипта / значение по умолчанию |
| 12 | **Длинная сессия без утечек `state`** | 10 минут игры, `reload` раз в ~30 с, `Play` / `Stop` | Счётчики `instances` / `modules` / `gc objects` в Statistics и память в диспетчере задач не растут |
| 13 | **Корректный выход** | Закрыть окно во время спавна, после `reload`, с `Faulted`-скриптами, из `Edit` и из `Play` | Без зависания и падения, сцена сохранилась в свой файл |
| 14 | Бесконечный цикл | `while True: pass` | Движок зависает - это известное ограничение (`D10` не делаем) |

Все сценарии прогоняем в `Debug` и `RelWithDebInfo`.

## Работа с гитом

- Базовая ветка - `feature/scripting`. Каждая задача - своя ветка от неё (`scripting/t0-runtime`, `scripting/t1-serialization`, ...) и `pr` обратно;
- `external/python` и `external/runtime` коммитятся в `T0` одним коммитом (бинарники, около 10 МБ);
- После каждой задачи проверять, что сборка `RelWithDebInfo` и `Debug` без ошибок и предупреждений, и что `benchmark.json` не изменился случайно после запуска.