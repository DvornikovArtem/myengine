# B1: материал по именам шейдера (потребитель №2 рефлексии)

## Зачем это нужно

Рефлексия ([A1](architecture.md), раздел 3.3) даёт `ShaderLayout`: какие переменные есть в cbuffer'ах и какие текстуры объявлены в шейдере. Потребитель №1 строит по нему root signature, а потребитель №2 (эта задача) подаёт в шейдер значения: материал называет переменные и текстуры шейдера по именам, а C++ не знает ни регистров, ни смещений, ни числа root-параметров. Раньше материал умел одну текстуру и один цвет (`texture`, `tint`), и они были зашиты в `Draw()`.

Это критерий приёмки лабы (раздел 1 [architecture.md](architecture.md)): преподаватель просит добавить в шейдер константу или текстуру, правится HLSL и `.material.json`, C++ не пересобирается, результат виден в кадре. Пошаговый показ на настоящем шейдере и материале проекта: раздел «Демо для преподавателя» ниже.

Задача разбита на три части: B1.1 (`MaterialParams` и новые ключи материала, без GPU), B1.2 (путь до кадра: `DrawItem::material`, слоты в адаптере), B1.3 (правила ошибок, живой критерий в тесте, этот документ).

## Что сделано

| Что | Где | За что отвечает |
| --- | --- | --- |
| `MaterialParams`, `MaterialValue`, `MaterialValues`, `ApplyMaterialValues`, `MaterialLayoutHash` | [MaterialParams.h](../../engine/include/myengine/render/MaterialParams.h), [MaterialParams.cpp](../../engine/src/render/MaterialParams.cpp) | Теневая копия каждого cbuffer'а и таблица текстур по именам, проверка имени и типа по layout, перенос старых ключей, сообщения. Без DX-типов |
| Ключи `textures` и `params` в `.material.json`, зеркала `texturePath`/`tint` | [ResourceManager.h](../../engine/include/myengine/resource/ResourceManager.h) (`MaterialAsset`, `ToMaterialValues`), [ResourceManager.cpp](../../engine/src/resource/ResourceManager.cpp) (`LoadMaterialInternal`, `ParseMaterialValue`) | Разбор файла, значения без привязки к шейдеру |
| `SaveMaterial` | [ResourceManager.cpp](../../engine/src/resource/ResourceManager.cpp) | Запись нового и старого формата, правка `tint` при `gColor` в `params` |
| `ResourceManager::GetMaterialParams`, `MaterialRuntime`, `RebuildMaterialParams`, `RevalidateMaterials` | [ResourceManager.h](../../engine/include/myengine/resource/ResourceManager.h), [ResourceManager.cpp](../../engine/src/resource/ResourceManager.cpp) | Параметры материала под layout его шейдера, пересборка, сообщения один раз |
| `DrawItem::material` | [RenderTypes.h](../../engine/include/myengine/render/RenderTypes.h) | Ссылка на параметры материала вместо одной текстуры и одного цвета |
| `DrawSlots`, `ConstantBufferSlot`, `materialLayoutHash`, `Draw` | [Dx12RenderAdapter.h](../../engine/include/myengine/render/dx12/Dx12RenderAdapter.h), [Dx12RenderAdapter.cpp](../../engine/src/render/dx12/Dx12RenderAdapter.cpp) | Слоты находятся один раз при создании программы; `Draw` копирует теневые cbuffer'ы, пишет семантики движка, ставит все текстуры программы |
| Выдача `DrawItem::material` | [RenderSystem.cpp](../../engine/src/ecs/systems/RenderSystem.cpp), [ThumbnailService.cpp](../../engine/src/editor/ThumbnailService.cpp) | Сцена и миниатюры берут параметры через `GetMaterialParams` |
| Тест `material_params` | [MaterialParamsTests.cpp](../../tests/render/MaterialParamsTests.cpp) | Сеттеры, байты по смещениям, ошибки, старые ключи, материалы проекта, сохранение (без GPU) |
| Тест `material_render` | [MaterialRenderTests.cpp](../../tests/render/MaterialRenderTests.cpp), [material_probe.hlsl](../../tests/render/shaders/material_probe.hlsl), [material_probe.shader.json](../../tests/render/shaders/material_probe.shader.json) | Рисование в render target на настоящем устройстве, чтение пикселей, живой критерий |

## Формат `.material.json`

Новый формат и старый читаются вместе:

```json
{
  "shader": "../shaders/textured_lit.shader.json",
  "textures": { "gTexture": "../textures/debug.bmp" },
  "params": { "gColor": [1.0, 0.75, 0.12, 1.0], "gRoughness": 0.4 }
}
```

Старый материал работает без правок (тест `material_params` читает все материалы из `assets/materials`, их больше 40):

```json
{
  "shader": "../shaders/textured_lit.shader.json",
  "texture": "../textures/debug.bmp",
  "tint": [1.0, 0.75, 0.12, 1.0]
}
```

### Ключи

| Ключ | Что значит |
| --- | --- |
| `shader` | Путь к `.shader.json` относительно файла материала |
| `textures` | Объект `{имя текстуры шейдера: путь}`. Имя - объявление `Texture2D` в HLSL (`gTexture`, `gDetail`) |
| `params` | Объект `{имя переменной cbuffer'а: значение}` |
| `texture` (старый) | Текстура с наименьшим регистром `t` в шейдере (в `textured_lit` это `gTexture`, `t0`), как бы ни были расположены объявления |
| `tint` (старый) | Переменная `gColor`. Нет ключа: белый `[1, 1, 1, 1]` |

Путь текстуры не `string` или пустой: запись пропускается, в лог идёт предупреждение. `textures` и `params` не объект: тоже предупреждение, ключ пропускается. Материал грузится, если в нём есть `shader` (изменено в B3, см. [b3-material-editor.md](b3-material-editor.md)): `texture` и `textures` не обязательны, даже если в шейдере есть `Texture2D`. Незаданная текстура рисуется белой 1x1. Если адаптер не умеет сообщать layout, код без `MaterialParams` (`RenderSystem`, миниатюры) при пустом `texturePath` тоже берёт белую 1x1 (`ResourceManager::GetWhiteTextureHandle`).

### Значения `params`

| В файле | Подходит для переменной | Замечание |
| --- | --- | --- |
| Число | `float`, `int`, `uint` (какую выбирать, решает тип в layout) | `int`: целое в диапазоне `int`; `uint`: целое от 0; иначе ошибка типа |
| `true` / `false` | `bool` | В cbuffer это DWORD 0 или 1. Для остальных типов - ошибка типа |
| Массив из 2, 3 или 4 чисел | `float2`, `float3`, `float4` | Длина должна совпасть с типом |
| Массив из 16 чисел | `float4x4` | По строкам, как `XMFLOAT4X4`; в буфер пишется транспонированно (так же, как рендерер пишет `gModel`) |

Остальное не читается и пропускается с предупреждением при загрузке файла (`material <путь>: param '<имя>': ...`): строка, объект, массив не из чисел, массив длиной 1, 5-15 или больше 16. Переменные-массивы (`float gWeights[2]`) пока не поддержаны: ошибка «arrays of Float are not supported yet».

### Совместимость старых ключей

- `texture` кладётся в текстуру шейдера с наименьшим регистром `t`. Если шейдер без текстур: предупреждение «'texture' is ignored: the shader has no texture».
- `tint` кладётся в `gColor`. Если в шейдере нет `gColor` и `tint` не белый: предупреждение. Белый `tint` по умолчанию молчит.
- **Новый ключ побеждает.** Если `textures` задаёт первую текстуру шейдера, `texture` игнорируется с предупреждением. Если `params` задаёт `gColor`, `tint` игнорируется с предупреждением (когда они различаются).
- **Зеркала в `MaterialAsset`** нужны коду, который знает одну текстуру и один цвет (Material Editor, миниатюры, `RenderSystem` без параметров): если задан только `textures`, `texturePath` равен первой записи (по порядку имён); если `gColor` в `params` - массив из 4 чисел, `tint` равен ему. Совпадающее зеркало не считается конфликтом и сообщения не даёт.

### `SaveMaterial`

| Пишет | Не пишет |
| --- | --- |
| `shader` (путь относительно файла материала) | `texture`, если он только повторяет запись `textures` |
| `texture`, если `textures` нет или `texture` - отдельная текстура | `tint`, если `gColor` есть в `params` |
| `textures`, если не пуст | `textures` и `params`, если они пусты (старый материал сохраняется с теми же ключами: `shader`, `texture`, `tint`) |
| `tint`, если `gColor` нет в `params` | |
| `params`, если не пуст; целые числа остаются целыми (`"gMode": 2`) | |

Правка `tint` при `gColor` в `params` правит именно `gColor`: сохранённое значение `gColor` берётся из `tint`, остальные параметры не теряются. После записи файл перечитывается (`Reload<MaterialAsset>`).

## API `MaterialParams`

Заголовок [MaterialParams.h](../../engine/include/myengine/render/MaterialParams.h), без DX-типов, строится только на `ShaderLayout`. `MaterialParams` создаётся по layout шейдера: для каждого cbuffer'а заводится массив нулевых байт размером с буфер, для каждой `Texture2D` - слот с именем, регистром и пустой текстурой.

| Что | Назначение |
| --- | --- |
| `SetFloat`, `SetFloat2`, `SetFloat3`, `SetFloat4`, `SetMatrix` (16 `float` по строкам), `SetInt`, `SetUInt`, `SetBool`, `SetTexture` | Типизированные setter'ы. Имя и тип должны совпасть с layout точно (`SetFloat` пишет только в `Float`) |
| `SetValue(имя, MaterialValue)` | Сырое значение из файла; число становится `Float`, `Int` или `UInt` по типу переменной |
| `ConstantBufferCount()`, `ConstantBufferBytes(i)` | Теневые копии cbuffer'ов в порядке `Layout().constantBuffers` |
| `Textures()`, `FindTexture(имя)`, `FirstTexture()` | Таблица текстур; `FirstTexture` - с наименьшим регистром `t` |
| `LayoutHash()`, `MaterialLayoutHash(layout)` | Хеш cbuffer'ов (имена, размеры, переменные с типами и смещениями) и текстур (имена, регистры); рендерер использует параметры только для того layout, под который они построены |
| `IsEngineSemantic(имя)` | `gModel`, `gViewProjection`, `gLightDirection`: их пишет рендерер, материал не может |
| `DescribeNames()` | Строка вида `variables: gColor (Float4), ...; textures: gTexture` для сообщений |
| `ApplyMaterialValues(params, MaterialValues, resolver, programName)` | Кладёт в `params` всё, что сказал материал (новые ключи, затем старые по правилам совместимости), и возвращает список сообщений; то, что подходит, применяется. `resolver` превращает путь в `TextureHandle` |

Результат setter'а - `MaterialParamResult { status, reason, name, message }`.

| Поле | Значения |
| --- | --- |
| `status` | `Ok`, `Warning` (значение не используется, но вызов корректен), `Error` (имя или тип не подошли, ничего не записано) |
| `reason` | `None`, `UnknownName`, `TypeMismatch`, `EngineSemantic`, `Invalid` |
| `name`, `message` | Переменная или текстура, о которой вызов; текст (для `Ok` пуст) |

Сам setter на неизвестное имя отвечает `Error` (тест `material_params`); в `ApplyMaterialValues` такое сообщение понижается до `Warning` с текстом правила ошибок (ниже).

### Где живут параметры и когда пересобираются

`ResourceManager::GetMaterialParams(материал)` возвращает `const MaterialParams*` для `DrawItem::material`. Состояние хранится в `MaterialRuntime` на ключ материала: параметры, объект материала, хеш содержимого, хендл и версия шейдера, поколение текстур, кадр последней проверки, набор уже выданных сообщений (`reported`).

Параметры пересобираются (`RebuildMaterialParams`), если:

1. Материал - новый объект (файл перечитан, `Reload`).
2. Изменилось содержимое материала (хеш `params`, `textures`, `texturePath`, `tint`): правка в редакторе.
3. Изменился шейдер: другой хендл или другая версия (горячая перезагрузка).
4. Изменилось поколение текстур: текстура, которую ждёт материал, дозагрузилась (и заменила заглушку) или не загрузилась.

Для 1-3 заодно сбрасывается `reported`, и сообщения материала становятся новыми. После горячей перезагрузки шейдера по скану файлов `RevalidateMaterials` сверяет все кэшированные материалы с новым layout сразу, а не при первом draw. `ReloadMaterial` тоже сверяет материал с шейдером сразу, поэтому сообщения появляются при сохранении файла.

Проверка идёт раз в кадр на материал: первый draw материала в кадре считает, остальные берут готовый ответ. «Кадр» - вызов `UpdateHotReload`.

**Время жизни указателя.** Указатель действителен в кадре, в котором его выдали, и ещё некоторое время после замены: заменённые параметры хранятся в `retiredMaterialParams_` и освобождаются, когда после замены прошло 4 вызова `UpdateHotReload`. Так `DrawItem` кадра никогда не указывает на освобождённую память. Если адаптер не сообщает layout программы, `GetMaterialParams` возвращает `nullptr`, и `DrawItem` приносит текстуру и цвет сам (`DrawItem::texture`, `DrawItem::color`).

## Путь до кадра

1. **Слоты находятся один раз**, при создании программы в адаптере (по `ProgramBinding` потребителя №1): для каждого cbuffer'а `ConstantBufferSlot` (`layoutIndex` - индекс буфера в `ShaderLayout` и в `MaterialParams`, root-параметр, размер, смещения `gModel`, `gViewProjection`, `gColor`, `gLightDirection`), для каждой текстуры - слот в `DrawSlots::textures` (root-параметр и индекс в таблице текстур `MaterialParams`). Там же считается `materialLayoutHash` программы и находится первая текстура (наименьший регистр). Поиска по строке на каждый draw нет.
2. **`Draw`** берёт `DrawItem::material`. Для каждого cbuffer'а: копирует в него теневую копию материала, затем пишет поверх `gModel`, `gViewProjection` и `gLightDirection` (свет сцены, либо свет цели при рисовании в render target). `gColor` и все остальные переменные приходят из материала. Затем отправляет байты в root constants или в root CBV.
3. **Текстуры.** Для каждой текстуры программы ставится таблица дескрипторов из материала по имени. Текстура, которую материал не задал (или которой нет), - белая заглушка. Белая используется только так: для текстуры, которую материал не задавал вообще (о незагруженном файле: правила ошибок).
4. **Layout параметров не совпал с программой** (шейдер перезагружен, материал ещё не проверен): параметры не используются, cbuffer'ы нулевые, `gColor` белый (не чёрная вспышка), текстуры белые; одно предупреждение на программу: `Draw: the material parameters were built for another layout of <шейдер> (the shader changed?): zeros and white textures are used until the material is checked again`.
5. **`DrawItem` без материала** (`material == nullptr`) рисуется как раньше: `DrawItem::texture` идёт в первую текстуру шейдера, `DrawItem::color` в `gColor`.
6. **Миниатюры и превью** идут тем же путём: `ThumbnailService` получает `DrawItem::material` через `GetMaterialParams`, а превью в render target рисуются тем же `Draw` (свет берётся у цели).

## Правила ошибок

Каждое сообщение пишется в лог один раз на материал и версию (набор `MaterialRuntime::reported`, сбрасывается при смене материала, содержимого или версии шейдера): пересборка из-за дозагрузки текстуры сообщение не повторяет. Формат строки: `ResourceManager: material <путь материала>: <сообщение>`. Предупреждения идут уровнем `WARN`, ошибки - `ERROR`.

| Ситуация | Что в логе | Что в кадре |
| --- | --- | --- |
| Имени нет в layout (переменная или текстура: опечатка или имя убрали из шейдера) | Одно предупреждение: `'<имя>' is not in <шейдер>; names: variables: ...; textures: ...` | Остальное применяется, кадр рисуется |
| Не тот тип | Ошибка: `'gColor': expected Float4, got Float` | Переменная остаётся по умолчанию: нулём или значением `@default` шейдера ([B2](b2-shader-metadata.md)); у `gColor` в `textured_lit` это белый |
| Файл текстуры не найден или не загрузился | Ошибка: `the texture <путь> could not be loaded: the fallback texture (magenta and black) stands in` | В слот идёт запасная текстура `ResourceManager` (шахматка magenta и чёрная), а не белая. Белая - только для текстуры, которую материал вообще не задал |
| Семантика движка в `params` (`gModel`, `gViewProjection`, `gLightDirection`) | Предупреждение: `'gModel' is set by the renderer: the value from the material is ignored` | Значение игнорируется, пишет рендерер |
| Конфликт старого и нового ключа | Предупреждение: `'texture' is ignored: ...` или `'tint' is ignored: ...` | Побеждает новый ключ |
| Переменная-массив | Ошибка: `'<имя>': arrays of <тип> are not supported yet` | Переменная остаётся нулём |
| Запись файла не читается (не тот JSON-тип, массив длиной не 2/3/4/16, путь не строка) | Предупреждение при загрузке: `material <путь>: param '<имя>': ...` | Запись пропущена, материал грузится |

В тексте «names» перечислены переменные с типами (семантики движка помечены `set by the renderer`) и текстуры шейдера, например: `variables: gModel (Float4x4, set by the renderer), gViewProjection (Float4x4, set by the renderer), gColor (Float4), gLightDirection (Float4, set by the renderer); textures: gTexture`.

**Противоречие в документе лабы и принятое решение.** [architecture.md](architecture.md) противоречит сам себе: в 3.8 материал с несуществующим именем или не тем типом «рисуется предупреждающей заглушкой», а в сценариях 6-8 (раздел 4) значение просто становится значением по умолчанию, а остальное рисуется. Принято правило из таблицы выше; оно вынесено на ревью вместе с задачей B1: материал не заменяется заглушкой, неподходящее значение остаётся нулём, подходящие применяются.

## Демо для преподавателя (живой критерий)

Шейдер: [textured_lit.hlsl](../../assets/shaders/textured_lit.hlsl) (CRLF), материал: [gold.material.json](../../assets/materials/gold.material.json). Сцена по умолчанию (`assets/scenes/coin_guard_demo.json`) рисует gold на монетах (prefab `coin`, их спавнит `CoinSpawner` в Play); можно взять любой другой материал на `textured_lit`, например `default.material.json` (ящик игрока, виден и в Edit). Движок запущен, лог: `build/app/<Config>/logs/myengine.log`. Файлы шейдера и материалов опрашиваются раз в 500 мс, после сохранения нужно подождать около секунды.

### Шаг А: новая константа

1. В `textured_lit.hlsl` в `cbuffer DrawCB` после строки `float4 gLightDirection;` добавить:

   ```hlsl
   float gRoughness;
   ```

2. В `PSMain` после строки `float lighting = gLightDirection.w + ndotl * (1.0f - gLightDirection.w);` добавить:

   ```hlsl
   lighting *= 1.0f - gRoughness;
   ```

3. Сохранить. В логе появится `shader reloaded ... (version 2)`; картинка не меняется (не заданное материалом значение равно 0). `DrawCB` был 160 байт (40 DWORD), после добавления `float` cbuffer округляется до 176 байт (44 DWORD); это по-прежнему root constants (лимит 64 DWORD).
4. В `gold.material.json` добавить `params` рядом с `texture` и `tint`:

   ```json
   {
     "shader": "../shaders/textured_lit.shader.json",
     "texture": "../textures/debug.bmp",
     "tint": [1.0, 0.75, 0.12, 1.0],
     "params": { "gRoughness": 0.4 }
   }
   ```

5. Сохранить. Объект темнеет (свет умножается на 0,6). C++ не пересобирался.

### Шаг Б: новая текстура

1. После строки `Texture2D gTexture : register(t0);` добавить:

   ```hlsl
   Texture2D gDetail : register(t1);
   ```

2. В `PSMain` после строки `float4 texColor = gTexture.Sample(gSampler, input.UV);` добавить:

   ```hlsl
   texColor *= gDetail.Sample(gSampler, input.UV);
   ```

3. Сохранить. Root signature пересобрана рефлексией (появилась вторая таблица дескрипторов); картинка не меняется: незаданная текстура - белая заглушка.
4. В материал добавить:

   ```json
   "textures": { "gDetail": "../textures/debug.bmp" }
   ```

5. Сохранить. На объекте видна текстура-шахматка поверх (`gDetail` умножается на цвет `gTexture`; в gold `gTexture` - тоже `debug.bmp`, для более заметного эффекта можно указать `../textures/african_head_diffuse.dds`).

Старый `texture` остаётся и означает `gTexture` (наименьший регистр `t0`); `gDetail` - новый ключ `textures`, конфликта нет.

### Шаг В: сообщения об ошибках

Каждый пункт начинать от рабочего состояния шагов А-Б (или повторять после правки). Строка в логе имеет вид `[время] [WARN] ResourceManager: material <путь gold.material.json в нижнем регистре>: ...`.

1. **Переменная исчезла из шейдера.** Убрать из `textured_lit.hlsl` обе строки `gRoughness` (объявление и `lighting *= ...`), оставив `"gRoughness": 0.4` в `params`. В логе ровно одно предупреждение:

   ```text
   ResourceManager: material <путь>: 'gRoughness' is not in textured_lit.shader.json; names: variables: gModel (Float4x4, set by the renderer), gViewProjection (Float4x4, set by the renderer), gColor (Float4), gLightDirection (Float4, set by the renderer); textures: gTexture, gDetail
   ```

   (список `textures` зависит от текущего шейдера: если `gDetail` из шага Б оставлен, он в списке). Кадр рисуется, остальное применено.
2. **Несуществующее имя.** В `params` записать `{"gNoSuchName": 1}`: такое же сообщение с именем `gNoSuchName`.
3. **Не тот тип.** `"gColor": 0.5` вместо массива: в логе ошибка `'gColor': expected Float4, got Float`, значение не принято, `gColor` остаётся по умолчанию: у `textured_lit` это белый `@default 1 1 1 1`, поэтому объект выглядит как без tint. Переменная без `@default` осталась бы нулём.
4. **Нет файла текстуры.** `"textures": { "gTexture": "нет_файла.bmp" }`: ошибка `the texture <путь> could not be loaded: the fallback texture (magenta and black) stands in` и фиолетово-чёрная шахматка вместо белой.

### Шаг Г: вернуть всё

```text
git checkout assets/shaders/textured_lit.hlsl assets/materials/gold.material.json
```

(или убрать добавленные строки руками). Шейдер перезагрузится ещё раз, материал вернётся к исходному виду.

Те же правки на копии шейдера выполняет автотест `material_render` (`TestTheLiveCriterion`) и сверяет пиксели и лог, поэтому шаги проверены.

## Проверка

| Тест | Что проверяет |
| --- | --- |
| `material_params` (таймаут 120 с) | Без GPU. Layout берётся рефлексией настоящего HLSL. Setter'ы пишут байты по смещениям из layout (все типы: `float`, `float2`, `float3`, `float4`, матрица транспонируется, `int`, `uint`, `bool`); только записанные байты меняются; текстуры по именам и `FirstTexture` независимо от порядка объявлений; ошибки имени и типа с перечнем имён и ничего не пишут; семантики движка дают предупреждение; сырые значения (число в `float`/`int`/`uint`, bool, векторы, матрица, дробное в `int`, отрицательное в `uint`); старые ключи (`texture` в `t0`, `tint` в `gColor`, белый по умолчанию); «новый ключ побеждает» и отсутствие сообщений для зеркала; сообщения «грубого» материала; все материалы из `assets/materials` применяются к `textured_lit` без сообщений; сохранение старого материала с теми же ключами; сохранение и загрузка `textures`/`params`, правка `tint` при `gColor` в `params`; материал только из новых ключей и материал с нечитаемыми записями. Итог: `OK: material params, setters by the names of the layout, ...` |
| `material_render` (таймаут 180 с) | Настоящее устройство DX12 (аппаратное или WARP), скрытое окно, render target 32x32, чтение пикселя: `gColor` из `params`; вторая текстура по имени; константа из `params`; старые ключи; текстура, которую материал не задал (белая заглушка); шейдер без текстур и материал без текстур; `DrawItem` без материала; шейдер, перезагруженный с новой константой (в том числе через скан файлов); живой критерий `TestTheLiveCriterion` - копия `textured_lit.hlsl` правится так, как в демо выше: константа (`gRoughness`), текстура (`gDetail`), убранное имя (одно предупреждение), опечатка, значение не того типа, несуществующий файл текстуры (запасная текстура, одна ошибка), параметры другого layout (белое, одно предупреждение). Итоговая строка начинается с `OK: material parameters reach the frame` |

`material_render` пропускается с кодом 77 (`SKIP_RETURN_CODE 77` в [CMakeLists.txt](../../tests/CMakeLists.txt)), если адаптер DX12 не инициализировался (нет устройства; WARP тоже сгодится), нельзя создать окно (нет рабочего стола) или поверхность. В выводе тогда `SKIPPED: <причина>`.

Запуск (из корня репозитория, после сборки):

```text
ctest --test-dir build -C Debug -R "material_(params|render)" --output-on-failure
```

Если `build/` собран до этой задачи, нужно перегенерировать проекты (`cmake -S . -B build`), иначе новые `.cpp` не попадут в сборку.
