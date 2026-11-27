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

struct StopResult {
    bool                  ok{false};
    std::vector<uint32_t> stoppedPids;
    std::string           message;
};

struct WaitResult {
    bool        ok{false};
    bool        timedOut{false};
    bool        crashed{false};
    std::string state;
    std::string lastSave;
    uint32_t    pid{0};
    std::string details;
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
StopResult stopGame(uint32_t targetPid = 0, uint32_t waitMs = 5000);
WaitResult waitForGameState(const std::filesystem::path& loaderDir,
                            const std::string& targetState,
                            const std::string& expectedSave,
                            uint32_t timeoutMs = 60000);
std::vector<std::string> readFilteredLogTail(const std::filesystem::path& dataDir,
                                             size_t maxLines,
                                             std::string_view severityFilter = {},
                                             std::string_view searchFilter = {});
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
