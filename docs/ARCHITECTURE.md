# GRML Architecture Overview

GRML splits responsibilities cleanly between a thin desktop UI, a native Win32 CLI engine, and an injected in-process runtime:

```text
GloriousRepublicModLoader.exe (Deno Desktop + WebView2)
   │  Thin HTTP + static UI bridge (launcher-ui/main.ts + ui/*)
   ▼  stdin/stdout JSON (SUBSYSTEM:WINDOWS, zero console popups)
grml_injector.exe (src/launcher/)
   │  • Discovery: plugins, manifests, topo-sort, UI schemas, Media VFS
   │  • Config I/O: comment-preserving grml.ini, grml_vfs.ini, mod .ini files
   │  • Diagnostics: SOVIET64.exe process status + WER EventID 1000 query
   │  • Injection: CreateProcessW(CREATE_SUSPENDED) -> LoadLibraryW(grml_runtime.dll) -> GrmlBootstrap
   ▼
grml_runtime.dll (src/runtime/, injected into SOVIET64.exe)
   ├── pe_scanner       Single-pass .text AOB scan (config/grml_offsets.ini)
   ├── vfs_redirect     Zero-alloc Media VFS hooks (CreateFileW/A/2, GetFileAttributes*)
   ├── save_events      Centralized CreateFileW save load/unload/write tracker
   ├── plugin_supervisor SEH-guarded load, rollback on start failure, CrashGuard filter
   ├── tesmio_bridge    RML API v1 + Tesmio API v3/v4 host + per-plugin hook rollback
   ├── ring_logger      Thread-safe ring-buffer + immediate-flush grml.log writer
   └── overlay_hub      DX11/ImGui [F9] Mod Settings dialog, telemetry & SEH-sandboxed tabs
```

## Shared Library (`src/common/`)

Both `grml_injector.exe` and `grml_runtime.dll` link against `grml_common` so INI parsing/updating, manifest topological sorting, PE/AOB scanning, declarative/auto-built UI schemas, and Media VFS indexing share a single C++20 implementation with zero logic duplication.
