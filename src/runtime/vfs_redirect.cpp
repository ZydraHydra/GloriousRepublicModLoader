#include "runtime/vfs_redirect.hpp"
#include "runtime/ring_logger.hpp"
#include "common/vfs_index.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <string_view>
#include <vector>

namespace grml::vfs_redirect {

namespace {

using PFN_CreateFileA          = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PFN_CreateFile2          = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using PFN_GetFileAttributesW   = DWORD(WINAPI*)(LPCWSTR);
using PFN_GetFileAttributesA   = DWORD(WINAPI*)(LPCSTR);
using PFN_GetFileAttributesExW = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

PFN_CreateFileA          g_origCreateFileA          = nullptr;
PFN_CreateFile2          g_origCreateFile2          = nullptr;
PFN_GetFileAttributesW   g_origGetFileAttributesW   = nullptr;
PFN_GetFileAttributesA   g_origGetFileAttributesA   = nullptr;
PFN_GetFileAttributesExW g_origGetFileAttributesExW = nullptr;

std::vector<void*>    g_hookTargets;
std::atomic<bool>     g_installed{false};
std::atomic<bool>     g_active{false};
std::atomic<uint64_t> g_redirectHits{0};

thread_local int g_bypassDepth = 0;

struct ScopedBypass {
    ScopedBypass() noexcept { ++g_bypassDepth; }
    ~ScopedBypass() noexcept { --g_bypassDepth; }
};

[[nodiscard]] inline wchar_t toLowerAscii(wchar_t c) noexcept
{
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

[[nodiscard]] inline bool isSep(wchar_t c) noexcept
{
    return c == L'\\' || c == L'/';
}

[[nodiscard]] const wchar_t* findMediaSovietSegment(const wchar_t* path) noexcept
{
    constexpr wchar_t kNeedle[]  = L"media_soviet";
    constexpr size_t  kNeedleLen = 12;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (toLowerAscii(*p) != L'm') continue;
        if (p != path && !isSep(p[-1])) continue;
        size_t i = 1;
        for (; i < kNeedleLen; ++i) {
            const wchar_t ch = p[i];
            if (ch == L'\0' || toLowerAscii(ch) != kNeedle[i]) break;
        }
        if (i == kNeedleLen && isSep(p[kNeedleLen])) return p;
    }
    return nullptr;
}

[[nodiscard]] const wchar_t* tryResolveA(
    const char* lpFileName,
    wchar_t (&wideIn)[kNormBufCap],
    wchar_t (&normBuf)[kNormBufCap]) noexcept
{
    if (!lpFileName) return nullptr;
    const int wLen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wideIn, static_cast<int>(kNormBufCap));
    if (wLen <= 0) return nullptr;
    return tryResolveReadOnlyW(wideIn, normBuf);
}

HANDLE WINAPI detourCreateFileA(LPCSTR                lpFileName,
                                DWORD                 dwDesiredAccess,
                                DWORD                 dwShareMode,
                                LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                DWORD                 dwCreationDisposition,
                                DWORD                 dwFlagsAndAttributes,
                                HANDLE                hTemplateFile)
{
    const bool isReadOnly = ((dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) == 0) &&
                            (dwCreationDisposition == OPEN_EXISTING);
    if (isReadOnly) {
        wchar_t wideIn[kNormBufCap];
        wchar_t normBuf[kNormBufCap];
        if (const wchar_t* redirected = tryResolveA(lpFileName, wideIn, normBuf)) {
            ScopedBypass bypass;
            return CreateFileW(redirected, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                               dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
        }
    }
    return g_origCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                             dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

HANDLE WINAPI detourCreateFile2(LPCWSTR                           lpFileName,
                                DWORD                             dwDesiredAccess,
                                DWORD                             dwShareMode,
                                DWORD                             dwCreationDisposition,
                                LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams)
{
    const bool isReadOnly = ((dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) == 0) &&
                            (dwCreationDisposition == OPEN_EXISTING);
    if (isReadOnly) {
        wchar_t normBuf[kNormBufCap];
        if (const wchar_t* redirected = tryResolveReadOnlyW(lpFileName, normBuf)) {
            lpFileName = redirected;
        }
    }
    return g_origCreateFile2(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams);
}

DWORD WINAPI detourGetFileAttributesW(LPCWSTR lpFileName)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* redirected = tryResolveReadOnlyW(lpFileName, normBuf)) {
        lpFileName = redirected;
    }
    return g_origGetFileAttributesW(lpFileName);
}

DWORD WINAPI detourGetFileAttributesA(LPCSTR lpFileName)
{
    wchar_t wideIn[kNormBufCap];
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* redirected = tryResolveA(lpFileName, wideIn, normBuf)) {
        ScopedBypass bypass;
        return GetFileAttributesW(redirected);
    }
    return g_origGetFileAttributesA(lpFileName);
}

BOOL WINAPI detourGetFileAttributesExW(LPCWSTR                lpFileName,
                                       GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                       LPVOID                 lpFileInformation)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* redirected = tryResolveReadOnlyW(lpFileName, normBuf)) {
        lpFileName = redirected;
    }
    return g_origGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

bool hookWin32Api(const wchar_t* dll, const char* fn, void* detour, void** origOut)
{
    void* target = nullptr;
    if (MH_CreateHookApiEx(dll, fn, detour, origOut, &target) != MH_OK || !target) return false;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return false;
    }
    g_hookTargets.push_back(target);
    return true;
}

} // namespace

const wchar_t* tryResolveReadOnlyW(const wchar_t* lpFileName,
                                   wchar_t (&normBuf)[kNormBufCap]) noexcept
{
    const auto& vfs = vfs::VfsIndex::get();
    if (g_bypassDepth > 0 || !vfs.isBuilt() || !vfs.isEnabled() || !lpFileName) {
        return nullptr;
    }
    const auto& vfsMap = vfs.map();
    if (vfsMap.empty()) return nullptr;

    const wchar_t* seg = findMediaSovietSegment(lpFileName);
    if (!seg) return nullptr;

    size_t len = 0;
    for (const wchar_t* p = seg; *p != L'\0'; ++p) {
        wchar_t ch = (*p == L'/') ? L'\\' : toLowerAscii(*p);
        if (ch == L'\\' && len > 0 && normBuf[len - 1] == L'\\') continue;
        if (len + 1 >= kNormBufCap) return nullptr;
        normBuf[len++] = ch;
    }
    if (len == 0) return nullptr;
    normBuf[len] = L'\0';

    const auto it = vfsMap.find(std::wstring_view(normBuf, len));
    if (it == vfsMap.end()) return nullptr;

    g_redirectHits.fetch_add(1, std::memory_order_relaxed);
    return it->second.c_str();
}

bool install(const std::filesystem::path& loaderDir,
             const std::filesystem::path& gameDir)
{
    if (g_installed.exchange(true)) return true;

    auto& vfs = vfs::VfsIndex::get();
    vfs.build(loaderDir, gameDir, true);

    logger::logMessage(GRML_LOG_INFO, "MediaVFS",
                       "Asset Overrides indexed: packs=%zu enabled_files=%zu conflicts=%zu active=%s",
                       vfs.packs().size(), vfs.indexedFileCount(), vfs.conflictCount(),
                       vfs.isEnabled() ? "true" : "false");

    if (vfs.indexedFileCount() == 0) {
        // Zero overhead when no override packs are enabled
        return true;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }

    const wchar_t* kMod = GetModuleHandleW(L"kernelbase.dll") ? L"kernelbase.dll" : L"kernel32.dll";
    hookWin32Api(kMod, "CreateFileA", reinterpret_cast<void*>(detourCreateFileA),
                 reinterpret_cast<void**>(&g_origCreateFileA));
    hookWin32Api(kMod, "CreateFile2", reinterpret_cast<void*>(detourCreateFile2),
                 reinterpret_cast<void**>(&g_origCreateFile2));
    hookWin32Api(kMod, "GetFileAttributesW", reinterpret_cast<void*>(detourGetFileAttributesW),
                 reinterpret_cast<void**>(&g_origGetFileAttributesW));
    hookWin32Api(kMod, "GetFileAttributesA", reinterpret_cast<void*>(detourGetFileAttributesA),
                 reinterpret_cast<void**>(&g_origGetFileAttributesA));
    hookWin32Api(kMod, "GetFileAttributesExW", reinterpret_cast<void*>(detourGetFileAttributesExW),
                 reinterpret_cast<void**>(&g_origGetFileAttributesExW));

    g_active.store(true, std::memory_order_relaxed);
    return true;
}

void shutdown()
{
    if (!g_installed.exchange(false)) return;
    g_active.store(false, std::memory_order_relaxed);
    for (void* target : g_hookTargets) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    g_hookTargets.clear();
}

uint64_t redirectHits() noexcept
{
    return g_redirectHits.load(std::memory_order_relaxed);
}

bool isHookActive() noexcept
{
    return g_active.load(std::memory_order_relaxed);
}

} // namespace grml::vfs_redirect
