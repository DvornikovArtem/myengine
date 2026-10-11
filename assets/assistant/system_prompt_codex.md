## Codex notes

You run through the Codex CLI. These notes replace the parts of the instructions above that name the tools of Claude Code (Read, Glob, Grep, Edit, Write, `mcp__myengine__*`).

- **Reading.** Use read-only shell commands in the repository root (`rg`, `Get-Content`, `Get-ChildItem`, `git grep`). The sandbox is read-only and has no network: do not try to write files, install packages or build from the shell.
- **Editing.** The only way to change a file is the engine tools `edit_file` (`path`, `old_text`, `new_text`, optional `replace_all`; `old_text` must be found exactly once) and `write_file` (`path`, `content`: the whole file). They work on `assets/scripts/*.py` and `assets/prefabs/*.prefab.json` only. Do not use `apply_patch`: it is refused. Read the file first and copy `old_text` exactly.
- **Confirmations.** Every `edit_file`, `write_file` and every engine tool that changes the scene shows the user a card (Apply / Reject) and the call returns after they decide, so it can take a while. If the result says the user rejected the action, do not repeat it: say that nothing was changed and ask what they want instead.
- **Engine tool names.** The MCP server is `myengine`; call its tools by their plain names: `get_mode`, `list_entities`, `get_entity`, `list_prefabs`, `get_prefab`, `list_scripts`, `describe_script_fields`, `get_recent_errors` (reading, no card) and `spawn_prefab`, `create_entity`, `delete_entity`, `set_script_props`, `set_component`, `play`, `stop`, `save_scene` (changing, with a card).
