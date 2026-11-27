# GRML Launcher MCP Server

`GloriousRepublicModLoader.exe` includes an embedded **Model Context Protocol (MCP)** server (**Streamable HTTP `2025-03-26`**) on `/mcp`. It lets AI agents manage mods, playlists, `.ini` configs, and VFS overrides for players, and enables mod developers to automate end-to-end build-test-debug loops (launching `SOVIET64.exe` directly into a test save, waiting for save load, inspecting `grml.log` / crashes, and stopping the game).

---

## Enabling & Connecting

For security, the MCP endpoint is **disabled by default** (`mcp_enabled = 0` in `grml.ini`).

### Option 1: Via Launcher Settings (GUI)
1. Open `GloriousRepublicModLoader.exe` and go to **Settings → Developer & Advanced**.
2. Toggle **Agent MCP Server (HTTP)** on (default port: `38420`, configurable via `mcp_port`).
3. Click **Copy Client Config** to copy the JSON snippet for your MCP client:
   ```json
   {
     "mcpServers": {
       "grml": {
         "url": "http://127.0.0.1:38420/mcp"
       }
     }
   }
   ```

### Option 2: Headless Mode (`--serve-only --mcp`)
For CI or terminal-driven agent workflows without opening the WebView2 window:
```powershell
.\GloriousRepublicModLoader.exe --serve-only --mcp --port 38420
```
Passing `--mcp` enables `/mcp` for that process without modifying `grml.ini`.

### Discovery & Security
- The HTTP server binds strictly to `127.0.0.1` (`INADDR_LOOPBACK`) and rejects non-localhost `Origin` headers with `403 Forbidden`.
- While MCP is active, `%LOCALAPPDATA%\GRML\grml_mcp.json` is written with `{"enabled": true, "port": <port>, "mcpUrl": "http://127.0.0.1:<port>/mcp", "pid": <pid>}` and removed on clean shutdown.
- When `mcp_enabled = 0` (and `--mcp` is not passed), `POST /mcp` returns `403 Forbidden`.

---

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/mcp` | JSON-RPC 2.0 requests (`initialize`, `ping`, `tools/list`, `tools/call`, `resources/list`, `resources/read`, notifications) |
| `GET` | `/mcp` | SSE (`text/event-stream`) keepalive stream |
| `DELETE` | `/mcp` | Close an active `Mcp-Session-Id` session |
| `GET` | `/api/mcp-info` | Returns `{ "enabled", "port", "mcpUrl" }` for the launcher UI |

---

## Available Tools (16)

### Mods, Playlists & Config
- `list_mods` — List discovered mods (`id`, `name`, `version`, `abi`, `enabled`, `readyStatus`, `dllPath`, `iniPath`, `hasUiSchema`, `missingOffsets`).
- `set_mods_enabled` — Enable or disable mod IDs in `grml.ini` `[plugins]` (comment-preserving).
- `list_playlists` — List saved playlists from `grml_playlists.json`.
- `apply_playlist` — Apply a named playlist to `grml.ini` `[plugins]`.
- `get_mod_ini` — Read a mod's `.ini` sections, key/value pairs, and `rawText` (by `modId` or `iniPath`).
- `set_mod_ini` — Update `[section] key=value` pairs (comment-preserving) or write `rawText`.
- `get_vfs_overrides` — Inspect Media VFS packs, order, and conflicts.
- `set_vfs_overrides` — Update `grml_vfs.ini` enabled state, pack order, and per-pack toggles.

### Saves, Direct-to-Save Launch & Test Loop
- `list_saves` — Scan WRSR save folders (including `dev_save_path` when enabled) with timestamps and missing Workshop IDs.
- `launch_game` — Launch `SOVIET64.exe` with `grml_runtime.dll` injected. Supports `saveName` (writes `auto_load_save` in `grml_session.json` for direct-to-save/test-scenario boot), `tempDisabledIds`, and `gameExe`.
- `stop_game` — Cleanly terminate running `SOVIET64.exe` process(es) and wait for exit.
- `get_game_status` — Return running `SOVIET64.exe` PIDs and `grml_session.json`.
- `wait_for_game_state` — Poll until `save_loaded` (optionally matching `expectedSave`), `process_running`, or `process_exited` (returns early on `[CrashGuard]` or unexpected exit).

### Diagnostics & Offsets
- `get_logs` — Tail `%LOCALAPPDATA%\GRML\grml.log` with optional `lines`, `severity`, and `query` filters.
- `get_diagnostics` — Return `CrashGuard` log hits and Windows WER EventID 1000 crash attribution.
- `get_offsets_status` — Return `grml_offsets.ini` resolution status (`OK` / `CACHED` / `MISSING`) for the detected build.

---

## Available Resources (3)

- `grml://state` (`application/json`) — Full discovery JSON (mods, game version, `grml.ini`, VFS state).
- `grml://logs/latest` (`text/plain`) — Latest 250 lines of `grml.log`.
- `grml://session` (`application/json`) — Current `grml_session.json`.

---

## Example Mod Developer Test Loop

1. `set_mod_ini` / `set_mods_enabled` — Configure the mod under test.
2. `launch_game({ "saveName": "my_test_scenario" })` — Boot `SOVIET64.exe` directly into the test save.
3. `wait_for_game_state({ "targetState": "save_loaded", "expectedSave": "my_test_scenario", "timeoutMs": 90000 })` — Wait until the save finishes loading (or catch an early crash).
4. `get_logs({ "query": "MyMod" })` / `get_diagnostics()` — Verify mod initialization and absence of errors.
5. `stop_game()` — Terminate `SOVIET64.exe` cleanly before the next build/test cycle.
