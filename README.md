# Glorious Republic Mod Loader (GRML)

**Version 2.0.0** — A fast, lightweight mod loader, in-game settings hub, asset override manager, and save inspector for **Workers & Resources: Soviet Republic (`SOVIET64.exe` `1.2.0.9+`)**.

GRML lets you manage Steam Workshop (`784150`) and local DLL mods, tweak mod `.ini` settings from a clean UI or **in-game**, layer non-destructive asset packs, and diagnose crashes or missing Workshop items in your saves—while staying **100% compatible** with existing **RML v1**, **TesmioLoader v3/v4**, and standalone DLL mods.

---

## Features for Players

- **Clean, Self-Contained Launcher:** Runs natively via Microsoft Edge WebView2 (`GloriousRepublicModLoader.exe`) with zero extra runtimes to install. All your settings, playlists, and logs are stored safely in `%LOCALAPPDATA%\GRML` so updates never overwrite your configuration.
- **Playlists & Mod Management:**
  - Automatically discovers subscribed **Steam Workshop (`784150`)** mods, local `plugins/`, and `workshop_wip/` mods.
  - Newly discovered mods start **disabled by default** until you turn them on.
  - Filter by name, Workshop ID, ABI, or supported game version, and save reusable **Playlists** to switch setups in one click.
  - Automatic dependency & conflict checking with clear version-compatibility badges.
- **Visual Mod Settings (Launcher + In-Game Overlay):**
  - Edit mod settings with sliders, toggles, dropdowns, and tooltips—no manual `.ini` editing required.
  - Even legacy mods that only ship an `.ini` file automatically get a structured settings UI.
  - Press **`F9`** in-game (or rebind any key in **Settings**) to open **GRML Mod Settings** right inside WRSR.
- **Asset Overrides (Media VFS):**
  - Layer `media_soviet\` / `override\` folders or `.zip` packs (`<GameDir>\media_overrides\` and compatible Workshop items) without modifying game files.
  - Reorder pack priority and inspect file conflicts before launching.
- **Save Inspector & Post-Crash Recovery:**
  - Scan `media_soviet\save\` on demand (sorted newest-first) to see each save's date, size, map template, and referenced Workshop items.
  - Immediately spot **missing Workshop IDs** with **Copy Missing IDs** and direct Steam Client / Web links to resubscribe.
  - If `SOVIET64.exe` crashes after loading a save, GRML alerts you with a one-click **Inspect Save** banner.
- **Diagnostics & CrashGuard:**
  - View live `grml.log` output (with severity highlighting, search, and level filters), Windows crash attribution, and a one-click **Disable Mod** button if a plugin faults.

---

## Quick Start

1. Download and extract the latest **GRML** release (`GloriousRepublicModLoader.exe`, `grml_runtime.dll`, `grml_offsets.ini`) to any folder.
2. Run **`GloriousRepublicModLoader.exe`**.
3. Check the detected WRSR version in the top bar (if your game is in a custom location, set `SOVIET64.exe` under **Settings**).
4. Enable the mods you want in **Mods & Load Order** (and configure any optional packs in **Asset Overrides**).
5. Click **▶ Launch WRSR**. Press **`F9`** (or your custom hotkey) in-game to open **GRML Mod Settings**.

### Where Your Data Is Stored
All user files live in `%LOCALAPPDATA%\GRML`:
- `grml.ini` — enabled mods, game path, discovery options, and overlay hotkey
- `grml_vfs.ini` — Asset Override pack order and toggles
- `grml_playlists.json` — saved mod playlists
- `grml.log` & `grml_session.json` — runtime log and last-loaded-save crash breadcrumb

---

## For Mod Authors & Developers

GRML provides a pure-C ABI (`include/grml_api.h`), declarative UI schemas (`*.ui.yaml` / `*.ui.json` / `"settingsUi"`), and centralized single-pass AOB / `CreateFileW` hooks while preserving full backwards compatibility with **RML v1** (`RmlPluginQuery` / `RmlPluginLoad`) and **TesmioLoader v3/v4** (`TsmPluginApiVersion` / `TsmPluginInit` / `TsmPluginStart`).

### Developer Documentation
- [Architecture Overview](docs/ARCHITECTURE.md)
- [Plugin SDK (`include/grml_api.h`)](docs/PLUGIN_SDK.md)
- [Declarative & Auto-Built UI Schema](docs/DECLARATIVE_UI_SCHEMA.md)
- [Hooking & Performance Rules](docs/HOOKING_AND_PERFORMANCE_RULES.md)
- [Repository Guidelines (`AGENTS.md`)](AGENTS.md)
- [Contributing Guide](CONTRIBUTING.md)

### Building from Source

#### Prerequisites
- Windows 10/11 x64
- Visual Studio 2022 (MSVC C++20 toolset) + CMake 3.24+ + Ninja
- [Node.js](https://nodejs.org/) 20+ (with `npm`)

#### One-Command Clean Build
From PowerShell at the repository root:
```powershell
.\build.ps1
```
This builds the SvelteKit static UI (`src/launcher-ui/web/`), compiles `GloriousRepublicModLoader.exe` (with embedded WebView2 + UI assets) and `grml_runtime.dll` in Release x64 (`/MT`), and outputs the clean 3-file distribution into `build\release\`.

### Repository Layout
```text
├── assets/                            # Audio/branding source assets
├── cmake/                             # CMake asset & offset embedding helpers
├── config/                            # Default grml.ini & grml_offsets.ini
├── docs/                              # Architecture, SDK, UI Schema & Hooking rules
├── include/grml_api.h                 # Canonical pure-C host/plugin ABI
├── src/
│   ├── common/                        # PE/AOB scanner, INI, JSON, Manifest, UI Schema, VFS
│   ├── launcher/                      # Native Win32 + WebView2 host, HTTP API, injector
│   ├── launcher-ui/web/               # Svelte 5 + SvelteKit static UI + reusable components
│   └── runtime/                       # grml_runtime.dll (F9 hub, CrashGuard, VFS, Compat)
├── build.ps1                          # Full clean build script -> build\release\
├── CMakeLists.txt
├── CHANGELOG.md
└── LICENSE                            # MIT License
```

---

## Special Thanks & Acknowledgements

GRML stands on the shoulders of the *Workers & Resources: Soviet Republic* modding pioneers who defined the plugin ABIs and loader ecosystem that GRML remains compatible with:

- **Ultimate (`Ultimate-Universe`)** — author of [Republic Mod Loader (WRSR-RepublicModLoader)](https://github.com/Ultimate-Universe/WRSR-RepublicModLoader) ([Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749)), for designing RML API v1, safe symbol resolution, and Steam Workshop integration patterns.
- **Tesmio (`MaxLegend`)** — creator of [TesmioLoader](https://github.com/MaxLegend/TesmioLoader), for pioneering native code injection and the Tesmio API v3/v4 plugin ecosystem in *Workers & Resources: Soviet Republic*.

---

## License

Licensed under the **MIT License** ([LICENSE](LICENSE)).
