# Declarative Offsets, PE-Hash RVA Caching & Signature Tooling

This document captures the design, binary research, and maintenance workflow for `config/grml_offsets.ini`, GRML's `O(1)` PE-hash RVA cache (`grml_offset_cache.ini`), `scripts/gen_sig.py`, OTA updates, and automated Steam update monitoring.

---

## 1. Self-Describing `grml_offsets.ini` Schema

Declarative offset sections in [config/grml_offsets.ini](../config/grml_offsets.ini) allow `grml_runtime.dll`, `GloriousRepublicModLoader.exe`, and `GrmlHostApi::ResolveNamedOffset` plugins to resolve game addresses and struct displacements without hardcoding resolution math in C++.

Every section (excluding `[signatures]`, `[builds]`, `[general]`, and `[profile.*]`) that defines a non-empty `pattern` is treated as a declarative offset entry:

| Property | Required | Default | Description |
| :--- | :---: | :---: | :--- |
| `pattern` | Yes | — | Space-separated hex bytes with `??` (or `?`) wildcards. |
| `resolve` | No | `function_start` | Resolution formula: `function_start`, `rip_rel32`, `call_rel32`, or `disp32`. |
| `op_offset` | For `rip_rel32`, `call_rel32`, `disp32` | `0` | Byte offset from `match_addr` to the 4-byte displacement/immediate. |
| `instr_len` | For `rip_rel32`, `call_rel32` | `0` | Byte offset from `match_addr` to the next instruction (`RIP` base). |
| `extra_offset` | No | `0` | Signed post-resolution byte adjustment (decimal or hex). |
| `anchor_xref` | No | `""` | Nearby immutable string literal or RTTI class name for patch migration. |
| `verified_build` | No | `""` | Target `SOVIET64.exe` version string (e.g. `1.2.0.9`). |
| `category` | No | `General` | Grouping category in the launcher Diagnostics UI and [OFFSET_CATALOG.md](OFFSET_CATALOG.md) (`Core`, `TimeAndSim`, `Economy`, `EntityManagers`, `Templates`, `SaveLoad`, `CameraAndUI`). |
| `required` | No | `false` | If `true`/`1`, missing this offset emits an `AOB_MISSING` diagnostic card in the launcher; if `false`, it degrades silently unless an enabled mod declares it in `required_offsets`. Pass `--strict` to `gen_sig.py --verify-all` to fail on optional misses too. |

### Resolution Formulas
- **`function_start`:** `match_addr + extra_offset`
- **`rip_rel32`:** `match_addr + instr_len + *(int32_t*)(match_addr + op_offset) + extra_offset`
- **`call_rel32`:** `match_addr + instr_len + *(int32_t*)(match_addr + op_offset) + extra_offset`
- **`disp32`:** `*(uint32_t*)(match_addr + op_offset) + extra_offset` (returns a struct field displacement or immediate rather than an executable pointer)

### Layered Load Order & Profile Inheritance
1. **INI Merge Order** (`grml::offsets::loadMergedOffsetsIni` in `src/common/offset_resolver.cpp`):
   1. Embedded baseline compiled into the binaries (`embedded_grml_offsets.hpp`).
   2. External `<LoaderDir>\grml_offsets.ini` (if present).
   3. OTA override `%LOCALAPPDATA%\GRML\grml_offsets.ini` (if present and distinct from `<LoaderDir>\grml_offsets.ini`).
2. **Versioned Struct Profiles (`[profile.<ver>]`):**
   - Matched against `SOVIET64.exe` PE `TimeDateStamp` (and optional `SizeOfImage`), supporting comma-separated values and recursive `base = <parent>` inheritance (`[profile.1.2.0.9]`, `[profile.1.2.0.8]`, `[profile.1.1.1.13]`, `[profile.1.1.1.9]`, `[profile.1.1.1.7]` -> `[profile.default]`).

---

## 2. Verified `SOVIET64.exe` Binary Fingerprints & Results

Inspection of `SOVIET64.exe` across three installed game versions established two key facts:
1. **`VS_VERSION_INFO` is empty on `SOVIET64.exe`:** Windows `GetFileVersionInfoW` / `VS_FIXEDFILEINFO` returns empty/`0.0.0.0`, so GRML identifies the exact build via SHA-256 lookup in `[builds]` (falling back to `VS_FIXEDFILEINFO` only if populated in a future build).
2. **All 4 core declarative signatures match uniquely (`count == 1`) across `1.1.1.7`, `1.2.0.8`, and `1.2.0.9`:**

| Build | Steam BuildID | PE `TimeDateStamp` | `SizeOfImage` | `AddressOfEntryPoint` | `in_game_flag` | `resource_vector` | `resource_stride` | `evaluate_dist_task` |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1.2.0.9** | `26953348` | `0x6B05A574` | `0xAE9000` | `0x891898` | `0x11D8DF` → `0x9DEC98` | `0xEBA24` → `0xA2B270` | `0x380` | `0x1FE750` |
| **1.2.0.8** | — | `0x6B0097B2` | `0xAE9000` | `0x8913A8` | `0x11DA4F` → `0x9DEC98` | `0xEBB94` → `0xA2B268` | `0x380` | `0x1FE850` |
| **1.1.1.7** | — | `0x69C4098C` | `0xA9D000` | `0x84E978` | `0x105E4F` → `0x993C65` | `0xD3FB4` → `0x9E11C0` | `0x340` | `0x1E3B60` |

---

## 3. Single-Pass Scanning, Graceful Degradation & `O(1)` RVA Cache

1. **PE-Hash RVA Cache (`%LOCALAPPDATA%\GRML\grml_offset_cache.ini`):**
   - Computes a composite fingerprint combining `SOVIET64.exe`'s PE COFF `TimeDateStamp`, `SizeOfImage`, `AddressOfEntryPoint`, and a canonical 64-bit FNV-1a hash of the merged `grml_offsets.ini`.
   - **Cache Hit:** Reconstructs `module_base + cached_rva` and verifies every non-wildcard byte of `pattern` within `.text` bounds in `O(1)`, marking the entry `CACHED` (or `MISSING` immediately if cached as `MISSING`).
   - **Cache Miss / Byte Mismatch:** Queues any unverified entries alongside plugin `GrmlHostApi::RegisterPatternScan` requests into a single sequential `.text` pass (`runSinglePassAobScan`), marks unique matches `OK`, and rewrites `grml_offset_cache.ini` (recording both `OK` RVAs and `MISSING` entries).
   - **Pre-Flight Launcher Validation:** `GloriousRepublicModLoader.exe` runs the same `O(1)` cache check / `SEC_IMAGE` `.text` scan on disk before launching `SOVIET64.exe` and cross-checks each mod's `required_offsets` / `optional_offsets` (`Ready`, `Degraded`, `Needs Offset Update`, `Legacy`).
2. **Strict Single-Match & Graceful Degradation:**
   - Entries with `match_count != 1` are logged to `grml.log` (`[OFFSET MISSING]`) and published to `%LOCALAPPDATA%\GRML\grml_session.json` as `MISSING` without crashing `SOVIET64.exe`.
   - `GrmlHostApi::ResolveNamedOffset(section_name, &out_value)` returns `1` only for `OK` or `CACHED` entries and `0` otherwise.
   - See [OFFSET_CATALOG.md](OFFSET_CATALOG.md) for the full 6-category verified base offset catalog and `scripts/harvest_anchors.py` anchor-harvesting workflow.

---

## 4. Signature Generator & Batch Verifier (`scripts/gen_sig.py`)

### Python Environment Setup
Dependencies are pinned in `scripts/requirements.txt` (`pefile`, `capstone`) using an isolated `.venv/` (gitignored):
```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r scripts\requirements.txt
```

### Batch Verification Mode
Test every declarative entry in `config/grml_offsets.ini` against any `SOVIET64.exe` binary (returns exit code `1` if any required entry fails):
```powershell
.venv\Scripts\python scripts\gen_sig.py --exe "<PathToSOVIET64.exe>" --verify-all
```

### Signature Generation Mode
Disassemble x86-64 forward from `--rva` with Capstone, auto-wildcard `RIP`-relative `disp32`, relative `CALL`/`JMP`/`Jcc` `rel32`, and large struct `disp32` (`abs(disp) >= 0x80`), stop on unique `.text` match (`count == 1`), append **2 extra instructions** as a collision safety margin, and print or upsert into `config/grml_offsets.ini`:
```powershell
.venv\Scripts\python scripts\gen_sig.py `
  --exe "<PathToSOVIET64.exe>" `
  --rva 0x0011D8DF `
  --name in_game_flag `
  --mode rip_rel32 `
  --anchor "optional_literal_or_rtti" `
  --write
```

---

## 5. Launcher Diagnostics, GitHub OTA & Steam Update Monitoring

- **Launcher Offset Diagnostics:** `GloriousRepublicModLoader.exe --discover` (and `/api/state` -> **Diagnostics & Logs**) reads live runtime results from `grml_session.json` when the PE/INI fingerprint matches, or verifies `grml_offset_cache.ini` / `.text` directly on disk so per-entry `OK` / `CACHED` / `MISSING` health is visible before launching.
- **GitHub OTA `grml_offsets.ini` Updater:** Clicking **Update Offsets (GitHub OTA)** in **Diagnostics & Logs** or **Settings** (`POST /api/update-offsets`) downloads `config/grml_offsets.ini` from `raw.githubusercontent.com/ZydraHydra/GloriousRepublicModLoader/main/config/grml_offsets.ini` (overridable via `grml.ini` `[general] offsets_ota_url`), validates INI syntax, writes UTF-8 without BOM, and invalidates `grml_offset_cache.ini`.
- **Automated Daily Steam Update Check:** [.github/workflows/check-wrsr-update.yml](../.github/workflows/check-wrsr-update.yml) queries `https://api.steamcmd.net/v1/info/784150` daily at `06:00 UTC` against [.github/wrsr_build_state.json](../.github/wrsr_build_state.json) and opens a `game-update` GitHub Issue with an offset-verification checklist whenever a new `public` Steam `buildid` is published.
