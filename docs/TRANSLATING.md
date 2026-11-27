# Translating the GRML Launcher UI

The GRML launcher UI loads all user-facing text strings from JSON locale files under `src/launcher/ui/static/locales/`.

> **Scope:** Only the launcher UI (navigation, labels, buttons, tooltips, descriptions, and UI status messages) is translated. Technical internals—such as `grml.log` entries, CrashGuard diagnostics, offset tables, and backend error messages—remain in English.

## Adding a New Language

1. Copy [`src/launcher/ui/static/locales/en.json`](../src/launcher/ui/static/locales/en.json) to a new file named `<code >.json` using a standard BCP-47 / ISO 639-1 language code (for example `ru.json`, `pl.json`, `cs.json`, `zh-CN.json`).
2. Update the `_meta` block at the top of the file:
   ```json
   "_meta": {
     "code": "pl",
     "name": "Polish",
     "nativeName": "Polski"
   }
   ```
3. Translate the string values while keeping:
   - Placeholder tokens like `{count}`, `{version}`, `{name}`, `{save}`, `{hotkey}` intact.
   - Inline HTML tags like `<code>...</code>` and `<strong>...</strong>` intact.
4. Any missing keys automatically fall back to English (`en.json`).

## Testing Without Rebuilding

The launcher HTTP server automatically discovers and serves external locale files from either:

- `<GRML_DIR>\locales\<code >.json` (next to `GloriousRepublicModLoader.exe`), or
- `%LOCALAPPDATA%\GRML\locales\<code >.json`

External files override built-in embedded translations of the same language code, so you can drop a new `.json` file into `locales\`, restart or reload the launcher, and select it under **Settings → Launcher Language**.

## Bundling Into a Release Build

Any `.json` file in `src/launcher/ui/static/locales/` is automatically bundled by SvelteKit and embedded into `GloriousRepublicModLoader.exe` by `.\build.ps1`.

---

## Translating Mod In-Game & Declarative UIs

Mods can also ship their own `locales\<code >.json` files next to their plugin DLL to translate both their **Launcher Mod Inspector** settings and their **In-Game `F9` Mod Settings** UI:

- **Declarative / Zero-Code UIs (`.ui.yaml`, `.ui.json`, `settingsUi`, or auto-built `.ini` UIs):** See [DECLARATIVE_UI_SCHEMA.md](DECLARATIVE_UI_SCHEMA.md#multi-language-support-localeslangjson).
- **Native C++ Plugins (`GrmlPluginInit`):**
  - `host->GetLanguage()` returns the active language code from `grml.ini` (e.g. `"en"`, `"de"`).
  - `host->LoadTranslations(PLUGIN_ID, nullptr)` loads `<DllDir>\locales\en.json` (fallback) and `<DllDir>\locales\<lang>.json`.
  - `host->Translate(PLUGIN_ID, "ui.my_label")` returns a session-lifetime UTF-8 `const char*` ready to pass to `ui->Checkbox`, `ui->Button`, `ui->Text`, etc.

