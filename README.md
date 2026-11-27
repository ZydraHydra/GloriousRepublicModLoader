# Glorious Republic Mod Loader (GRML)

**Version 2.0.0** — Modern Windows x64 Mod Loader, Asset Override VFS, and Plugin Host for **Workers & Resources: Soviet Republic (`SOVIET64.exe` `1.2.0.9+`)**

GRML is a ground-up **C++20 (`/MT`) + Deno Desktop** mod loader and asset-override runtime inspired by the WRSR modding ecosystem. It preserves **100% backwards compatibility** with legacy RML v1 (`*.rml.json`), TesmioLoader API v3/v4, and manifestless native DLL plugins while introducing a unified pure-C plugin SDK (`include/grml_api.h`), integrated Media VFS asset overrides, zero-code declarative/auto-scanned `.ini` UIs, and runtime crash attribution.

---

## Key Features

- **Deno Desktop Launcher (`GloriousRepublicModLoader.exe`):**
  - Clean web-UI desktop app backed by a zero-console (`SUBSYSTEM:WINDOWS`) C++20 CLI engine (`grml_injector.exe`).
  - **Safe Opt-In Defaults:** Newly discovered local and Steam Workshop (`784150`) mods start disabled (`0`) until explicitly enabled in `grml.ini`.
  - **Playlists & Topological Load Order:** Resolves `dependencies`, `loadAfter`, `loadBefore`, and `conflictsWith` before launch.
  - **Diagnostics & Crash Attribution:** Live `grml.log` viewer, Windows Event Log (`EventID 1000`) fallback, and one-click **Disable Mod** for faulting plugins.
- **Integrated Asset Overrides (Media VFS):**
  - Non-destructive virtual filesystem redirection for `media_soviet\` and `override\` folders or `.zip` packs (`<GameDir>\media_overrides\` and Steam Workshop `784150`), managed via the **Asset Overrides** view and `grml_vfs.ini` with priority ordering and pre-launch conflict inspection.
- **Universal Mod Settings UI (Launcher + In-Game `F9`):**
  - Supports `<Mod>.ui.yaml`, `<Mod>.ui.json`, and inline `"settingsUi"` manifest schemas ([DECLARATIVE_UI_SCHEMA.md](docs/DECLARATIVE_UI_SCHEMA.md)).
  - **Automatic INI Scanner & UI Builder:** DLL mods that bring only an `.ini` file automatically get structured controls, Title Case labels, comment-derived tooltips, and comment-preserving saves in both the Launcher and `F9`.
- **Hardened Runtime (`grml_runtime.dll`):**
  - Single-pass `.text` AOB scanner externalized in `config/grml_offsets.ini` (`count == 1` validation).
  - Centralized `CreateFileW` save-lifecycle tracker (`RegisterSaveEvents` / `RegisterSaveWriteEvent`).
  - SEH-guarded plugin load/init/start with automatic per-plugin hook rollback & `FreeLibrary` unload on startup failure, SEH-sandboxed `F9` tabs, reverse-order unload, and `CrashGuard` unhandled-exception reporting.
- **Canonical Pure-C SDK (`include/grml_api.h`):**
  - Strictly `extern "C"`, `/MT`-safe (`abi_version` + `struct_size`), zero C++ STL across DLL boundaries, and zero `ImGuiContext*` sharing ([PLUGIN_SDK.md](docs/PLUGIN_SDK.md)).

---

## Quick Start (Players)

1. Download the latest `GRML-v*-win64.zip` release (or build from source) and extract it to a folder of your choice.
2. Run `GloriousRepublicModLoader.exe`.
3. Verify the detected `SOVIET64.exe` path in the top bar.
4. Enable the mods you want in **Mods & Load Order** (and optional texture/asset packs in **Asset Overrides**).
5. Click **Launch WRSR**. Press **`F9`** in-game to open **GRML Mod Settings**.

---

## Building from Source

### Prerequisites
- Windows 10/11 x64
- Visual Studio 2022 (MSVC C++20 toolset) + CMake 3.21+ + Ninja
- [Deno](https://deno.com/) 2.x (with `deno desktop`)

### 1. Build Native C++20 Binaries (`grml_injector.exe`, `grml_runtime.dll`)
From an **x64 Native Tools Command Prompt for VS 2022**:
```powershell
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build\release -j
```

### 2. Build the Deno Desktop Launcher (`GloriousRepublicModLoader.exe`)
```powershell
cd launcher-ui
deno check main.ts
deno task build
```

All release outputs are staged in `build\release\bin\`.

---

## Repository Layout

```text
├── .github/workflows/ci-release.yml   # Windows x64 CI + release packaging
├── cmake/                             # CMake helpers (embedded offsets)
├── config/                            # Default grml.ini & grml_offsets.ini
├── docs/                              # Architecture, SDK, UI Schema & Hooking rules
├── include/grml_api.h                 # Canonical pure-C host/plugin ABI
├── launcher-ui/                       # Deno Desktop frontend + thin HTTP bridge
├── scripts/fetch_references.ps1       # Optional upstream RML reference fetcher
├── src/
│   ├── common/                        # PE/AOB scanner, INI, JSON, Manifest, UI Schema, VFS
│   ├── launcher/                      # grml_injector.exe (CLI engine + suspended injector)
│   └── runtime/                       # grml_runtime.dll (F9 hub, CrashGuard, VFS, Compat)
├── AGENTS.md                          # Repository engineering invariants
├── CHANGELOG.md
├── CONTRIBUTING.md
└── LICENSE                            # MIT License
```

---

## Documentation

- [Architecture Overview](docs/ARCHITECTURE.md)
- [Plugin SDK (`include/grml_api.h`)](docs/PLUGIN_SDK.md)
- [Declarative & Auto-Built UI Schema](docs/DECLARATIVE_UI_SCHEMA.md)
- [Hooking & Performance Rules](docs/HOOKING_AND_PERFORMANCE_RULES.md)
- [Contributing Guide](CONTRIBUTING.md)

---

## Special Thanks & Acknowledgements

GRML stands on the shoulders of the *Workers & Resources: Soviet Republic* modding pioneers who defined the plugin ABIs and loader ecosystem that GRML remains compatible with:

- **Ultimate (`Ultimate-Universe`)** — author of [Republic Mod Loader (WRSR-RepublicModLoader)](https://github.com/Ultimate-Universe/WRSR-RepublicModLoader) ([Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749)), for designing RML API v1, safe symbol resolution, and Steam Workshop integration patterns.
- **Tesmio (`MaxLegend`)** — creator of [TesmioLoader](https://github.com/MaxLegend/TesmioLoader), for pioneering native code injection and the Tesmio API v3/v4 plugin ecosystem in *Workers & Resources: Soviet Republic*.

---

## License

Licensed under the **MIT License** ([LICENSE](LICENSE)).
