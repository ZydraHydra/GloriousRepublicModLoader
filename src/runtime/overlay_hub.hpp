#pragma once
// ============================================================================
// overlay_hub.hpp — GRML F9 Mod Settings Dialog, Telemetry & Sandboxed Tabs
// ============================================================================
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "grml_api.h"
#include "common/ui_schema.hpp"

namespace grml::overlay {

struct LoadedModSummary {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string loaderApi;
    uint32_t    loadOrder{0};
    bool        loaded{false};
    bool        failed{false};
    std::string status;
};

void setInGameFlagPtr(const volatile uint8_t* ptr) noexcept;
void setPluginsDirectory(const std::filesystem::path& pluginsDir);
void publishLoadedMods(std::vector<LoadedModSummary> mods);
bool hasRegisteredTabForPlugin(const std::string& pluginId);
int32_t registerExternalUiSchemaTab(const std::string& pluginId,
                                    const std::string& fallbackTitle,
                                    const std::filesystem::path& dllPath,
                                    const std::filesystem::path& manifestPath,
                                    const UiSchema& schema);

bool install(uint16_t toggleVk = 0x78 /* VK_F9 */);
void shutdown();

const GrmlUiApi* uiApi() noexcept;

} // namespace grml::overlay
