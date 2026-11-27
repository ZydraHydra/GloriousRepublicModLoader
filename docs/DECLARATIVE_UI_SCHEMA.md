# Declarative UI Schema for External Mods (`.ui.yaml` / `.ui.json`)

External mods—including legacy **TesmioLoader**, **RML v1**, and raw native DLLs with **zero GRML C++ SDK integration**—can define a declarative settings UI that works automatically in **both**:

1. The **GRML Deno Desktop Launcher** (before game launch)
2. The **In-Game `F9` Mod Settings Hub** (`grml_runtime.dll`)

Values are read from and saved directly to the mod's `.ini` file (UTF-8 without BOM), preserving all unknown sections and keys.

---

## Discovery Order & Zero-Config Auto UI Builder

For any plugin `<ModName>.dll`, GRML searches in order:
1. `<ModName>.ui.json` beside the DLL
2. `<ModName>.ui.yaml` or `<ModName>.ui.yml` beside the DLL
3. Inline `"settingsUi"` object inside `<ModName>.grml.json` or `<ModName>.rml.json`
4. **Automatic INI Scanner & UI Builder (Zero Config):** If no explicit schema exists and the mod brings its own `.ini` (`<ModName>.ini`, manifest `"ini"`/`"iniFile"`, or the unique `.ini` in that mod folder), GRML scans sections, keys, values (`bool`, `int`, `float`, `string`), preceding `;`/`#` comment tooltips, and optional `Range A..B` hints to generate a ready-to-use UI automatically in **both** the Launcher and `F9`. Edits preserve existing `.ini` comments and casing in-place.

You can also click **Export `.ui.yaml`** inside the launcher to save an explicit editable schema file alongside the DLL.

---

## Supported Control Types

| Type | Rendered Widget | INI Value | Supported Fields |
|---|---|---|---|
| `header` | Section divider & title | *(none)* | `label` |
| `bool` | Checkbox / toggle | `1` / `0` | `section`, `key`, `label`, `default`, `tooltip` |
| `int` | Slider + numeric box | Integer (`42`) | `section`, `key`, `label`, `min`, `max`, `step`, `default`, `format`, `tooltip` |
| `float` | Float slider + numeric box | Decimal (`1.5000`) | `section`, `key`, `label`, `min`, `max`, `step`, `default`, `format`, `tooltip` |
| `select` | Dropdown combo box | Selected option string | `section`, `key`, `label`, `options`, `default`, `tooltip` |
| `string` | Single-line text input | Raw string | `section`, `key`, `label`, `default`, `tooltip` |

---

## Example (`MyMod.ui.yaml`)

```yaml
tabTitle: My Mod Settings
iniFile: MyMod.ini
mainMenuAvailable: true
controls:
  - type: header
    label: General
  - type: bool
    section: settings
    key: enabled
    label: Enable Mod
    default: true
    tooltip: Master toggle stored in MyMod.ini
  - type: float
    section: settings
    key: speed_multiplier
    label: Speed Multiplier
    min: 0.25
    max: 10.0
    step: 0.25
    format: "%.2fx"
    default: 2.0
  - type: select
    section: settings
    key: mode
    label: Mode
    options: [auto, manual, off]
    default: auto
```
