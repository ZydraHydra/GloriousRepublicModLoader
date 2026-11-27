#pragma once
// ============================================================================
// plugin_supervisor.hpp — Hardened SEH plugin lifecycle, AOB pass & shutdown
// ============================================================================
#include <filesystem>

namespace grml::supervisor {

// Runs in-process GRML bootstrap from GrmlBootstrap / RmlBootstrap.
bool bootstrap(const std::filesystem::path& loaderRootDir);

// Unloads all plugins in strict reverse-load order inside SEH guards.
void shutdown();

} // namespace grml::supervisor
