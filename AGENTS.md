# Glorious Republic Mod Loader (GRML) — Agent Guidelines

Read [docs/HOOKING_AND_PERFORMANCE_RULES.md](docs/HOOKING_AND_PERFORMANCE_RULES.md) before modifying any hook, AOB scanner, or runtime code.

---

## 1. Architecture & Backwards-Compatibility Invariants

GRML is the host loader (`GloriousRepublicModLoader` Deno Desktop launcher + `grml_injector.exe` + `grml_runtime.dll`) for **Workers & Resources: Soviet Republic (`SOVIET64.exe` `1.2.0.9+`)** and owns the canonical plugin SDK header (`include/grml_api.h`).

1. **100% Backwards Compatibility Required:**
   - Must discover and load **both** `*.grml.json` and legacy `*.rml.json` manifests (and manifestless native DLLs).
   - Must preserve **RML API v1** (`RmlPluginQuery` / `RmlPluginLoad`) and **TesmioLoader API v4** (`TsmPluginApiVersion` / `TsmPluginInit` / `TsmPluginStart`) compatibility.
   - Must seamlessly host external/standalone plugins (both via `GrmlPluginInit` and legacy `Local\WRSR_ModOverlay_<PID>` docking).
2. **In-Game `F9` Is a Mod Settings Dialog:**
   - The `F9` overlay (`GRML Mod Settings [F9]`) hosts per-mod configuration tabs, live hook telemetry (`Calls/Frame` + `ms/Frame`), and the read-only **Loaded Mods** overview + ring-buffer log viewer.
   - Mod enable/disable management belongs in the launcher (`grml.ini`), not in-game toggles.
   - Every external `render_tab()` callback must run inside `ImGui::PushID` + `ImGui::BeginChild` state guards and an SEH `__try` / `__except` handler (`[UI Faulted - Disabled]` on exception).

---

## 2. Canonical Pure-C ABI (`include/grml_api.h`)

- `include/grml_api.h` is the **single source of truth** for the GRML host/plugin contract. Mods load/dock into GRML, never the reverse.
- Must remain **strictly pure C** (`extern "C"`), zero C++ STL types, zero `ImGuiContext*` pointers, and zero cross-DLL heap frees (`/MT` static CRT safety).
- Every ABI struct must begin with `uint32_t abi_version` and `uint32_t struct_size`, and changes must be **append-only**.
- Downstream mod repositories mirror `include/grml_api.h`; never introduce breaking or unversioned ABI edits.

---

## 3. Hooking, Signatures & Build Rules

- Enforce every rule in [docs/HOOKING_AND_PERFORMANCE_RULES.md](docs/HOOKING_AND_PERFORMANCE_RULES.md) on all GRML hooks (`CreateFileW` save tracker, DX11 `Present`/`ResizeBuffers`, `DirectInput`/`C3D_INPUT` guards, Workshop forward filter).
- Externalize all wildcarded AOB patterns in `config/grml_offsets.ini`. Verify exact single-match (`count == 1`) at startup; if an optional pattern misses, log the exact missing pattern name and disable only that feature cleanly.
- Use wide strings (`std::filesystem::path` / `wchar_t`) and Win32 `*W` APIs for all Workshop (`784150`) and plugin filesystem paths.
- Support universal declarative mod UI schemas (`*.ui.yaml`, `*.ui.json`, or `"settingsUi"` inside `*.grml.json`/`*.rml.json`, documented in `docs/DECLARATIVE_UI_SCHEMA.md`) in both the Deno Desktop launcher (`src/launcher-ui/`) and in-game `F9`, backed directly by each mod's UTF-8 `.ini` file.
- **Never deploy or modify Steam Workshop (`784150`) content autonomously:** Never copy binaries, manifests, `.ui.*` files, or `.ini` changes into `steamapps\workshop\content\784150\*` or run Workshop uploads without explicit user confirmation. Keep test deployments inside `build\<config>\bin\plugins\`.
- Build the native C++20 (`/MT` static CRT) injector and runtime (`grml_injector.exe`, `grml_runtime.dll`) with **CMake**:
  ```powershell
  cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build\release -j
  ```
- Build the Deno Desktop launcher (`build\release\bin\GloriousRepublicModLoader\GloriousRepublicModLoader.exe`) from `src/launcher-ui/`:
  ```powershell
  deno check main.ts
  deno task build
  ```
