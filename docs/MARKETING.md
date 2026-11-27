# GRML Alpha Release & Marketing Kit

Ready-to-use announcements, release notes, and publishing checklists for **Glorious Republic Mod Loader (GRML)**.

---

## 1. Steam Workshop Publishing Checklist

1. **Test staging locally (dry-run):**
   ```powershell
   .\scripts\publish_workshop.ps1 -DryRun
   ```
2. **First-time Workshop creation (creates a new Workshop ID and saves it to `steamcmd/workshop.json`):**
   - Copy `.env.example` to `.env.local` and set `STEAMCMD_EXE` and `STEAM_USERNAME`.
   - Run:
     ```powershell
     .\scripts\publish_workshop.ps1 -Visibility 2
     ```
   - Commit the updated `steamcmd/workshop.json` (`publishedFileId`) to `main`.
3. **Automated CI Releases (GitHub Actions):**
   - Add repository secrets in **Settings → Secrets and variables → Actions**:
     - `STEAM_USERNAME`
     - `STEAM_CONFIG_VDF` (base64-encoded SteamCMD `config/config.vdf` with cached Steam Guard token) **or** `STEAM_PASSWORD`
   - Push a tag like `git tag v2.1.0-alpha.1 && git push origin v2.1.0-alpha.1` (or trigger **Build, Release & Steam Workshop Publish** via `workflow_dispatch`).
   - Once verified on Steam Workshop, switch visibility from `2` (Hidden) to `0` (Public) and upload gallery screenshots from `docs/screenshots/` (including `01_mods.png`, `04_save_inspector.png`, `08_offsets.png`, and `09_ingame_overlay.jpg`) on the Workshop item page.

---

## 2. GitHub Release Notes (`v2.1.0-alpha.1`)

```markdown
## Glorious Republic Mod Loader (GRML) v2.1.0-alpha.1

> **Alpha Release Notice:** This is an early public alpha. Please back up your saves in `media_soviet\save\` before testing and report any bugs or broken signatures via [GitHub Issues](https://github.com/ZydraHydra/GloriousRepublicModLoader/issues).

### Highlights
- **Modern Self-Contained Launcher (WebView2 + Svelte 5):** Manage Steam Workshop (`784150`), `plugins/`, and `workshop_wip/` mods, save reusable **Playlists**, and search/filter by name, ID, ABI, or WRSR version—all stored safely in `%LOCALAPPDATA%\GRML`.
- **In-Game `F9` Settings Hub:** Matching left-sidebar card UI inside WRSR for configuring mods live, inspecting hook telemetry (`Calls/Frame` + `ms/Frame`), and viewing logs—even for legacy `.ini`-only mods.
- **Asset Overrides (Media VFS):** Non-destructively layer `media_soviet\` / `override\` folders and `.zip` packs with priority ordering and conflict inspection.
- **Direct-to-Save Launch & Intro Skip:** Click **▶ Continue `<SaveName>`** to boot straight into your latest save (with anti-crash-loop protection) while skipping the 9.5s intro logo timer.
- **Save Inspector & CrashGuard:** Spot missing Workshop IDs in any save with one-click copy & Steam links, plus post-crash attribution and live `grml.log` filtering.
- **Offsets & Signatures Page:** Multi-version AOB pattern tracking (`1.1.1.7`, `1.2.0.8`, `1.2.0.9`), live `OK` / `CACHED` / `MISSING` status, pre-flight mod compatibility checks, and one-click OTA offset updates.
- **100% Backwards Compatible:** Supports `*.grml.json`, legacy `*.rml.json`, manifestless DLLs, RML API v1, TesmioLoader API v3/v4, and `Local\WRSR_ModOverlay_<PID>` docking.

### Quick Start
1. Download `GRML-v2.1.0-alpha.1-win64.zip` below and extract all 3 files (`GloriousRepublicModLoader.exe`, `grml_runtime.dll`, `grml_offsets.ini`) to any folder.
2. Run `GloriousRepublicModLoader.exe`.
3. Enable the mods you want and click **▶ Launch WRSR** (press `F9` in-game for Mod Settings).
```

---

## 3. Mod Author Pitch (GitHub & Steam Workshop)

```markdown
Building native C++ DLL mods for WRSR? **GRML** gives you a single-header pure-C ABI (`include/grml_api.h`) while keeping your mod compatible with legacy loaders:
- **Zero ImGui/MinHook boilerplate:** Register settings tabs, tables, and hotkeys directly via `GrmlUiApi` and `GrmlHookApi` (with automatic SEH sandboxing and hook rollback on fault).
- **Declarative UI (`*.ui.yaml` / `*.ui.json`):** Define sliders, toggles, dropdowns, and tooltips once — rendered identically in both the launcher and in-game `F9`.
- **Shared Multi-Version Offsets (`grml_offsets.ini`):** Declare `required_offsets` / `optional_offsets` in your manifest instead of shipping duplicate AOB scanners in every DLL.
```
