#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace grml::launcher_api {

struct LaunchResult {
    bool        ok{false};
    uint32_t    processId{0};
    std::string json;
};

std::string jsonEscape(std::string_view s);
std::string buildDiscoverJson(const std::filesystem::path& loaderDir,
                              const std::filesystem::path& gameOverride);
std::string buildInspectSavesJson(const std::filesystem::path& loaderDir,
                                  const std::filesystem::path& gameOverride);
std::vector<uint32_t> getRunningSovietPids();
std::string buildStatusJson();
std::string buildBackupProgressJson();
std::string buildCrashEventsJson();
LaunchResult launchGame(const std::filesystem::path& loaderDir,
                        const std::filesystem::path& gameOverride,
                        const std::vector<std::string>& tempDisabledIds = {});
bool saveConfigFromJson(const std::filesystem::path& loaderDir,
                        const std::string& body,
                        std::string& outJson);
bool saveVfsFromJson(const std::filesystem::path& loaderDir,
                     const std::string& body,
                     std::string& outJson);
std::string readIniJson(const std::filesystem::path& iniPath);
bool saveIniFromJson(const std::string& body, std::string& outJson);
bool generateSchemaFromJson(const std::string& body, std::string& outJson);

} // namespace grml::launcher_api
