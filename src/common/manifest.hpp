#pragma once
// ============================================================================
// manifest.hpp — Plugin discovery (GRML + legacy RML + Tesmio) & topo sort
// ============================================================================
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace grml {

enum class PluginLoaderKind {
    GrmlNative,
    RmlNative,
    TesmioLegacy,
    RawNative
};

struct DiscoveredPlugin {
    std::string               id;
    std::string               name;
    std::string               version;
    std::string               author;
    std::string               description;
    std::filesystem::path     dllPath;
    std::filesystem::path     manifestPath;
    std::filesystem::path     sourceRoot;
    std::string               workshopId;
    std::string               sha256;

    PluginLoaderKind          loaderKind{PluginLoaderKind::RawNative};
    bool                      hasGrmlInit{false};
    bool                      hasRmlQuery{false};
    bool                      hasRmlLoad{false};
    bool                      hasTsmInit{false};
    bool                      hasTsmStart{false};
    bool                      hasTsmApiVer{false};

    int                       defaultPriority{1000};
    bool                      enabledByDefault{true};
    bool                      enabled{true};

    std::vector<std::string>  dependencies;
    std::vector<std::string>  conflictsWith;
    std::vector<std::string>  loadAfter;
    std::vector<std::string>  loadBefore;

    std::string               warning;
    bool                      blockedByDependencyOrConflict{false};
};

struct DiscoveryResult {
    std::vector<DiscoveredPlugin> plugins;
    std::vector<std::string>      warnings;
};

class IniFile;

// Discovers all plugins across local `plugins/`, Workshop `784150` items, and
// `media_soviet/workshop_wip/`. All paths use std::filesystem::path (wide).
DiscoveryResult discoverAllPlugins(const std::filesystem::path& loaderBaseDir,
                                   const std::filesystem::path& gameExePath,
                                   const IniFile& grmlConfig);

// Orders enabled plugins using topological sort over dependencies, loadAfter,
// and loadBefore, checking conflictsWith and missing dependencies.
std::vector<DiscoveredPlugin> resolveLoadOrder(std::vector<DiscoveredPlugin> plugins,
                                               std::vector<std::string>& outWarnings);

const char* loaderKindDisplayName(const DiscoveredPlugin& p) noexcept;

} // namespace grml
