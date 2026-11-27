# GRML Architecture Overview

GRML splits responsibilities cleanly between a thin desktop UI, a native Win32 CLI engine, and an injected in-process runtime:

```text
GloriousRepublicModLoader.exe (Native Win32 + WebView2, src/launcher/)
   │  • Embedded SvelteKit SPA (src/launcher/ui/) + in-process HTTP/API
   │  • Discovery: plugins, manifests, topo-sort, UI schemas, Media VFS, Save Inspector
   │  • Config I/O: comment-preserving grml.ini, grml_vfs.ini, configs/<id>.ini (%LOCALAPPDATA%\GRML)
   │  • Diagnostics & Updates: SOVIET64.exe status, WER EventID 1000, GitHub Release & Offset OTA check
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
   └── overlay_hub      DX11/ImGui Mod Settings dialog, standalone windows, hotkeys & telemetry
```

## Shared Library (`src/common/`)

Both `GloriousRepublicModLoader.exe` and `grml_runtime.dll` link against `grml_common` so INI parsing/updating, manifest topological sorting, PE/AOB scanning, declarative/auto-built UI schemas, and Media VFS indexing share a single C++20 implementation with zero logic duplication.
