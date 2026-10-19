# B2: метаданные параметров шейдера (подсказки для редактора и значения по умолчанию)

## Зачем это нужно

Рефлексия ([A1](architecture.md), раздел 3.3) знает имя, тип и смещение каждой переменной cbuffer'а, но не знает, что с ней делать человеку: какое имя показать, в каких пределах двигать ползунок, что `float4 gColor` - цвет, а `gModel` показывать не надо. Редактор материалов (следующая задача лабы, B3) строит виджеты по типу переменной и по этим подсказкам. Шейдер остаётся единственной точкой правды: подсказки пишутся комментариями рядом с объявлением в HLSL, а не в отдельном файле и не в C++.

Вторая польза: параметр, который материал не задал, получает значение из шейдера (`@default`), а не ноль (раздел 3.5 [architecture.md](architecture.md): «ноль, либо значение из метаданных шейдера»).

Комментарии не меняют код шейдера, ошибка в комментарии - только предупреждение: шейдер компилируется, программа создаётся, hot reload не ломается.

## Что сделано

| Что | Где | За что отвечает |
| --- | --- | --- |
| `ApplyShaderMetadata` | [ShaderMetadata.h](../../engine/include/myengine/render/ShaderMetadata.h), [ShaderMetadata.cpp](../../engine/src/render/ShaderMetadata.cpp) | Разбор комментариев `// @...` в исходнике HLSL (и в его `#include`), перенос тегов в `ShaderLayout` по имени, предупреждения с файлом и строкой. Без DX-типов |
| `ShaderVariableMetadata`, `ShaderVariable::metadata`, `ShaderResource::label`, `ShaderResource::hidden`, `ValueTypeComponents`, подсказки в `FormatShaderLayout` | [ShaderLayout.h](../../engine/include/myengine/render/ShaderLayout.h), [ShaderLayout.cpp](../../engine/src/render/ShaderLayout.cpp) | Где лежат метаданные; число компонент типа; печать метаданных в таблице layout'а |
| Вызов `ApplyShaderMetadata` в `ShaderReflector::Build` | [ShaderReflector.cpp](../../engine/src/render/dx12/ShaderReflector.cpp) | Метаданные накладываются на объединённый layout вершинного и пиксельного шейдеров, результат попадает в кэш |
| `@default` вместо нуля, правило для `gColor`, `MaterialValues::hasTint` | [MaterialParams.h](../../engine/include/myengine/render/MaterialParams.h), [MaterialParams.cpp](../../engine/src/render/MaterialParams.cpp) | Конструктор `MaterialParams` кладёт `@default`; `ApplyMaterialValues` решает, какой будет `gColor` |
| `MaterialAsset::tintSet`, `ToMaterialValues` | [ResourceManager.h](../../engine/include/myengine/resource/ResourceManager.h), [ResourceManager.cpp](../../engine/src/resource/ResourceManager.cpp) | Запоминают, что файл материала сам назвал цвет (ключ `tint` или `gColor` в `params`) |
| Комментарии в `textured_lit.hlsl` | [textured_lit.hlsl](../../assets/shaders/textured_lit.hlsl) | Метаданные настоящего шейдера проекта (код шейдера не менялся) |
| Тест `shader_metadata` | [ShaderMetadataTests.cpp](../../tests/render/ShaderMetadataTests.cpp), [metadata_main.hlsl](../../tests/render/shaders/metadata_main.hlsl), [metadata_ok.hlsli](../../tests/render/shaders/metadata_ok.hlsli), [metadata_errors.hlsl](../../tests/render/shaders/metadata_errors.hlsl), [metadata_bad.hlsli](../../tests/render/shaders/metadata_bad.hlsli), [metadata_color.hlsl](../../tests/render/shaders/metadata_color.hlsl) | Все теги, все ошибки, include, `@default` в параметрах материала, `gColor`, пересборка (без GPU) |

## Синтаксис

Метаданные - комментарий `//`, который начинается с `@`. Он стоит в строке объявления или на строках прямо над ней:

```hlsl
cbuffer DrawCB : register(b0)
{
    // @label "Roughness" @min 0 @max 1 @step 0.01 @default 0.5
    float gRoughness;

    float4 gColor;   // @color @label "Tint" @default 1 1 1 1
};

// @label "Texture"
Texture2D gTexture : register(t0);
```

- Несколько строк подряд над объявлением складываются, строка в строке объявления добавляется к ним.
- Обычный комментарий без `@` между такими строками цепочку не рвёт. Пустая строка рвёт: комментарии над ней к объявлению под ней не относятся.
- Теги идут через пробел. Значения тега тянутся до следующего `@` и разделяются пробелами или запятыми.
- Блочные комментарии `/* ... */` с метаданными игнорируются.

### Теги

| Тег | Значение | Для чего |
| --- | --- | --- |
| `@label "текст"` | Текст в кавычках. UTF-8, пробелы и запятые внутри допустимы, `\"` - кавычка, `\\` - обратная косая черта | Имя в интерфейсе (переменная и текстура) |
| `@min N` | Одно число | Нижняя граница (для каждой компоненты) |
| `@max N` | Одно число | Верхняя граница |
| `@step N` | Одно число больше нуля | Шаг ползунка |
| `@default ...` | Числа через пробел или запятую, ровно по числу компонент типа | Значение шейдера для незаданного параметра |
| `@color` | Без значения; только `Float3` и `Float4` | Переменная - цвет |
| `@hidden` | Без значения | Редактор не показывает |

Число компонент для `@default` (`ValueTypeComponents`):

| Тип | Чисел |
| --- | --- |
| `Float`, `Int`, `UInt`, `Bool` | 1 |
| `Float2` (и `Int2`, `UInt2`) | 2 |
| `Float3` (и `Int3`, `UInt3`) | 3 |
| `Float4` (и `Int4`, `UInt4`) | 4 |
| `Float3x3` | 9 |
| `Float4x4` | 16 |

Для `Bool` вместо чисел можно написать `true` или `false`: `bool gFlag; // @default true`. Метка с кириллицей работает: `@label "Шероховатость, а не Roughness"`.

Для текстур (`Texture2D ... : register(tN);`) действуют только `@label` и `@hidden`.

### Какие объявления читаются

| Читается | Не поддержано |
| --- | --- |
| Переменные внутри `cbuffer`, в том числе пришедшие из `#include` внутри `cbuffer`. Допускаются модификаторы (`row_major`, `static`, `const`...), типы с шаблоном (`vector<float, 4>`), массивы, `: packoffset(...)` | Несколько переменных в одном объявлении через запятую (`float a, b;`): такое объявление не распознаётся |
| `Texture1D`, `Texture2D`, `Texture3D`, `TextureCube` (и `Array`, `MS`) с `register(...)` на верхнем уровне | Условная компиляция не вычисляется: `#if` и `#ifdef` читаются как обычный текст, объявление в отключённой ветке читается как любое другое |
| `#include "файл"`: файл читается относительно папки включающего файла, как у компилятора; каждый файл один раз, глубина до 16 | Блочные комментарии с метаданными |

Файл, которого нет, разбор пропускает молча: об этом говорит компилятор. Если исходник не лежит в файле (`CompileProgramFromSource`), `#include` не читаются, а сам текст разбирается.

## Правила ошибок

Каждая ошибка - предупреждение вида `файл(строка): warning: shader metadata: ...` (`ShaderDiagnostic` с `warning = true`). Компиляцию, создание программы и hot reload оно не ломает: применяется всё остальное, неверный тег просто отбрасывается. Строка и файл - те, где стоит комментарий; для ошибки в `#include` это файл include и строка в нём. Для ошибки в теге сообщение начинается с `'имя': `.

| Ситуация | Что происходит | Сообщение (после `shader metadata: `) |
| --- | --- | --- |
| Неизвестный тег (`@frobnicate`) | Тег отброшен | `'gX': unknown tag @frobnicate (the tags are @label, @min, @max, @step, @default, @color, @hidden)`. У текстуры: `(a texture takes @label and @hidden)` |
| `@label` без кавычек | Метка не задана | `'gX': @label needs a text in quotes, like @label "Roughness"` |
| `@label` с незакрытой кавычкой | Метка не задана | `'gX': @label: the quote is not closed` |
| `@min`, `@max`, `@step` не с одним числом | Тег отброшен | `'gX': @min needs one number, got abc`; если значений несколько, после первого стоит `...` |
| `@step` меньше или равен нулю | Шаг не задан | `'gX': @step must be more than zero, got 0` |
| `@default` не из чисел | Значение не задано | `'gX': @default: abc is not a number` |
| `@default` не того количества чисел | Значение не задано | `'gX': @default needs 3 numbers for a Float3, got 2` |
| `@default` у типа без поддержки (тип неизвестен) | Значение не задано | `'gX': @default is not supported for a variable of this type` |
| `@min` больше `@max` | Оба игнорируются; предупреждение на строке более позднего из двух | `'gX': @min 5 is more than @max 1: both are ignored` |
| `@default` вне `[min, max]` | Предупреждение, `@default` остаётся, диапазон остаётся (одно предупреждение, даже если вышла не одна компонента) | `'gX': @default 2 is outside of [0, 1]; it stays` (отсутствующая граница пишется `-inf` или `inf`) |
| `@color` не на `Float3` и не на `Float4` | Флаг не ставится | `'gX': @color works for a Float3 or a Float4, this is a Float` |
| `@color` со значением | Флаг не ставится | `'gX': @color takes no value, got 1` |
| `@hidden` со значением | Флаг не ставится | `'gX': @hidden takes no value, got 1` |
| `@min`, `@max`, `@step`, `@default`, `@color` у текстуры | Тег отброшен | `'gTexture': @min does not work for a texture (only @label and @hidden do)` |
| Имени нет в layout (переменная) | Метаданные этого объявления не применяются | `'gX' is not a variable of a constant buffer in the layout of the shader` |
| Имени нет в layout (текстура): неиспользуемую текстуру компилятор не привязывает | Метаданные этого объявления не применяются | `'gUnused' is not a texture in the layout of the shader (a texture that the shader does not use is not bound)` |
| Комментарий с `@`, за которым нет объявления (пустая строка, директива `#`, код, который не объявление переменной cbuffer или текстуры, конец файла) | Комментарий не применяется | `this comment is not followed by a declaration of a variable of a constant buffer or of a texture` |

Пример строки в логе: `...\metadata_errors.hlsl(5): warning: shader metadata: 'gBadTag': unknown tag @frobnicate (the tags are @label, @min, @max, @step, @default, @color, @hidden)`.

## Где хранится

| Что | Где |
| --- | --- |
| Метаданные переменной | `ShaderVariable::metadata` (`ShaderVariableMetadata`): `label`, `hasMin`/`min`, `hasMax`/`max`, `hasStep`/`step`, `defaultValue` (пусто: нет; иначе по числу на компоненту), `color`, `hidden`. `Empty()` - ничего не задано. Это не часть раскладки байтов |
| Метаданные текстуры | `ShaderResource::label`, `ShaderResource::hidden` |
| Разбор | `ApplyShaderMetadata(file, source, sourceName, layout, diagnostics)` в [ShaderMetadata.cpp](../../engine/src/render/ShaderMetadata.cpp): читает исходник построчно, собирает объявления с тегами, затем применяет их к layout по имени |

Разбор вызывается в `ShaderReflector::Build` после объединения layout'ов вершинного и пиксельного шейдеров (`MergeShaderLayouts`) и только если сборка прошла (нет ошибок компиляции, рефлексии и слияния). Результат лежит в кэше вместе с layout: ключ кэша - текст исходника и всех `#include`, поэтому:

- тот же текст - программа из кэша вместе с метаданными;
- при hot reload шейдера метаданные пересобираются; правка одного комментария (в шейдере или в include) тоже даёт новую программу;
- ошибка в комментарии не мешает сборке: программа создаётся, предупреждение идёт в лог вместе с остальными диагностиками компиляции.

Предупреждения видны и в `--dump-shader-layouts`, и в окне Shader Layouts. Там же `FormatShaderLayout` печатает метаданные после `|` в порядке `label`, `color`, `hidden`, `min`, `max`, `step`, `default`:

```text
| label "Tint", color, default 1 0.75 0.12 1
| label "Roughness", min 0, max 1, step 0.01, default 0.5
| label "Albedo map"
```

Это концы строк таблицы (переменная `gTint`, переменная `gRoughness`, текстура); начало строки - имя, тип и смещение, как раньше.

## Связь с `MaterialParams`

Конструктор `MaterialParams` кладёт `@default` в теневую копию cbuffer'а вместо нуля. Значение из материала (`params`, `tint`, setter) перекрывает его.

| Тип переменной | `@default` |
| --- | --- |
| `Float`, `Int`, `UInt` | Кладётся |
| `Bool` | Кладётся (`0` - false, иначе true) |
| `Float2`, `Float3`, `Float4` | Кладётся |
| Остальные типы (`Int2..4`, `UInt2..4`, `Float3x3`, `Float4x4`), массивы | Остаются нулём (setter'а для них нет) |
| Семантики движка (`gModel`, `gViewProjection`, `gLightDirection`) | `@default` не затрагивает: эти значения пишет рендерер |

Значение материала неверного типа отклоняется с ошибкой ([правила B1](b1-material-params.md)), и переменная остаётся тем, чем была до него: `@default` шейдера или нулём, если `@default` нет. У `gColor` в `textured_lit` это белый.

### Правило для `gColor`

Раньше материал без `tint` получал белый цвет. Теперь:

| Материал | `gColor` |
| --- | --- |
| Нет `tint` и нет `params.gColor` | `@default` шейдера; если его нет - белый `[1, 1, 1, 1]` (как у старых материалов) |
| Есть `tint` (в том числе белый: ключ есть в файле) | Этот `tint`: он перекрывает `@default` |
| Есть `params.gColor` | Он выигрывает у всех |

Различать «`tint` не указан» и «`tint` указан белым» помогает `MaterialAsset::tintSet` (ключ `tint` в файле или `gColor` в `params`), он же приходит в `MaterialValues::hasTint`. `tint`, отличный от белого, считается заданным и без флага.

## `textured_lit.hlsl`

В [textured_lit.hlsl](../../assets/shaders/textured_lit.hlsl) добавлены только комментарии:

| Объявление | Метаданные |
| --- | --- |
| `gModel`, `gViewProjection`, `gLightDirection` | `@hidden` (их пишет рендерер) |
| `gColor` | `@color @label "Tint" @default 1 1 1 1` |
| `gTexture` | `@label "Texture"` |

Код шейдера не менялся. DXBC вершинного и пиксельного шейдера (`fxc` с `/Qstrip_debug` и без) до и после совпадает побайтно (проверено компиляцией `fxc`, хеши MD5 одинаковы). Шаги демо из [b1-material-params.md](b1-material-params.md) остаются верными: они вставляют строки после `float4 gLightDirection;` и `Texture2D gTexture : register(t0);`, а комментарии стоят над объявлениями.

## Что редактор берёт из метаданных (для B3)

Это намерение следующей задачи, не готовая функция: имя в интерфейсе - `label`, а если он пуст - имя переменной; `min`/`max`/`step` задают ползунок; `color` включает выбор цвета; `hidden` убирает параметр из окна; `default` - значение для кнопки «сбросить».

## Проверка

| Тест | Что проверяет |
| --- | --- |
| `shader_metadata` (таймаут 120 с) | Без GPU. Layout берётся рефлексией настоящих HLSL из [tests/render/shaders](../../tests/render/shaders). Все теги и их сочетания; метка с кириллицей, пробелами и запятой; комментарий над объявлением и в строке объявления; несколько строк над объявлением с обычным комментарием между ними; каждая ошибка даёт ровно одно предупреждение с правильными файлом и строкой, и других предупреждений нет; верное рядом с ошибочным применяется; метаданные переменной из `#include` внутри cbuffer и ошибка в include с файлом include; `@default` в `MaterialParams` (`Float`, `Float2`, `Float3`, `Float4`, `Int`, `Bool`, из include, нет `@default`: ноль, семантика движка не затрагивается) и перекрытие значением материала или setter'ом; правило `gColor` (нет `tint`: `@default`, нет `@default`: белый, явный белый `tint`, `tint`, `params.gColor`); настоящий `textured_lit.hlsl` без предупреждений; метаданные в таблице `FormatShaderLayout`; пересборка после правки комментария в шейдере и в include (не из кэша), а тот же текст - из кэша с теми же метаданными; новая ошибка после правки - одно предупреждение, программа создаётся. Итог: `OK: shader metadata: every tag, every mistake with its file and line, ...` |

Запуск (из корня репозитория, после сборки):

```text
ctest --test-dir build -C Debug -R "shader_metadata|material_params" --output-on-failure
```

`material_params` в команде - чтобы убедиться, что старые материалы по-прежнему применяются без сообщений. Если `build/` собран до этой задачи, нужно перегенерировать проекты (`cmake -S . -B build`), иначе новые `.cpp` не попадут в сборку.
