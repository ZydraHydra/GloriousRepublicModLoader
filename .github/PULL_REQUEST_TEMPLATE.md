## Summary
<!-- Briefly describe what this PR changes and why. -->

## Affected Area
- [ ] Launcher UI (`src/launcher/ui/`)
- [ ] Native Launcher / HTTP API (`src/launcher/`)
- [ ] Runtime / Hooks / `F9` Overlay (`src/runtime/`)
- [ ] Common / PE Scanner / Offsets (`src/common/`, `config/grml_offsets.ini`)
- [ ] Public Pure-C ABI (`include/grml_api.h`)
- [ ] Docs / Scripts / CI / Steam Workshop

## Checklist
- [ ] **Backwards compatibility:** Preserves `*.grml.json`, legacy `*.rml.json`, manifestless DLLs, RML API v1, TesmioLoader API v3/v4, and `Local\WRSR_ModOverlay_<PID>` IPC.
- [ ] **Pure-C ABI safety:** If `include/grml_api.h` changed, changes are append-only, pure C11 (`grml_api_c_check`), and `/MT` heap-safe.
- [ ] **Frontend & i18n checks:** `npm run check` (includes `check:i18n` and `svelte-check`) and `npm run build` pass.
- [ ] **Full native build:** `.\build.ps1` succeeds cleanly in Release x64 (`/MT`).
- [ ] **Clean-room & hygiene:** No secrets, game binaries, or local Windows user paths in tracked files.
- [ ] **Docs updated:** Updated `CHANGELOG.md` and relevant `docs/` if behavior, offsets, or ABI changed.

## Manual Test Notes
- **WRSR Version(s):**
- **Mods tested with GRML:**
- **What was tested in-game / in launcher:**
