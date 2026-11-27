#include "runtime/save_events.hpp"
#include "runtime/ring_logger.hpp"
#include "runtime/vfs_redirect.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace grml::save_events {

namespace {

struct LoadUnloadSub {
    GrmlSaveEventFn onLoad{nullptr};
    GrmlSaveEventFn onUnload{nullptr};
    void*           userData{nullptr};
};

struct WriteSub {
    GrmlSaveEventFn onWrite{nullptr};
    void*           userData{nullptr};
};

std::mutex                 g_subMutex;
std::vector<LoadUnloadSub> g_loadUnloadSubs;
std::vector<WriteSub>      g_writeSubs;

std::mutex       g_dirMutex;
std::wstring     g_activeSaveDir;
std::atomic<bool> g_wasInGame{false};
std::atomic<bool> g_installed{false};
void*             g_hookTarget = nullptr;

std::filesystem::path   g_sessionJsonPath;
std::mutex              g_sessionMutex;
std::condition_variable g_sessionCv;
std::thread             g_sessionThread;
bool                    g_sessionStop = false;
bool                    g_sessionDirty = false;
std::wstring            g_pendingSaveName;
std::wstring            g_pendingSavePath;

std::string wideToUtf8Local(std::wstring_view w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string jsonEscapeLocal(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

std::string currentUtcIso8601()
{
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void writeSessionJsonFile(const std::filesystem::path& outPath,
                          const std::wstring& saveName,
                          const std::wstring& savePath)
{
    if (outPath.empty()) return;
    const std::string nameUtf8 = jsonEscapeLocal(wideToUtf8Local(saveName));
    const std::string pathUtf8 = jsonEscapeLocal(wideToUtf8Local(savePath));
    const std::string tsUtc    = currentUtcIso8601();
    const DWORD pid            = GetCurrentProcessId();

    std::string json = "{\n";
    json += "  \"last_loaded_save\": \"" + nameUtf8 + "\",\n";
    json += "  \"last_loaded_save_path\": \"" + pathUtf8 + "\",\n";
    json += "  \"timestamp_utc\": \"" + tsUtc + "\",\n";
    json += "  \"pid\": " + std::to_string(pid) + "\n";
    json += "}\n";

    std::filesystem::path tmpPath = outPath;
    tmpPath += L".tmp";
    HANDLE hf = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL ok = WriteFile(hf, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    CloseHandle(hf);
    if (ok && written == json.size()) {
        MoveFileExW(tmpPath.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    } else {
        DeleteFileW(tmpPath.c_str());
    }
}

void sessionWriterThreadProc()
{
    for (;;) {
        std::wstring saveName;
        std::wstring savePath;
        {
            std::unique_lock lock(g_sessionMutex);
            g_sessionCv.wait(lock, [] { return g_sessionStop || g_sessionDirty; });
            if (g_sessionStop && !g_sessionDirty) break;
            g_sessionDirty = false;
            saveName = g_pendingSaveName;
            savePath = g_pendingSavePath;
        }
        writeSessionJsonFile(g_sessionJsonPath, saveName, savePath);
    }
}

void queueSessionBreadcrumb(const std::wstring& saveDir)
{
    if (g_sessionJsonPath.empty() || saveDir.empty()) return;
    const std::wstring saveName = std::filesystem::path(saveDir).filename().wstring();
    if (saveName.empty()) return;
    {
        std::lock_guard lock(g_sessionMutex);
        g_pendingSaveName = saveName;
        g_pendingSavePath = saveDir;
        g_sessionDirty    = true;
    }
    g_sessionCv.notify_one();
}

using PFN_CreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD,
                                        LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
PFN_CreateFileW g_origCreateFileW = nullptr;

// Zero-alloc suffix check on stack/view
bool endsWithFileNameNoAlloc(std::wstring_view path, std::wstring_view targetLower) noexcept
{
    if (path.size() < targetLower.size() + 1) {
        return false;
    }
    const size_t  off  = path.size() - targetLower.size();
    const wchar_t prev = path[off - 1];
    if (prev != L'\\' && prev != L'/') {
        return false;
    }
    for (size_t i = 0; i < targetLower.size(); ++i) {
        wchar_t c = path[off + i];
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c + (L'a' - L'A'));
        }
        if (c != targetLower[i]) {
            return false;
        }
    }
    return true;
}

std::wstring extractSaveDir(std::wstring_view rawPath)
{
    std::wstring norm(rawPath);
    for (auto& c : norm) {
        if (c == L'/') c = L'\\';
    }
    if (norm.size() >= 2 && norm[0] == L'.' && norm[1] == L'\\') {
        norm.erase(0, 2);
    }

    std::wstring lower = norm;
    for (auto& c : lower) {
        c = static_cast<wchar_t>(std::towlower(c));
    }

    constexpr std::wstring_view kNeedles[] = {
        L"\\media_soviet\\save\\",
        L"\\media_soviet\\save_cloud\\",
        L"media_soviet\\save\\",
        L"media_soviet\\save_cloud\\",
    };

    size_t nameStart = std::wstring::npos;
    for (const auto needle : kNeedles) {
        const size_t pos = lower.find(needle);
        if (pos == std::wstring::npos) continue;
        if (needle.front() != L'\\' && pos != 0) continue;
        nameStart = pos + needle.size();
        break;
    }
    if (nameStart == std::wstring::npos) return {};

    const size_t nextSlash = lower.find(L'\\', nameStart);
    if (nextSlash == std::wstring::npos || nextSlash == nameStart) return {};

    std::wstring relOrFull = norm.substr(0, nextSlash);
    wchar_t fullBuf[MAX_PATH * 4]{};
    const DWORD fullLen = GetFullPathNameW(relOrFull.c_str(), _countof(fullBuf), fullBuf, nullptr);
    if (fullLen > 0 && fullLen < _countof(fullBuf)) {
        return std::wstring(fullBuf, fullLen);
    }
    return relOrFull;
}

void invokeUnloadCallbacks()
{
    std::vector<LoadUnloadSub> subs;
    {
        std::lock_guard lock(g_subMutex);
        subs = g_loadUnloadSubs;
    }
    for (const auto& s : subs) {
        if (s.onUnload) {
            s.onUnload(nullptr, s.userData);
        }
    }
}

void invokeLoadCallbacks(const std::wstring& saveDir)
{
    std::vector<LoadUnloadSub> subs;
    {
        std::lock_guard lock(g_subMutex);
        subs = g_loadUnloadSubs;
    }
    for (const auto& s : subs) {
        if (s.onLoad) {
            s.onLoad(saveDir.c_str(), s.userData);
        }
    }
}

void invokeWriteCallbacks(const std::wstring& saveDir)
{
    std::vector<WriteSub> subs;
    {
        std::lock_guard lock(g_subMutex);
        subs = g_writeSubs;
    }
    for (const auto& s : subs) {
        if (s.onWrite) {
            s.onWrite(saveDir.c_str(), s.userData);
        }
    }
}

HANDLE WINAPI detourCreateFileW(LPCWSTR               lpFileName,
                                DWORD                 dwDesiredAccess,
                                DWORD                 dwShareMode,
                                LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                DWORD                 dwCreationDisposition,
                                DWORD                 dwFlagsAndAttributes,
                                HANDLE                hTemplateFile)
{
    const bool isWriteOpen = (dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    wchar_t vfsNormBuf[vfs_redirect::kNormBufCap];
    if (!isWriteOpen && dwCreationDisposition == OPEN_EXISTING) {
        if (const wchar_t* redirected = vfs_redirect::tryResolveReadOnlyW(lpFileName, vfsNormBuf)) {
            lpFileName = redirected;
        }
    }

    HANDLE h = g_origCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                                 lpSecurityAttributes, dwCreationDisposition,
                                 dwFlagsAndAttributes, hTemplateFile);

    static thread_local bool t_inHook = false;
    if (t_inHook || !lpFileName || h == INVALID_HANDLE_VALUE) {
        return h;
    }

    // Zero-alloc hot-path filter: ignore >99.9% of file opens immediately
    const std::wstring_view pathView(lpFileName);
    const bool isWrite = (dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    const bool isHeader = endsWithFileNameNoAlloc(pathView, L"header.bin");
    const bool isBuildings = !isWrite && !isHeader && endsWithFileNameNoAlloc(pathView, L"buildings.bin");
    if (isWrite) {
        if (!isHeader) {
            return h;
        }
    } else {
        if (!isHeader && !isBuildings) {
            return h;
        }
    }

    const std::wstring saveDir = extractSaveDir(pathView);
    if (saveDir.empty()) {
        return h;
    }

    if (!isWrite && isHeader) {
        t_inHook = true;
        queueSessionBreadcrumb(saveDir);
        t_inHook = false;
        return h;
    }

    t_inHook = true;

    bool dirChanged = false;
    bool hadPrevDir = false;
    {
        std::lock_guard lock(g_dirMutex);
        if (saveDir != g_activeSaveDir) {
            hadPrevDir      = !g_activeSaveDir.empty();
            g_activeSaveDir = saveDir;
            dirChanged      = true;
        }
    }

    if (isWrite) {
        invokeWriteCallbacks(saveDir);
    } else if (dirChanged) {
        if (hadPrevDir) {
            invokeUnloadCallbacks();
        }
        g_wasInGame.store(true, std::memory_order_relaxed);
        invokeLoadCallbacks(saveDir);
        logger::logMessage(GRML_LOG_INFO, "SaveEvents", "Loaded save detected (buildings.bin)");
    }

    t_inHook = false;
    return h;
}

} // namespace

bool install(const std::filesystem::path& loaderDir)
{
    if (g_installed.exchange(true)) {
        return true;
    }

    if (!loaderDir.empty()) {
        g_sessionJsonPath = loaderDir / L"grml_session.json";
        {
            std::lock_guard lock(g_sessionMutex);
            g_sessionStop  = false;
            g_sessionDirty = false;
        }
        g_sessionThread = std::thread(sessionWriterThreadProc);
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        g_installed.store(false);
        return false;
    }

    void* target = nullptr;
    MH_STATUS st = MH_CreateHookApiEx(L"kernelbase.dll", "CreateFileW",
                                       reinterpret_cast<void*>(detourCreateFileW),
                                       reinterpret_cast<void**>(&g_origCreateFileW),
                                       &target);
    if (st != MH_OK) {
        st = MH_CreateHookApiEx(L"kernel32.dll", "CreateFileW",
                                reinterpret_cast<void*>(detourCreateFileW),
                                reinterpret_cast<void**>(&g_origCreateFileW),
                                &target);
    }
    if (st != MH_OK || !target || MH_EnableHook(target) != MH_OK) {
        g_installed.store(false);
        return false;
    }

    g_hookTarget = target;
    logger::logMessage(GRML_LOG_INFO, "SaveEvents", "Centralized CreateFileW save lifecycle hook installed");
    return true;
}

void shutdown()
{
    if (!g_installed.exchange(false)) return;
    if (g_hookTarget) {
        MH_DisableHook(g_hookTarget);
        MH_RemoveHook(g_hookTarget);
        g_hookTarget = nullptr;
    }
    if (g_sessionThread.joinable()) {
        {
            std::lock_guard lock(g_sessionMutex);
            g_sessionStop = true;
        }
        g_sessionCv.notify_all();
        g_sessionThread.join();
    }
}

void registerLoadUnload(GrmlSaveEventFn onLoad, GrmlSaveEventFn onUnload, void* userData)
{
    if (!onLoad && !onUnload) return;
    std::lock_guard lock(g_subMutex);
    g_loadUnloadSubs.push_back({onLoad, onUnload, userData});
}

void registerWrite(GrmlSaveEventFn onWrite, void* userData)
{
    if (!onWrite) return;
    std::lock_guard lock(g_subMutex);
    g_writeSubs.push_back({onWrite, userData});
}

void notifyInGameState(bool inGame)
{
    const bool prev = g_wasInGame.exchange(inGame, std::memory_order_relaxed);
    if (prev && !inGame) {
        {
            std::lock_guard lock(g_dirMutex);
            g_activeSaveDir.clear();
        }
        invokeUnloadCallbacks();
        logger::logMessage(GRML_LOG_INFO, "SaveEvents", "Returned to Main Menu — notified save unload subscribers");
    }
}

} // namespace grml::save_events
