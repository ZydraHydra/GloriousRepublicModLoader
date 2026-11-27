#include "common/manifest.hpp"
#include "common/ini_util.hpp"
#include "common/json_min.hpp"
#include "common/pe_scanner.hpp"
#include "grml_api.h"

#include <algorithm>
#include <cctype>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace grml {

namespace {

std::string toLowerAscii(std::string_view s)
{
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool endsWithInsensitive(std::wstring_view s, std::wstring_view suffix)
{
    if (s.size() < suffix.size()) return false;
    const auto tail = s.substr(s.size() - suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (towlower(tail[i]) != towlower(suffix[i])) return false;
    }
    return true;
}

std::string syntheticIdFromFilename(const std::filesystem::path& dllPath)
{
    const std::string stem = toLowerAscii(wideToUtf8(dllPath.stem().wstring()));
    std::string clean;
    bool lastDot = false;
    for (char c : stem) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            clean += c;
            lastDot = false;
        } else if (!lastDot && !clean.empty()) {
            clean += '.';
            lastDot = true;
        }
    }
    while (!clean.empty() && clean.back() == '.') clean.pop_back();
    if (clean.empty()) clean = "plugin";
    return "local." + clean;
}

struct ParsedManifest {
    std::string              id;
    std::string              name;
    std::string              version;
    std::string              author;
    std::string              description;
    std::string              dll;
    std::string              image;
    int                      defaultPriority{1000};
    bool                     enabledByDefault{false};
    std::vector<std::string> dependencies;
    std::vector<std::string> conflictsWith;
    std::vector<std::string> loadAfter;
    std::vector<std::string> loadBefore;
    std::vector<std::string> supportedGameVersions;
    std::vector<std::string> requiredOffsets;
    std::vector<std::string> optionalOffsets;
    std::filesystem::path    manifestPath;
    bool                     isGrmlManifest{false};
};

std::optional<ParsedManifest> parseManifestFile(const std::filesystem::path& path, bool isGrml, std::string& outWarn)
{
    std::string raw;
    if (!readFileBytesW(path, raw)) {
        outWarn = wideToUtf8(path.filename().wstring()) + ": unable to read manifest";
        return std::nullopt;
    }
    auto root = parseJson(raw);
    if (!root || !root->isObject()) {
        outWarn = wideToUtf8(path.filename().wstring()) + ": invalid JSON manifest";
        return std::nullopt;
    }

    ParsedManifest m;
    m.manifestPath   = path;
    m.isGrmlManifest = isGrml;
    if (auto* v = root->get("id")) m.id = v->asString();
    if (auto* v = root->get("name")) m.name = v->asString();
    if (auto* v = root->get("version")) m.version = v->asString();
    if (auto* v = root->get("author")) m.author = v->asString();
    if (auto* v = root->get("description")) m.description = v->asString();
    if (auto* v = root->get("dll")) m.dll = v->asString();
    if (auto* v = root->get("image")) m.image = v->asString();
    if (auto* v = root->get("icon")) m.image = v->asString();
    if (auto* v = root->get("previewImage")) m.image = v->asString();
    if (auto* v = root->get("preview_image")) m.image = v->asString();
    if (auto* v = root->get("defaultPriority")) m.defaultPriority = v->asInt(1000);
    if (auto* v = root->get("default_priority")) m.defaultPriority = v->asInt(m.defaultPriority);
    if (auto* v = root->get("enabledByDefault")) m.enabledByDefault = v->asBool(false);
    if (auto* v = root->get("enabled_by_default")) m.enabledByDefault = v->asBool(m.enabledByDefault);

    auto readList = [&](const char* k1, const char* k2, std::vector<std::string>& dst) {
        if (auto* v = root->get(k1)) {
            auto arr = v->asStringArray();
            dst.insert(dst.end(), arr.begin(), arr.end());
        }
        if (k2) {
            if (auto* v = root->get(k2)) {
                auto arr = v->asStringArray();
                dst.insert(dst.end(), arr.begin(), arr.end());
            }
        }
    };
    readList("dependencies", nullptr, m.dependencies);
    readList("conflicts", "conflicts_with", m.conflictsWith);
    readList("loadAfter", "load_after", m.loadAfter);
    readList("loadBefore", "load_before", m.loadBefore);
    readList("supportedGameVersions", "supported_game_versions", m.supportedGameVersions);
    readList("gameVersions", "game_versions", m.supportedGameVersions);
    readList("required_offsets", "requiredOffsets", m.requiredOffsets);
    readList("optional_offsets", "optionalOffsets", m.optionalOffsets);

    if (m.id.empty() || m.name.empty() || m.dll.empty()) {
        outWarn = wideToUtf8(path.filename().wstring()) + ": manifest missing id/name/dll";
        return std::nullopt;
    }
    return m;
}

void collectPluginDirs(const std::filesystem::path& root,
                       std::vector<std::pair<std::filesystem::path, std::string>>& outDirs,
                       const std::string& wsId)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    auto maybeAdd = [&](const std::filesystem::path& candidate) {
        if (std::filesystem::is_directory(candidate, ec)) {
            outDirs.emplace_back(candidate, wsId);
        }
    };
    maybeAdd(root / L"plugins");
    maybeAdd(root / L"plugin");

    // 1-level-deep wrapped Workshop payload folders (<item>/<subpkg>/plugins)
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        const auto name = toLowerAscii(wideToUtf8(entry.path().filename().wstring()));
        if (name == "plugin" || name == "plugins") continue;
        maybeAdd(entry.path() / L"plugins");
        maybeAdd(entry.path() / L"plugin");
    }
}

} // namespace

const char* loaderKindDisplayName(const DiscoveredPlugin& p) noexcept
{
    static const std::string kGrmlAbiLabel = "GRML v" + std::to_string(GRML_API_VERSION);
    if (p.hasGrmlInit) return kGrmlAbiLabel.c_str();
    switch (p.loaderKind) {
    case PluginLoaderKind::GrmlNative:   return kGrmlAbiLabel.c_str();
    case PluginLoaderKind::RmlNative:    return "RML v1";
    case PluginLoaderKind::TesmioLegacy: return "Tesmio v4";
    case PluginLoaderKind::RawNative:    return "Raw DLL";
    }
    return "Unknown";
}

bool isGameVersionSupported(const DiscoveredPlugin& p, std::string_view gameVersion) noexcept
{
    if (p.supportedGameVersions.empty() || gameVersion.empty() || gameVersion == "unknown") {
        return true;
    }
    const std::string target = toLowerAscii(gameVersion);
    for (const auto& raw : p.supportedGameVersions) {
        const std::string entry = toLowerAscii(raw);
        if (entry == "*" || entry == "latest" || entry == target) {
            return true;
        }
        if (!entry.empty() && entry.back() == '+') {
            const std::string base = entry.substr(0, entry.size() - 1);
            if (target >= base) {
                return true;
            }
        }
    }
    return false;
}

DiscoveryResult discoverAllPlugins(const std::filesystem::path& loaderBaseDir,
                                   const std::filesystem::path& gameExePath,
                                   const IniFile& grmlConfig)
{
    DiscoveryResult result;
    std::vector<std::pair<std::filesystem::path, std::string>> scanDirs;

    const bool scanWorkshop = grmlConfig.getBool("general", "scan_workshop", true);
    const bool scanWip      = grmlConfig.getBool("general", "scan_workshop_wip", true);
    const EffectiveGamePaths effPaths = resolveEffectiveGamePaths(gameExePath, grmlConfig);

    std::error_code ec;
    if (scanWorkshop && !effPaths.workshopRoot.empty()) {
        const auto& wsRoot = effPaths.workshopRoot;
        if (std::filesystem::is_directory(wsRoot, ec)) {
            for (const auto& item : std::filesystem::directory_iterator(wsRoot, ec)) {
                if (!item.is_directory(ec)) continue;
                const auto& itemDir = item.path();
                const bool isLoaderFolder =
                    std::filesystem::exists(itemDir / L"rml_runtime.dll", ec) ||
                    std::filesystem::exists(itemDir / L"rml_injector.exe", ec) ||
                    std::filesystem::exists(itemDir / L"grml_runtime.dll", ec) ||
                    std::filesystem::exists(itemDir / L"GloriousRepublicModLoader.exe", ec);
                const std::string wsId = isLoaderFolder
                    ? std::string{}
                    : wideToUtf8(itemDir.filename().wstring());
                // Don't tag plugins inside a loader's own Workshop folder with the loader's Workshop ID
                collectPluginDirs(itemDir, scanDirs, wsId);
            }
        }
    }

    if (scanWip && !effPaths.workshopWipRoot.empty()) {
        const auto& wipRoot = effPaths.workshopWipRoot;
        if (std::filesystem::is_directory(wipRoot, ec)) {
            for (const auto& item : std::filesystem::directory_iterator(wipRoot, ec)) {
                if (item.is_directory(ec)) {
                    collectPluginDirs(item.path(), scanDirs, "");
                }
            }
        }
    }

    // Local loader plugin directories (highest familiarity for manual installs)
    const std::filesystem::path effectiveBase =
        (_wcsicmp(loaderBaseDir.filename().c_str(), L"core") == 0 && loaderBaseDir.has_parent_path())
            ? loaderBaseDir.parent_path()
            : loaderBaseDir;
    if (std::filesystem::is_directory(effectiveBase / L"plugins", ec)) {
        scanDirs.emplace_back(effectiveBase / L"plugins", "");
    }
    if (std::filesystem::is_directory(effectiveBase / L"plugin", ec)) {
        scanDirs.emplace_back(effectiveBase / L"plugin", "");
    }
    if (std::filesystem::is_directory(effectiveBase / L"rml" / L"plugins", ec)) {
        scanDirs.emplace_back(effectiveBase / L"rml" / L"plugins", "");
    }
    const auto appDataPlugins = getAppDataDir() / L"plugins";
    if (!appDataPlugins.empty() && std::filesystem::is_directory(appDataPlugins, ec)) {
        scanDirs.emplace_back(appDataPlugins, "");
    }

    std::unordered_set<std::wstring> seenDirPaths;
    std::unordered_set<std::string>  seenSha256;
    std::unordered_set<std::string>  seenIds;

    for (const auto& [dir, wsId] : scanDirs) {
        std::filesystem::path canonDir = std::filesystem::weakly_canonical(dir, ec);
        if (ec) canonDir = dir.lexically_normal();
        std::wstring lowerDir = canonDir.wstring();
        for (auto& ch : lowerDir) ch = static_cast<wchar_t>(towlower(ch));
        if (!seenDirPaths.insert(lowerDir).second) continue;

        // Pass 1: Collect manifests in `dir` (*.grml.json takes precedence over *.rml.json)
        std::unordered_map<std::wstring, ParsedManifest> manifestsByDll;
        for (const auto& entry : std::filesystem::directory_iterator(canonDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto fname = entry.path().filename().wstring();
            const bool isGrml = endsWithInsensitive(fname, L".grml.json");
            const bool isRml  = endsWithInsensitive(fname, L".rml.json");
            if (!isGrml && !isRml) continue;

            std::string warn;
            auto parsed = parseManifestFile(entry.path(), isGrml, warn);
            if (!parsed) {
                if (!warn.empty()) result.warnings.push_back(warn);
                continue;
            }
            std::wstring dllKey = utf8ToWide(parsed->dll);
            for (auto& ch : dllKey) ch = static_cast<wchar_t>(towlower(ch));

            auto existingIt = manifestsByDll.find(dllKey);
            if (existingIt == manifestsByDll.end() || (!existingIt->second.isGrmlManifest && isGrml)) {
                manifestsByDll.insert_or_assign(dllKey, std::move(*parsed));
            }
        }

        // Pass 2: Inspect every *.dll in `dir`
        for (const auto& entry : std::filesystem::directory_iterator(canonDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (!endsWithInsensitive(entry.path().filename().wstring(), L".dll")) continue;

            const std::wstring dllNameLower = [&]() {
                std::wstring s = entry.path().filename().wstring();
                for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
                return s;
            }();
            // Never treat the loader runtime itself as a plugin
            if (dllNameLower == L"grml_runtime.dll" || dllNameLower == L"rml_runtime.dll") {
                continue;
            }

            const DiskPeExports peExp = inspectDiskPeExports(entry.path());
            if (!peExp.isValidAmd64Dll) {
                result.warnings.push_back("Skipped non-AMD64/invalid DLL: " + wideToUtf8(entry.path().wstring()));
                continue;
            }

            const std::string sha = computeFileSha256(entry.path());
            if (!sha.empty() && !seenSha256.insert(sha).second) {
                continue; // Identical binary already discovered
            }

            DiscoveredPlugin p;
            p.dllPath    = entry.path();
            p.sourceRoot = canonDir;
            p.workshopId = wsId;
            p.sha256     = sha;

            auto hasExp = [&](const char* name) {
                return std::find(peExp.exports.begin(), peExp.exports.end(), name) != peExp.exports.end();
            };
            p.hasGrmlInit  = hasExp("GrmlPluginInit");
            p.hasRmlQuery  = hasExp("RmlPluginQuery");
            p.hasRmlLoad   = hasExp("RmlPluginLoad");
            p.hasTsmApiVer = hasExp("TsmPluginApiVersion");
            p.hasTsmInit   = hasExp("TsmPluginInit");
            p.hasTsmStart  = hasExp("TsmPluginStart");

            if (p.hasGrmlInit) {
                p.loaderKind = PluginLoaderKind::GrmlNative;
            } else if (p.hasRmlQuery && p.hasRmlLoad) {
                p.loaderKind = PluginLoaderKind::RmlNative;
            } else if (p.hasTsmInit || p.hasTsmApiVer || p.hasTsmStart) {
                p.loaderKind = PluginLoaderKind::TesmioLegacy;
            } else {
                p.loaderKind = PluginLoaderKind::RawNative;
            }

            auto findModImage = [&](const std::string& explicitImg) -> std::filesystem::path {
                const auto dllDir = entry.path().parent_path();
                const auto stem   = entry.path().stem().wstring();
                if (!explicitImg.empty()) {
                    std::filesystem::path cand = utf8ToWide(explicitImg);
                    if (cand.is_relative()) {
                        if (std::filesystem::is_regular_file(dllDir / cand, ec)) return dllDir / cand;
                        if (!p.manifestPath.empty() && std::filesystem::is_regular_file(p.manifestPath.parent_path() / cand, ec)) {
                            return p.manifestPath.parent_path() / cand;
                        }
                    } else if (std::filesystem::is_regular_file(cand, ec)) {
                        return cand;
                    }
                }
                const std::filesystem::path searchDirs[] = {
                    dllDir,
                    dllDir.parent_path(),
                    dllDir.parent_path() / L"steamcmd",
                };
                const std::wstring exts[] = {L".png", L".jpg", L".jpeg", L".webp"};
                for (const auto& ext : exts) {
                    if (std::filesystem::is_regular_file(dllDir / (stem + ext), ec)) return dllDir / (stem + ext);
                }
                for (const auto& sd : searchDirs) {
                    if (sd.empty() || !std::filesystem::is_directory(sd, ec)) continue;
                    for (const wchar_t* base : {L"previewimage", L"preview", L"icon", L"logo"}) {
                        for (const auto& ext : exts) {
                            const auto cand = sd / (std::wstring(base) + ext);
                            if (std::filesystem::is_regular_file(cand, ec)) return cand;
                        }
                    }
                }
                return {};
            };

            auto manIt = manifestsByDll.find(dllNameLower);
            if (manIt != manifestsByDll.end()) {
                const auto& m = manIt->second;
                p.id                    = m.id;
                p.name                  = m.name;
                p.version               = m.version.empty() ? "1.0.0" : m.version;
                p.author                = m.author;
                p.description           = m.description;
                p.manifestPath          = m.manifestPath;
                p.hasGrmlManifest       = m.isGrmlManifest;
                p.defaultPriority       = m.defaultPriority;
                p.enabledByDefault      = m.enabledByDefault;
                p.dependencies          = m.dependencies;
                p.conflictsWith         = m.conflictsWith;
                p.loadAfter             = m.loadAfter;
                p.loadBefore            = m.loadBefore;
                p.supportedGameVersions = m.supportedGameVersions;
                p.requiredOffsets       = m.requiredOffsets;
                p.optionalOffsets       = m.optionalOffsets;
                p.imagePath             = findModImage(m.image);
            } else {
                p.id        = syntheticIdFromFilename(entry.path());
                p.name      = wideToUtf8(entry.path().stem().wstring());
                p.version   = "unversioned";
                p.imagePath = findModImage("");
            }

            if (!seenIds.insert(p.id).second) {
                p.id += "." + (sha.size() >= 8 ? sha.substr(0, 8) : "dup");
                seenIds.insert(p.id);
            }

            // Safe opt-in default: newly discovered mods start disabled until enabled in grml.ini.
            // External MediaRedirector.dll is permanently blocked because Asset Overrides (Media VFS)
            // is integrated directly into GRML.
            const std::string idLow   = toLowerAscii(p.id);
            const std::string stemLow = toLowerAscii(wideToUtf8(p.dllPath.stem().wstring()));
            if (idLow == "zydrahydra.media_redirector" || stemLow == "mediaredirector") {
                p.enabled                       = false;
                p.blockedByDependencyOrConflict = true;
                p.warning = "Built into GRML (Asset Overrides). External MediaRedirector.dll cannot be loaded.";
            } else {
                p.enabled = grmlConfig.getBool("plugins", p.id, false);
            }
            result.plugins.push_back(std::move(p));
        }
    }

    return result;
}

std::vector<DiscoveredPlugin> resolveLoadOrder(std::vector<DiscoveredPlugin> plugins,
                                               std::vector<std::string>& outWarnings)
{
    std::vector<DiscoveredPlugin> enabled;
    enabled.reserve(plugins.size());
    for (auto& p : plugins) {
        if (p.enabled) {
            enabled.push_back(std::move(p));
        }
    }
    if (enabled.empty()) return enabled;

    std::unordered_map<std::string, size_t> idToIdx;
    for (size_t i = 0; i < enabled.size(); ++i) {
        idToIdx[enabled[i].id] = i;
    }

    // 1. Validate missing dependencies & active conflicts_with before ordering
    std::vector<bool> active(enabled.size(), true);
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < enabled.size(); ++i) {
            if (!active[i]) continue;
            for (const auto& dep : enabled[i].dependencies) {
                auto it = idToIdx.find(dep);
                if (it == idToIdx.end() || !active[it->second]) {
                    active[i] = false;
                    enabled[i].blockedByDependencyOrConflict = true;
                    enabled[i].warning = "Missing required dependency: " + dep;
                    outWarnings.push_back(enabled[i].id + ": " + enabled[i].warning);
                    changed = true;
                    break;
                }
            }
        }
    }

    for (size_t i = 0; i < enabled.size(); ++i) {
        if (!active[i]) continue;
        for (const auto& conf : enabled[i].conflictsWith) {
            auto it = idToIdx.find(conf);
            if (it != idToIdx.end() && active[it->second] && it->second != i) {
                // Disable the lower-priority (higher defaultPriority or later) conflicting plugin
                const size_t victim = (enabled[i].defaultPriority >= enabled[it->second].defaultPriority) ? i : it->second;
                const size_t keeper = (victim == i) ? it->second : i;
                active[victim] = false;
                enabled[victim].blockedByDependencyOrConflict = true;
                enabled[victim].warning = "Conflicts with active plugin: " + enabled[keeper].id;
                outWarnings.push_back(enabled[victim].id + ": " + enabled[victim].warning);
            }
        }
    }

    // 2. Build topological DAG for active plugins
    const size_t n = enabled.size();
    std::vector<std::vector<size_t>> adj(n);
    std::vector<size_t> indegree(n, 0);

    auto addEdge = [&](size_t before, size_t after) {
        if (before == after || !active[before] || !active[after]) return;
        for (size_t x : adj[before]) {
            if (x == after) return;
        }
        adj[before].push_back(after);
        ++indegree[after];
    };

    for (size_t i = 0; i < n; ++i) {
        if (!active[i]) continue;
        for (const auto& dep : enabled[i].dependencies) {
            if (auto it = idToIdx.find(dep); it != idToIdx.end()) addEdge(it->second, i);
        }
        for (const auto& afterId : enabled[i].loadAfter) {
            if (auto it = idToIdx.find(afterId); it != idToIdx.end()) addEdge(it->second, i);
        }
        for (const auto& beforeId : enabled[i].loadBefore) {
            if (auto it = idToIdx.find(beforeId); it != idToIdx.end()) addEdge(i, it->second);
        }
    }

    // Priority min-heap: smaller defaultPriority loads first, tie-break by id
    auto cmp = [&](size_t a, size_t b) {
        if (enabled[a].defaultPriority != enabled[b].defaultPriority) {
            return enabled[a].defaultPriority > enabled[b].defaultPriority;
        }
        return enabled[a].id > enabled[b].id;
    };
    std::priority_queue<size_t, std::vector<size_t>, decltype(cmp)> pq(cmp);
    for (size_t i = 0; i < n; ++i) {
        if (active[i] && indegree[i] == 0) {
            pq.push(i);
        }
    }

    std::vector<DiscoveredPlugin> ordered;
    std::vector<bool> emitted(n, false);
    while (!pq.empty()) {
        const size_t u = pq.top();
        pq.pop();
        emitted[u] = true;
        ordered.push_back(enabled[u]);
        for (size_t v : adj[u]) {
            if (--indegree[v] == 0) {
                pq.push(v);
            }
        }
    }

    // Break any cycle deterministically with a warning rather than silently dropping plugins
    std::vector<size_t> remaining;
    for (size_t i = 0; i < n; ++i) {
        if (active[i] && !emitted[i]) remaining.push_back(i);
    }
    if (!remaining.empty()) {
        std::sort(remaining.begin(), remaining.end(), [&](size_t a, size_t b) {
            if (enabled[a].defaultPriority != enabled[b].defaultPriority) {
                return enabled[a].defaultPriority < enabled[b].defaultPriority;
            }
            return enabled[a].id < enabled[b].id;
        });
        outWarnings.push_back("Dependency cycle detected among enabled plugins; falling back to priority order for cyclic nodes.");
        for (size_t idx : remaining) {
            enabled[idx].warning = "In dependency cycle (priority fallback)";
            ordered.push_back(enabled[idx]);
        }
    }

    return ordered;
}

} // namespace grml
