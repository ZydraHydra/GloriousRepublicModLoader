#pragma once
#include <cstdint>
#include <filesystem>

namespace grml::http_server {

bool start(const std::filesystem::path& loaderDir, uint16_t& outPort);
void stop();

} // namespace grml::http_server
