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
using PFN_GetFileAttributesExA = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using PFN_FindFirstFileW       = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using PFN_FindFirstFileA       = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using PFN_FindFirstFileExW     = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using PFN_CreateDirectoryW     = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using PFN_CreateDirectoryA     = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
using PFN_DeleteFileW          = BOOL(WINAPI*)(LPCWSTR);
using PFN_DeleteFileA          = BOOL(WINAPI*)(LPCSTR);
using PFN_RemoveDirectoryW     = BOOL(WINAPI*)(LPCWSTR);
using PFN_RemoveDirectoryA     = BOOL(WINAPI*)(LPCSTR);
using PFN_MoveFileW            = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using PFN_MoveFileExW          = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);

PFN_CreateFileA          g_origCreateFileA          = nullptr;
PFN_CreateFile2          g_origCreateFile2          = nullptr;
PFN_GetFileAttributesW   g_origGetFileAttributesW   = nullptr;
PFN_GetFileAttributesA   g_origGetFileAttributesA   = nullptr;
PFN_GetFileAttributesExW g_origGetFileAttributesExW = nullptr;
PFN_GetFileAttributesExA g_origGetFileAttributesExA = nullptr;
PFN_FindFirstFileW       g_origFindFirstFileW       = nullptr;
PFN_FindFirstFileA       g_origFindFirstFileA       = nullptr;
PFN_FindFirstFileExW     g_origFindFirstFileExW     = nullptr;
PFN_CreateDirectoryW     g_origCreateDirectoryW     = nullptr;
PFN_CreateDirectoryA     g_origCreateDirectoryA     = nullptr;
PFN_DeleteFileW          g_origDeleteFileW          = nullptr;
PFN_DeleteFileA          g_origDeleteFileA          = nullptr;
PFN_RemoveDirectoryW     g_origRemoveDirectoryW     = nullptr;
PFN_RemoveDirectoryA     g_origRemoveDirectoryA     = nullptr;
PFN_MoveFileW            g_origMoveFileW            = nullptr;
PFN_MoveFileExW          g_origMoveFileExW          = nullptr;

std::vector<void*>    g_hookTargets;
std::atomic<bool>     g_installed{false};
std::atomic<bool>     g_active{false};
std::atomic<bool>     g_devActive{false};
std::atomic<uint64_t> g_redirectHits{0};
std::atomic<uint64_t> g_devRedirectHits{0};

struct DevRule {
    bool    active{false};
    bool    isSave{false};
    const wchar_t* segment{nullptr};
    size_t  segmentLen{0};
    wchar_t targetW[kNormBufCap]{};
    size_t  targetLenW{0};
};

DevRule g_devRules[3]{};

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

[[nodiscard]] const wchar_t* findSegmentBoundary(const wchar_t* path,
                                                 const wchar_t* needle,
                                                 size_t needleLen) noexcept
{
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (toLowerAscii(*p) != needle[0]) continue;
        if (p != path && !isSep(p[-1])) continue;
        size_t i = 1;
        for (; i < needleLen; ++i) {
            const wchar_t ch = p[i];
            if (ch == L'\0') break;
            const wchar_t normCh = (ch == L'/') ? L'\\' : toLowerAscii(ch);
            if (normCh != needle[i]) break;
        }
        if (i == needleLen && (p[needleLen] == L'\0' || isSep(p[needleLen]))) {
            return p;
        }
    }
    return nullptr;
}

[[nodiscard]] bool startsWithPathPrefix(const wchar_t* path,
                                        const wchar_t* prefix,
                                        size_t prefixLen) noexcept
{
    if (prefixLen == 0) return false;
    for (size_t i = 0; i < prefixLen; ++i) {
        const wchar_t a = (path[i] == L'/') ? L'\\' : toLowerAscii(path[i]);
        const wchar_t b = (prefix[i] == L'/') ? L'\\' : toLowerAscii(prefix[i]);
        if (a == L'\0' || a != b) return false;
    }
    return path[prefixLen] == L'\0' || isSep(path[prefixLen]);
}

[[nodiscard]] const char* tryResolveDevA(
    const char* lpFileName,
    wchar_t (&wideIn)[kNormBufCap],
    wchar_t (&outW)[kNormBufCap],
    char (&outA)[kNormBufCap]) noexcept
{
    if (!lpFileName) return nullptr;
    const int wLen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wideIn, static_cast<int>(kNormBufCap));
    if (wLen <= 0) return nullptr;
    const wchar_t* resolvedW = tryResolveDevPathW(wideIn, outW);
    if (!resolvedW) return nullptr;
    const int aLen = WideCharToMultiByte(CP_ACP, 0, resolvedW, -1, outA, static_cast<int>(kNormBufCap), nullptr, nullptr);
    return (aLen > 0) ? outA : nullptr;
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
    wchar_t wideIn[kNormBufCap];
    wchar_t normBuf[kNormBufCap];
    if (isReadOnly) {
        if (const wchar_t* redirected = tryResolveA(lpFileName, wideIn, normBuf)) {
            ScopedBypass bypass;
            return CreateFileW(redirected, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                               dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
        }
    }
    if (g_devActive.load(std::memory_order_relaxed) && lpFileName) {
        const int wLen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wideIn, static_cast<int>(kNormBufCap));
        if (wLen > 0) {
            if (const wchar_t* redirected = tryResolveDevPathW(wideIn, normBuf)) {
                ScopedBypass bypass;
                return CreateFileW(redirected, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                   dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            }
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
    wchar_t normBuf[kNormBufCap];
    bool redirected = false;
    if (isReadOnly) {
        if (const wchar_t* r = tryResolveReadOnlyW(lpFileName, normBuf)) {
            lpFileName = r;
            redirected = true;
        }
    }
    if (!redirected) {
        if (const wchar_t* r = tryResolveDevPathW(lpFileName, normBuf)) {
            lpFileName = r;
        }
    }
    return g_origCreateFile2(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams);
}

DWORD WINAPI detourGetFileAttributesW(LPCWSTR lpFileName)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* r = tryResolveReadOnlyW(lpFileName, normBuf)) {
        lpFileName = r;
    } else if (const wchar_t* d = tryResolveDevPathW(lpFileName, normBuf)) {
        lpFileName = d;
    }
    return g_origGetFileAttributesW(lpFileName);
}

DWORD WINAPI detourGetFileAttributesA(LPCSTR lpFileName)
{
    wchar_t wideIn[kNormBufCap];
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* r = tryResolveA(lpFileName, wideIn, normBuf)) {
        ScopedBypass bypass;
        return GetFileAttributesW(r);
    }
    if (g_devActive.load(std::memory_order_relaxed) && lpFileName) {
        const int wLen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wideIn, static_cast<int>(kNormBufCap));
        if (wLen > 0) {
            if (const wchar_t* d = tryResolveDevPathW(wideIn, normBuf)) {
                ScopedBypass bypass;
                return GetFileAttributesW(d);
            }
        }
    }
    return g_origGetFileAttributesA(lpFileName);
}

BOOL WINAPI detourGetFileAttributesExW(LPCWSTR                lpFileName,
                                       GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                       LPVOID                 lpFileInformation)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* r = tryResolveReadOnlyW(lpFileName, normBuf)) {
        lpFileName = r;
    } else if (const wchar_t* d = tryResolveDevPathW(lpFileName, normBuf)) {
        lpFileName = d;
    }
    return g_origGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

BOOL WINAPI detourGetFileAttributesExA(LPCSTR                 lpFileName,
                                       GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                       LPVOID                 lpFileInformation)
{
    wchar_t wideIn[kNormBufCap];
    wchar_t normBuf[kNormBufCap];
    if (g_devActive.load(std::memory_order_relaxed) && lpFileName) {
        const int wLen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wideIn, static_cast<int>(kNormBufCap));
        if (wLen > 0) {
            if (const wchar_t* d = tryResolveDevPathW(wideIn, normBuf)) {
                ScopedBypass bypass;
                return GetFileAttributesExW(d, fInfoLevelId, lpFileInformation);
            }
        }
    }
    return g_origGetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
}

HANDLE WINAPI detourFindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* d = tryResolveDevPathW(lpFileName, normBuf)) {
        lpFileName = d;
    }
    return g_origFindFirstFileW(lpFileName, lpFindFileData);
}

HANDLE WINAPI detourFindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    wchar_t wideIn[kNormBufCap];
    wchar_t outW[kNormBufCap];
    char    outA[kNormBufCap];
    if (tryResolveDevA(lpFileName, wideIn, outW, outA)) {
        lpFileName = outA;
    }
    return g_origFindFirstFileA(lpFileName, lpFindFileData);
}

HANDLE WINAPI detourFindFirstFileExW(LPCWSTR            lpFileName,
                                     FINDEX_INFO_LEVELS fInfoLevelId,
                                     LPVOID             lpFindFileData,
                                     FINDEX_SEARCH_OPS  fSearchOp,
                                     LPVOID             lpSearchFilter,
                                     DWORD              dwAdditionalFlags)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* d = tryResolveDevPathW(lpFileName, normBuf)) {
        lpFileName = d;
    }
    return g_origFindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData,
                                  fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

BOOL WINAPI detourCreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* d = tryResolveDevPathW(lpPathName, normBuf)) {
        lpPathName = d;
    }
    return g_origCreateDirectoryW(lpPathName, lpSecurityAttributes);
}

BOOL WINAPI detourCreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    wchar_t wideIn[kNormBufCap];
    wchar_t normBuf[kNormBufCap];
    if (g_devActive.load(std::memory_order_relaxed) && lpPathName) {
        const int wLen = MultiByteToWideChar(CP_ACP, 0, lpPathName, -1, wideIn, static_cast<int>(kNormBufCap));
        if (wLen > 0) {
            if (const wchar_t* d = tryResolveDevPathW(wideIn, normBuf)) {
                ScopedBypass bypass;
                return CreateDirectoryW(d, lpSecurityAttributes);
            }
        }
    }
    return g_origCreateDirectoryA(lpPathName, lpSecurityAttributes);
}

BOOL WINAPI detourDeleteFileW(LPCWSTR lpFileName)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* d = tryResolveDevPathW(lpFileName, normBuf)) {
        lpFileName = d;
    }
    return g_origDeleteFileW(lpFileName);
}

BOOL WINAPI detourDeleteFileA(LPCSTR lpFileName)
{
    wchar_t wideIn[kNormBufCap];
    wchar_t normBuf[kNormBufCap];
    if (g_devActive.load(std::memory_order_relaxed) && lpFileName) {
        const int wLen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wideIn, static_cast<int>(kNormBufCap));
        if (wLen > 0) {
            if (const wchar_t* d = tryResolveDevPathW(wideIn, normBuf)) {
                ScopedBypass bypass;
                return DeleteFileW(d);
            }
        }
    }
    return g_origDeleteFileA(lpFileName);
}

BOOL WINAPI detourRemoveDirectoryW(LPCWSTR lpPathName)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* d = tryResolveDevPathW(lpPathName, normBuf)) {
        lpPathName = d;
    }
    return g_origRemoveDirectoryW(lpPathName);
}

BOOL WINAPI detourRemoveDirectoryA(LPCSTR lpPathName)
{
    wchar_t wideIn[kNormBufCap];
    wchar_t normBuf[kNormBufCap];
    if (g_devActive.load(std::memory_order_relaxed) && lpPathName) {
        const int wLen = MultiByteToWideChar(CP_ACP, 0, lpPathName, -1, wideIn, static_cast<int>(kNormBufCap));
        if (wLen > 0) {
            if (const wchar_t* d = tryResolveDevPathW(wideIn, normBuf)) {
                ScopedBypass bypass;
                return RemoveDirectoryW(d);
            }
        }
    }
    return g_origRemoveDirectoryA(lpPathName);
}

BOOL WINAPI detourMoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName)
{
    wchar_t buf1[kNormBufCap];
    wchar_t buf2[kNormBufCap];
    if (const wchar_t* d1 = tryResolveDevPathW(lpExistingFileName, buf1)) lpExistingFileName = d1;
    if (const wchar_t* d2 = tryResolveDevPathW(lpNewFileName, buf2)) lpNewFileName = d2;
    return g_origMoveFileW(lpExistingFileName, lpNewFileName);
}

BOOL WINAPI detourMoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags)
{
    wchar_t buf1[kNormBufCap];
    wchar_t buf2[kNormBufCap];
    if (const wchar_t* d1 = tryResolveDevPathW(lpExistingFileName, buf1)) lpExistingFileName = d1;
    if (const wchar_t* d2 = tryResolveDevPathW(lpNewFileName, buf2)) lpNewFileName = d2;
    return g_origMoveFileExW(lpExistingFileName, lpNewFileName, dwFlags);
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

const wchar_t* tryResolveDevPathW(const wchar_t* lpFileName,
                                  wchar_t (&outBuf)[kNormBufCap],
                                  bool* outIsSavePath) noexcept
{
    if (g_bypassDepth > 0 || !g_devActive.load(std::memory_order_relaxed) || !lpFileName || !*lpFileName) {
        return nullptr;
    }
    for (const auto& rule : g_devRules) {
        if (!rule.active) continue;
        if (startsWithPathPrefix(lpFileName, rule.targetW, rule.targetLenW)) {
            return nullptr;
        }
        const wchar_t* seg = findSegmentBoundary(lpFileName, rule.segment, rule.segmentLen);
        if (!seg) continue;

        const wchar_t* tail = seg + rule.segmentLen;
        if (rule.targetLenW + 1 >= kNormBufCap) return nullptr;
        std::wmemcpy(outBuf, rule.targetW, rule.targetLenW);
        size_t pos = rule.targetLenW;
        for (const wchar_t* p = tail; *p != L'\0'; ++p) {
            const wchar_t ch = (*p == L'/') ? L'\\' : *p;
            if (ch == L'\\' && pos > 0 && outBuf[pos - 1] == L'\\') continue;
            if (pos + 1 >= kNormBufCap) return nullptr;
            outBuf[pos++] = ch;
        }
        outBuf[pos] = L'\0';
        if (outIsSavePath) *outIsSavePath = rule.isSave;
        g_devRedirectHits.fetch_add(1, std::memory_order_relaxed);
        return outBuf;
    }
    return nullptr;
}

bool install(const std::filesystem::path& loaderDir,
             const std::filesystem::path& gameDir,
             const DevPathOverrides& devOverrides)
{
    if (g_installed.exchange(true)) return true;

    auto& vfs = vfs::VfsIndex::get();
    vfs.build(loaderDir, gameDir, true, devOverrides.workshopRoot, devOverrides.overridesRoot);

    logger::logMessage(GRML_LOG_INFO, "MediaVFS",
                       "Asset Overrides indexed: packs=%zu enabled_files=%zu conflicts=%zu active=%s",
                       vfs.packs().size(), vfs.indexedFileCount(), vfs.conflictCount(),
                       vfs.isEnabled() ? "true" : "false");

    bool anyDevRule = false;
    if (devOverrides.enabled) {
        auto initRule = [&](size_t idx, const wchar_t* seg, size_t segLen,
                            const std::filesystem::path& target, bool isSave) {
            if (target.empty()) return;
            std::wstring norm = target.lexically_normal().wstring();
            while (!norm.empty() && (norm.back() == L'\\' || norm.back() == L'/')) norm.pop_back();
            if (norm.empty() || norm.size() + 2 >= kNormBufCap) return;
            g_devRules[idx].active     = true;
            g_devRules[idx].isSave     = isSave;
            g_devRules[idx].segment    = seg;
            g_devRules[idx].segmentLen = segLen;
            g_devRules[idx].targetLenW = norm.size();
            std::wmemcpy(g_devRules[idx].targetW, norm.c_str(), norm.size() + 1);
            anyDevRule = true;
        };
        initRule(0, L"media_soviet\\save",         17, devOverrides.saveRoot,        true);
        initRule(1, L"media_soviet\\workshop_wip", 25, devOverrides.workshopWipRoot, false);
        initRule(2, L"workshop\\content\\784150",  23, devOverrides.workshopRoot,    false);
    }

    if (vfs.indexedFileCount() == 0 && !anyDevRule) {
        // Zero overhead when no override packs or dev path overrides are enabled
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

    if (anyDevRule) {
        hookWin32Api(kMod, "GetFileAttributesExA", reinterpret_cast<void*>(detourGetFileAttributesExA),
                     reinterpret_cast<void**>(&g_origGetFileAttributesExA));
        hookWin32Api(kMod, "FindFirstFileW", reinterpret_cast<void*>(detourFindFirstFileW),
                     reinterpret_cast<void**>(&g_origFindFirstFileW));
        hookWin32Api(kMod, "FindFirstFileA", reinterpret_cast<void*>(detourFindFirstFileA),
                     reinterpret_cast<void**>(&g_origFindFirstFileA));
        hookWin32Api(kMod, "FindFirstFileExW", reinterpret_cast<void*>(detourFindFirstFileExW),
                     reinterpret_cast<void**>(&g_origFindFirstFileExW));
        hookWin32Api(kMod, "CreateDirectoryW", reinterpret_cast<void*>(detourCreateDirectoryW),
                     reinterpret_cast<void**>(&g_origCreateDirectoryW));
        hookWin32Api(kMod, "CreateDirectoryA", reinterpret_cast<void*>(detourCreateDirectoryA),
                     reinterpret_cast<void**>(&g_origCreateDirectoryA));
        hookWin32Api(kMod, "DeleteFileW", reinterpret_cast<void*>(detourDeleteFileW),
                     reinterpret_cast<void**>(&g_origDeleteFileW));
        hookWin32Api(kMod, "DeleteFileA", reinterpret_cast<void*>(detourDeleteFileA),
                     reinterpret_cast<void**>(&g_origDeleteFileA));
        hookWin32Api(kMod, "RemoveDirectoryW", reinterpret_cast<void*>(detourRemoveDirectoryW),
                     reinterpret_cast<void**>(&g_origRemoveDirectoryW));
        hookWin32Api(kMod, "RemoveDirectoryA", reinterpret_cast<void*>(detourRemoveDirectoryA),
                     reinterpret_cast<void**>(&g_origRemoveDirectoryA));
        hookWin32Api(kMod, "MoveFileW", reinterpret_cast<void*>(detourMoveFileW),
                     reinterpret_cast<void**>(&g_origMoveFileW));
        hookWin32Api(kMod, "MoveFileExW", reinterpret_cast<void*>(detourMoveFileExW),
                     reinterpret_cast<void**>(&g_origMoveFileExW));
        g_devActive.store(true, std::memory_order_release);
    }

    if (vfs.indexedFileCount() > 0) {
        g_active.store(true, std::memory_order_relaxed);
    }
    return true;
}

void shutdown()
{
    if (!g_installed.exchange(false)) return;
    g_active.store(false, std::memory_order_relaxed);
    g_devActive.store(false, std::memory_order_relaxed);
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

uint64_t devRedirectHits() noexcept
{
    return g_devRedirectHits.load(std::memory_order_relaxed);
}

bool isHookActive() noexcept
{
    return g_active.load(std::memory_order_relaxed);
}

bool isDevRedirectActive() noexcept
{
    return g_devActive.load(std::memory_order_relaxed);
}

} // namespace grml::vfs_redirect
