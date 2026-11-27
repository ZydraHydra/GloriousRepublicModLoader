#pragma once
// ============================================================================
// injector.hpp — Suspended launch + wide-path grml_runtime.dll injector
// ============================================================================
#include <cstdint>
#include <filesystem>
#include <string>

namespace grml::launcher {

struct LaunchResult {
    bool     ok{false};
    uint32_t processId{0};
    std::string message;
};

LaunchResult launchAndInjectWrsr(const std::filesystem::path& loaderDir,
                                 const std::filesystem::path& gameExe,
                                 const std::vector<std::string>& tempDisabledIds = {});

} // namespace grml::launcher
