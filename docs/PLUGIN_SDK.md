# GRML Plugin SDK (`include/grml_api.h`, ABI v2)

Mods integrate with GRML by exporting one optional pure-C entrypoint:

```c
#include "grml_api.h"

GRML_EXPORT void GRML_CALL GrmlPluginInit(const GrmlHostApi* host)
{
    if (!host || host->struct_size < sizeof(GrmlHostApi)) return;
    /* Register AOB scans, hooks, save events, hotkeys, or Mod Settings tabs */
}
```

Mods compiled with `/MT` can continue exporting `RmlPluginQuery` / `RmlPluginLoad` or `TsmPluginInit` alongside `GrmlPluginInit` for full backwards compatibility with legacy loaders.

---

## Host Capabilities (`GrmlHostApi`)

- **`RegisterPatternScan(pattern, &out_addr)`:** Enqueues a wildcarded `.text` signature (`"48 8B 05 ?? ?? ?? ?? 84 C0"`) into GRML's single-pass startup scanner.
- **`RegisterPostScanCallback(cb, user_data)`:** Invoked after the single-pass `.text` scan completes so mods can install hooks using resolved addresses.
- **`RegisterSaveEvents(on_load, on_unload, user_data)` / `RegisterSaveWriteEvent(on_write, user_data)`:** Subscribes to GRML's centralized `CreateFileW` save tracker without installing duplicate OS file hooks.
- **`Log(level, plugin_id, fmt, ...)`:** Writes to GRML's unified `grml.log` and in-game log viewer.
- **`GetGameInfo(&info)`:** Returns `SOVIET64.exe` image base/size, PE `TimeDateStamp`, version string, SHA-256, and GRML version.
- **`GetPluginConfigPath(plugin_id, default_ini_path, out_buf, cap)` *(v2)*:** Resolves `%LOCALAPPDATA%\GRML\configs\<plugin_id>.ini` (update-safe across Workshop updates) and seeds it from `default_ini_path` if missing.
- **`RegisterService(name, version, ptr)` / `GetService(name, version)` *(v2)*:** Inter-mod service registry.
- **`ResolveNamedOffset(section_name, &out_value)` *(v2)*:** Resolves a declarative section from `grml_offsets.ini` (e.g. `"in_game_flag"`, `"evaluate_dist_task"`), returning `1` on `OK`/`CACHED` and `0` on `MISSING`. Guaranteed ready inside `GrmlPostScanFn`.
- **`hooks` (`GrmlHookApi*`) *(v2)*:** Centralized `MinHook` engine (`CreateHook`, `CreateApiHook`, `EnableHook`, `DisableHook`, `RemoveHook`) with per-plugin rollback on failure or shutdown.
- **`wrsr` (`GrmlWrsrApi*`) *(v2)*:** Shared WRSR helpers (`IsInGame`, `ResolveRipTarget`, `GetSymbol`, `GetOffsets` -> `GrmlWrsrOffsets`, `GetTextSection`) backed by `grml_offsets.ini`.

---

## UI, Windows & Hotkeys (`GrmlUiApi`)

- **`RegisterTab(plugin_id, title, render_cb, user_data)`:** Registers an SEH-sandboxed tab in the in-game Mod Settings dialog (`F9` or rebound key) using GRML's internal ImGui context (`Checkbox`, `SliderFloat`, `SliderInt`, `InputFloat`, `InputInt`, `InputText`, `Combo`, `BeginTable`/`EndTable`/`TableNextRow`/`TableNextColumn`/`TableSetupColumn`/`TableHeadersRow`, `PushItemWidth`/`PopItemWidth`, `Indent`/`Unindent`, `Button`, `Text`, `TextColored`, `Separator`, `SameLine`, `BeginDisabled`/`EndDisabled`, `SetItemTooltip`).
- **`RegisterDeclarativeIniTab(&desc)`:** Zero-ImGui-code declarative `.ini` tab bound directly to live POD pointers and a UTF-8 `.ini` file.
- **`RegisterWindow(plugin_id, window_id, title, default_vk, render_cb, user_data)` *(v2)*:** Registers an SEH-sandboxed standalone hotkeyed ImGui window (with `SetWindowVisible` / `IsWindowVisible`) inside GRML's single DX11 `Present` loop.
- **`RegisterHotkey(plugin_id, action_id, label, default_vk, on_trigger, user_data)` *(v2)*:** Centralized hotkey registration with automatic conflict detection against GRML and other mods.
- **`ReportHookTiming(plugin_id, hook_name, calls, elapsed_ms)`:** Publishes per-frame hook cost to the in-game telemetry table.

---

## Manifest Format (`<Mod>.grml.json` or `<Mod>.rml.json`)

```json
{
  "id": "author.my_mod",
  "name": "My Mod",
  "version": "1.0.0",
  "author": "Author",
  "description": "Short summary shown in the GRML Launcher.",
  "dll": "MyMod.dll",
  "previewImage": "preview.png",
  "defaultPriority": 1000,
  "supportedGameVersions": ["1.2.0.9"],
  "dependencies": [],
  "loadAfter": [],
  "loadBefore": [],
  "conflictsWith": []
}
```
