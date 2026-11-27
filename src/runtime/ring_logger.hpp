#pragma once
// ============================================================================
// ring_logger.hpp — Unified thread-safe ring-buffer + grml.log logger
// ============================================================================
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "grml_api.h"

namespace grml::logger {

struct LogEntry {
    char        timestamp[16]{};
    int32_t     level{GRML_LOG_INFO};
    std::string component;
    std::string message;
};

void init(const std::filesystem::path& logFilePath);
void flush();
void shutdown();

void logMessage(int32_t level, const char* component, const char* fmt, ...);
void logMessageV(int32_t level, const char* component, const char* fmt, va_list args);

// Snapshot copy for the F9 Mod Settings log viewer (cold UI path only)
std::vector<LogEntry> snapshotRecent(size_t maxCount = 200);

} // namespace grml::logger
