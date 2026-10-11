# AI-ассистент в редакторе (обзор)

В редакторе есть панель **View → Assistant**: чат с ассистентом, который отвечает на вопросы по проекту, правит скрипты (`assets/scripts`) и prefab (`assets/prefabs`) обычными файлами (правки подхватывает hot reload, [скриптинг](../scripting/README.md)) и через инструменты движка действует в открытой сцене: читает её, создаёт и удаляет объекты, меняет компоненты и `props` скриптов, запускает и останавливает Play. Любое изменение сначала показывается карточкой **Apply / Reject**; изменения сцены откатываются Ctrl+Z. Работа разделена на задачи AI1–AI3 и AS3.

## Задачи

| Задача | Содержание | Статус | Документ |
| --- | --- | --- | --- |
| AI1 | Панель Assistant, бэкенд через Claude Code CLI (дочерний процесс, stream-json), ограничение правок папками `assets/scripts` и `assets/prefabs` | Реализована в ветке `editor/ai1-assistant`; три сценария карточки прогнаны на живом Claude Code 2.1.296 (через `myengine_assistant_live`) | [ai1-assistant-panel.md](ai1-assistant-panel.md) |
| AI2 | Инструменты движка (16 штук) через MCP-мост `myengine_mcp.exe` и именованный пайп, карточки «Apply / Reject» перед изменением сцены и правкой файлов, undo сцены | Реализована в ветке `editor/ai2-assistant-tools`; сценарии прогнаны на живом Claude Code 2.1.296 через `myengine_assistant_live --engine-tools` (headless-сцена); в окне редактора мышью не прокликано | [ai2-engine-tools.md](ai2-engine-tools.md) |
| AS3 | Ассистент v2: список чатов с историей из сессий CLI, модель и effort у каждого чата, вложения (картинки, файлы), markdown в ответах | Реализована в ветке `editor/as3-assistant-v2`; прогнана в окне редактора на живом Claude Code 2.1.296 | [as3-assistant-v2.md](as3-assistant-v2.md) |
| AI3 | Бэкенд по API-ключу, без установленного CLI | Не начата | — |

## Быстрый старт

1. Установить Claude Code CLI ([инструкция](https://code.claude.com/docs/en/setup)) и один раз войти в него в терминале.
2. `run.bat`, пункт View → Assistant (по умолчанию открыта внизу).
3. Спросить, например: «где лежат скрипты игры и как устроен GameManager?» или «какие объекты в сцене?».
4. Попросить изменение: «поставь 3 монеты вокруг игрока». В панели появится карточка; **Apply** создаёт монеты (Ctrl+Z убирает), **Reject** возвращает модели отказ.

Если `claude` не найден, панель покажет причину и подскажет, как задать путь (поле в панели или `MYENGINE_CLAUDE_PATH`). Если рядом с `myengine.exe` нет `myengine_mcp.exe`, панель работает как в AI1 (только файлы) и пишет предупреждение в лог. Подробности, схемы, ограничения безопасности и проверка: панель и CLI — в [ai1-assistant-panel.md](ai1-assistant-panel.md), инструменты движка, мост и подтверждения — в [ai2-engine-tools.md](ai2-engine-tools.md).

## Тесты

```powershell
ctest --test-dir build -C Debug -R assistant --output-on-failure
```

Шаблон `assistant` запускает два теста. Тест `assistant` (AI1) работает на заглушке `myengine_fake_claude` и настоящий CLI не требует. Тест `assistant_bridge` (AI2) проверяет инструменты движка, настоящий `myengine_mcp.exe` по настоящему пайпу, подтверждения и правила разрешений, тоже без настоящего CLI.

Ручная проверка на настоящем CLI (в CTest не входит, тратит деньги, нужен вход в `claude`) — цель `myengine_assistant_live`: она прогоняет одну реплику через тот же бэкенд и печатает события; с `--engine-tools` поднимает headless-сцену и мост и отвечает на карточки (`--approve all|none`). Параметры и результаты прогонов — в [ai1-assistant-panel.md](ai1-assistant-panel.md) и [ai2-engine-tools.md](ai2-engine-tools.md).
