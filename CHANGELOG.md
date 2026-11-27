# Changelog

## Glorious Republic Mod Loader (GRML) v2.1.0

### Offsets & Signatures Page
- **Dedicated "Offsets & Signatures" launcher page (`OffsetsView.svelte`):** Displays all 6 offset categories (`core`, `simulation`, `rendering`, `input`, `audio`, `ui`) with per-row `OK` / `CACHED` / `MISSING` status badges, aligned fixed-width columns, latest verified game build badge, and a `+N` hover popover listing all older verified builds.
- **Multi-version AOB patterns in `config/grml_offsets.ini`:** Each offset entry now carries separate `pattern_1_1_1_7`, `pattern_1_2_0_8`, and `pattern_1_2_0_9` variants so the runtime can resolve the correct byte sequence without manual edits on game updates.
- **`scripts/harvest_anchors.py`:** New tool that scrapes a live `SOVIET64.exe` binary for unique anchor byte-sequences to seed new AOB pattern entries.
- **`scripts/gen_sig.py` extensions:** `--category` flag to target a specific offset group, `--required` / `--strict` flags to gate launch on offset availability, automatic `grml_offsets.ini` write-back.
- **Pre-flight mod compatibility gate:** Launcher cross-checks each mod's `required_offsets` / `optional_offsets` manifest keys before launch and classifies the mod as `Ready`, `Degraded`, `Needs Offset Update`, or `Legacy`; offers one-click temporary disable for affected mods.
- **`docs/OFFSET_CATALOG.md`:** New full catalog doc listing all 31 declared offsets across the 6 categories with descriptions, ABI fields, and version coverage.
- **21 locale files updated** with `nav.offsets` translation key for the new nav tab.

## Glorious Republic Mod Loader (GRML) v2.0.0

### Highlights & Architecture
- **Modernized CMake + C++20 (`/MT`) Build:** Standardized native targets (`grml_common`, `grml_injector.exe`, `grml_runtime.dll`) with GitHub Actions Windows x64 release packaging ([.github/workflows/ci-release.yml](.github/workflows/ci-release.yml)).
- **Native Win32 + WebView2 Launcher (`src/launcher/` + `src/launcher/ui/`):** Replaced the legacy Go/Win32 UI with a self-contained `SUBSYSTEM:WINDOWS` Microsoft Edge WebView2 host embedding the SvelteKit 5 SPA and an in-process HTTP API, plus GitHub Release update checking.
- **Canonical Pure-C SDK v2 (`include/grml_api.h`):** Introduced `GrmlPluginInit`, `GrmlHostApi` (single-pass `.text` AOB scanner, centralized `CreateFileW` save events, unified ring-buffer logging, `%LOCALAPPDATA%\GRML\configs\` path helper, `RegisterService`/`GetService`), `GrmlHookApi` (centralized `MinHook` engine with per-plugin rollback), `GrmlWrsrApi` + `GrmlWrsrOffsets` (shared `IsInGame`, RIP-relative helpers, symbols, and profile-resolved offsets), and `GrmlUiApi` (ImGui widgets/tables, `RegisterDeclarativeIniTab`, standalone `RegisterWindow`, centralized `RegisterHotkey` conflict detection, and `ReportHookTiming` telemetry).
- **100% Backwards Compatibility:** Discovers `*.grml.json`, legacy `*.rml.json`, and manifestless DLLs while preserving v1 `struct_size` compatibility, RML API v1, TesmioLoader API v3/v4, and `Local\WRSR_ModOverlay_<PID>` docking.

### Asset Overrides, UI & Reliability
- **Integrated Asset Overrides (Media VFS):** Built-in `media_soviet\` / `override\` folder and `.zip` VFS redirection (`grml_vfs.ini`), pre-launch **Asset Overrides** priority/conflict manager in the launcher, and read-only active override stats in-game.
- **Universal Declarative UI & Auto INI Scanner:** Added `<Mod>.ui.yaml`, `<Mod>.ui.json`, and `"settingsUi"` support plus an automatic `.ini` scanner & UI builder that infers controls, Title Case labels, ranges, and comment tooltips while preserving existing `.ini` comments on save.
- **Safety & Diagnostics:** Added opt-in default (`enabled=0`) for newly discovered mods/packs, SEH-guarded plugin loading with per-plugin hook rollback and unload on startup failure, SEH-sandboxed in-game tabs/windows, `CrashGuard` unhandled-exception blame logging, main-menu `GRML <ver> [<hotkey>]` status tag, and launch-driven Crash Attribution in the launcher.

---

## Republic Mod Loader v1.0.1 (Upstream)

### Workshop and playlists
- Raised the enabled Workshop-item capacity from **4,096 to 40,000** across playlist/configuration handling and runtime-plan generation.

### Launcher
- `RepublicModLoader.exe` can be copied outside the RML Workshop folder and locate its runtime files from Workshop item `3787969749` across Steam's configured library folders.
- Preserves Steam client session environment while preventing non-Steam shortcut App/Game/Overlay IDs from leaking into WRSR.

### Build
- Clang/clang-cl native-runtime builds disable builtin CRT substitution.

## Republic Mod Loader v1.0.0 (Upstream)

- Initial public release: standalone Windows x64 launcher and native runtime for *Workers & Resources: Soviet Republic*, RML API 1, and Tesmio API 3/4 compatibility host.
