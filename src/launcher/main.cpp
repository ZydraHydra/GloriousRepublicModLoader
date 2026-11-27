// ============================================================================
// main.cpp — grml_injector.exe CLI engine for Deno Desktop Launcher & scripts
// ============================================================================
// Commands:
//   grml_injector.exe --discover [--game <path>]   Emit discovery + VFS JSON
//   grml_injector.exe --launch   [--game <path>]   Launch SOVIET64.exe + inject GRML
//   grml_injector.exe --status                     Emit running SOVIET64.exe PIDs JSON
//   grml_injector.exe --crash-events               Emit latest WER crash event JSON
//   grml_injector.exe --save-vfs                   Save grml_vfs.ini from stdin JSON
//   grml_injector.exe --save-config                Save grml.ini from stdin JSON
//   grml_injector.exe --read-ini --ini <path>      Read INI sections + rawText JSON
//   grml_injector.exe --save-ini                   Update/write INI from stdin JSON
//   grml_injector.exe --generate-schema            Generate .ui.yaml/.ui.json from stdin JSON
//   grml_injector.exe                              Same as --launch
// ============================================================================
#include <windows.h>
#include <tlhelp32.h>
#include <winevt.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/ini_util.hpp"
#include "common/json_min.hpp"
#include "common/manifest.hpp"
#include "common/pe_scanner.hpp"
#include "common/ui_schema.hpp"
#include "common/vfs_index.hpp"
#include "launcher/injector.hpp"

namespace {

std::filesystem::path getExeDir()
{
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, _countof(buf));
    if (len == 0 || len >= _countof(buf)) return {};
    return std::filesystem::path(buf, buf + len).parent_path();
}

std::string readStdinUtf8()
{
    std::string out;
    char buf[4096];
    for (;;) {
        const size_t n = std::fread(buf, 1, sizeof(buf), stdin);
        if (n == 0) break;
        out.append(buf, n);
    }
    return out;
}

std::string jsonEsc(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

std::string jsonStrArray(const std::vector<std::string>& arr)
{
    std::string out = "[";
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) out += ',';
        out += "\"" + jsonEsc(arr[i]) + "\"";
    }
    out += "]";
    return out;
}

std::string serializeGrmlIniJson(const grml::IniFile& cfg)
{
    std::string out = "{";
    const auto secs = cfg.sectionOrder();
    for (size_t i = 0; i < secs.size(); ++i) {
        if (i > 0) out += ',';
        out += "\"" + jsonEsc(secs[i]) + "\":{";
        if (const auto* kv = cfg.sectionKeys(secs[i])) {
            size_t j = 0;
            for (const auto& [k, v] : *kv) {
                if (j++ > 0) out += ',';
                out += "\"" + jsonEsc(k) + "\":\"" + jsonEsc(v) + "\"";
            }
        }
        out += "}";
    }
    out += "}";
    return out;
}

int runDiscover(const std::filesystem::path& loaderDir, const std::filesystem::path& gameOverride)
{
    grml::IniFile cfg;
    cfg.loadFromFile(loaderDir / L"grml.ini");

    const std::filesystem::path cfgGame =
        !gameOverride.empty() ? gameOverride : std::filesystem::path(grml::utf8ToWide(cfg.getString("general", "game_path", "")));
    const std::filesystem::path gameExe = grml::discoverWrsrGameExe(loaderDir, cfgGame);

    std::string gameSha;
    grml::PeModuleInfo peInfo{};
    if (!gameExe.empty()) {
        gameSha = grml::computeFileSha256(gameExe);
        HANDLE hf = CreateFileW(gameExe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            HANDLE hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
            if (hm) {
                void* view = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
                if (view) {
                    grml::inspectLoadedPe(static_cast<uint8_t*>(view), peInfo);
                    UnmapViewOfFile(view);
                }
                CloseHandle(hm);
            }
            CloseHandle(hf);
        }
    }

    grml::IniFile offsets;
    offsets.loadFromFile(loaderDir / L"grml_offsets.ini");
    const std::string gameVer = gameSha.empty() ? "unknown" : offsets.getString("builds", gameSha, "unknown");

    grml::DiscoveryResult disc = grml::discoverAllPlugins(loaderDir, gameExe, cfg);
    std::vector<std::string> topoWarn;
    auto ordered = grml::resolveLoadOrder(disc.plugins, topoWarn);

    std::vector<grml::DiscoveredPlugin> combined;
    combined.reserve(disc.plugins.size());
    for (const auto& o : ordered) combined.push_back(o);
    for (const auto& d : disc.plugins) {
        if (!d.enabled) combined.push_back(d);
    }

    // Build Asset Overrides (Media VFS) index using the exact same C++ engine
    auto& vfs = grml::vfs::VfsIndex::get();
    vfs.build(loaderDir, gameExe.empty() ? std::filesystem::path{} : gameExe.parent_path(), true);

    std::printf("{\n");
    std::printf("  \"loaderVersion\": \"%s\",\n", jsonEsc(GRML_BUILD_VERSION).c_str());
    std::printf("  \"loaderDir\": \"%s\",\n", jsonEsc(grml::wideToUtf8(loaderDir.wstring())).c_str());
    std::printf("  \"gameExe\": \"%s\",\n", jsonEsc(grml::wideToUtf8(gameExe.wstring())).c_str());
    std::printf("  \"gameVersion\": \"%s\",\n", jsonEsc(gameVer).c_str());
    std::printf("  \"gameSha256\": \"%s\",\n", jsonEsc(gameSha).c_str());
    std::printf("  \"peTimestamp\": \"0x%08X\",\n", peInfo.timeDateStamp);
    std::printf("  \"peSizeOfImage\": \"0x%zX\",\n", peInfo.imageSize);
    std::printf("  \"warnings\": %s,\n", jsonStrArray(topoWarn).c_str());
    std::printf("  \"grmlIni\": %s,\n", serializeGrmlIniJson(cfg).c_str());
    std::printf("  \"vfs\": %s,\n", vfs.serializeJson().c_str());
    std::printf("  \"mods\": [\n");

    uint32_t orderNum = 1;
    for (size_t i = 0; i < combined.size(); ++i) {
        const auto& p       = combined[i];
        const auto  schema  = grml::discoverModUiSchema(p.dllPath, p.manifestPath, p.name);
        const auto  iniPath = grml::findModIniPath(p.dllPath, p.manifestPath, &schema);
        const uint32_t ord  = (p.enabled && !p.blockedByDependencyOrConflict) ? orderNum++ : 0;

        std::printf("    {\n");
        std::printf("      \"id\": \"%s\",\n", jsonEsc(p.id).c_str());
        std::printf("      \"name\": \"%s\",\n", jsonEsc(p.name).c_str());
        std::printf("      \"version\": \"%s\",\n", jsonEsc(p.version).c_str());
        std::printf("      \"author\": \"%s\",\n", jsonEsc(p.author).c_str());
        std::printf("      \"description\": \"%s\",\n", jsonEsc(p.description).c_str());
        std::printf("      \"abi\": \"%s\",\n", jsonEsc(grml::loaderKindDisplayName(p)).c_str());
        std::printf("      \"workshopId\": \"%s\",\n", jsonEsc(p.workshopId).c_str());
        std::printf("      \"dllPath\": \"%s\",\n", jsonEsc(grml::wideToUtf8(p.dllPath.wstring())).c_str());
        std::printf("      \"manifestPath\": \"%s\",\n", jsonEsc(grml::wideToUtf8(p.manifestPath.wstring())).c_str());
        std::printf("      \"iniPath\": \"%s\",\n", jsonEsc(grml::wideToUtf8(iniPath.wstring())).c_str());
        std::printf("      \"enabled\": %s,\n", p.enabled ? "true" : "false");
        std::printf("      \"blocked\": %s,\n", p.blockedByDependencyOrConflict ? "true" : "false");
        std::printf("      \"loadOrder\": %u,\n", ord);
        std::printf("      \"defaultPriority\": %d,\n", p.defaultPriority);
        std::printf("      \"warning\": \"%s\",\n", jsonEsc(p.warning).c_str());
        std::printf("      \"dependencies\": %s,\n", jsonStrArray(p.dependencies).c_str());
        std::printf("      \"conflictsWith\": %s,\n", jsonStrArray(p.conflictsWith).c_str());
        std::printf("      \"loadAfter\": %s,\n", jsonStrArray(p.loadAfter).c_str());
        std::printf("      \"uiSchema\": %s\n", grml::serializeUiSchemaJson(schema).c_str());
        std::printf("    }%s\n", (i + 1 < combined.size()) ? "," : "");
    }
    std::printf("  ]\n");
    std::printf("}\n");
    return 0;
}

int runLaunch(const std::filesystem::path& loaderDir, const std::filesystem::path& gameOverride)
{
    grml::IniFile cfg;
    cfg.loadFromFile(loaderDir / L"grml.ini");
    const std::filesystem::path cfgGame =
        !gameOverride.empty() ? gameOverride : std::filesystem::path(grml::utf8ToWide(cfg.getString("general", "game_path", "")));
    const std::filesystem::path gameExe = grml::discoverWrsrGameExe(loaderDir, cfgGame);

    const auto res = grml::launcher::launchAndInjectWrsr(loaderDir, gameExe);
    std::printf("{\"ok\":%s,\"processId\":%u,\"message\":\"%s\"}\n",
                res.ok ? "true" : "false",
                res.processId,
                jsonEsc(res.message).c_str());
    return res.ok ? 0 : 1;
}

int runStatus()
{
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"SOVIET64.exe") == 0) {
                    pids.push_back(pe.th32ProcessID);
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    std::printf("{\"pids\":[");
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i > 0) std::printf(",");
        std::printf("%lu", pids[i]);
    }
    std::printf("]}\n");
    return 0;
}

std::string extractXmlNthData(const std::string& xml, size_t targetIdx)
{
    size_t pos = 0;
    size_t idx = 0;
    while ((pos = xml.find("<Data>", pos)) != std::string::npos) {
        const size_t start = pos + 6;
        const size_t end   = xml.find("</Data>", start);
        if (end == std::string::npos) break;
        if (idx == targetIdx) {
            return xml.substr(start, end - start);
        }
        ++idx;
        pos = end + 7;
    }
    return {};
}

int runCrashEvents()
{
    EVT_HANDLE hQuery = EvtQuery(
        nullptr,
        L"Application",
        L"*[System[(EventID=1000) and TimeCreated[timediff(@SystemTime) <= 600000]]]",
        EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!hQuery) {
        std::printf("{\"found\":false}\n");
        return 0;
    }

    EVT_HANDLE events[8]{};
    DWORD returned = 0;
    if (EvtNext(hQuery, 8, events, 2000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            DWORD needed = 0;
            DWORD props  = 0;
            EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &needed, &props);
            if (needed > 0 && needed < 65536) {
                std::wstring xmlW(needed / sizeof(wchar_t) + 1, L'\0');
                if (EvtRender(nullptr, events[i], EvtRenderEventXml, needed, xmlW.data(), &needed, &props)) {
                    const std::string xml     = grml::wideToUtf8(xmlW.c_str());
                    const std::string appName = extractXmlNthData(xml, 0);
                    if (_stricmp(appName.c_str(), "SOVIET64.exe") == 0) {
                        const std::string modName = extractXmlNthData(xml, 3);
                        const std::string excCode = extractXmlNthData(xml, 6);
                        const std::string offset  = extractXmlNthData(xml, 7);
                        const std::string modPath = extractXmlNthData(xml, 11);
                        std::printf(
                            "{\"found\":true,\"time\":\"recent\",\"faultingModuleName\":\"%s\","
                            "\"faultingModulePath\":\"%s\",\"exceptionCode\":\"0x%s\",\"faultOffset\":\"0x%s\"}\n",
                            jsonEsc(modName).c_str(),
                            jsonEsc(modPath).c_str(),
                            jsonEsc(excCode).c_str(),
                            jsonEsc(offset).c_str());
                        for (DWORD j = i; j < returned; ++j) EvtClose(events[j]);
                        EvtClose(hQuery);
                        return 0;
                    }
                }
            }
            EvtClose(events[i]);
        }
    }
    EvtClose(hQuery);
    std::printf("{\"found\":false}\n");
    return 0;
}

int runSaveConfig(const std::filesystem::path& loaderDir)
{
    const auto root = grml::parseJson(readStdinUtf8());
    if (!root || !root->isObject()) {
        std::printf("{\"ok\":false,\"error\":\"Invalid JSON\"}\n");
        return 1;
    }
    std::vector<grml::IniKeyUpdate> updates;
    if (const auto* gen = root->get("general"); gen && gen->isObject()) {
        for (const auto& [k, v] : std::get<grml::JsonObject>(gen->data)) {
            updates.push_back({"general", k, v.asString()});
        }
    }
    if (const auto* plugs = root->get("plugins"); plugs && plugs->isObject()) {
        for (const auto& [k, v] : std::get<grml::JsonObject>(plugs->data)) {
            updates.push_back({"plugins", k, v.asBool(false) ? "1" : "0"});
        }
    }
    const bool ok = grml::updateIniFilePreservingComments(loaderDir / L"grml.ini", updates);
    std::printf("{\"ok\":%s}\n", ok ? "true" : "false");
    return ok ? 0 : 1;
}

int runSaveVfs(const std::filesystem::path& loaderDir)
{
    const auto root = grml::parseJson(readStdinUtf8());
    if (!root || !root->isObject()) {
        std::printf("{\"ok\":false,\"error\":\"Invalid JSON\"}\n");
        return 1;
    }
    const bool enabled      = root->get("enabled") ? root->get("enabled")->asBool(true) : true;
    const auto orderedIds   = root->get("orderedIds") ? root->get("orderedIds")->asStringArray() : std::vector<std::string>{};
    std::unordered_map<std::string, bool> enabledMap;
    if (const auto* em = root->get("enabledMap"); em && em->isObject()) {
        for (const auto& [k, v] : std::get<grml::JsonObject>(em->data)) {
            enabledMap[k] = v.asBool(false);
        }
    }
    const bool ok = grml::vfs::VfsIndex::saveConfig(loaderDir, enabled, orderedIds, enabledMap);
    std::printf("{\"ok\":%s}\n", ok ? "true" : "false");
    return ok ? 0 : 1;
}

int runReadIni(const std::filesystem::path& iniPath)
{
    if (iniPath.empty()) {
        std::printf("{\"exists\":false,\"sections\":[],\"rawText\":\"\"}\n");
        return 0;
    }
    std::string raw;
    if (!grml::readFileBytesW(iniPath, raw)) {
        std::printf("{\"exists\":false,\"iniPath\":\"%s\",\"sections\":[],\"rawText\":\"\"}\n",
                    jsonEsc(grml::wideToUtf8(iniPath.wstring())).c_str());
        return 0;
    }
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }

    const auto schema = grml::buildUiSchemaFromIniFile(iniPath);
    struct SecOut { std::string name; std::vector<std::pair<std::string, std::string>> entries; };
    std::vector<SecOut> secs;
    for (const auto& c : schema.controls) {
        if (c.type == grml::UiControlType::Header) {
            secs.push_back({c.section, {}});
        } else {
            if (secs.empty() || secs.back().name != c.section) {
                secs.push_back({c.section, {}});
            }
            secs.back().entries.emplace_back(c.key, c.defaultValue);
        }
    }

    std::printf("{\"exists\":true,\"iniPath\":\"%s\",\"sections\":[",
                jsonEsc(grml::wideToUtf8(iniPath.wstring())).c_str());
    for (size_t i = 0; i < secs.size(); ++i) {
        if (i > 0) std::printf(",");
        std::printf("{\"name\":\"%s\",\"entries\":[", jsonEsc(secs[i].name).c_str());
        for (size_t j = 0; j < secs[i].entries.size(); ++j) {
            if (j > 0) std::printf(",");
            std::printf("{\"key\":\"%s\",\"value\":\"%s\"}",
                        jsonEsc(secs[i].entries[j].first).c_str(),
                        jsonEsc(secs[i].entries[j].second).c_str());
        }
        std::printf("]}");
    }
    std::printf("],\"rawText\":\"%s\"}\n", jsonEsc(raw).c_str());
    return 0;
}

int runSaveIni()
{
    const auto root = grml::parseJson(readStdinUtf8());
    if (!root || !root->isObject() || !root->get("iniPath")) {
        std::printf("{\"ok\":false,\"error\":\"Missing iniPath\"}\n");
        return 1;
    }
    const std::filesystem::path iniPath = grml::utf8ToWide(root->get("iniPath")->asString());
    if (const auto* raw = root->get("rawText"); raw && raw->isString()) {
        std::string txt = raw->asString();
        if (txt.size() >= 3 &&
            static_cast<unsigned char>(txt[0]) == 0xEF &&
            static_cast<unsigned char>(txt[1]) == 0xBB &&
            static_cast<unsigned char>(txt[2]) == 0xBF) {
            txt.erase(0, 3);
        }
        const bool ok = grml::writeFileBytesW(iniPath, txt);
        std::printf("{\"ok\":%s}\n", ok ? "true" : "false");
        return ok ? 0 : 1;
    }

    std::vector<grml::IniKeyUpdate> updates;
    if (const auto* arr = root->get("updates"); arr && arr->isArray()) {
        for (const auto& item : std::get<grml::JsonArray>(arr->data)) {
            if (!item.isObject()) continue;
            const auto* sec = item.get("section");
            const auto* key = item.get("key");
            const auto* val = item.get("value");
            if (key) {
                updates.push_back({
                    sec ? sec->asString("settings") : std::string("settings"),
                    key->asString(),
                    val ? val->asString() : std::string(),
                });
            }
        }
    }
    const bool ok = grml::updateIniFilePreservingComments(iniPath, updates);
    std::printf("{\"ok\":%s}\n", ok ? "true" : "false");
    return ok ? 0 : 1;
}

int runGenerateSchema()
{
    const auto root = grml::parseJson(readStdinUtf8());
    if (!root || !root->isObject() || !root->get("dllPath") || !root->get("iniPath")) {
        std::printf("{\"ok\":false,\"error\":\"Missing dllPath or iniPath\"}\n");
        return 1;
    }
    const std::filesystem::path dllPath = grml::utf8ToWide(root->get("dllPath")->asString());
    const std::filesystem::path iniPath = grml::utf8ToWide(root->get("iniPath")->asString());
    const std::string format = root->get("format") ? root->get("format")->asString("yaml") : "yaml";

    auto schema = grml::buildUiSchemaFromIniFile(iniPath);
    if (!schema.valid) {
        schema.tabTitle          = grml::wideToUtf8(dllPath.stem().wstring());
        schema.iniFile           = grml::wideToUtf8(iniPath.filename().wstring());
        schema.mainMenuAvailable = true;
        schema.controls.push_back({grml::UiControlType::Header, "settings", "", "[settings]", "", "", 0.0, 1.0, 0.0, false, "", {}});
        schema.controls.push_back({grml::UiControlType::Bool, "settings", "enabled", "Enabled", "", "", 0.0, 1.0, 0.0, false, "1", {}});
    }

    const std::filesystem::path stemPath = dllPath.parent_path() / dllPath.stem();
    if (format == "json") {
        schema.autoGenerated = false;
        schema.valid         = true;
        const std::filesystem::path outPath(stemPath.wstring() + L".ui.json");
        const bool ok = grml::writeFileBytesW(outPath, grml::serializeUiSchemaJson(schema) + "\n");
        std::printf("{\"ok\":%s,\"outPath\":\"%s\"}\n", ok ? "true" : "false",
                    jsonEsc(grml::wideToUtf8(outPath.wstring())).c_str());
        return ok ? 0 : 1;
    }

    auto yamlEscape = [](std::string_view s) {
        std::string out;
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\n' || c == '\r') out += ' ';
            else out += c;
        }
        return out;
    };

    std::string y;
    y += "tabTitle: \"" + yamlEscape(schema.tabTitle) + "\"\n";
    y += "iniFile: \"" + yamlEscape(schema.iniFile) + "\"\n";
    y += "mainMenuAvailable: true\ncontrols:\n";
    for (const auto& c : schema.controls) {
        const char* tname = "string";
        switch (c.type) {
        case grml::UiControlType::Header: tname = "header"; break;
        case grml::UiControlType::Bool:   tname = "bool"; break;
        case grml::UiControlType::Int:    tname = "int"; break;
        case grml::UiControlType::Float:  tname = "float"; break;
        case grml::UiControlType::Select: tname = "select"; break;
        case grml::UiControlType::String: tname = "string"; break;
        }
        y += "  - type: " + std::string(tname) + "\n";
        if (c.type == grml::UiControlType::Header) {
            y += "    label: \"" + yamlEscape(c.label) + "\"\n";
            continue;
        }
        y += "    section: \"" + yamlEscape(c.section) + "\"\n";
        y += "    key: \"" + yamlEscape(c.key) + "\"\n";
        y += "    label: \"" + yamlEscape(c.label) + "\"\n";
        if (!c.tooltip.empty()) y += "    tooltip: \"" + yamlEscape(c.tooltip) + "\"\n";
        if (c.type == grml::UiControlType::Int || c.type == grml::UiControlType::Float) {
            char nb[96]{};
            std::snprintf(nb, sizeof(nb), "    min: %g\n    max: %g\n", c.minValue, c.maxValue);
            y += nb;
            if (c.step > 0.0) {
                char sb[48]{};
                std::snprintf(sb, sizeof(sb), "    step: %g\n", c.step);
                y += sb;
            }
        }
        y += "    default: \"" + yamlEscape(c.defaultValue) + "\"\n";
    }

    const std::filesystem::path outPath(stemPath.wstring() + L".ui.yaml");
    const bool ok = grml::writeFileBytesW(outPath, y);
    std::printf("{\"ok\":%s,\"outPath\":\"%s\"}\n", ok ? "true" : "false",
                jsonEsc(grml::wideToUtf8(outPath.wstring())).c_str());
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fOut = nullptr;
            FILE* fErr = nullptr;
            freopen_s(&fOut, "CONOUT$", "w", stdout);
            freopen_s(&fErr, "CONOUT$", "w", stderr);
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    const std::filesystem::path loaderDir = getExeDir();

    enum class Mode { Launch, Discover, Status, CrashEvents, SaveVfs, SaveConfig, ReadIni, SaveIni, GenSchema };
    Mode mode = Mode::Launch;
    std::filesystem::path gameOverride;
    std::filesystem::path iniOverride;

    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--discover") == 0) mode = Mode::Discover;
        else if (std::wcscmp(argv[i], L"--status") == 0) mode = Mode::Status;
        else if (std::wcscmp(argv[i], L"--crash-events") == 0) mode = Mode::CrashEvents;
        else if (std::wcscmp(argv[i], L"--save-vfs") == 0) mode = Mode::SaveVfs;
        else if (std::wcscmp(argv[i], L"--save-config") == 0) mode = Mode::SaveConfig;
        else if (std::wcscmp(argv[i], L"--read-ini") == 0) mode = Mode::ReadIni;
        else if (std::wcscmp(argv[i], L"--save-ini") == 0) mode = Mode::SaveIni;
        else if (std::wcscmp(argv[i], L"--generate-schema") == 0) mode = Mode::GenSchema;
        else if (std::wcscmp(argv[i], L"--game") == 0 && i + 1 < argc) gameOverride = argv[++i];
        else if (std::wcscmp(argv[i], L"--ini") == 0 && i + 1 < argc) iniOverride = argv[++i];
    }

    switch (mode) {
    case Mode::Discover:    return runDiscover(loaderDir, gameOverride);
    case Mode::Status:      return runStatus();
    case Mode::CrashEvents: return runCrashEvents();
    case Mode::SaveVfs:     return runSaveVfs(loaderDir);
    case Mode::SaveConfig:  return runSaveConfig(loaderDir);
    case Mode::ReadIni:     return runReadIni(iniOverride);
    case Mode::SaveIni:     return runSaveIni();
    case Mode::GenSchema:   return runGenerateSchema();
    case Mode::Launch:      return runLaunch(loaderDir, gameOverride);
    }
    return 1;
}
