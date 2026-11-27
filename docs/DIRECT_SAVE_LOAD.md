# Direct-to-Save Launch (`SOVIET64.exe` `1.2.0.9`)

This document records the reverse-engineered save-loading and Main Menu readiness symbols in `SOVIET64.exe` (`1.2.0.9`, `PE TimeDateStamp = 0x6B05A574`, `SizeOfImage = 0xAE9000`), verified via Cheat Engine and `dumpbin /disasm`. All targets are resolved at runtime via single-match (`count == 1`) wildcarded AOB patterns in [grml_offsets.ini](file:///e:/____PROJECTS/GloriousRepublicModLoader/config/grml_offsets.ini).

---

## 1. Main Menu Readiness & `MainMenuManager` Singleton

In the main frame loop (`RVA 0x11DB42`, `main_menu_loop_aob`), `SOVIET64.exe` gates Main Menu rendering on `g_menuActive` (`RVA 0x9DDC75`) and passes the static `g_mainMenuObj` singleton (`RVA 0xA1EF20`) in `rcx` (`__fastcall`):

```asm
; RVA 0x11DB42 (main_menu_loop_aob)
44 38 35 2C 01 8C 00     cmp  byte ptr [g_menuActive], r14b   ; 0x9DDC75 == 1 when menu active
74 4E                    je   skip_menu
...
48 8D 0D A6 13 90 00     lea  rcx, [g_mainMenuObj]            ; 0xA1EF20
E8 11 E9 15 00           call MainMenu_RenderAndUpdate        ; RVA 0x27C490
```

Inside `MainMenu_RenderAndUpdate` (`RVA 0x27CA1C`, `main_menu_uistate_aob`), `uiState` at `[g_mainMenuObj + 0x5074]` is `1` on the root Main Menu (and `2` inside the Load Game submenu) once initial boot (`media_soviet/` and Steam Workshop `784150` template indexing) has completed and `g_menuActive` becomes `1`:

```asm
; RVA 0x27CA1C (main_menu_uistate_aob)
8B 87 74 50 00 00        mov  eax, [rdi + 5074h]              ; g_mainMenuObj->uiState (1=Main Menu, 2=Load Submenu)
83 F8 02                 cmp  eax, 2                          ; 2 == Load Game submenu
75 0D                    jne  check_in_game
48 8B CF                 mov  rcx, rdi
E8 E1 4E 02 00           call MainMenu_Draw                   ; RVA 0x2A1910
```

**Readiness Condition:** `g_menuActive != 0 && (*((int32_t*)(g_mainMenuObj + 0x5074)) == 1 || == 2) && *g_inGameFlag == 0`.

---

## 2. `LoadSaveHeaderAndCheckItems` & Save Load Trigger

Inside `MainMenu_Draw` (`RVA 0x2A4514..0x2A4846`), clicking **Load** on a local save performs three steps:

1. **Populate active save strings on `g_mainMenuObj`:**
   - `g_mainMenuObj + 0xA20` (`char[260]`): relative save folder path (`"save/<SaveFolderName>"`, relative to `media_soviet/`).
   - `g_mainMenuObj + 0x720` (`wchar_t[128]`): display name.
2. **Call `LoadSaveHeaderAndCheckItems(MainMenuObj* rcx, const SaveEntry* rdx)` (`RVA 0x45EB00`, `load_save_header_aob`):**
   - x64 `__fastcall` (`rcx = g_mainMenuObj`, `rdx = &entry` where `entry` is `0x20C` bytes: `+0x004` `wchar_t[128]` display name, `+0x104` `char[260]` `"save/<SaveFolderName>"`).
   - Opens `<entry+0x104>/header.bin`, parses header metadata, and checks Workshop dependencies.
3. **Trigger the main-loop map/save transition (`RVA 0x2A4820`, `load_save_trigger_aob`):**
   ```asm
   41 8B 86 78 50 00 00     mov  eax, [r14 + 5078h]
   FF C8                    dec  eax
   C6 05 6A A4 73 00 01     mov  byte ptr [g_loadRequested], 1   ; RVA 0x9DEC9A
   A9 FD FF FF FF           test eax, 0FFFFFFFDh
   C6 05 38 94 73 00 00     mov  byte ptr [g_newGameFlag], 0     ; RVA 0x9DDC74
   0F 94 C0                 sete al
   41 88 86 C0 10 00 00     mov  [r14 + 10C0h], al
   ```
   On the next main-loop iteration (`RVA 0x11CCAF`), `g_loadRequested != 0` invokes the world-load transition (`RVA 0x11D190`), tearing down the Main Menu and deserializing the save.

---

## 3. One-Shot Anti-Crash-Loop Contract (`grml_session.json`)

1. Launcher writes `"auto_load_save": "<SaveFolderName>"` when **Continue** or **Launch Save** is clicked (and clears it on standard **Play**).
2. `grml_runtime.dll` reads `"auto_load_save"` once during startup (`save_events::install`), immediately rewrites `grml_session.json` with `"auto_load_save": ""` and `"last_loaded_save": "<SaveFolderName>"` **before** any save deserialization begins, and arms a `std::atomic<bool>` one-shot request.
3. Inside `overlay_hub`'s main-thread `Present` hook (zero overhead once the `std::atomic<bool>` guard is cleared), GRML waits for the Main Menu readiness condition, verifies `<GameDir>\media_soviet\save\<SaveFolderName>\header.bin` exists on disk, and executes the load sequence once on the main thread.
