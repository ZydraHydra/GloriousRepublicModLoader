# GRML Plugin SDK (`include/grml_api.h`)

Mods integrate with GRML by exporting one optional pure-C entrypoint:

```c
#include "grml_api.h"

GRML_EXPORT void GRML_CALL GrmlPluginInit(const GrmlHostApi* host)
{
    if (!host || host->struct_size < sizeof(GrmlHostApi)) return;
    /* Register AOB scans, save events, or F9 Mod Settings tabs */
}
```

Mods compiled with `/MT` can continue exporting `RmlPluginQuery` / `RmlPluginLoad` or `TsmPluginInit` alongside `GrmlPluginInit` for full backwards compatibility with legacy loaders.

---

## Host Capabilities (`GrmlHostApi`)

- **`RegisterPatternScan(pattern, &out_addr)`:** Enqueues a wildcarded `.text` signature (`"48 8B 05 ?? ?? ?? ?? 84 C0"`) into GRML's single-pass startup scanner.
- **`RegisterPostScanCallback(cb, user_data)`:** Invoked after the single-pass `.text` scan completes so mods can install hooks using resolved addresses.
- **`RegisterSaveEvents(on_load, on_unload, user_data)` / `RegisterSaveWriteEvent(on_write, user_data)`:** Subscribes to GRML's centralized `CreateFileW` save tracker without installing duplicate OS file hooks.
- **`Log(level, plugin_id, fmt, ...)`:** Writes to GRML's unified `grml.log` and `F9` log viewer.
- **`GetGameInfo(&info)`:** Returns `SOVIET64.exe` image base/size, PE `TimeDateStamp`, version string, SHA-256, and GRML version.

---

## UI & Telemetry (`GrmlUiApi`)

- **`RegisterTab(plugin_id, title, render_cb, user_data)`:** Registers an SEH-sandboxed tab in `GRML Mod Settings [F9]` using GRML's internal ImGui context (`Checkbox`, `SliderFloat`, `SliderInt`, `Button`, `Text`, `TextColored`, `Separator`, `SameLine`, `BeginDisabled`, `EndDisabled`, `SetItemTooltip`).
- **`RegisterDeclarativeIniTab(&desc)`:** Zero-ImGui-code declarative `.ini` tab bound directly to live POD pointers and a UTF-8 `.ini` file.
- **`ReportHookTiming(plugin_id, hook_name, calls, elapsed_ms)`:** Publishes per-frame hook cost to the `F9` telemetry table.

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
  "defaultPriority": 1000,
  "dependencies": [],
  "loadAfter": [],
  "loadBefore": [],
  "conflictsWith": []
}
```
