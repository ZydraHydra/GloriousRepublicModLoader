# Changelog

## Glorious Republic Mod Loader (GRML) v2.0.0

### Highlights & Architecture
- **Modernized CMake + C++20 (`/MT`) Build:** Standardized native targets (`grml_common`, `grml_injector.exe`, `grml_runtime.dll`) with GitHub Actions Windows x64 release packaging ([.github/workflows/ci-release.yml](.github/workflows/ci-release.yml)).
- **Deno Desktop Launcher (`launcher-ui/`):** Replaced the legacy Go/Win32 UI with a web-technology Deno Desktop launcher backed by `grml_injector.exe` (`SUBSYSTEM:WINDOWS`, zero console popups).
- **Canonical Pure-C SDK (`include/grml_api.h`):** Introduced `GrmlPluginInit`, `GrmlHostApi` (single-pass `.text` AOB scanner, centralized `CreateFileW` save events, unified ring-buffer logging), and `GrmlUiApi` (safe ImGui widget wrappers, `RegisterDeclarativeIniTab`, and `ReportHookTiming` telemetry).
- **100% Backwards Compatibility:** Discovers `*.grml.json`, legacy `*.rml.json`, and manifestless DLLs while preserving RML API v1, TesmioLoader API v3/v4, and `Local\WRSR_ModOverlay_<PID>` docking.

### Asset Overrides, UI & Reliability
- **Integrated Asset Overrides (Media VFS):** Built-in `media_soviet\` / `override\` folder and `.zip` VFS redirection (`grml_vfs.ini`), pre-launch **Asset Overrides** priority/conflict manager in the launcher, and live toggle/stats in `F9`.
- **Universal Declarative UI & Auto INI Scanner:** Added `<Mod>.ui.yaml`, `<Mod>.ui.json`, and `"settingsUi"` support plus an automatic `.ini` scanner & UI builder that infers controls, Title Case labels, ranges, and comment tooltips while preserving existing `.ini` comments on save.
- **Safety & Diagnostics:** Added opt-in default (`enabled=0`) for newly discovered mods/packs, SEH-guarded plugin loading with per-plugin hook rollback and unload on startup failure, SEH-sandboxed `F9` tabs, `CrashGuard` unhandled-exception blame logging, and launch-driven Crash Attribution in the launcher.

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
