#include "runtime/ring_logger.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace grml::logger {

namespace {

constexpr size_t kMaxRingEntries = 512;

std::mutex            g_mutex;
HANDLE                g_file = INVALID_HANDLE_VALUE;
std::deque<LogEntry>  g_ring;

const char* levelLabel(int32_t lvl) noexcept
{
    switch (lvl) {
    case GRML_LOG_TRACE: return "TRACE";
    case GRML_LOG_WARN:  return "WARN ";
    case GRML_LOG_ERROR: return "ERROR";
    default:             return "INFO ";
    }
}

} // namespace

void init(const std::filesystem::path& logFilePath)
{
    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }

    std::error_code ec;
    if (logFilePath.has_parent_path()) {
        std::filesystem::create_directories(logFilePath.parent_path(), ec);
    }

    g_file = CreateFileW(logFilePath.c_str(),
                         GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr,
                         CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
}

void flush()
{
    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
    }
}

void shutdown()
{
    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void logMessageV(int32_t level, const char* component, const char* fmt, va_list args)
{
    if (!fmt) return;

    char msgBuf[1536]{};
    std::vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char tsBuf[16]{};
    std::snprintf(tsBuf, sizeof(tsBuf), "%02u:%02u:%02u.%03u",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    const char* comp = (component && *component) ? component : "GRML";

    char lineBuf[2048]{};
    const int lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "[%s] [%s] [%s] %s\r\n",
                                      tsBuf, levelLabel(level), comp, msgBuf);

    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE && lineLen > 0) {
        DWORD written = 0;
        WriteFile(g_file, lineBuf, static_cast<DWORD>(lineLen), &written, nullptr);
        if (level >= GRML_LOG_ERROR) {
            FlushFileBuffers(g_file);
        }
    }

    LogEntry entry;
    std::snprintf(entry.timestamp, sizeof(entry.timestamp), "%s", tsBuf);
    entry.level     = level;
    entry.component = comp;
    entry.message   = msgBuf;
    if (g_ring.size() >= kMaxRingEntries) {
        g_ring.pop_front();
    }
    g_ring.push_back(std::move(entry));
}

void logMessage(int32_t level, const char* component, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logMessageV(level, component, fmt, args);
    va_end(args);
}

std::vector<LogEntry> snapshotRecent(size_t maxCount)
{
    std::lock_guard lock(g_mutex);
    const size_t total = g_ring.size();
    const size_t start = (total > maxCount) ? (total - maxCount) : 0;
    std::vector<LogEntry> out;
    out.reserve(total - start);
    for (size_t i = start; i < total; ++i) {
        out.push_back(g_ring[i]);
    }
    return out;
}

} // namespace grml::logger
