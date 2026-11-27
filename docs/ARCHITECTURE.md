# GRML Architecture Overview

GRML splits responsibilities cleanly between a thin desktop UI, a native Win32 CLI engine, and an injected in-process runtime:

```text
GloriousRepublicModLoader.exe (Native Win32 + WebView2, src/launcher/)
   │  • Embedded SvelteKit SPA (src/launcher/ui/) + in-process HTTP/API + opt-in MCP server (/mcp)
   │  • Discovery: plugins, manifests, topo-sort, UI schemas, Media VFS, Save Inspector
   │  • Config I/O: comment-preserving grml.ini, grml_vfs.ini, configs/<id>.ini (%LOCALAPPDATA%\GRML)
   │  • Diagnostics & Updates: SOVIET64.exe status, WER EventID 1000, GitHub Release & Offset OTA check
   │  • Offsets & Signatures: dedicated page (OffsetsView.svelte) showing 6 categories of grml_offsets.ini
   │    signatures with live OK/CACHED/MISSING status, latest verified build + hover history, OTA update
   │  • Pre-flight Gate: cross-checks each mod's required_offsets/optional_offsets before launch and
   │    classifies mods as Ready / Degraded / Needs Offset Update / Legacy
   │  • Injection: CreateProcessW(CREATE_SUSPENDED) -> LoadLibraryW(grml_runtime.dll) -> GrmlBootstrap
   ▼
grml_runtime.dll (src/runtime/, injected into SOVIET64.exe)
   ├── offset_resolver  Declarative grml_offsets.ini resolver + O(1) PE-hash RVA cache (grml_offset_cache.ini)
   ├── pe_scanner       Single-pass .text AOB scan & profile offsets (config/grml_offsets.ini)
   ├── hook_manager     Centralized MinHook engine (GrmlHookApi) with per-plugin rollback
   ├── vfs_redirect     Zero-alloc Media VFS hooks (CreateFileW/A/2, GetFileAttributes*)
   ├── save_events      Centralized CreateFileW save load/unload/write tracker
   ├── plugin_supervisor SEH-guarded load, rollback on start failure, CrashGuard filter
   ├── tesmio_bridge    RML API v1 + Tesmio API v3/v4 host + per-plugin hook rollback
   ├── ring_logger      Thread-safe ring-buffer + immediate-flush grml.log writer
   └── overlay_hub      DX11/ImGui Mod Settings dialog (launcher-matched topbar + left sidebar, collapse/maximize controls), standalone windows, hotkeys & telemetry
```

## Launcher UI Pages (`src/launcher/ui/lib/views/`)

| View file | Nav label | Purpose |
|---|---|---|
| `ModsView.svelte` | Mods | Enable/disable plugins, version badges, dependency & offset checks |
| `PlaylistsView.svelte` | Playlists | Save/restore mod selection presets |
| `OverridesView.svelte` | Asset Overrides | Media VFS pack priority & conflict inspection |
| `SaveInspectorView.svelte` | Save Inspector | Scan saves, spot missing Workshop IDs, direct-to-save launch |
| `DiagnosticsView.svelte` | Diagnostics & Logs | Live grml.log, crash attribution, one-click mod disable |
| `UIGuideView.svelte` | UI Guide | Declarative & auto-built UI schema reference |
| `SettingsView.svelte` | Settings | Game path, hotkey, update check, discovery options |
| `OffsetsView.svelte` | Offsets & Signatures | Per-category offset health, build verification, OTA update |

## Shared Library (`src/common/`)

Both `GloriousRepublicModLoader.exe` and `grml_runtime.dll` link against `grml_common` so INI parsing/updating, manifest topological sorting, PE/AOB scanning, declarative/auto-built UI schemas, and Media VFS indexing share a single C++20 implementation with zero logic duplication.

## Offset Pipeline

```text
config/grml_offsets.ini          Declarative patterns (6 categories, multi-version AOB variants)
        │
        ▼
scripts/harvest_anchors.py       Scrapes SOVIET64.exe for anchor byte-sequences to seed new patterns
scripts/gen_sig.py               Scans a live SOVIET64.exe, generates/updates grml_offsets.ini entries
        │                        Flags: --category, --required, --strict
        ▼
grml_runtime.dll / pe_scanner    At injection time: single-pass .text scan, matches patterns,
        │                        writes RVA into O(1) PE-hash cache (grml_offset_cache.ini)
        ▼
GrmlWrsrOffsets (grml_api.h)     Exposes resolved offsets to mods via GrmlHostApi::GetWrsrOffsets()
        ▼
OffsetsView.svelte               Launcher page: live OK/CACHED/MISSING status per offset,
                                 latest verified build badge + +N hover popover for older builds
```
