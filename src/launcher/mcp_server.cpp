#include "launcher/mcp_server.hpp"
#include "launcher/launcher_api.hpp"
#include "common/ini_util.hpp"
#include "common/json_min.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace grml::mcp {

namespace {

std::mutex                      g_sessionMutex;
std::unordered_set<std::string> g_sessions;
std::atomic<uint64_t>           g_sessionSeq{1};

std::string createSessionId()
{
    const uint64_t seq = g_sessionSeq.fetch_add(1, std::memory_order_relaxed);
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "grml-%lu-%llu-%llu",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(GetTickCount64()),
                  static_cast<unsigned long long>(seq));
    std::string id(buf);
    std::lock_guard lock(g_sessionMutex);
    g_sessions.insert(id);
    return id;
}

std::string formatJsonId(const grml::JsonValue* idVal)
{
    if (!idVal || idVal->isNull()) return "null";
    if (idVal->isString()) {
        return "\"" + grml::launcher_api::jsonEscape(idVal->asString()) + "\"";
    }
    if (idVal->isNumber()) {
        const double d = idVal->asNumber(0.0);
        const long long i = static_cast<long long>(d);
        if (static_cast<double>(i) == d) return std::to_string(i);
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%g", d);
        return buf;
    }
    return "null";
}

std::string jsonRpcOk(std::string_view idJson, std::string_view resultJson)
{
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + std::string(idJson) +
           ",\"result\":" + std::string(resultJson) + "}";
}

std::string jsonRpcError(std::string_view idJson, int code, std::string_view message)
{
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + std::string(idJson) +
           ",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":\"" + grml::launcher_api::jsonEscape(message) + "\"}}";
}

std::string toolTextResult(std::string_view text, bool isError = false)
{
    return std::string("{\"content\":[{\"type\":\"text\",\"text\":\"") +
           grml::launcher_api::jsonEscape(text) + "\"}],\"isError\":" +
           (isError ? "true" : "false") + "}";
}

std::string resolveModIniPath(const std::filesystem::path& loaderDir,
                              const std::string& modId,
                              const std::string& explicitIniPath)
{
    if (!explicitIniPath.empty()) return explicitIniPath;
    if (modId.empty()) return {};
    const std::string disc = grml::launcher_api::buildDiscoverJson(loaderDir, {});
    const auto root = grml::parseJson(disc);
    if (!root || !root->isObject()) return {};
    const auto* mods = root->get("mods");
    if (!mods || !mods->isArray()) return {};
    for (const auto& m : std::get<grml::JsonArray>(mods->data)) {
        if (!m.isObject()) continue;
        const auto* idV = m.get("id");
        if (idV && idV->asString() == modId) {
            if (const auto* iniV = m.get("iniPath")) return iniV->asString();
        }
    }
    return {};
}

std::string strArg(const grml::JsonValue* args, std::string_view key, std::string_view fallback = "")
{
    if (!args || !args->isObject()) return std::string(fallback);
    if (const auto* v = args->get(key)) return v->asString(fallback);
    return std::string(fallback);
}

int intArg(const grml::JsonValue* args, std::string_view key, int fallback = 0)
{
    if (!args || !args->isObject()) return fallback;
    if (const auto* v = args->get(key)) return v->asInt(fallback);
    return fallback;
}

bool boolArg(const grml::JsonValue* args, std::string_view key, bool fallback = false)
{
    if (!args || !args->isObject()) return fallback;
    if (const auto* v = args->get(key)) return v->asBool(fallback);
    return fallback;
}

std::string buildToolsListJson()
{
    return R"GRML_JSON({
  "tools": [
    {
      "name": "list_mods",
      "description": "List all discovered GRML/RML/Tesmio mods with enabled status, ABI, paths, UI schema availability, and offset readiness.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "gameExe": { "type": "string", "description": "Optional override path to SOVIET64.exe" }
        }
      }
    },
    {
      "name": "set_mods_enabled",
      "description": "Enable or disable one or more mod IDs in grml.ini [plugins] without altering comments.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "mods": {
            "type": "object",
            "additionalProperties": { "type": "boolean" },
            "description": "Map of modId to enabled boolean (e.g. {\"my_mod\": true})"
          }
        },
        "required": ["mods"]
      }
    },
    {
      "name": "list_playlists",
      "description": "List saved mod playlists from grml_playlists.json.",
      "inputSchema": { "type": "object", "properties": {} }
    },
    {
      "name": "apply_playlist",
      "description": "Apply a named playlist by enabling its enabledIds and disabling all other discovered mods in grml.ini.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "name": { "type": "string", "description": "Playlist name" }
        },
        "required": ["name"]
      }
    },
    {
      "name": "get_mod_ini",
      "description": "Read a mod's .ini configuration sections, key/value pairs, and raw text.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "modId": { "type": "string", "description": "Discovered mod ID" },
          "iniPath": { "type": "string", "description": "Direct path to .ini file (optional if modId is provided)" }
        }
      }
    },
    {
      "name": "set_mod_ini",
      "description": "Update key/value pairs in a mod's .ini file (comment-preserving) or write rawText.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "modId": { "type": "string", "description": "Discovered mod ID" },
          "iniPath": { "type": "string", "description": "Direct path to .ini file (optional if modId is provided)" },
          "updates": {
            "type": "array",
            "items": {
              "type": "object",
              "properties": {
                "section": { "type": "string" },
                "key": { "type": "string" },
                "value": { "type": "string" }
              },
              "required": ["key", "value"]
            }
          },
          "rawText": { "type": "string", "description": "Optional full raw INI text replacement" }
        }
      }
    },
    {
      "name": "get_vfs_overrides",
      "description": "Get Media VFS pack list, priority order, enabled state, and file conflicts.",
      "inputSchema": { "type": "object", "properties": {} }
    },
    {
      "name": "set_vfs_overrides",
      "description": "Update Media VFS enabled flag, pack priority order, and per-pack enabled map in grml_vfs.ini.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "enabled": { "type": "boolean" },
          "orderedIds": { "type": "array", "items": { "type": "string" } },
          "enabledMap": { "type": "object", "additionalProperties": { "type": "boolean" } }
        },
        "required": ["enabled", "orderedIds", "enabledMap"]
      }
    },
    {
      "name": "list_saves",
      "description": "Scan WRSR save folders (including dev_save_path when enabled) with timestamps and missing Workshop IDs.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "gameExe": { "type": "string", "description": "Optional override path to SOVIET64.exe" }
        }
      }
    },
    {
      "name": "launch_game",
      "description": "Launch SOVIET64.exe with grml_runtime.dll injected. Optionally auto-load a save/test scenario via grml_session.json and temporarily disable specific mods.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "saveName": { "type": "string", "description": "Save folder name to auto-load directly after boot (e.g. a test scenario save)" },
          "tempDisabledIds": { "type": "array", "items": { "type": "string" }, "description": "Mod IDs to temporarily disable for this launch only" },
          "gameExe": { "type": "string", "description": "Optional override path to SOVIET64.exe" }
        }
      }
    },
    {
      "name": "stop_game",
      "description": "Terminate running SOVIET64.exe process(es) and wait for exit.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "pid": { "type": "integer", "description": "Optional specific PID to terminate (0 = all SOVIET64.exe processes)" },
          "waitMs": { "type": "integer", "description": "Wait timeout in ms (default 5000)" }
        }
      }
    },
    {
      "name": "get_game_status",
      "description": "Check running SOVIET64.exe PIDs and current grml_session.json state (last_loaded_save, auto_load_save).",
      "inputSchema": { "type": "object", "properties": {} }
    },
    {
      "name": "wait_for_game_state",
      "description": "Poll until SOVIET64.exe reaches a target state ('save_loaded', 'process_running', or 'process_exited') or crashes/times out.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "targetState": { "type": "string", "enum": ["save_loaded", "process_running", "process_exited"], "description": "Target state to wait for (default: save_loaded)" },
          "expectedSave": { "type": "string", "description": "Optional save folder name expected for save_loaded" },
          "timeoutMs": { "type": "integer", "description": "Timeout in milliseconds (default 60000, max 300000)" }
        }
      }
    },
    {
      "name": "get_logs",
      "description": "Read the tail of %LOCALAPPDATA%\\GRML\\grml.log with optional severity and substring filters.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "lines": { "type": "integer", "description": "Max lines to return (default 200)" },
          "severity": { "type": "string", "description": "Optional severity filter (INFO, WARN, ERROR, FATAL)" },
          "query": { "type": "string", "description": "Optional case-insensitive substring filter" }
        }
      }
    },
    {
      "name": "get_diagnostics",
      "description": "Get CrashGuard log hits and Windows WER EventID 1000 crash attribution.",
      "inputSchema": { "type": "object", "properties": {} }
    },
    {
      "name": "get_offsets_status",
      "description": "Get grml_offsets.ini resolution status (OK/CACHED/MISSING) for the detected SOVIET64.exe build.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "gameExe": { "type": "string", "description": "Optional override path to SOVIET64.exe" }
        }
      }
    }
  ]
})GRML_JSON";
}

std::string handleToolCall(const std::filesystem::path& loaderDir,
                           const std::string& name,
                           const grml::JsonValue* args)
{
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);

    if (name == "list_mods") {
        const std::filesystem::path gameExe = grml::utf8ToWide(strArg(args, "gameExe"));
        const std::string disc = grml::launcher_api::buildDiscoverJson(loaderDir, gameExe);
        const auto root = grml::parseJson(disc);
        if (!root || !root->isObject()) return toolTextResult("Discovery failed.", true);
        const auto* mods = root->get("mods");
        if (!mods) return toolTextResult("{\"mods\":[]}");
        // Return compact discover mods slice directly from JSON
        const size_t pos = disc.find("\"mods\":");
        if (pos == std::string::npos) return toolTextResult(disc);
        return toolTextResult(disc);
    }

    if (name == "set_mods_enabled") {
        if (!args || !args->isObject() || !args->get("mods") || !args->get("mods")->isObject()) {
            return toolTextResult("Missing 'mods' object argument.", true);
        }
        std::string cfgBody = "{\"plugins\":{";
        size_t i = 0;
        for (const auto& [k, v] : std::get<grml::JsonObject>(args->get("mods")->data)) {
            if (i++ > 0) cfgBody += ',';
            cfgBody += "\"" + grml::launcher_api::jsonEscape(k) + "\":" + (v.asBool(false) ? "true" : "false");
        }
        cfgBody += "}}";
        std::string outJson;
        const bool ok = grml::launcher_api::saveConfigFromJson(loaderDir, cfgBody, outJson);
        return toolTextResult(outJson, !ok);
    }

    if (name == "list_playlists") {
        std::string raw;
        if (!grml::readFileBytesW(dataDir / L"grml_playlists.json", raw) || raw.empty()) {
            return toolTextResult("{\"playlists\":[]}");
        }
        return toolTextResult("{\"playlists\":" + raw + "}");
    }

    if (name == "apply_playlist") {
        const std::string targetName = strArg(args, "name");
        if (targetName.empty()) return toolTextResult("Missing 'name' argument.", true);
        std::string raw;
        if (!grml::readFileBytesW(dataDir / L"grml_playlists.json", raw) || raw.empty()) {
            return toolTextResult("No playlists file found.", true);
        }
        const auto plArr = grml::parseJson(raw);
        if (!plArr || !plArr->isArray()) return toolTextResult("Invalid playlists JSON.", true);

        std::unordered_set<std::string> enabledSet;
        bool found = false;
        for (const auto& item : std::get<grml::JsonArray>(plArr->data)) {
            if (!item.isObject()) continue;
            if (item.get("name") && item.get("name")->asString() == targetName) {
                found = true;
                if (const auto* ids = item.get("enabledIds")) {
                    for (const auto& id : ids->asStringArray()) enabledSet.insert(id);
                }
                break;
            }
        }
        if (!found) return toolTextResult("Playlist '" + targetName + "' not found.", true);

        const std::string disc = grml::launcher_api::buildDiscoverJson(loaderDir, {});
        const auto discRoot = grml::parseJson(disc);
        std::string cfgBody = "{\"plugins\":{";
        size_t i = 0;
        if (discRoot && discRoot->isObject() && discRoot->get("mods") && discRoot->get("mods")->isArray()) {
            for (const auto& m : std::get<grml::JsonArray>(discRoot->get("mods")->data)) {
                if (!m.isObject() || !m.get("id")) continue;
                const std::string id = m.get("id")->asString();
                if (id.empty()) continue;
                if (i++ > 0) cfgBody += ',';
                cfgBody += "\"" + grml::launcher_api::jsonEscape(id) + "\":" +
                           (enabledSet.count(id) ? "true" : "false");
            }
        }
        cfgBody += "}}";
        std::string outJson;
        const bool ok = grml::launcher_api::saveConfigFromJson(loaderDir, cfgBody, outJson);
        return toolTextResult(outJson, !ok);
    }

    if (name == "get_mod_ini") {
        const std::string iniPath = resolveModIniPath(loaderDir, strArg(args, "modId"), strArg(args, "iniPath"));
        if (iniPath.empty()) return toolTextResult("Could not resolve iniPath from modId or iniPath.", true);
        return toolTextResult(grml::launcher_api::readIniJson(grml::utf8ToWide(iniPath)));
    }

    if (name == "set_mod_ini") {
        const std::string iniPath = resolveModIniPath(loaderDir, strArg(args, "modId"), strArg(args, "iniPath"));
        if (iniPath.empty()) return toolTextResult("Could not resolve iniPath from modId or iniPath.", true);
        std::string body = "{\"iniPath\":\"" + grml::launcher_api::jsonEscape(iniPath) + "\"";
        if (args && args->get("rawText") && args->get("rawText")->isString()) {
            body += ",\"rawText\":\"" + grml::launcher_api::jsonEscape(args->get("rawText")->asString()) + "\"";
        } else if (args && args->get("updates") && args->get("updates")->isArray()) {
            body += ",\"updates\":[";
            size_t i = 0;
            for (const auto& u : std::get<grml::JsonArray>(args->get("updates")->data)) {
                if (!u.isObject()) continue;
                if (i++ > 0) body += ',';
                const std::string sec = u.get("section") ? u.get("section")->asString("settings") : "settings";
                const std::string key = u.get("key") ? u.get("key")->asString() : "";
                const std::string val = u.get("value") ? u.get("value")->asString() : "";
                body += "{\"section\":\"" + grml::launcher_api::jsonEscape(sec) +
                        "\",\"key\":\"" + grml::launcher_api::jsonEscape(key) +
                        "\",\"value\":\"" + grml::launcher_api::jsonEscape(val) + "\"}";
            }
            body += "]";
        } else {
            return toolTextResult("Provide either 'updates' array or 'rawText'.", true);
        }
        body += "}";
        std::string outJson;
        const bool ok = grml::launcher_api::saveIniFromJson(body, outJson);
        return toolTextResult(outJson, !ok);
    }

    if (name == "get_vfs_overrides") {
        const std::string disc = grml::launcher_api::buildDiscoverJson(loaderDir, {});
        return toolTextResult(disc);
    }

    if (name == "set_vfs_overrides") {
        const bool enabled = boolArg(args, "enabled", true);
        const auto orderedIds = (args && args->get("orderedIds"))
            ? args->get("orderedIds")->asStringArray()
            : std::vector<std::string>{};
        std::string body = "{\"enabled\":" + std::string(enabled ? "true" : "false") + ",\"orderedIds\":[";
        for (size_t i = 0; i < orderedIds.size(); ++i) {
            if (i > 0) body += ',';
            body += "\"" + grml::launcher_api::jsonEscape(orderedIds[i]) + "\"";
        }
        body += "],\"enabledMap\":{";
        if (args && args->get("enabledMap") && args->get("enabledMap")->isObject()) {
            size_t i = 0;
            for (const auto& [k, v] : std::get<grml::JsonObject>(args->get("enabledMap")->data)) {
                if (i++ > 0) body += ',';
                body += "\"" + grml::launcher_api::jsonEscape(k) + "\":" + (v.asBool(false) ? "true" : "false");
            }
        }
        body += "}}";
        std::string outJson;
        const bool ok = grml::launcher_api::saveVfsFromJson(loaderDir, body, outJson);
        return toolTextResult(outJson, !ok);
    }

    if (name == "list_saves") {
        const std::filesystem::path gameExe = grml::utf8ToWide(strArg(args, "gameExe"));
        return toolTextResult(grml::launcher_api::buildInspectSavesJson(loaderDir, gameExe));
    }

    if (name == "launch_game") {
        const std::string saveName = strArg(args, "saveName");
        const std::filesystem::path sessionPath = dataDir / L"grml_session.json";
        if (!saveName.empty()) {
            const std::string sessionJson =
                "{\n  \"last_loaded_save\": \"\",\n  \"auto_load_save\": \"" +
                grml::launcher_api::jsonEscape(saveName) + "\"\n}\n";
            grml::writeFileBytesW(sessionPath, sessionJson);
        } else {
            DeleteFileW(sessionPath.c_str());
        }
        const std::filesystem::path gameExe = grml::utf8ToWide(strArg(args, "gameExe"));
        const auto tempDisabled = (args && args->get("tempDisabledIds"))
            ? args->get("tempDisabledIds")->asStringArray()
            : std::vector<std::string>{};
        const auto lr = grml::launcher_api::launchGame(loaderDir, gameExe, tempDisabled);
        return toolTextResult(lr.json, !lr.ok);
    }

    if (name == "stop_game") {
        const uint32_t pid    = static_cast<uint32_t>((std::max)(0, intArg(args, "pid", 0)));
        const uint32_t waitMs = static_cast<uint32_t>((std::max)(100, intArg(args, "waitMs", 5000)));
        const auto sr = grml::launcher_api::stopGame(pid, waitMs);
        std::string out = "{\"ok\":" + std::string(sr.ok ? "true" : "false") + ",\"stoppedPids\":[";
        for (size_t i = 0; i < sr.stoppedPids.size(); ++i) {
            if (i > 0) out += ',';
            out += std::to_string(sr.stoppedPids[i]);
        }
        out += "],\"message\":\"" + grml::launcher_api::jsonEscape(sr.message) + "\"}";
        return toolTextResult(out, !sr.ok);
    }

    if (name == "get_game_status") {
        const auto pids = grml::launcher_api::getRunningSovietPids();
        std::string sessRaw = "{}";
        grml::readFileBytesW(dataDir / L"grml_session.json", sessRaw);
        if (sessRaw.empty()) sessRaw = "{}";
        std::string out = "{\"running\":" + std::string(!pids.empty() ? "true" : "false") + ",\"pids\":[";
        for (size_t i = 0; i < pids.size(); ++i) {
            if (i > 0) out += ',';
            out += std::to_string(pids[i]);
        }
        out += "],\"session\":" + sessRaw + "}";
        return toolTextResult(out);
    }

    if (name == "wait_for_game_state") {
        const std::string targetState  = strArg(args, "targetState", "save_loaded");
        const std::string expectedSave = strArg(args, "expectedSave");
        const uint32_t timeoutMs       = static_cast<uint32_t>((std::max)(500, intArg(args, "timeoutMs", 60000)));
        const auto wr = grml::launcher_api::waitForGameState(loaderDir, targetState, expectedSave, timeoutMs);
        std::string out = "{\"ok\":" + std::string(wr.ok ? "true" : "false") +
                          ",\"timedOut\":" + std::string(wr.timedOut ? "true" : "false") +
                          ",\"crashed\":" + std::string(wr.crashed ? "true" : "false") +
                          ",\"state\":\"" + grml::launcher_api::jsonEscape(wr.state) + "\"" +
                          ",\"pid\":" + std::to_string(wr.pid) +
                          ",\"lastSave\":\"" + grml::launcher_api::jsonEscape(wr.lastSave) + "\"" +
                          ",\"details\":\"" + grml::launcher_api::jsonEscape(wr.details) + "\"}";
        return toolTextResult(out, !wr.ok);
    }

    if (name == "get_logs") {
        const size_t maxLines      = static_cast<size_t>((std::max)(1, intArg(args, "lines", 200)));
        const std::string severity = strArg(args, "severity");
        const std::string query    = strArg(args, "query");
        const auto lines = grml::launcher_api::readFilteredLogTail(dataDir, maxLines, severity, query);
        std::string out = "{\"count\":" + std::to_string(lines.size()) + ",\"lines\":[";
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) out += ',';
            out += "\"" + grml::launcher_api::jsonEscape(lines[i]) + "\"";
        }
        out += "]}";
        return toolTextResult(out);
    }

    if (name == "get_diagnostics") {
        const auto fatalLines = grml::launcher_api::readFilteredLogTail(dataDir, 20, "FATAL", "[CrashGuard]");
        const std::string werJson = grml::launcher_api::buildCrashEventsJson();
        std::string out = "{\"crashGuardHits\":[";
        for (size_t i = 0; i < fatalLines.size(); ++i) {
            if (i > 0) out += ',';
            out += "\"" + grml::launcher_api::jsonEscape(fatalLines[i]) + "\"";
        }
        out += "],\"werCrash\":" + werJson + "}";
        return toolTextResult(out);
    }

    if (name == "get_offsets_status") {
        const std::filesystem::path gameExe = grml::utf8ToWide(strArg(args, "gameExe"));
        return toolTextResult(grml::launcher_api::buildDiscoverJson(loaderDir, gameExe));
    }

    return toolTextResult("Unknown tool: " + name, true);
}

} // namespace

bool validateSession(std::string_view sessionId)
{
    if (sessionId.empty()) return false;
    std::lock_guard lock(g_sessionMutex);
    return g_sessions.count(std::string(sessionId)) > 0;
}

void closeSession(std::string_view sessionId)
{
    if (sessionId.empty()) return;
    std::lock_guard lock(g_sessionMutex);
    g_sessions.erase(std::string(sessionId));
}

HttpMcpResponse handlePost(const std::filesystem::path& loaderDir,
                           std::string_view /*sessionIdHeader*/,
                           std::string_view body)
{
    HttpMcpResponse res{};
    const auto reqOpt = grml::parseJson(body);
    if (!reqOpt || !reqOpt->isObject()) {
        res.statusCode = 400;
        res.statusText = "Bad Request";
        res.body       = jsonRpcError("null", -32700, "Parse error");
        return res;
    }

    const auto& req = *reqOpt;
    const auto* idVal = req.get("id");
    const bool isNotification = (idVal == nullptr);
    const std::string idJson = formatJsonId(idVal);
    const std::string method = req.get("method") ? req.get("method")->asString() : "";

    if (isNotification || method.rfind("notifications/", 0) == 0) {
        res.statusCode  = 202;
        res.statusText  = "Accepted";
        res.contentType = "text/plain; charset=utf-8";
        res.body        = "";
        return res;
    }

    if (method == "initialize") {
        res.sessionId = createSessionId();
        const std::string initResult =
            "{\"protocolVersion\":\"2025-03-26\","
            "\"capabilities\":{\"tools\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,\"listChanged\":false}},"
            "\"serverInfo\":{\"name\":\"grml-launcher\",\"version\":\"" +
            grml::launcher_api::jsonEscape(GRML_BUILD_VERSION) + "\"}}";
        res.body = jsonRpcOk(idJson, initResult);
        return res;
    }

    if (method == "ping") {
        res.body = jsonRpcOk(idJson, "{}");
        return res;
    }

    if (method == "tools/list") {
        res.body = jsonRpcOk(idJson, buildToolsListJson());
        return res;
    }

    if (method == "tools/call") {
        const auto* params = req.get("params");
        if (!params || !params->isObject() || !params->get("name")) {
            res.body = jsonRpcError(idJson, -32602, "Invalid params: missing tool name");
            return res;
        }
        const std::string toolName = params->get("name")->asString();
        const auto* args = params->get("arguments");
        const std::string toolRes = handleToolCall(loaderDir, toolName, args);
        res.body = jsonRpcOk(idJson, toolRes);
        return res;
    }

    if (method == "resources/list") {
        res.body = jsonRpcOk(idJson, R"GRML_JSON({
  "resources": [
    {"uri":"grml://state","name":"GRML Discovery State","mimeType":"application/json","description":"Discovered mods, game version, grml.ini, and VFS state"},
    {"uri":"grml://logs/latest","name":"GRML Log Tail","mimeType":"text/plain","description":"Latest 250 lines of grml.log"},
    {"uri":"grml://session","name":"GRML Session","mimeType":"application/json","description":"Current grml_session.json (last_loaded_save, auto_load_save, runtime diagnostics)"}
  ]
})GRML_JSON");
        return res;
    }

    if (method == "resources/read") {
        const auto* params = req.get("params");
        const std::string uri = (params && params->get("uri")) ? params->get("uri")->asString() : "";
        const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);

        if (uri == "grml://state") {
            const std::string disc = grml::launcher_api::buildDiscoverJson(loaderDir, {});
            res.body = jsonRpcOk(idJson, "{\"contents\":[{\"uri\":\"grml://state\",\"mimeType\":\"application/json\",\"text\":\"" +
                                                 grml::launcher_api::jsonEscape(disc) + "\"}]}");
            return res;
        }
        if (uri == "grml://logs/latest") {
            const auto lines = grml::launcher_api::readFilteredLogTail(dataDir, 250);
            std::string joined;
            for (const auto& l : lines) {
                joined += l;
                joined += '\n';
            }
            res.body = jsonRpcOk(idJson, "{\"contents\":[{\"uri\":\"grml://logs/latest\",\"mimeType\":\"text/plain\",\"text\":\"" +
                                                 grml::launcher_api::jsonEscape(joined) + "\"}]}");
            return res;
        }
        if (uri == "grml://session") {
            std::string sess = "{}";
            grml::readFileBytesW(dataDir / L"grml_session.json", sess);
            if (sess.empty()) sess = "{}";
            res.body = jsonRpcOk(idJson, "{\"contents\":[{\"uri\":\"grml://session\",\"mimeType\":\"application/json\",\"text\":\"" +
                                                 grml::launcher_api::jsonEscape(sess) + "\"}]}");
            return res;
        }
        res.body = jsonRpcError(idJson, -32002, "Resource not found: " + uri);
        return res;
    }

    res.body = jsonRpcError(idJson, -32601, "Method not found: " + method);
    return res;
}

} // namespace grml::mcp
