#pragma once
// ============================================================================
// hook_manager.hpp — Centralized MinHook Engine for GRML Plugins (GrmlHookApi)
// ============================================================================
#include <string>

#include "grml_api.h"

namespace grml::hooks {

const GrmlHookApi* hookApi() noexcept;
void rollbackPluginHooks(const std::string& pluginId);
void shutdown();

} // namespace grml::hooks
