# AI-ассистент в редакторе (обзор)

В редакторе есть панель **View → Assistant**: чат с ассистентом, который отвечает на вопросы по проекту и правит скрипты (`assets/scripts`) и prefab (`assets/prefabs`) обычными файлами; правки подхватывает hot reload ([скриптинг](../scripting/README.md)). Работа разделена на задачи AI1–AI3.

## Задачи

| Задача | Содержание | Статус | Документ |
| --- | --- | --- | --- |
| AI1 | Панель Assistant, бэкенд через Claude Code CLI (дочерний процесс, stream-json), ограничение правок папками `assets/scripts` и `assets/prefabs` | Реализована в ветке `editor/ai1-assistant`; три сценария карточки прогнаны на живом Claude Code 2.1.296 (через `myengine_assistant_live`) | [ai1-assistant-panel.md](ai1-assistant-panel.md) |
| AI2 | Инструменты движка (MCP-мост), подтверждения «Применить / Отклонить» | Не начата | — |
| AI3 | Бэкенд по API-ключу, без установленного CLI | Не начата | — |

## Быстрый старт

1. Установить Claude Code CLI ([инструкция](https://code.claude.com/docs/en/setup)) и один раз войти в него в терминале.
2. `run.bat`, пункт View → Assistant (по умолчанию открыта внизу).
3. Спросить, например: «где лежат скрипты игры и как устроен GameManager?».

Если `claude` не найден, панель покажет причину и подскажет, как задать путь (поле в панели или `MYENGINE_CLAUDE_PATH`). Подробности, схемы, ограничения безопасности и проверка — в [ai1-assistant-panel.md](ai1-assistant-panel.md).

## Тесты

```powershell
ctest --test-dir build -C Debug -R assistant --output-on-failure
```

Тест `assistant` работает на заглушке `myengine_fake_claude` и настоящий CLI не требует.

Ручная проверка на настоящем CLI (в CTest не входит, тратит деньги, нужен вход в `claude`) — цель `myengine_assistant_live`: она прогоняет одну реплику через тот же бэкенд и печатает события. Параметры и результаты прогонов — в [ai1-assistant-panel.md](ai1-assistant-panel.md).
