# Contributing to Glorious Republic Mod Loader (GRML)

Thank you for contributing to **GRML**! Please review these guidelines before opening a pull request.

---

## 1. Engineering Invariants

1. **Preserve Backwards Compatibility:**
   - Do not break discovery of `*.grml.json`, legacy `*.rml.json`, or manifestless DLLs.
   - Maintain RML API v1 (`RmlPluginQuery` / `RmlPluginLoad`), TesmioLoader API v3/v4 (`TsmPlugin*`), and `Local\WRSR_ModOverlay_<PID>` IPC compatibility.
2. **Keep `include/grml_api.h` Strictly Pure C & Append-Only:**
   - All structs must start with `uint32_t abi_version` and `uint32_t struct_size`.
   - Zero C++ STL types, zero `ImGuiContext*` pointers, and zero cross-DLL heap allocations/frees (`/MT` static CRT safety).
   - Validate pure-C compilation via the `grml_api_c_check` C11 target.
3. **Follow Hooking & Performance Rules:**
   - Read [docs/HOOKING_AND_PERFORMANCE_RULES.md](docs/HOOKING_AND_PERFORMANCE_RULES.md) before touching any hook, AOB pattern, or hot path.
   - Keep `config/grml_offsets.ini` updated with wildcarded AOB patterns and `count == 1` validation.
4. **Wide Paths & UTF-8 Without BOM:**
   - Use `std::filesystem::path` / `wchar_t` and Win32 `*W` APIs for all game, plugin, and Workshop paths.
   - Read and write `.ini` / `.json` files as UTF-8 without BOM, preserving user comments on `.ini` updates.
5. **Never Modify Steam Workshop Content Autonomously:**
   - Keep all test builds and deployments inside `build\<config>\bin\`.

---

## 2. Local Verification Checklist

Before submitting a PR:

1. **SvelteKit UI Check (`src/launcher/ui/`):**
   ```powershell
   cd src/launcher/ui
   npm run check
   npm run build
   ```
2. **Full Clean Build (UI + Native C++20 `/MT`):**
   ```powershell
   .\build.ps1
   ```
3. **CLI Smoke Check:**
   ```powershell
   .\build\release\GloriousRepublicModLoader.exe --discover
   ```
   Verify `grml_api_c_check` compiles cleanly and `GloriousRepublicModLoader.exe` / `grml_runtime.dll` remain statically linked (`/MT`).
