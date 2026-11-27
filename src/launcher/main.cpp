// ============================================================================
// main.cpp — GloriousRepublicModLoader.exe native WebView2 host + CLI engine
// ============================================================================
// CLI Commands:
//   GloriousRepublicModLoader.exe --discover [--game <path>]   Emit discovery + VFS JSON
//   GloriousRepublicModLoader.exe --launch   [--game <path>]   Launch SOVIET64.exe + inject GRML
//   GloriousRepublicModLoader.exe --inspect-saves              Scan saves & Workshop diffs
//   GloriousRepublicModLoader.exe --status                     Emit running SOVIET64.exe PIDs JSON
//   GloriousRepublicModLoader.exe --crash-events               Emit latest WER crash event JSON
//   GloriousRepublicModLoader.exe --save-vfs                   Save grml_vfs.ini from stdin JSON
//   GloriousRepublicModLoader.exe --save-config                Save grml.ini from stdin JSON
//   GloriousRepublicModLoader.exe --read-ini --ini <path>      Read INI sections + rawText JSON
//   GloriousRepublicModLoader.exe --save-ini                   Update/write INI from stdin JSON
//   GloriousRepublicModLoader.exe --generate-schema            Generate .ui.yaml/.ui.json from stdin JSON
//   GloriousRepublicModLoader.exe (no args)                    Open WebView2 Launcher UI
// ============================================================================
#include <windows.h>
#include <tlhelp32.h>
#include <winevt.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/ini_util.hpp"
#include "common/json_min.hpp"
#include "common/manifest.hpp"
#include "common/offset_resolver.hpp"
#include "common/pe_scanner.hpp"
#include "common/ui_schema.hpp"
#include "common/vfs_index.hpp"
#include "embedded_grml_ini.hpp"
#include "embedded_grml_offsets.hpp"
#include "launcher/http_server.hpp"
#include "launcher/injector.hpp"
#include "launcher/launcher_api.hpp"
#include "launcher/webview_host.hpp"

#include <shellapi.h>

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

std::string buildDiscoverJsonStr(const std::filesystem::path& loaderDir, const std::filesystem::path& gameOverride)
{
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);
    const std::filesystem::path grmlIniPath = dataDir / L"grml.ini";
    grml::syncDefaultIniKeys(grmlIniPath, grml::generated::kEmbeddedDefaultGrmlIni);
    grml::IniFile cfg;
    cfg.loadFromFile(grmlIniPath);

    const std::filesystem::path cfgGame =
        !gameOverride.empty() ? gameOverride : std::filesystem::path(grml::utf8ToWide(cfg.getString("general", "game_path", "")));
    const std::filesystem::path gameExe = grml::discoverWrsrGameExe(loaderDir, cfgGame);

    std::string gameSha;
    std::string peFileVer;
    grml::PeModuleInfo peInfo{};
    HANDLE hfMapped = INVALID_HANDLE_VALUE;
    HANDLE hmMapped = nullptr;
    void*  mappedView = nullptr;
    if (!gameExe.empty()) {
        gameSha   = grml::computeFileSha256(gameExe);
        peFileVer = grml::readPeFileVersion(gameExe);
        hfMapped = CreateFileW(gameExe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hfMapped != INVALID_HANDLE_VALUE) {
            hmMapped = CreateFileMappingW(hfMapped, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
            if (hmMapped) {
                mappedView = MapViewOfFile(hmMapped, FILE_MAP_READ, 0, 0, 0);
                if (mappedView) {
                    grml::inspectLoadedPe(static_cast<uint8_t*>(mappedView), peInfo);
                }
            }
        }
    }

    grml::IniFile offsets;
    uint64_t offsetsHash = 0;
    grml::offsets::loadMergedOffsetsIni(loaderDir, dataDir, grml::generated::kEmbeddedGrmlOffsetsIni,
                                        offsets, offsetsHash);
    std::string gameVer = gameSha.empty() ? "unknown" : offsets.getString("builds", gameSha, "unknown");
    if (gameVer == "unknown" && !peFileVer.empty()) {
        gameVer = peFileVer;
    }

    // Collect offset definitions & resolve via grml_session.json (live runtime) or PE cache/.text scan
    const auto offsetDefs = grml::offsets::collectOffsetDefinitions(offsets, gameVer == "unknown" ? std::string_view{} : gameVer);
    std::vector<grml::offsets::ResolvedOffsetEntry> resolvedOffsets;
    bool loadedFromSession = false;
    {
        std::string sessRaw;
        if (grml::readFileBytesW(dataDir / L"grml_session.json", sessRaw)) {
            const auto parsed = grml::parseJson(sessRaw);
            if (parsed && parsed->isObject()) {
                char expectedTs[32]{}, expectedImg[32]{}, expectedEp[32]{}, expectedHash[32]{};
                std::snprintf(expectedTs, sizeof(expectedTs), "0x%08X", peInfo.timeDateStamp);
                std::snprintf(expectedImg, sizeof(expectedImg), "0x%X", static_cast<unsigned>(peInfo.imageSize));
                std::snprintf(expectedEp, sizeof(expectedEp), "0x%X", peInfo.addressOfEntryPoint);
                std::snprintf(expectedHash, sizeof(expectedHash), "0x%016llX", static_cast<unsigned long long>(offsetsHash));

                const auto* tsV   = parsed->get("pe_timestamp");
                const auto* imgV  = parsed->get("pe_size_of_image");
                const auto* epV   = parsed->get("pe_entry_point");
                const auto* hashV = parsed->get("offsets_fnv1a64");
                const auto* arrV  = parsed->get("offsets");
                if (arrV && arrV->isArray() &&
                    tsV && tsV->asString() == expectedTs &&
                    imgV && imgV->asString() == expectedImg &&
                    epV && epV->asString() == expectedEp &&
                    hashV && hashV->asString() == expectedHash) {
                    const auto& arr = std::get<grml::JsonArray>(arrV->data);
                    for (const auto& d : offsetDefs) {
                        grml::offsets::ResolvedOffsetEntry re;
                        re.def = d;
                        for (const auto& item : arr) {
                            if (!item.isObject()) continue;
                            const auto* nm = item.get("name");
                            if (nm && nm->asString() == d.name) {
                                const std::string st = item.get("status") ? item.get("status")->asString() : "";
                                if (st == "OK") re.status = grml::offsets::OffsetStatus::Ok;
                                else if (st == "CACHED") re.status = grml::offsets::OffsetStatus::Cached;
                                else if (st == "MISSING") re.status = grml::offsets::OffsetStatus::Missing;
                                re.matchCount = static_cast<size_t>(item.get("match_count") ? item.get("match_count")->asInt(0) : 0);
                                const std::string rvaS = item.get("rva") ? item.get("rva")->asString() : "";
                                const std::string resS = item.get("resolved") ? item.get("resolved")->asString() : "";
                                if (!rvaS.empty()) re.matchedRva = static_cast<uint32_t>(std::strtoul(rvaS.c_str(), nullptr, 0));
                                if (!resS.empty()) re.resolvedValue = static_cast<uintptr_t>(std::strtoull(resS.c_str(), nullptr, 0));
                                break;
                            }
                        }
                        resolvedOffsets.push_back(std::move(re));
                    }
                    loadedFromSession = true;
                }
            }
        }
    }
    if (!loadedFromSession) {
        if (peInfo.base && peInfo.textBase && peInfo.textSize > 0) {
            const auto cachePath = dataDir / L"grml_offset_cache.ini";
            const bool allCached = grml::offsets::tryResolveFromCache(
                cachePath, peInfo, offsetsHash, offsetDefs, resolvedOffsets);
            if (!allCached) {
                std::vector<void*> sites(resolvedOffsets.size(), nullptr);
                std::vector<grml::AobPatternRequest> reqs;
                std::vector<size_t> reqIndices;
                for (size_t i = 0; i < resolvedOffsets.size(); ++i) {
                    if (resolvedOffsets[i].status != grml::offsets::OffsetStatus::Cached) {
                        reqs.push_back({resolvedOffsets[i].def.name,
                                        resolvedOffsets[i].def.pattern,
                                        &sites[i],
                                        resolvedOffsets[i].def.required});
                        reqIndices.push_back(i);
                    }
                }
                if (!reqs.empty()) {
                    grml::runSinglePassAobScan(peInfo.textBase, peInfo.textSize, reqs);
                    for (size_t k = 0; k < reqs.size(); ++k) {
                        const size_t idx = reqIndices[k];
                        auto& entry = resolvedOffsets[idx];
                        entry.matchCount = reqs[k].matchCount;
                        const auto* site = static_cast<const uint8_t*>(sites[idx]);
                        uintptr_t val = 0;
                        if (reqs[k].matchCount == 1 && site &&
                            grml::offsets::applyOffsetResolveRule(peInfo, site, entry.def, val)) {
                            entry.status        = grml::offsets::OffsetStatus::Ok;
                            entry.matchedRva    = static_cast<uint32_t>(site - peInfo.base);
                            // Store RVA-based value when mapped from SEC_IMAGE (convert VA -> RVA for pointer modes)
                            entry.resolvedValue = (entry.def.resolveMode == grml::offsets::OffsetResolveMode::Disp32)
                                ? val
                                : static_cast<uintptr_t>(val - reinterpret_cast<uintptr_t>(peInfo.base));
                        } else {
                            entry.status = grml::offsets::OffsetStatus::Missing;
                        }
                    }
                    grml::offsets::writeOffsetCache(cachePath, peInfo, offsetsHash, resolvedOffsets);
                }
            } else {
                for (auto& entry : resolvedOffsets) {
                    if (entry.status == grml::offsets::OffsetStatus::Cached &&
                        entry.def.resolveMode != grml::offsets::OffsetResolveMode::Disp32) {
                        entry.resolvedValue -= reinterpret_cast<uintptr_t>(peInfo.base);
                    }
                }
            }
        } else {
            for (const auto& d : offsetDefs) {
                grml::offsets::ResolvedOffsetEntry re;
                re.def = d;
                re.status = grml::offsets::OffsetStatus::Unscanned;
                resolvedOffsets.push_back(std::move(re));
            }
        }
    }
    if (mappedView) UnmapViewOfFile(mappedView);
    if (hmMapped) CloseHandle(hmMapped);
    if (hfMapped != INVALID_HANDLE_VALUE) CloseHandle(hfMapped);

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
    const grml::EffectiveGamePaths effPaths = grml::resolveEffectiveGamePaths(gameExe, cfg);
    auto& vfs = grml::vfs::VfsIndex::get();
    vfs.build(dataDir, gameExe.empty() ? std::filesystem::path{} : gameExe.parent_path(), true,
              effPaths.workshopRoot, effPaths.overridesRoot);

    std::string out;
    out.reserve(65536);
    auto addFmt = [&](const char* fmt, auto&&... args) {
        char buf[4096];
        const int n = std::snprintf(buf, sizeof(buf), fmt, args...);
        if (n > 0) out.append(buf, static_cast<size_t>(std::min<int>(n, sizeof(buf) - 1)));
    };

    const std::string detectedGameLang = gameExe.empty()
        ? std::string{}
        : grml::detectWrsrGameLanguage(gameExe.parent_path());

    out += "{\n";
    out += "  \"loaderVersion\": \"" + jsonEsc(GRML_BUILD_VERSION) + "\",\n";
    out += "  \"loaderDir\": \"" + jsonEsc(grml::wideToUtf8(loaderDir.wstring())) + "\",\n";
    out += "  \"dataDir\": \"" + jsonEsc(grml::wideToUtf8(dataDir.wstring())) + "\",\n";
    out += "  \"gameExe\": \"" + jsonEsc(grml::wideToUtf8(gameExe.wstring())) + "\",\n";
    out += "  \"gameVersion\": \"" + jsonEsc(gameVer) + "\",\n";
    out += "  \"gameLanguage\": \"" + jsonEsc(detectedGameLang) + "\",\n";
    out += "  \"peFileVersion\": \"" + jsonEsc(peFileVer) + "\",\n";
    out += "  \"gameSha256\": \"" + jsonEsc(gameSha) + "\",\n";
    addFmt("  \"peTimestamp\": \"0x%08X\",\n", peInfo.timeDateStamp);
    addFmt("  \"peSizeOfImage\": \"0x%zX\",\n", peInfo.imageSize);
    addFmt("  \"peEntryPoint\": \"0x%X\",\n", peInfo.addressOfEntryPoint);
    addFmt("  \"offsetsHash\": \"0x%016llX\",\n", static_cast<unsigned long long>(offsetsHash));
    out += "  \"offsets\": [\n";
    for (size_t i = 0; i < resolvedOffsets.size(); ++i) {
        const auto& e = resolvedOffsets[i];
        char rvaBuf[32]{}, resBuf[32]{};
        if (e.status == grml::offsets::OffsetStatus::Ok || e.status == grml::offsets::OffsetStatus::Cached) {
            std::snprintf(rvaBuf, sizeof(rvaBuf), "0x%X", e.matchedRva);
            std::snprintf(resBuf, sizeof(resBuf), "0x%llX", static_cast<unsigned long long>(e.resolvedValue));
        }
        out += "    {";
        out += "\"name\":\"" + jsonEsc(e.def.name) + "\",";
        out += "\"status\":\"" + std::string(grml::offsets::offsetStatusToString(e.status)) + "\",";
        out += "\"matchCount\":" + std::to_string(e.matchCount) + ",";
        out += "\"resolve\":\"" + std::string(grml::offsets::resolveModeToString(e.def.resolveMode)) + "\",";
        out += "\"category\":\"" + jsonEsc(e.def.category) + "\",";
        out += "\"verifiedBuild\":\"" + jsonEsc(e.def.verifiedBuild) + "\",";
        out += "\"anchorXref\":\"" + jsonEsc(e.def.anchorXref) + "\",";
        out += "\"required\":" + std::string(e.def.required ? "true" : "false") + ",";
        out += "\"rva\":\"" + std::string(rvaBuf) + "\",";
        out += "\"resolved\":\"" + std::string(resBuf) + "\"";
        out += std::string("}") + ((i + 1 < resolvedOffsets.size()) ? ",\n" : "\n");
    }
    out += "  ],\n";
    const grml::SteamDefaultPaths steamDefs = grml::discoverSteamDefaultPaths();
    wchar_t devEnvBuf[16]{};
    const DWORD devEnvLen = GetEnvironmentVariableW(L"GRML_DEV", devEnvBuf, _countof(devEnvBuf));
    const bool showDevSettings =
        (devEnvLen > 0 && (std::wcscmp(devEnvBuf, L"1") == 0 || _wcsicmp(devEnvBuf, L"true") == 0)) ||
        cfg.getBool("general", "show_dev_settings", false);
    out += std::string("  \"showDevSettings\": ") + (showDevSettings ? "true" : "false") + ",\n";
    out += "  \"steamDefaults\": {\n";
    out += "    \"saveRoot\": \"" + jsonEsc(grml::wideToUtf8(steamDefs.saveRoot.wstring())) + "\",\n";
    out += "    \"workshopRoot\": \"" + jsonEsc(grml::wideToUtf8(steamDefs.workshopRoot.wstring())) + "\",\n";
    out += "    \"workshopWipRoot\": \"" + jsonEsc(grml::wideToUtf8(steamDefs.workshopWipRoot.wstring())) + "\",\n";
    out += "    \"overridesRoot\": \"" + jsonEsc(grml::wideToUtf8(steamDefs.overridesRoot.wstring())) + "\"\n";
    out += "  },\n";
    out += "  \"warnings\": " + jsonStrArray(topoWarn) + ",\n";
    out += "  \"grmlIni\": " + serializeGrmlIniJson(cfg) + ",\n";
    out += "  \"vfs\": " + vfs.serializeJson() + ",\n";
    out += "  \"mods\": [\n";

    std::string activeLang = cfg.getString("general", "language", "auto");
    if (activeLang.empty() || _stricmp(activeLang.c_str(), "auto") == 0) {
        activeLang = detectedGameLang.empty() ? "en" : detectedGameLang;
    }

    uint32_t orderNum = 1;
    for (size_t i = 0; i < combined.size(); ++i) {
        const auto& p       = combined[i];
        auto        schema  = grml::discoverModUiSchema(p.dllPath, p.manifestPath, p.name);
        grml::applyModUiTranslations(schema, p.dllPath, activeLang);
        const auto  iniPath = grml::findModIniPath(p.dllPath, p.manifestPath, &schema);
        const uint32_t ord  = (p.enabled && !p.blockedByDependencyOrConflict) ? orderNum++ : 0;

        out += "    {\n";
        out += "      \"id\": \"" + jsonEsc(p.id) + "\",\n";
        out += "      \"name\": \"" + jsonEsc(p.name) + "\",\n";
        out += "      \"version\": \"" + jsonEsc(p.version) + "\",\n";
        out += "      \"author\": \"" + jsonEsc(p.author) + "\",\n";
        out += "      \"description\": \"" + jsonEsc(p.description) + "\",\n";
        out += "      \"abi\": \"" + jsonEsc(grml::loaderKindDisplayName(p)) + "\",\n";
        out += "      \"workshopId\": \"" + jsonEsc(p.workshopId) + "\",\n";
        out += "      \"dllPath\": \"" + jsonEsc(grml::wideToUtf8(p.dllPath.wstring())) + "\",\n";
        out += "      \"manifestPath\": \"" + jsonEsc(grml::wideToUtf8(p.manifestPath.wstring())) + "\",\n";
        out += "      \"imagePath\": \"" + jsonEsc(grml::wideToUtf8(p.imagePath.wstring())) + "\",\n";
        out += "      \"iniPath\": \"" + jsonEsc(grml::wideToUtf8(iniPath.wstring())) + "\",\n";
        out += "      \"enabled\": " + std::string(p.enabled ? "true" : "false") + ",\n";
        out += "      \"blocked\": " + std::string(p.blockedByDependencyOrConflict ? "true" : "false") + ",\n";
        addFmt("      \"loadOrder\": %u,\n", ord);
        addFmt("      \"defaultPriority\": %d,\n", p.defaultPriority);
        out += "      \"warning\": \"" + jsonEsc(p.warning) + "\",\n";
        out += "      \"dependencies\": " + jsonStrArray(p.dependencies) + ",\n";
        out += "      \"conflictsWith\": " + jsonStrArray(p.conflictsWith) + ",\n";
        out += "      \"loadAfter\": " + jsonStrArray(p.loadAfter) + ",\n";
        std::vector<std::string> missingReq;
        std::vector<std::string> missingOpt;
        auto isOffsetOk = [&](const std::string& offName) -> bool {
            std::string want = offName;
            for (char& c : want) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            }
            for (const auto& re : resolvedOffsets) {
                if (re.def.name == want) {
                    return re.status == grml::offsets::OffsetStatus::Ok ||
                           re.status == grml::offsets::OffsetStatus::Cached;
                }
            }
            return false;
        };
        for (const auto& ro : p.requiredOffsets) {
            if (!isOffsetOk(ro)) missingReq.push_back(ro);
        }
        for (const auto& oo : p.optionalOffsets) {
            if (!isOffsetOk(oo)) missingOpt.push_back(oo);
        }
        std::string offsetCompat = "legacy";
        if (p.hasGrmlManifest || !p.requiredOffsets.empty() || !p.optionalOffsets.empty()) {
            if (!missingReq.empty()) offsetCompat = "needs_update";
            else if (!missingOpt.empty()) offsetCompat = "degraded";
            else offsetCompat = "ready";
        }

        out += "      \"supportedGameVersions\": " + jsonStrArray(p.supportedGameVersions) + ",\n";
        out += "      \"gameVersionMismatch\": " + std::string(grml::isGameVersionSupported(p, gameVer) ? "false" : "true") + ",\n";
        out += "      \"hasGrmlManifest\": " + std::string(p.hasGrmlManifest ? "true" : "false") + ",\n";
        out += "      \"requiredOffsets\": " + jsonStrArray(p.requiredOffsets) + ",\n";
        out += "      \"optionalOffsets\": " + jsonStrArray(p.optionalOffsets) + ",\n";
        out += "      \"missingRequiredOffsets\": " + jsonStrArray(missingReq) + ",\n";
        out += "      \"missingOptionalOffsets\": " + jsonStrArray(missingOpt) + ",\n";
        out += "      \"offsetCompatibility\": \"" + offsetCompat + "\",\n";
        out += "      \"uiSchema\": " + grml::serializeUiSchemaJson(schema) + "\n";
        out += std::string("    }") + ((i + 1 < combined.size()) ? ",\n" : "\n");
    }
    out += "  ]\n}\n";
    return out;
}

int runLaunch(const std::filesystem::path& loaderDir, const std::filesystem::path& gameOverride)
{
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);
    grml::IniFile cfg;
    cfg.loadFromFile(dataDir / L"grml.ini");
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
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);
    const bool ok = grml::updateIniFilePreservingComments(dataDir / L"grml.ini", updates);
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
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);
    const bool ok = grml::vfs::VfsIndex::saveConfig(dataDir, enabled, orderedIds, enabledMap);
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

bool isAllAsciiDigits(std::string_view s)
{
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

struct HeaderInspection {
    uint32_t                 headerVersion{0};
    std::string              displayName;
    std::vector<std::string> workshopIds;
    std::unordered_map<std::string, std::vector<std::string>> assetsByWorkshopId;
};

HeaderInspection inspectHeaderBin(const std::filesystem::path& headerPath)
{
    HeaderInspection out;
    HANDLE hf = CreateFileW(headerPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return out;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hf, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 8LL * 1024LL * 1024LL) {
        CloseHandle(hf);
        return out;
    }

    std::string buf(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(hf, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr);
    CloseHandle(hf);
    if (!ok || bytesRead == 0) return out;
    buf.resize(bytesRead);

    if (buf.size() >= 6) {
        std::memcpy(&out.headerVersion, buf.data(), sizeof(uint32_t));
        std::wstring nameW;
        for (size_t off = 4; off + 1 < buf.size() && off < 260; off += 2) {
            wchar_t wc = static_cast<unsigned char>(buf[off]) |
                         (static_cast<unsigned char>(buf[off + 1]) << 8);
            if (wc == 0) break;
            if (wc < 32) {
                nameW.clear();
                break;
            }
            nameW.push_back(wc);
        }
        if (!nameW.empty()) {
            out.displayName = grml::wideToUtf8(nameW);
        }
    }

    std::set<std::string> ids;
    const size_t n = buf.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c < '0' || c > '9') continue;
        if (i > 0) {
            const unsigned char prev = static_cast<unsigned char>(buf[i - 1]);
            if ((prev >= '0' && prev <= '9') || prev == '/' || prev == '\\' || prev == '.' || prev == '-') {
                continue;
            }
        }
        size_t j = i;
        while (j < n && buf[j] >= '0' && buf[j] <= '9' && (j - i) <= 13) {
            ++j;
        }
        const size_t len = j - i;
        if (len >= 8 && len <= 12 && j + 1 < n && buf[j] == '/') {
            const unsigned char next = static_cast<unsigned char>(buf[j + 1]);
            const bool validNext = (next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
                                   (next >= '0' && next <= '9') || next == '_';
            if (validNext) {
                const std::string wsId = buf.substr(i, len);
                ids.insert(wsId);
                size_t aEnd = j + 1;
                while (aEnd < n && static_cast<unsigned char>(buf[aEnd]) >= 32 &&
                       static_cast<unsigned char>(buf[aEnd]) < 127 && (aEnd - (j + 1)) < 96) {
                    ++aEnd;
                }
                std::string asset = buf.substr(j + 1, aEnd - (j + 1));
                if (!asset.empty()) {
                    auto& vec = out.assetsByWorkshopId[wsId];
                    if (std::find(vec.begin(), vec.end(), asset) == vec.end() && vec.size() < 8) {
                        vec.push_back(std::move(asset));
                    }
                }
            }
        }
        i = j;
    }
    out.workshopIds.assign(ids.begin(), ids.end());
    return out;
}

std::string readWorkshopItemTypeLabel(const std::filesystem::path& itemDir)
{
    std::string raw;
    if (!grml::readFileBytesW(itemDir / L"workshopconfig.ini", raw)) return {};
    const size_t pos = raw.find("$ITEM_TYPE");
    if (pos == std::string::npos) return {};
    size_t valStart = pos + 10;
    while (valStart < raw.size() && (raw[valStart] == ' ' || raw[valStart] == '\t')) ++valStart;
    size_t valEnd = valStart;
    while (valEnd < raw.size() && raw[valEnd] != '\r' && raw[valEnd] != '\n') ++valEnd;
    std::string val = raw.substr(valStart, valEnd - valStart);
    constexpr std::string_view kPrefix = "WORKSHOP_ITEMTYPE_";
    if (val.rfind(kPrefix, 0) == 0) {
        val = val.substr(kPrefix.size());
    }
    for (size_t i = 0; i < val.size(); ++i) {
        if (val[i] == '_') val[i] = ' ';
        else if (i > 0 && val[i - 1] != ' ' && val[i] >= 'A' && val[i] <= 'Z') {
            val[i] = static_cast<char>(val[i] + ('a' - 'A'));
        }
    }
    return val;
}

struct SaveStatsMeta {
    int         year{0};
    int         day{0};
    int         population{0};
    double      avgLifespan{0.0};
    std::string originalMapName;
};

SaveStatsMeta readStatsIniTail(const std::filesystem::path& statsPath)
{
    SaveStatsMeta out;
    HANDLE hf = CreateFileW(statsPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return out;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hf, &sz) || sz.QuadPart <= 0) {
        CloseHandle(hf);
        return out;
    }

    constexpr LONGLONG kTailBytes = 262144;
    const LONGLONG offset = (sz.QuadPart > kTailBytes) ? (sz.QuadPart - kTailBytes) : 0;
    LARGE_INTEGER liOff{};
    liOff.QuadPart = offset;
    SetFilePointerEx(hf, liOff, nullptr, FILE_BEGIN);

    const DWORD toRead = static_cast<DWORD>(sz.QuadPart - offset);
    std::string tail(toRead, '\0');
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(hf, tail.data(), toRead, &bytesRead, nullptr);
    CloseHandle(hf);
    if (!ok || bytesRead == 0) return out;
    tail.resize(bytesRead);

    auto extractStr = [](std::string_view src, std::string_view key) -> std::string {
        size_t searchFrom = 0;
        while (searchFrom < src.size()) {
            const size_t pos = src.find(key, searchFrom);
            if (pos == std::string_view::npos) return {};
            const size_t after = pos + key.size();
            if (after < src.size() && (src[after] == ' ' || src[after] == '\t')) {
                size_t s = after;
                while (s < src.size() && (src[s] == ' ' || src[s] == '\t')) ++s;
                size_t e = s;
                while (e < src.size() && src[e] != '\r' && src[e] != '\n') ++e;
                while (e > s && (src[e - 1] == ' ' || src[e - 1] == '\t')) --e;
                return std::string(src.substr(s, e - s));
            }
            searchFrom = after;
        }
        return {};
    };

    out.originalMapName = extractStr(tail, "$OriginalMapName");

    const size_t recPos = tail.rfind("$STAT_RECORD");
    if (recPos == std::string::npos) return out;
    const size_t curPos = tail.find("$STAT_CURRENT", recPos);
    const std::string_view rec = (curPos != std::string::npos)
        ? std::string_view(tail).substr(recPos, curPos - recPos)
        : std::string_view(tail).substr(recPos);

    auto extractInt = [&](std::string_view key) -> int {
        const std::string v = extractStr(rec, key);
        return v.empty() ? 0 : std::atoi(v.c_str());
    };
    auto extractDouble = [&](std::string_view key) -> double {
        const std::string v = extractStr(rec, key);
        return v.empty() ? 0.0 : std::atof(v.c_str());
    };

    out.year        = extractInt("$DATE_YEAR");
    out.day         = extractInt("$DATE_DAY");
    out.population  = extractInt("$Citizens_Adults") + extractInt("$Citizens_SmallChilds") + extractInt("$Citizens_MediumChilds");
    out.avgLifespan = extractDouble("$Citizens_AverageLifespan");
    return out;
}

void readScriptIniMeta(const std::filesystem::path& scriptPath, std::string& worldSize)
{
    std::string raw;
    if (!grml::readFileBytesW(scriptPath, raw)) return;
    const size_t pos = raw.find("$WORLDSIZE");
    if (pos == std::string::npos) return;
    size_t s = pos + 10;
    while (s < raw.size() && (raw[s] == ' ' || raw[s] == '\t')) ++s;
    size_t e = s;
    while (e < raw.size() && raw[e] != '\r' && raw[e] != '\n') ++e;
    double wx = 0, wy = 0;
    if (std::sscanf(raw.substr(s, e - s).c_str(), "%lf %lf", &wx, &wy) == 2 && wx > 0 && wy > 0) {
        char buf[48]{};
        std::snprintf(buf, sizeof(buf), "%.0f x %.0f km", wx / 1000.0, wy / 1000.0);
        worldSize = buf;
    }
}

std::string buildInspectSavesJsonStr(const std::filesystem::path& loaderDir, const std::filesystem::path& gameOverride, bool& outOk)
{
    outOk = false;
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);
    grml::IniFile cfg;
    cfg.loadFromFile(dataDir / L"grml.ini");
    const std::filesystem::path cfgGame =
        !gameOverride.empty() ? gameOverride : std::filesystem::path(grml::utf8ToWide(cfg.getString("general", "game_path", "")));
    const std::filesystem::path gameExe = grml::discoverWrsrGameExe(loaderDir, cfgGame);
    if (gameExe.empty()) {
        return "{\"ok\":false,\"error\":\"WRSR executable not found.\",\"saves\":[]}\n";
    }
    outOk = true;

    const grml::EffectiveGamePaths effPaths = grml::resolveEffectiveGamePaths(gameExe, cfg);
    const std::filesystem::path gameDir      = gameExe.parent_path();
    const std::filesystem::path saveRoot     = effPaths.saveRoot;
    const std::filesystem::path workshopRoot = effPaths.workshopRoot;

    struct InstalledWorkshopInfo {
        std::string itemType;
    };
    std::unordered_map<std::string, InstalledWorkshopInfo> installedIds;
    std::error_code ec;
    if (std::filesystem::is_directory(workshopRoot, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(workshopRoot, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::string name = grml::wideToUtf8(entry.path().filename().wstring());
            if (isAllAsciiDigits(name)) {
                installedIds[name] = {};
            }
        }
    }
    const std::filesystem::path extraRoots[] = {
        effPaths.workshopWipRoot,
        loaderDir / L"overrides",
        effPaths.overridesRoot,
        gameDir / L"override",
    };
    for (const auto& er : extraRoots) {
        if (!std::filesystem::is_directory(er, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(er, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::string name = grml::wideToUtf8(entry.path().filename().wstring());
            if (isAllAsciiDigits(name)) {
                installedIds[name] = {};
            }
        }
    }

    struct InstalledModRef {
        std::string              id;
        std::string              itemType;
        std::vector<std::string> assets;
    };
    struct SaveInfo {
        std::string                  name;
        std::string                  displayName;
        std::string                  path;
        uint32_t                     headerVersion{0};
        uint64_t                     lastModifiedMs{0};
        std::string                  lastModifiedDisplay;
        uint64_t                     totalBytes{0};
        uint32_t                     fileCount{0};
        std::string                  worldSize;
        SaveStatsMeta                stats;
        std::vector<std::string>     errors;
        std::vector<std::string>     referencedIds;
        std::vector<std::string>     missingIds;
        std::vector<InstalledModRef> installedMods;
    };
    std::vector<SaveInfo> saves;

    if (std::filesystem::is_directory(saveRoot, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(saveRoot, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::filesystem::path headerPath = entry.path() / L"header.bin";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!GetFileAttributesExW(headerPath.c_str(), GetFileExInfoStandard, &fad)) continue;

            ULARGE_INTEGER ft{};
            ft.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
            ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            const uint64_t epochMs = (ft.QuadPart >= 116444736000000000ULL)
                ? (ft.QuadPart - 116444736000000000ULL) / 10000ULL
                : 0ULL;

            FILETIME localFt{};
            SYSTEMTIME st{};
            char tsBuf[32]{};
            if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &localFt) &&
                FileTimeToSystemTime(&localFt, &st)) {
                std::snprintf(tsBuf, sizeof(tsBuf), "%04u-%02u-%02u %02u:%02u:%02u",
                              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            }

            SaveInfo info;
            info.name                = grml::wideToUtf8(entry.path().filename().wstring());
            info.path                = grml::wideToUtf8(entry.path().wstring());
            info.lastModifiedMs      = epochMs;
            info.lastModifiedDisplay = tsBuf;

            for (const auto& f : std::filesystem::directory_iterator(entry.path(), ec)) {
                if (f.is_regular_file(ec)) {
                    ++info.fileCount;
                    const auto fsz = f.file_size(ec);
                    if (!ec) info.totalBytes += fsz;
                }
            }
            if (!std::filesystem::exists(entry.path() / L"buildings.bin", ec)) {
                info.errors.push_back("Missing buildings.bin");
            }
            if (fad.nFileSizeLow < 16 && fad.nFileSizeHigh == 0) {
                info.errors.push_back("header.bin appears truncated");
            }
            readScriptIniMeta(entry.path() / L"script.ini", info.worldSize);
            info.stats = readStatsIniTail(entry.path() / L"stats.ini");

            HeaderInspection hi = inspectHeaderBin(headerPath);
            info.headerVersion  = hi.headerVersion;
            info.displayName    = std::move(hi.displayName);
            info.referencedIds  = std::move(hi.workshopIds);
            for (const auto& id : info.referencedIds) {
                auto it = installedIds.find(id);
                if (it == installedIds.end()) {
                    info.missingIds.push_back(id);
                } else {
                    if (it->second.itemType.empty()) {
                        it->second.itemType = readWorkshopItemTypeLabel(workshopRoot / grml::utf8ToWide(id));
                    }
                    InstalledModRef im;
                    im.id       = id;
                    im.itemType = it->second.itemType;
                    auto ait    = hi.assetsByWorkshopId.find(id);
                    if (ait != hi.assetsByWorkshopId.end()) im.assets = ait->second;
                    info.installedMods.push_back(std::move(im));
                }
            }
            saves.push_back(std::move(info));
        }
    }

    std::sort(saves.begin(), saves.end(), [](const SaveInfo& a, const SaveInfo& b) {
        if (a.lastModifiedMs != b.lastModifiedMs) return a.lastModifiedMs > b.lastModifiedMs;
        return a.name < b.name;
    });

    std::string out;
    out.reserve(32768);
    auto addFmt = [&](const char* fmt, auto&&... args) {
        char buf[1024];
        const int n = std::snprintf(buf, sizeof(buf), fmt, args...);
        if (n > 0) out.append(buf, static_cast<size_t>(std::min<int>(n, sizeof(buf) - 1)));
    };

    out += "{\n";
    out += "  \"ok\": true,\n";
    out += "  \"saveRoot\": \"" + jsonEsc(grml::wideToUtf8(saveRoot.wstring())) + "\",\n";
    out += "  \"workshopRoot\": \"" + jsonEsc(grml::wideToUtf8(workshopRoot.wstring())) + "\",\n";
    addFmt("  \"installedWorkshopCount\": %zu,\n", installedIds.size());
    out += "  \"saves\": [\n";
    for (size_t i = 0; i < saves.size(); ++i) {
        const auto& s = saves[i];
        std::string status;
        if (!s.errors.empty()) {
            status = "Save Error";
        } else if (s.missingIds.empty()) {
            status = "Ready";
        } else {
            status = "Missing " + std::to_string(s.missingIds.size()) +
                     (s.missingIds.size() == 1 ? " Workshop Item" : " Workshop Items");
        }
        out += "    {\n";
        out += "      \"name\": \"" + jsonEsc(s.name) + "\",\n";
        out += "      \"displayName\": \"" + jsonEsc(s.displayName) + "\",\n";
        out += "      \"path\": \"" + jsonEsc(s.path) + "\",\n";
        addFmt("      \"headerVersion\": %u,\n", s.headerVersion);
        addFmt("      \"lastModifiedMs\": %llu,\n", static_cast<unsigned long long>(s.lastModifiedMs));
        out += "      \"lastModifiedDisplay\": \"" + jsonEsc(s.lastModifiedDisplay) + "\",\n";
        addFmt("      \"totalBytes\": %llu,\n", static_cast<unsigned long long>(s.totalBytes));
        addFmt("      \"fileCount\": %u,\n", s.fileCount);
        out += "      \"worldSize\": \"" + jsonEsc(s.worldSize) + "\",\n";
        addFmt("      \"inGameYear\": %d,\n", s.stats.year);
        addFmt("      \"inGameDay\": %d,\n", s.stats.day);
        addFmt("      \"population\": %d,\n", s.stats.population);
        addFmt("      \"avgLifespan\": %.1f,\n", s.stats.avgLifespan);
        out += "      \"originalMapName\": \"" + jsonEsc(s.stats.originalMapName) + "\",\n";
        out += "      \"errors\": " + jsonStrArray(s.errors) + ",\n";
        out += "      \"referencedWorkshopIds\": " + jsonStrArray(s.referencedIds) + ",\n";
        out += "      \"missingWorkshopIds\": " + jsonStrArray(s.missingIds) + ",\n";
        out += "      \"installedMods\": [";
        for (size_t j = 0; j < s.installedMods.size(); ++j) {
            const auto& im = s.installedMods[j];
            if (j > 0) out += ",";
            out += "{\"id\":\"" + jsonEsc(im.id) + "\",\"itemType\":\"" + jsonEsc(im.itemType) +
                   "\",\"assets\":" + jsonStrArray(im.assets) + "}";
        }
        out += "],\n";
        out += "      \"status\": \"" + jsonEsc(status) + "\"\n";
        out += std::string("    }") + ((i + 1 < saves.size()) ? ",\n" : "\n");
    }
    out += "  ]\n}\n";
    return out;
}

std::string buildCrashEventsJsonStr()
{
    EVT_HANDLE hQuery = EvtQuery(
        nullptr,
        L"Application",
        L"*[System[(EventID=1000) and TimeCreated[timediff(@SystemTime) <= 600000]]]",
        EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!hQuery) return "{\"found\":false}\n";

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
                        for (DWORD j = i; j < returned; ++j) EvtClose(events[j]);
                        EvtClose(hQuery);
                        return "{\"found\":true,\"time\":\"recent\",\"faultingModuleName\":\"" + jsonEsc(modName) +
                               "\",\"faultingModulePath\":\"" + jsonEsc(modPath) +
                               "\",\"exceptionCode\":\"0x" + jsonEsc(excCode) +
                               "\",\"faultOffset\":\"0x" + jsonEsc(offset) + "\"}\n";
                    }
                }
            }
            EvtClose(events[i]);
        }
    }
    EvtClose(hQuery);
    return "{\"found\":false}\n";
}

} // namespace

namespace grml::launcher_api {

std::string jsonEscape(std::string_view s) { return ::jsonEsc(s); }

std::string buildDiscoverJson(const std::filesystem::path& loaderDir, const std::filesystem::path& gameOverride)
{
    return ::buildDiscoverJsonStr(loaderDir, gameOverride);
}

std::string buildInspectSavesJson(const std::filesystem::path& loaderDir, const std::filesystem::path& gameOverride)
{
    bool ok = false;
    return ::buildInspectSavesJsonStr(loaderDir, gameOverride, ok);
}

std::vector<uint32_t> getRunningSovietPids()
{
    std::vector<uint32_t> pids;
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
    return pids;
}

std::string buildStatusJson()
{
    const auto pids = getRunningSovietPids();
    std::string out = "{\"pids\":[";
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i > 0) out += ",";
        out += std::to_string(pids[i]);
    }
    out += "]}\n";
    return out;
}

std::string buildCrashEventsJson() { return ::buildCrashEventsJsonStr(); }

namespace {

struct BackupProgressState {
    bool        active{false};
    std::string phase{"idle"};
    size_t      completedFiles{0};
    size_t      totalFiles{0};
    uint64_t    copiedBytes{0};
    uint64_t    totalBytes{0};
    std::string currentItem;
};

std::mutex          g_backupProgressMutex;
BackupProgressState g_backupProgress;

void setBackupProgress(bool active,
                       const char* phase,
                       size_t completedFiles,
                       size_t totalFiles,
                       uint64_t copiedBytes,
                       uint64_t totalBytes,
                       std::string currentItem)
{
    std::lock_guard lock(g_backupProgressMutex);
    g_backupProgress.active         = active;
    g_backupProgress.phase          = phase ? phase : "idle";
    g_backupProgress.completedFiles = completedFiles;
    g_backupProgress.totalFiles     = totalFiles;
    g_backupProgress.copiedBytes    = copiedBytes;
    g_backupProgress.totalBytes     = totalBytes;
    g_backupProgress.currentItem    = std::move(currentItem);
}

bool isSubPathOrSame(const std::filesystem::path& candidate, const std::filesystem::path& base)
{
    std::error_code ec;
    const auto cNorm = std::filesystem::weakly_canonical(candidate, ec).lexically_normal();
    const auto bNorm = std::filesystem::weakly_canonical(base, ec).lexically_normal();
    if (cNorm.empty() || bNorm.empty()) return false;
    auto cIt = cNorm.begin();
    auto bIt = bNorm.begin();
    for (; bIt != bNorm.end(); ++bIt, ++cIt) {
        if (cIt == cNorm.end()) return false;
        if (_wcsicmp(cIt->c_str(), bIt->c_str()) != 0) return false;
    }
    return true;
}

std::wstring resolveBackupGameVersionDir(const std::filesystem::path& loaderDir,
                                         const std::filesystem::path& dataDir,
                                         const std::filesystem::path& gameExe)
{
    std::string ver;
    const std::string gameSha = grml::computeFileSha256(gameExe);
    if (!gameSha.empty()) {
        grml::IniFile offsets;
        uint64_t      offsetsHash = 0;
        grml::offsets::loadMergedOffsetsIni(loaderDir, dataDir, grml::generated::kEmbeddedGrmlOffsetsIni,
                                            offsets, offsetsHash);
        ver = offsets.getString("builds", gameSha, "");
    }
    if (ver.empty() || ver == "unknown") {
        const std::string peVer = grml::readPeFileVersion(gameExe);
        if (!peVer.empty()) ver = peVer;
    }
    if (ver.empty()) ver = "unknown";
    for (char& ch : ver) {
        if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    return grml::utf8ToWide(ver);
}

void backupSavesBeforeLaunch(const std::filesystem::path& loaderDir,
                             const std::filesystem::path& dataDir,
                             const std::filesystem::path& gameExe,
                             const grml::IniFile& cfg)
{
    if (!cfg.getBool("general", "auto_backup_saves", true)) return;
    if (gameExe.empty()) return;

    std::error_code ec;
    const std::filesystem::path gameDir  = gameExe.parent_path();
    const std::filesystem::path saveRoot = grml::resolveEffectiveGamePaths(gameDir, cfg).saveRoot;
    if (saveRoot.empty() || !std::filesystem::is_directory(saveRoot, ec)) return;

    const std::string customPath = cfg.getString("general", "save_backup_path", "");
    std::filesystem::path baseBackupRoot = customPath.empty()
        ? (dataDir / L"save_backups")
        : std::filesystem::path(grml::utf8ToWide(customPath));
    if (baseBackupRoot.empty()) baseBackupRoot = dataDir / L"save_backups";

    // Prevent recursive backup loops if baseBackupRoot is inside saveRoot
    if (isSubPathOrSame(baseBackupRoot, saveRoot)) return;

    const std::filesystem::path backupRoot =
        baseBackupRoot / resolveBackupGameVersionDir(loaderDir, dataDir, gameExe);

    struct BackupGuard {
        ~BackupGuard() { setBackupProgress(false, "done", 0, 0, 0, 0, ""); }
    } guard;

    setBackupProgress(true, "scanning", 0, 0, 0, 0, "");

    struct CopyItem {
        std::filesystem::path src;
        std::filesystem::path rel;
        uint64_t              size{0};
    };
    std::vector<std::filesystem::path> dirs;
    std::vector<CopyItem>              files;
    uint64_t                           totalBytes = 0;

    for (const auto& ent : std::filesystem::recursive_directory_iterator(saveRoot, ec)) {
        std::error_code relEc;
        const auto rel = std::filesystem::relative(ent.path(), saveRoot, relEc);
        if (relEc || rel.empty()) continue;
        if (ent.is_directory(ec)) {
            dirs.push_back(rel);
        } else if (ent.is_regular_file(ec)) {
            const uint64_t sz = ent.file_size(ec);
            totalBytes += sz;
            files.push_back({ent.path(), rel, sz});
        }
    }
    if (files.empty() && dirs.empty()) return;

    std::filesystem::create_directories(backupRoot, ec);
    if (ec) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stampBuf[64]{};
    swprintf_s(stampBuf, L"save_backup_%04u-%02u-%02u_%02u-%02u-%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::filesystem::path targetDir = backupRoot / stampBuf;
    for (int suffix = 1; std::filesystem::exists(targetDir, ec) && suffix < 100; ++suffix) {
        targetDir = backupRoot / (std::wstring(stampBuf) + L"_" + std::to_wstring(suffix));
    }

    std::filesystem::create_directories(targetDir, ec);
    if (ec) return;
    for (const auto& d : dirs) {
        std::filesystem::create_directories(targetDir / d, ec);
    }

    uint64_t copiedBytes    = 0;
    size_t   completedFiles = 0;
    for (const auto& item : files) {
        setBackupProgress(true, "copying", completedFiles, files.size(), copiedBytes, totalBytes,
                          grml::wideToUtf8(item.rel.wstring()));
        const auto dst = targetDir / item.rel;
        if (!dst.parent_path().empty()) {
            std::filesystem::create_directories(dst.parent_path(), ec);
        }
        std::filesystem::copy_file(item.src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::filesystem::remove_all(targetDir, ec);
            return;
        }
        copiedBytes += item.size;
        ++completedFiles;
    }

    setBackupProgress(true, "pruning", completedFiles, files.size(), copiedBytes, totalBytes, "");

    const int maxBackups = (std::max)(1, cfg.getInt("general", "max_save_backups", 3));
    std::vector<std::filesystem::path> backups;
    for (const auto& ent : std::filesystem::directory_iterator(backupRoot, ec)) {
        if (!ent.is_directory(ec)) continue;
        const std::wstring name = ent.path().filename().wstring();
        if (name.rfind(L"save_backup_", 0) == 0) {
            backups.push_back(ent.path());
        }
    }
    std::sort(backups.begin(), backups.end(), [](const auto& a, const auto& b) {
        return a.filename().wstring() < b.filename().wstring();
    });
    while (backups.size() > static_cast<size_t>(maxBackups)) {
        std::filesystem::remove_all(backups.front(), ec);
        backups.erase(backups.begin());
    }
}

} // namespace

std::string buildBackupProgressJson()
{
    std::lock_guard lock(g_backupProgressMutex);
    std::string out = "{";
    out += "\"active\":" + std::string(g_backupProgress.active ? "true" : "false") + ",";
    out += "\"phase\":\"" + ::jsonEsc(g_backupProgress.phase) + "\",";
    out += "\"completedFiles\":" + std::to_string(g_backupProgress.completedFiles) + ",";
    out += "\"totalFiles\":" + std::to_string(g_backupProgress.totalFiles) + ",";
    out += "\"copiedBytes\":" + std::to_string(g_backupProgress.copiedBytes) + ",";
    out += "\"totalBytes\":" + std::to_string(g_backupProgress.totalBytes) + ",";
    out += "\"currentItem\":\"" + ::jsonEsc(g_backupProgress.currentItem) + "\"";
    out += "}\n";
    return out;
}

LaunchResult launchGame(const std::filesystem::path& loaderDir,
                        const std::filesystem::path& gameOverride,
                        const std::vector<std::string>& tempDisabledIds)
{
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);
    grml::IniFile cfg;
    cfg.loadFromFile(dataDir / L"grml.ini");
    const std::filesystem::path cfgGame =
        !gameOverride.empty() ? gameOverride : std::filesystem::path(grml::utf8ToWide(cfg.getString("general", "game_path", "")));
    const std::filesystem::path gameExe = grml::discoverWrsrGameExe(loaderDir, cfgGame);

    backupSavesBeforeLaunch(loaderDir, dataDir, gameExe, cfg);

    const auto res = grml::launcher::launchAndInjectWrsr(loaderDir, gameExe, tempDisabledIds);
    LaunchResult lr;
    lr.ok        = res.ok;
    lr.processId = res.processId;
    lr.json      = std::string("{\"ok\":") + (res.ok ? "true" : "false") +
                   ",\"processId\":" + std::to_string(res.processId) +
                   ",\"message\":\"" + ::jsonEsc(res.message) + "\"}\n";
    return lr;
}

bool saveConfigFromJson(const std::filesystem::path& loaderDir, const std::string& body, std::string& outJson)
{
    const auto root = grml::parseJson(body);
    if (!root || !root->isObject()) {
        outJson = "{\"ok\":false,\"error\":\"Invalid JSON\"}\n";
        return false;
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
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);
    const bool ok = grml::updateIniFilePreservingComments(dataDir / L"grml.ini", updates);
    outJson = std::string("{\"ok\":") + (ok ? "true" : "false") + "}\n";
    return ok;
}

bool saveVfsFromJson(const std::filesystem::path& loaderDir, const std::string& body, std::string& outJson)
{
    const auto root = grml::parseJson(body);
    if (!root || !root->isObject()) {
        outJson = "{\"ok\":false,\"error\":\"Invalid JSON\"}\n";
        return false;
    }
    const bool enabled    = root->get("enabled") ? root->get("enabled")->asBool(true) : true;
    const auto orderedIds = root->get("orderedIds") ? root->get("orderedIds")->asStringArray() : std::vector<std::string>{};
    std::unordered_map<std::string, bool> enabledMap;
    if (const auto* em = root->get("enabledMap"); em && em->isObject()) {
        for (const auto& [k, v] : std::get<grml::JsonObject>(em->data)) {
            enabledMap[k] = v.asBool(false);
        }
    }
    const std::filesystem::path dataDir = grml::getAppDataDir(loaderDir);
    const bool ok = grml::vfs::VfsIndex::saveConfig(dataDir, enabled, orderedIds, enabledMap);
    outJson = std::string("{\"ok\":") + (ok ? "true" : "false") + "}\n";
    return ok;
}

std::string readIniJson(const std::filesystem::path& iniPath)
{
    if (iniPath.empty()) return "{\"exists\":false,\"sections\":[],\"rawText\":\"\"}\n";
    std::string raw;
    if (!grml::readFileBytesW(iniPath, raw)) {
        return "{\"exists\":false,\"iniPath\":\"" + ::jsonEsc(grml::wideToUtf8(iniPath.wstring())) +
               "\",\"sections\":[],\"rawText\":\"\"}\n";
    }
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    const auto schema = grml::buildUiSchemaFromIniFile(iniPath);
    struct SecOut { std::string name; std::vector<std::pair<std::string, std::string>> entries; };
    std::vector<SecOut> secs;
    for (const auto& c : schema.controls) {
        if (c.type == grml::UiControlType::Header) {
            secs.push_back({c.section, {}});
        } else {
            if (secs.empty() || secs.back().name != c.section) secs.push_back({c.section, {}});
            secs.back().entries.emplace_back(c.key, c.defaultValue);
        }
    }
    std::string out = "{\"exists\":true,\"iniPath\":\"" + ::jsonEsc(grml::wideToUtf8(iniPath.wstring())) + "\",\"sections\":[";
    for (size_t i = 0; i < secs.size(); ++i) {
        if (i > 0) out += ",";
        out += "{\"name\":\"" + ::jsonEsc(secs[i].name) + "\",\"entries\":[";
        for (size_t j = 0; j < secs[i].entries.size(); ++j) {
            if (j > 0) out += ",";
            out += "{\"key\":\"" + ::jsonEsc(secs[i].entries[j].first) + "\",\"value\":\"" +
                   ::jsonEsc(secs[i].entries[j].second) + "\"}";
        }
        out += "]}";
    }
    out += "],\"rawText\":\"" + ::jsonEsc(raw) + "\"}\n";
    return out;
}

bool saveIniFromJson(const std::string& body, std::string& outJson)
{
    const auto root = grml::parseJson(body);
    if (!root || !root->isObject() || !root->get("iniPath")) {
        outJson = "{\"ok\":false,\"error\":\"Missing iniPath\"}\n";
        return false;
    }
    const std::filesystem::path iniPath = grml::utf8ToWide(root->get("iniPath")->asString());
    if (const auto* raw = root->get("rawText"); raw && raw->isString()) {
        std::string txt = raw->asString();
        if (txt.size() >= 3 && static_cast<unsigned char>(txt[0]) == 0xEF &&
            static_cast<unsigned char>(txt[1]) == 0xBB && static_cast<unsigned char>(txt[2]) == 0xBF) {
            txt.erase(0, 3);
        }
        const bool ok = grml::writeFileBytesW(iniPath, txt);
        outJson = std::string("{\"ok\":") + (ok ? "true" : "false") + "}\n";
        return ok;
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
    outJson = std::string("{\"ok\":") + (ok ? "true" : "false") + "}\n";
    return ok;
}

bool generateSchemaFromJson(const std::string& body, std::string& outJson)
{
    const auto root = grml::parseJson(body);
    if (!root || !root->isObject() || !root->get("dllPath") || !root->get("iniPath")) {
        outJson = "{\"ok\":false,\"error\":\"Missing dllPath or iniPath\"}\n";
        return false;
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
        outJson = std::string("{\"ok\":") + (ok ? "true" : "false") +
                  ",\"outPath\":\"" + ::jsonEsc(grml::wideToUtf8(outPath.wstring())) + "\"}\n";
        return ok;
    }

    auto yamlEscape = [](std::string_view s) {
        std::string r;
        for (char c : s) {
            if (c == '"') r += "\\\"";
            else if (c == '\n' || c == '\r') r += ' ';
            else r += c;
        }
        return r;
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
    outJson = std::string("{\"ok\":") + (ok ? "true" : "false") +
              ",\"outPath\":\"" + ::jsonEsc(grml::wideToUtf8(outPath.wstring())) + "\"}\n";
    return ok;
}

} // namespace grml::launcher_api

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::filesystem::path loaderDir = getExeDir();

    bool hasDevFlag = false;
    bool onlyGuiFlags = true;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--dev") == 0) {
            hasDevFlag = true;
        } else {
            onlyGuiFlags = false;
        }
    }
    if (hasDevFlag) {
        SetEnvironmentVariableW(L"GRML_DEV", L"1");
    }

    if (argc <= 1 || onlyGuiFlags) {
        uint16_t port = 0;
        if (!grml::http_server::start(loaderDir, port)) {
            MessageBoxW(nullptr, L"Failed to start GRML local HTTP server.", L"GRML Error", MB_OK | MB_ICONERROR);
            if (argv) LocalFree(argv);
            return 1;
        }
        const int rc = grml::webview_host::runWindow(hInstance, port);
        grml::http_server::stop();
        if (argv) LocalFree(argv);
        return rc;
    }

    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fOut = nullptr;
            FILE* fErr = nullptr;
            freopen_s(&fOut, "CONOUT$", "w", stdout);
            freopen_s(&fErr, "CONOUT$", "w", stderr);
        }
    }
    SetConsoleOutputCP(CP_UTF8);

    enum class Mode { Launch, Discover, InspectSaves, Status, CrashEvents, SaveVfs, SaveConfig, ReadIni, SaveIni, GenSchema, ServeOnly };
    Mode mode = Mode::Launch;
    std::filesystem::path gameOverride;
    std::filesystem::path iniOverride;

    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--discover") == 0) mode = Mode::Discover;
        else if (std::wcscmp(argv[i], L"--inspect-saves") == 0) mode = Mode::InspectSaves;
        else if (std::wcscmp(argv[i], L"--status") == 0) mode = Mode::Status;
        else if (std::wcscmp(argv[i], L"--crash-events") == 0) mode = Mode::CrashEvents;
        else if (std::wcscmp(argv[i], L"--save-vfs") == 0) mode = Mode::SaveVfs;
        else if (std::wcscmp(argv[i], L"--save-config") == 0) mode = Mode::SaveConfig;
        else if (std::wcscmp(argv[i], L"--read-ini") == 0) mode = Mode::ReadIni;
        else if (std::wcscmp(argv[i], L"--save-ini") == 0) mode = Mode::SaveIni;
        else if (std::wcscmp(argv[i], L"--generate-schema") == 0) mode = Mode::GenSchema;
        else if (std::wcscmp(argv[i], L"--serve-only") == 0) mode = Mode::ServeOnly;
        else if (std::wcscmp(argv[i], L"--launch") == 0) mode = Mode::Launch;
        else if (std::wcscmp(argv[i], L"--game") == 0 && i + 1 < argc) gameOverride = argv[++i];
        else if (std::wcscmp(argv[i], L"--ini") == 0 && i + 1 < argc) iniOverride = argv[++i];
    }
    if (argv) LocalFree(argv);

    int rc = 0;
    std::string outJson;
    switch (mode) {
    case Mode::Discover:
        outJson = grml::launcher_api::buildDiscoverJson(loaderDir, gameOverride);
        break;
    case Mode::InspectSaves:
        outJson = grml::launcher_api::buildInspectSavesJson(loaderDir, gameOverride);
        break;
    case Mode::Status:
        outJson = grml::launcher_api::buildStatusJson();
        break;
    case Mode::CrashEvents:
        outJson = grml::launcher_api::buildCrashEventsJson();
        break;
    case Mode::SaveVfs:
        rc = grml::launcher_api::saveVfsFromJson(loaderDir, readStdinUtf8(), outJson) ? 0 : 1;
        break;
    case Mode::SaveConfig:
        rc = grml::launcher_api::saveConfigFromJson(loaderDir, readStdinUtf8(), outJson) ? 0 : 1;
        break;
    case Mode::ReadIni:
        outJson = grml::launcher_api::readIniJson(iniOverride);
        break;
    case Mode::SaveIni:
        rc = grml::launcher_api::saveIniFromJson(readStdinUtf8(), outJson) ? 0 : 1;
        break;
    case Mode::GenSchema:
        rc = grml::launcher_api::generateSchemaFromJson(readStdinUtf8(), outJson) ? 0 : 1;
        break;
    case Mode::ServeOnly: {
        uint16_t port = 0;
        if (!grml::http_server::start(loaderDir, port)) {
            std::fprintf(stderr, "Failed to start GRML HTTP server.\n");
            return 1;
        }
        std::printf("GRML_SERVE_URL=http://127.0.0.1:%u/\n", static_cast<unsigned>(port));
        std::fflush(stdout);
        // Wait until stdin closes
        char buf[64];
        while (std::fread(buf, 1, sizeof(buf), stdin) > 0) {}
        grml::http_server::stop();
        return 0;
    }
    case Mode::Launch: {
        const auto lr = grml::launcher_api::launchGame(loaderDir, gameOverride);
        outJson = lr.json;
        rc = lr.ok ? 0 : 1;
        break;
    }
    }
    std::fwrite(outJson.data(), 1, outJson.size(), stdout);
    return rc;
}
