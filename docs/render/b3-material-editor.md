# B3: Material Editor из рефлексии шейдера (потребитель №3 рефлексии)

## Зачем это нужно

Рефлексия ([A1](architecture.md), раздел 3.3) даёт `ShaderLayout`. Потребитель №1 строит по нему root signature, потребитель №2 ([B1](b1-material-params.md)) подаёт в шейдер значения материала по именам, а метаданные из комментариев HLSL ([B2](b2-shader-metadata.md)) говорят, как называть переменную, в каких пределах её двигать и что у неё по умолчанию. Эта задача - потребитель №3: панель Material Editor строится из тех же данных.

Раньше панель правила три поля: шейдер, одну текстуру и `Tint`. Всё остальное (`params`, вторую текстуру) приходилось писать в `.material.json` руками. Теперь человек правит HLSL, сохраняет, и в панели сразу появляется нужный виджет: слайдер с границами из `@min`/`@max`, чекбокс для `bool`, плашка цвета для `@color`, строка с выбором файла для новой `Texture2D`. C++ не пересобирается.

Задача разбита на четыре части: B3.1 (модель строк без ImGui, правки данных материала), B3.2 (форма с секцией Parameters), B3.3 (секция Textures), B3.4 (рассинхрон «материал - шейдер», баннеры ошибок, фильтр, этот документ).

## Что сделано

| Что | Где | За что отвечает |
| --- | --- | --- |
| `MaterialEditorModel`, `MaterialRow`, `MaterialEntry`, `BuildMaterialEditorModel`, `MaterialEditorModelCache` | [MaterialEditorModel.h](../../engine/include/myengine/editor/MaterialEditorModel.h), [MaterialEditorModel.cpp](../../engine/src/editor/MaterialEditorModel.cpp) | Строки панели по layout шейдера и материалу: виджет, состояние, значение, границы, подсказки; записи «Not in the shader». Без ImGui и без GPU |
| Функции правок (`EditMaterialParam`, `ResetMaterialParam`, `SetMaterialTexture`, ...) | [MaterialEditorModel.h](../../engine/include/myengine/editor/MaterialEditorModel.h), [MaterialEditorModel.cpp](../../engine/src/editor/MaterialEditorModel.cpp) | Как действие в панели меняет `MaterialAsset`: ключ нужного типа, Reset удаляет ключ, текстура попадает в нужный ключ |
| `DescribeShaderBanner`, `MaterialFilterIsShown`, `MaterialRowMatchesFilter`, `MaterialEntryName/ValueText/Tooltip` | [MaterialEditorModel.cpp](../../engine/src/editor/MaterialEditorModel.cpp) | Тексты и правила, которые нужны панели помимо строк |
| `SceneEditor::DrawMaterialForm`, строки виджетов, `DrawMaterialWriteBanner` | [SceneEditorMaterialForm.cpp](../../engine/src/ui/editor/SceneEditorMaterialForm.cpp) | Форма: секции Material, Textures, Parameters, Not in the shader; виджеты, Reset, запись в историю |
| `SceneEditor::DrawMaterialCategory`, `DrawMaterialCategoryPlain`, `CommitMaterialWidgetEdit`, `PushMaterialAssetCommand`, `ApplyMaterialAsset` | [SceneEditorAssets.cpp](../../engine/src/ui/editor/SceneEditorAssets.cpp) | Диспетчер (какую форму рисовать), старый вид панели, жест правки, запись файла и история |
| `SceneEditor::FinishAbandonedMaterialGesture`, поля `materialModels_`, `materialEditPath_`, `materialEditLabel_`, `materialParamFilter_`, `materialWriteFailures_` | [SceneEditor.h](../../engine/include/myengine/ui/SceneEditor.h), [SceneEditorAssets.cpp](../../engine/src/ui/editor/SceneEditorAssets.cpp), [SceneEditor.cpp](../../engine/src/ui/editor/SceneEditor.cpp) | Кэш моделей по материалам, страховка жеста, фильтр, материалы с неудавшейся записью |
| `DragVector`, `DragVectorFieldWidth`, `PropertyLabelOptions`, `BeginCategory` с цветом иконки, `AssetPickerOptions::missing` и `emptyIsWhite` | [EditorWidgets.h](../../engine/src/ui/editor/EditorWidgets.h), [EditorWidgets.cpp](../../engine/src/ui/editor/EditorWidgets.cpp) | Общие виджеты, которые понадобились форме |
| `ShaderAsset::error`, `ResourceManager::GetShaderError`, `GetWhiteTextureHandle`, `HashMaterialAsset` | [ResourceManager.h](../../engine/include/myengine/resource/ResourceManager.h), [ResourceManager.cpp](../../engine/src/resource/ResourceManager.cpp) | Текст ошибки шейдера для баннера, белая текстура 1x1, хеш содержимого материала для кэша |
| Правки загрузчика и сохранения материала | [ResourceManager.cpp](../../engine/src/resource/ResourceManager.cpp) | Раздел «Изменения загрузчика и сохранения материала» ниже |
| Белая текстура у материала без текстуры | [RenderSystem.cpp](../../engine/src/ecs/systems/RenderSystem.cpp), [ThumbnailService.cpp](../../engine/src/editor/ThumbnailService.cpp) | Путь без `GetMaterialParams` берёт белую 1x1 при пустом `texturePath` |
| `tintSet` при правке Tint в старом виде панели и в инспекторе | [SceneEditorAssets.cpp](../../engine/src/ui/editor/SceneEditorAssets.cpp), [SceneEditorDetails.cpp](../../engine/src/ui/editor/SceneEditorDetails.cpp), [SceneEditorInternal.h](../../engine/src/ui/editor/SceneEditorInternal.h) | Правка цвета отмечает, что материал сам назвал цвет; `MaterialEquals` в истории сравнивает `tintSet` |
| Тест `material_editor_model` | [MaterialEditorModelTests.cpp](../../tests/render/MaterialEditorModelTests.cpp), [editor_model.hlsl](../../tests/render/shaders/editor_model.hlsl) | Строки, состояния, значения кадра, правки, Reset, записи «Not in the shader», текстуры, кэш, баннер, фильтр (без GPU) |
| Тесты `material_params`, `shader_hot_reload` | [MaterialParamsTests.cpp](../../tests/render/MaterialParamsTests.cpp), [ShaderHotReloadTests.cpp](../../tests/resource/ShaderHotReloadTests.cpp) | Новые проверки сохранения материала и текста ошибки шейдера |

## Как строится панель

### Модель

Модель живёт отдельно от ImGui: [MaterialEditorModel.h](../../engine/include/myengine/editor/MaterialEditorModel.h) и [MaterialEditorModel.cpp](../../engine/src/editor/MaterialEditorModel.cpp). `BuildMaterialEditorModel(layout, asset, options)` принимает `ShaderLayout` шейдера материала (вместе с метаданными из комментариев) и `MaterialAsset` и возвращает `MaterialEditorModel`:

| Поле | Что там |
| --- | --- |
| `parameters` | Строки переменных cbuffer'ов в порядке объявлений в HLSL |
| `textures` | Строки `Texture2D` в порядке регистров `t` |
| `hidden` | `@hidden` и семантики движка (`gModel`, `gViewProjection`, `gLightDirection`): панель их не показывает |
| `notInShader` | Записи материала, которых нет в шейдере (секция «Not in the shader») |
| `messages` | Сообщения `ApplyMaterialValues` (ошибки и предупреждения) |

Значения строк не считаются заново. Модель создаёт временный `MaterialParams` по тому же layout, прогоняет через него материал (`ApplyMaterialValues`, правила [B1](b1-material-params.md)) и читает значения из байтов этой копии. Поэтому панель показывает ровно то, что получит кадр: `@default` для незаданного, белый `gColor` без `@default`, число, приведённое к типу переменной. Сообщения о несовпадении типа тоже берутся из этого прогона (`MaterialParamReason::TypeMismatch`). Путь текстуры в слоте находится так же: при прогоне путь превращается во временный номер, по нему строка получает свой `texturePath`.

`MaterialRow` хранит всё нужное виджету: `name`, `label`, `tooltip` (`gRoughness (Float)`, для текстуры `gDetail (t1)`), `detail` (`Range 0 - 1, step 0.01, default 0.5`: только то, что сказал шейдер), `widget`, `state`, `hasKey` (запись есть в материале: Reset удалит ключ), `isTint` (это `gColor`), границы, шаг, число знаков, скорость drag, `defaultValue`, `value`, для текстуры - `registerIndex`, `firstTexture`, `texturePath`, `fileMissing`.

### Кэш

Строить модель каждый кадр дорого, поэтому `MaterialEditorModelCache::Get(layout, asset, options)` возвращает прежнюю модель, пока не изменилось одно из:

- хеш содержимого материала (`HashMaterialAsset`, включает `tintSet`);
- `LayoutHash` (`MaterialLayoutHash`: имена, типы, смещения, регистры);
- хеш метаданных (`@label`, границы, `@default`, `@color`, `@hidden`: `MaterialLayoutHash` их не покрывает);
- признак «файл не найден» у каждой текстуры материала (см. `MaterialEditorOptions::fileMissing`).

`MaterialEditorModelCache::Builds()` считает перестроения (тест `material_editor_model` проверяет, что без изменений модель не строится заново). Кэш лежит в `SceneEditor::materialModels_`, по одному на путь материала.

### Форма

Форма рисуется в [SceneEditorMaterialForm.cpp](../../engine/src/ui/editor/SceneEditorMaterialForm.cpp) (`SceneEditor::DrawMaterialForm`). Её вызывает диспетчер `SceneEditor::DrawMaterialCategory` в [SceneEditorAssets.cpp](../../engine/src/ui/editor/SceneEditorAssets.cpp). Одна и та же форма стоит в Material Editor и в просмотрщике материала (вкладка рядом с Viewport, [SceneEditorViewers.cpp](../../engine/src/ui/editor/SceneEditorViewers.cpp)).

Диспетчер выбирает один из трёх путей:

1. Шейдер не скомпилировался с первого раза (`ShaderAsset::fallback`): рисуется форма без модели, только баннер ошибки и секция Material.
2. Адаптер не умеет сообщать layout (`GetMaterialParams` вернул `nullptr`): старый вид панели, `DrawMaterialCategoryPlain` (Shader, Texture, Tint).
3. Иначе: модель из кэша и полная форма.

Секции формы:

| Секция | Что в ней |
| --- | --- |
| Material | Выбор шейдера (`Change Material Shader`). Новый шейдер: то, чего в нём нет, уходит в «Not in the shader», ничего не теряется |
| Textures | Строка на каждую `Texture2D` шейдера (без `@hidden`), число в правой части заголовка. Нет текстур: «The shader has no textures.» |
| Parameters | Строки переменных, фильтр (при длинном списке), кнопка «Reset all». Нет параметров: «The shader has no editable parameters.» |
| Not in the shader | Только когда есть такие записи: янтарный заголовок с предупреждающей иконкой и числом записей |

Пока идёт Play, секции отключены (`ImGui::BeginDisabled`), вверху баннер «Playing - edit the material after Stop.».

## Виджеты по типу и метаданным

Подпись строки: `@label`, а без него имя переменной. Подсказка на подписи: имя и тип (`gRoughness (Float)`), второй строкой диапазон, шаг и `default`, если шейдер их задал. Порядок строк - порядок объявлений в HLSL.

| Тип в HLSL | Виджет | Подробности |
| --- | --- | --- |
| `float` с `@min` и `@max` | Слайдер-заливка | Поле закрашено от левого края до значения, число читается по центру. `@step` даёт снап к сетке `min + k*step` |
| `float` без одной из границ | Drag с зажимом | Зажим только по заданной границе. Скорость drag равна `@step`, а без `@step` 0.01; формат по числу знаков `@step`, без него `%.3f` |
| `float2`, `float3` | `DragVector` в один ряд | Формат `%.2f` (по `@step`, если он задан). Если поле уже 56 px и `@step` нет, формат `%.1f`. Границы зажимают поля, когда заданы обе |
| `float4` без `@color` | `DragVector`: 2x2 (X Y над Z W) | Ряд из четырёх полей, где поле уже 56 px, ломается на два ряда по два |
| `float3`, `float4` с `@color` | Плашка 40x24 и поля 0..255 | Клик по плашке открывает выбор цвета. Диапазон и шаг для цвета не используются |
| `int`, `uint` | Слайдер при обеих границах, иначе drag | Значения целые; скорость drag 0.2. `uint` не ниже 0, даже если `@min` не задан |
| `bool` | Чекбокс | В материале `true` или `false` |
| `float4x4` | Серая строка «Matrix 4x4 - edit in the .material.json» | Правится в файле. Если значение в материале есть, доступен Reset |
| `int2..4`, `uint2..4`, `float3x3`, массивы | Серая строка «<тип> - not supported yet» | Так же: Reset, если значение есть |

Правки снапятся функцией `SnapMaterialValue`: к ближайшему шагу (от `@min`, а без него от нуля), внутрь `[min, max]`, до целого для `int` и `uint`, до точного `float` для `float`. В файл идёт именно это число.

`gColor` типа `float4` - особая строка Tint (`MaterialRow::isTint`): она работает с полем `MaterialAsset::tint` и признаком `tintSet`, как и раньше.

## Состояния строки и Reset

| Состояние (`MaterialRowState`) | Как выглядит | Что делает Reset |
| --- | --- | --- |
| `Set` - значение есть в материале | Обычный цвет, у строки стрелка Reset | Удаляет ключ из `params` |
| `NotSet` - значения нет | Подпись и виджет серые, в виджете значение `@default` (без `@default` 0, у `gColor` белый). Подсказка: «Default from the shader (@default). Edit to store a value in the material.» Правка создаёт ключ | Стрелки нет |
| `WrongType` - ключ есть, но тип не подходит (например, массив вместо числа) | Красная подпись; подсказка на подписи - сообщение (`'gRoughness': expected Float, got Float2`); виджет серый со значением `@default` | Убирает значение (подсказка «Remove the value of the wrong type from the material») |
| `Unsupported` - тип, который панель не умеет править | Серая строка | Убирает значение, если оно есть |
| `Hidden` - `@hidden` или семантика движка | Не показывается | - |

Reset для обычной переменной удаляет ключ из `params`. Для `gColor` ещё: `tintSet = false`, ключ `gColor` из `params` убран, `tint` возвращается к `@default` шейдера (белый, если его нет). Кнопка «Reset all» (`ResetAllMaterialParams`) убирает все заданные параметры, которые показывает панель; текстуры и записи «Not in the shader» она не трогает. Фильтр на неё не влияет: сбрасываются все параметры, а не только видимые.

### Запись и история

Каждый завершённый жест (отпускание кнопки мыши после перетаскивания, подтверждение ввода, клик) - одна запись в истории редактора. Названия записей:

| Действие | Название |
| --- | --- |
| Правка значения | `Change Material Param <подпись>` |
| Reset одной строки | `Reset Material Param <подпись>` |
| «Reset all» | `Reset Material Params` |
| Выбор шейдера | `Change Material Shader` |
| Выбор текстуры | `Change Material Texture <подпись>` |
| Reset текстуры | `Reset Material Texture <подпись>` |
| Корзина у записи «Not in the shader» | `Remove Material Entry <имя>` |
| «Remove all» | `Remove Material Entries` |

Во время перетаскивания материал в памяти меняется на каждом кадре, поэтому превью и сцена обновляются вживую. Файл `.material.json` пишется один раз, в конце жеста (`ApplyMaterialAsset`, затем `ResourceManager::SaveMaterial`). Поля вектора - отдельные жесты: правка X и правка Y дают две записи.

### Ленивое автозаполнение

В файл попадает только то, что человек тронул. Добавили в HLSL `float gRoughness` с `@default 0.5`: строка появилась в панели серой со значением 0.5, и кадр уже использует 0.5, но в `.material.json` ничего не записано. Поменяли `@default` в HLSL - все материалы, у которых своего значения нет, сразу получили новое. Ключ появится только после правки (или после Reset он исчезает).

## Текстуры

Строка на каждую `Texture2D` шейдера, кроме `@hidden`, в порядке регистров `t`. Подпись - `@label` или имя, подсказка - `gDetail (t1)` и путь. Виджет - `AssetPicker` (миниатюра, имя, строка типа), справа стрелка Reset, когда текстура задана.

| Состояние | Как выглядит |
| --- | --- |
| Задана | Миниатюра файла, имя файла |
| Не задана | Серая подпись, «Nothing assigned», белая миниатюра (`AssetPickerOptions::emptyIsWhite`); в кадре шейдер получает белую 1x1. Подсказка: «No texture: the shader gets a white 1x1.» |
| Файл не найден | Красное имя, «File not found», вместо миниатюры шахматка (как у запасной текстуры движка) со значком предупреждения (`AssetPickerOptions::missing`). Признак определяет `ResourceManager::LoadFailed(ключ)`, модель берёт его через `MaterialEditorOptions::fileMissing` |

### В какой ключ файла пишется текстура

Старый материал называет первую текстуру ключом `texture`, новый - `textures[имя]` (формат: [B1](b1-material-params.md)). Правила записи (`SetMaterialTexture`, `ResetMaterialTexture`):

- Первая текстура (с наименьшим регистром `t`) у материала, где её ещё нет в `textures`: пишется только в `texture`. Старый материал остаётся старым.
- Если у материала уже есть `textures[имя]`, правится эта запись. Старый `texture`, если он повторял её, идёт следом. В файл при этом попадает только `textures`: `SaveMaterial` не пишет `texture`, когда он повторяет запись `textures`.
- Остальные текстуры пишутся в `textures[имя]`.
- Когда выбирается вторая текстура, а первая лежала в старом `texture`, она переезжает в `textures` под именем первой текстуры шейдера. Иначе по имени было бы не отличить, какая из двух какая.
- Reset удаляет запись из `textures` (для первой - ещё и старый `texture`, если он был этой текстурой). Последнюю текстуру тоже можно сбросить: материал без текстур допустим.

## Рассинхрон материала и шейдера

### «Not in the shader»

Человек удалил переменную из HLSL, а в материале её значение осталось. Оно не пропадает и не ломает кадр: B1 игнорирует такую запись с одним предупреждением. Панель показывает такие записи в отдельной янтарной секции `Not in the shader` (поле `MaterialEditorModel::notInShader`):

| Вид записи (`MaterialEntryKind`) | Откуда |
| --- | --- |
| `Param` | Имя в `params`, которого нет среди переменных шейдера |
| `Texture` | Имя в `textures`, которого нет среди текстур шейдера |
| `EngineSemantic` | `gModel`, `gViewProjection`, `gLightDirection` в `params`: их пишет рендерер, значение игнорируется |
| `OldTint` | Старый `tint` (не белый) у шейдера без `gColor`; в списке называется `tint (old key)` |
| `OldTexture` | Старый `texture` у шейдера без текстур; `texture (old key)` |

У каждой записи предупреждение, имя, значение (у цвета ещё плашка; у текстуры «texture · путь») и корзина `Remove` (`RemoveMaterialEntry`). Когда записей больше одной, внизу кнопка «Remove all» (`RemoveAllMaterialEntries`). Подсказка на записи объясняет, что значение остаётся в файле и игнорируется, и предлагает вернуть переменную или убрать запись.

Значение не теряется: вернули переменную в HLSL - запись исчезает из секции и снова становится обычной строкой со своим значением.

### Баннеры

Логика выбора баннера шейдера - `DescribeShaderBanner(fallback, error)`.

| Ситуация | Баннер | Что в панели |
| --- | --- | --- |
| Первая компиляция шейдера не удалась, вместо него запасная программа (`ShaderAsset::fallback`) | Ошибка: `Shader error: <первая ошибка>. The fallback shader is drawn. Fix the HLSL: this panel updates after the save.` Текст - `ShaderAsset::error`, первая ошибка в виде `file(line,column): error: message` из диагностики компилятора. Если описания нет, `the shader did not compile` | Только баннер и секция Material (выбор шейдера); секции Textures, Parameters и Not in the shader скрыты, потому что layout запасной программы ничего не говорит о материале |
| Горячая перезагрузка не удалась, живёт прежняя версия | Предупреждение: `The last change of the shader did not compile, the previous version is used: <первая ошибка>`. Текст даёт `ResourceManager::GetShaderError` (последняя ошибка перезагрузки программы) | Полная форма по layout прежней версии |
| Шейдер работает | Нет | Полная форма |

Баннер уходит сам, как только шейдер исправлен и сохранён.

Третий баннер - про файл материала. Если запись `.material.json` не удалась (например, у файла атрибут «только чтение»), над секциями появляется ошибка «Could not write <файл> (is the file read-only?). The change is applied in memory only.» с кнопкой «Try again». Правка остаётся в памяти, в истории она есть, Undo работает; запись повторяется при Undo, Redo и по кнопке (`DrawMaterialWriteBanner`, `SceneEditor::materialWriteFailures_`). Баннер гаснет после первой успешной записи. Он показывается и в старом виде панели.

### Страховка жеста

Если посреди жеста (кнопка мыши зажата на виджете) панель закрыли, свернули или нажали Play, виджет уже не сообщит, что кнопку отпустили. `SceneEditor::FinishAbandonedMaterialGesture` каждый кадр проверяет это: если жест открыт, а активного виджета ImGui нет (или идёт Play), он завершается как отпущенный. Правка попадает в историю и в файл.

### Фильтр

Когда видимых параметров больше 12 (`kMaterialFilterThreshold`), над ними появляется поле «Filter parameters» (`MaterialFilterIsShown`). Оно ищет по подписи и по имени переменной без учёта регистра латиницы (`MaterialRowMatchesFilter`); пустой фильтр показывает все строки. Если ничего не подошло, «No parameter matches the filter.» Фильтр не сохраняется между запусками (`materialParamFilter_`), не меняет материал и не влияет на «Reset all».

## Совместимость

- `gColor` и старый `tint` - одна строка Tint. Значение живёт в `asset.tint`, и оно «задано», если `asset.tintSet` (ключ `tint` в файле, `gColor` в `params` или правка в панели). Если `gColor` лежит в `params`, правка Tint меняет и его.
- Формат файла не менялся. Старые материалы `{shader, texture, tint}` не мигрируют; материал, который не трогали, не меняется. Правка только Tint у такого материала не добавляет в файл `params` и `textures`.
- `texture` и `textures`: правила в разделе «Текстуры».

## Изменения загрузчика и сохранения материала

Они сделаны в B3.1, потому что без них панель портила бы файлы.

| Что | Как было / как стало |
| --- | --- |
| Ключ `tint` при сохранении | `SaveMaterial` пишет `tint` только если `tintSet`, а `gColor` в `params` нет. Раньше писал всегда: сброшенный Tint возвращался в файл белым и перекрывал `@default` шейдера |
| Материал без текстур | Загрузчик больше не требует текстуру, даже если в шейдере есть `Texture2D` (раньше: «material descriptor incomplete (the shader needs a texture)»). Незаданная текстура рисуется белой 1x1 (`ResourceManager::GetWhiteTextureHandle`). Старые потребители без `MaterialParams` (`RenderSystem`, `ThumbnailService`) при пустом `texturePath` берут эту же белую |
| Запись чисел `float` | В JSON число идёт кратчайшей записью, которая читается обратно в тот же `float` (`std::to_chars`): `0.4`, а не `0.4000000059604645`. Число, не равное точному `float`, остаётся как было (так читается `double` из файла). Касается `params`, `tint` и `gColor` |
| Хеш содержимого | `HashMaterialAsset` стал публичным (нужен кэшу панели) и учитывает `tintSet`: тот же `tint` - другой цвет, если у шейдера есть `@default` для `gColor` |
| История редактора | `MaterialEquals` ([SceneEditorInternal.h](../../engine/src/ui/editor/SceneEditorInternal.h)) сравнивает `tintSet`. Правка цвета в старом виде панели и в инспекторе рендерера теперь ставит `tintSet = true` |
| Текст ошибки шейдера | `ShaderAsset::error`: первая ошибка первой загрузки (UTF-8; компилятор пишет в ANSI-кодировке, ресурс-менеджер переводит). `GetShaderError` отдаёт её, а для работающего шейдера - ошибку последней неудачной перезагрузки |

## API модели

Всё в `myengine::editor`, заголовок [MaterialEditorModel.h](../../engine/include/myengine/editor/MaterialEditorModel.h). Каждая функция правки меняет `MaterialAsset` и возвращает, изменился ли он.

| Функция | Назначение |
| --- | --- |
| `BuildMaterialEditorModel(layout, asset, options)` | Строки панели. Шейдер должен быть настоящим, не запасной программой |
| `MaterialEditorModelCache::Get(layout, asset, options)` | Модель из кэша, пересборка при изменении материала, layout, метаданных или файлов |
| `SnapMaterialValue(value, row)` | Значение к шагу, в границы, до целого или точного `float` |
| `EditMaterialParam(row, components, asset)` | Создать или изменить ключ нужного типа. Для Tint правит `asset.tint`, `tintSet` и, если есть, `params.gColor` |
| `ResetMaterialParam(row, asset)` | Убрать ключ; для Tint ещё вернуть `tint` к `@default` |
| `ResetAllMaterialParams(model, asset)` | Сбросить все заданные параметры; текстуры и «Not in the shader» остаются |
| `SetMaterialTexture(row, key, asset)` | Текстура в нужный ключ (правила выше). Пустой ключ - то же, что Reset |
| `ResetMaterialTexture(row, asset)` | Убрать текстуру слота: шейдер получит белую 1x1 |
| `RemoveMaterialEntry(entry, asset)`, `RemoveAllMaterialEntries(model, asset)` | Убрать запись «Not in the shader» (старый `tint` становится белым, `tintSet = false`) |
| `ChangeMaterialShader(asset, key)` | Сменить шейдер, ничего не теряя |
| `DescribeShaderBanner(fallback, error)` | Тип и текст баннера шейдера |
| `MaterialFilterIsShown(model)`, `MaterialRowMatchesFilter(row, filter)` | Правила фильтра параметров |
| `MaterialEntryName(entry)`, `MaterialEntryValueText(entry)`, `MaterialEntryTooltip(entry)` | Тексты записи «Not in the shader» |

## Общие виджеты

Форме понадобились изменения в [EditorWidgets.h](../../engine/src/ui/editor/EditorWidgets.h) и [EditorWidgets.cpp](../../engine/src/ui/editor/EditorWidgets.cpp). Остальные панели выглядят как раньше.

| Что | Что изменилось |
| --- | --- |
| `DragVector(id, values, count, speed, format, afterField, minValue, maxValue, minFieldWidthForOneRow)` | Обобщение `DragVector3` на 2..4 поля (четвёртая полоска серая), с зажимом `minValue < maxValue` и переходом на два ряда по два для четырёх полей. `DragVector3` теперь обёртка над ним |
| `DragVectorFieldWidth(count, minFieldWidthForOneRow)` | Ширина одного поля при текущем курсоре; по ней форма решает, сколько знаков показывать |
| `PropertyLabel(text, PropertyLabelOptions)` | Параметры подписи: `dim`, `rowHeight`, `color` (красная подпись), `tooltip` и `tooltipDetail`, `centered`. Прежние вызовы `PropertyLabel(text, dim, rowHeight)` работают без изменений |
| `PropertyLabelOptions::centered` | Центр текста подписи ставится на центр виджета строки (поле 24 px или выбор файла). Без этого флага подпись стоит там же, где стояла |
| `BeginCategory(..., iconColor)` | Цвет иконки заголовка; 0 - прежний тусклый. Нужен янтарному заголовку «Not in the shader» |
| `AssetPickerOptions::missing`, `emptyIsWhite` | Состояния «файл не найден» и «белая миниатюра» у выбора текстуры |

## Проверка

| Тест | Что проверяет |
| --- | --- |
| `material_editor_model` (таймаут 120 с) | Без GPU. Layout берётся рефлексией [editor_model.hlsl](../../tests/render/shaders/editor_model.hlsl), где есть строка каждого вида. Строки пустого материала, их порядок и скрытые; виджеты и границы; состояния `Set`, `NotSet`, `WrongType`, `Unsupported`; значения строк равны байтам кадра; правки и снап; Reset и «Reset all»; записи «Not in the shader» всех видов; правила записи текстур (в том числе второй текстуры и Reset); кэш (когда пересобирается, когда нет); `DescribeShaderBanner`; фильтр; тексты записей. Итог: `OK: material editor model: rows, states, values of the frame, edits, resets, entries not in the shader, textures, cache` |
| `material_params` (таймаут 120 с) | Новое: `SaveMaterial` без `tint`, если он не задан; короткая запись `float` (`0.4`) и возврат тех же `float` при чтении; материал без единой текстуры загружается, его параметры строятся без сообщений, `gTexture` не задана (рисуется белой), цвет равен `@default`; материал без текстуры и материал со сброшенной текстурой сохраняются без ключа `texture` и читаются обратно |
| `shader_hot_reload` (таймаут 120 с) | Новое: `ShaderAsset::error` при первой неудачной компиляции, `GetShaderError` для упавшей перезагрузки живого шейдера, очистка после удачной перезагрузки |

Запуск (из корня репозитория, после сборки):

```text
ctest --test-dir build -C Debug -R "material_editor_model|material_params|shader_hot_reload" --output-on-failure
```

Если `build/` собран до этой задачи, нужно перегенерировать проекты (`cmake -S . -B build`), иначе новые `.cpp` не попадут в сборку.

Часть ImGui автоматически не проверяется: рисование виджетов, снап при перетаскивании, жесты и история, баннеры, фильтр. Это проверяется вручную по списку ниже.

## Ручная проверка

Запустить редактор, открыть материал двойным кликом в Content Browser (форма одна и та же в Material Editor и в просмотрщике материала). Для проверок нужен шейдер с разными типами переменных; удобно взять копию `textured_lit.hlsl` и добавить в неё переменные из списка.

1. **Серый слайдер без действий.** Добавить `// @min 0 @max 1 @default 0.5` и `float gRoughness;` (сценарий ниже). Строка серая, слайдер залит наполовину. Пока ничего не тронуто, `.material.json` не меняется и в Undo ничего нет.
2. **Перетаскивание и запись.** Потянуть слайдер: превью меняется вживую. После отпускания в файле `"gRoughness": 0.8` (или похожее число), без хвоста цифр вроде `0.800000011920929`.
3. **Ctrl+Z и Ctrl+Y.** Одно перетаскивание - одна запись в истории; Ctrl+Z возвращает значение и файл, Ctrl+Y повторяет.
4. **Reset.** У заданной строки нажать стрелку: ключ пропал из файла, строка снова серая со значением `@default`. «Reset all» убирает все заданные параметры, текстуры остаются.
5. **«Not in the shader» и возврат переменной.** Задать значение, удалить переменную из HLSL: строка пропадает из Parameters, появляется янтарная секция с этой записью. Вернуть переменную: запись уходит, строка снова обычная и со своим значением. Корзина убирает запись из файла.
6. **Вторая текстура.** Добавить `Texture2D gDetail : register(t1);` со сэмплированием: в Textures вторая строка с белой миниатюрой. Выбрать файл: в `.material.json` `textures` с `gDetail`, первая текстура не потеряна. Прописать в JSON несуществующий путь: красное имя, «File not found», шахматка.
7. **Баннер ошибки шейдера.** Сломать HLSL (опечатка в имени). Для шейдера, который ни разу не собирался: красный баннер, остался только выбор шейдера. Для уже работающего: янтарное предупреждение и прежние поля. Исправить и сохранить: баннер уходит сам.
8. **Виды виджетов.** `float4` без `@color`: 2x2 при узкой панели. `@color` на `float3`: плашка и поля 0..255. `bool`: чекбокс. `@hidden`: строки нет.
9. **Старый материал.** Материал `{shader, texture, tint}`: поменять только Tint. В файле остались `shader`, `texture`, `tint`; `params` и `textures` не появились.
10. **Play и прерванный жест.** Нажать Play: форма отключена, баннер «Playing - edit the material after Stop.». Зажать слайдер и закрыть панель (или нажать Play): правка попала в историю и в файл.
11. **Фильтр.** Шейдер с 13 и больше параметрами: появилось поле «Filter parameters»; поиск по подписи и по имени; на «Reset all» фильтр не влияет.
12. **Файл только для чтения.** Поставить атрибут «только чтение» на `.material.json` и поправить значение: баннер «Could not write ...» с «Try again». Снять атрибут, нажать кнопку: баннер ушёл, файл записан.

## Демо для преподавателя (живой критерий)

Шейдер: [textured_lit.hlsl](../../assets/shaders/textured_lit.hlsl) (CRLF), материал: [gold.material.json](../../assets/materials/gold.material.json). Редактор запущен, в Content Browser двойной клик по `gold.material.json` открывает материал с превью. Файлы шейдера и материалов опрашиваются раз в 500 мс, после сохранения HLSL нужно подождать около секунды. Весь показ занимает 1-2 минуты.

### Шаг А: новая константа

1. В `textured_lit.hlsl` в `cbuffer DrawCB` после строки `float4 gLightDirection;` добавить:

   ```hlsl
   // @label "Roughness" @min 0 @max 1 @step 0.01 @default 0.5
   float gRoughness;
   ```

2. В `PSMain` после строки `const float4 color = texColor * gColor;` заменить возврат на:

   ```hlsl
   return float4(color.rgb * lighting * (1.0f - gRoughness), color.a);
   ```

3. Сохранить. В секции Parameters появилась серая строка Roughness со слайдером на 0.5. Сфера уже потемнела вдвое: незаданное значение берётся из `@default`, а в `gold.material.json` ничего не записано.
4. Потянуть слайдер до 0.8: сфера темнеет, по отпусканию в `gold.material.json` появляется
   ```json
   "params": { "gRoughness": 0.8 }
   ```
   (в файле JSON поля идут отдельными строками). Ctrl+Z возвращает серое состояние, стрелка Reset делает то же.

### Шаг Б: новая текстура

1. После строки `Texture2D gTexture : register(t0);` добавить:

   ```hlsl
   // @label "Detail"
   Texture2D gDetail : register(t1);
   ```

2. В `PSMain` после строки `float4 texColor = gTexture.Sample(gSampler, input.UV);` (до строки с `color`) добавить:

   ```hlsl
   texColor *= gDetail.Sample(gSampler, input.UV * 4.0f);
   ```

3. Сохранить. В секции Textures вторая строка Detail с белой миниатюрой и «Nothing assigned». Картинка не изменилась: незаданная текстура - белая 1x1.
4. Выбрать файл в строке Detail (любую текстуру из списка). Выбранная текстура умножается на `gTexture` в 4 раза мельче по UV, на сфере виден её узор, а в `gold.material.json` появляется
   ```json
   "textures": { "gDetail": "../textures/<выбранный файл>", "gTexture": "../textures/debug.bmp" }
   ```
   `gTexture` переехал в `textures`, потому что без имени две текстуры не различить; старый `texture` из файла исчез.

### Шаг В (бонус): значение без переменной

1. Удалить из HLSL объявление `gRoughness` и его использование в возврате (оставить `return float4(color.rgb * lighting, color.a);`), сохранить.
2. Строка Roughness пропала из Parameters. Появилась секция Not in the shader с записью `gRoughness` и значением 0.8, а в файле значение осталось.
3. Вернуть обе правки из шага А: запись ушла из секции, Roughness снова обычная строка со значением 0.8.

### Шаг Г (бонус): ошибка в HLSL

1. В возврате написать `gRoughnes` (опечатка) и сохранить. Сфера остаётся с прежней версией шейдера, в панели появляется баннер «The last change of the shader did not compile, the previous version is used: ...» с первой ошибкой компилятора в виде `textured_lit.hlsl(строка,столбец): error: ...`, где упомянуто неизвестное имя `gRoughnes` (дословный текст задаёт компилятор).
2. Исправить опечатку и сохранить: баннер уходит сам.

### Возврат

```text
git checkout assets/shaders/textured_lit.hlsl assets/materials/gold.material.json
```

(или убрать добавленные строки руками).

## Ограничения

- Рисование ImGui автоматически не проверяется (список выше).
- Заливка слайдера считается по значению строки модели, то есть с отставанием в один кадр при перетаскивании.
- Текст ошибки шейдера идёт в виде `file(line,column): error: message` из диагностики компилятора; он не переводится и может быть длинным. Показывается первая ошибка.
- Кириллица в `@label` работает, если нужные глифы есть в атласе шрифта редактора (проверено вручную).
- Четыре поля `float4` при узкой панели идут в два ряда; `float2` и `float3` остаются в одном ряду и показывают на один знак меньше.
- Матрицы `float4x4` и типы `int2..4`, `uint2..4`, `float3x3`, а также массивы панель не правит, только показывает и позволяет убрать значение.
- Материал без единой текстуры допустим при загрузке и при записи: `SaveMaterial` не пишет ключ `texture` с пустым путём (это проверяет `material_params`).
- Фильтр параметров один на все материалы и не сохраняется между запусками.
