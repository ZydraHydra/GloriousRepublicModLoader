#pragma once
#include <cstdint>
#include <filesystem>

namespace grml::http_server {

struct StartOptions {
    uint16_t preferredPort{0};
    bool     forcePort{false};
    int      mcpOverride{-1}; // -1 = from grml.ini, 0 = disabled, 1 = enabled
};

bool start(const std::filesystem::path& loaderDir, uint16_t& outPort, const StartOptions& opts = {});
void stop();
bool isMcpEnabled();
uint16_t activePort();

} // namespace grml::http_server
