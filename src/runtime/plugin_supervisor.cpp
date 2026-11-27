#include "runtime/plugin_supervisor.hpp"
#include "runtime/hook_manager.hpp"
#include "runtime/overlay_hub.hpp"
#include "runtime/ring_logger.hpp"
#include "runtime/save_events.hpp"
#include "runtime/tesmio_bridge.hpp"
#include "runtime/vfs_redirect.hpp"
#include "common/ini_util.hpp"
#include "common/manifest.hpp"
#include "common/offset_resolver.hpp"
#include "common/pe_scanner.hpp"
#include "common/ui_schema.hpp"
#include "embedded_grml_offsets.hpp"
#include "grml_api.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace grml::supervisor {

namespace {

struct LoadedPluginRecord {
    DiscoveredPlugin meta;
    HMODULE          module{nullptr};
    uint32_t         loadOrder{0};
    bool             loaded{false};
    bool             failed{false};
    std::string      status;
};

struct PostScanEntry {
    GrmlPostScanFn cb{nullptr};
    void*          userData{nullptr};
};

struct CrashModuleInfo {
    HMODULE     module{nullptr};
    std::string id;
    std::string name;
    std::string dllPathUtf8;
};

std::atomic<bool>                g_bootstrapped{false};
std::mutex                       g_supMutex;
std::vector<LoadedPluginRecord>  g_loadedPlugins;
std::mutex                       g_crashModulesMutex;
std::vector<CrashModuleInfo>     g_crashModules;
std::atomic<bool>                g_inCrashFilter{false};
LPTOP_LEVEL_EXCEPTION_FILTER     g_prevExceptionFilter{nullptr};
std::vector<AobPatternRequest>   g_pendingScans;
std::vector<PostScanEntry>       g_postScanCallbacks;
bool                             g_scanExecuted{false};
std::filesystem::path            g_loaderRoot;
std::filesystem::path            g_dataRoot;
std::filesystem::path            g_gameExePath;
PeModuleInfo                     g_gamePe{};
std::string                      g_gameSha256;
std::string                      g_gameVersion{"unknown"};
const volatile uint8_t*          g_inGameFlagPtr{nullptr};
void*                            g_resourceVectorAddr{nullptr};
void*                            g_evalDistTaskAddr{nullptr};
void*                            g_resourceGetAddr{nullptr};
GrmlWrsrOffsets                  g_wrsrOffsets{};
std::vector<offsets::ResolvedOffsetEntry> g_resolvedOffsets;

struct RegisteredServiceEntry {
    std::string name;
    uint32_t    version{0};
    void*       ptr{nullptr};
};
std::mutex                          g_serviceMutex;
std::vector<RegisteredServiceEntry> g_services;

std::string                                        g_activeLanguage{"en"};
std::mutex                                         g_i18nMutex;
std::unordered_map<std::string, std::filesystem::path> g_pluginDirs;
std::unordered_map<std::string, TranslationMap>    g_pluginTranslations;
std::deque<std::string>                            g_translationStringPool;

bool isFatalCrashCode(DWORD code) noexcept
{
    return code == EXCEPTION_ACCESS_VIOLATION ||
           code == EXCEPTION_IN_PAGE_ERROR ||
           code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
           code == EXCEPTION_PRIV_INSTRUCTION ||
           code == EXCEPTION_STACK_OVERFLOW;
}

LONG WINAPI grmlUnhandledExceptionFilter(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord || !isFatalCrashCode(ep->ExceptionRecord->ExceptionCode)) {
        return g_prevExceptionFilter ? g_prevExceptionFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_inCrashFilter.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);

    HMODULE faultMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &faultMod);

    wchar_t modPathW[MAX_PATH * 4]{};
    std::string modPathUtf8 = "(unknown)";
    std::string modFileUtf8 = "(unknown)";
    if (faultMod && GetModuleFileNameW(faultMod, modPathW, _countof(modPathW)) > 0) {
        std::filesystem::path mp(modPathW);
        modPathUtf8 = wideToUtf8(mp.wstring());
        modFileUtf8 = wideToUtf8(mp.filename().wstring());
    }
    const size_t rva = faultMod ? static_cast<size_t>(addr - reinterpret_cast<uintptr_t>(faultMod)) : 0;

    std::string suspectId;
    std::string suspectName;
    {
        std::unique_lock lock(g_crashModulesMutex, std::try_to_lock);
        if (lock.owns_lock() && faultMod) {
            for (const auto& m : g_crashModules) {
                if (m.module == faultMod) {
                    suspectId   = m.id;
                    suspectName = m.name;
                    break;
                }
            }
        }
    }

    if (!suspectId.empty()) {
        logger::logMessage(GRML_LOG_ERROR, "CrashGuard",
                           "FATAL 0x%08lX in %s (+0x%zX) -> Suspect mod: %s (%s) [%s]",
                           code, modFileUtf8.c_str(), rva,
                           suspectId.c_str(), suspectName.c_str(), modPathUtf8.c_str());
    } else {
        logger::logMessage(GRML_LOG_ERROR, "CrashGuard",
                           "FATAL 0x%08lX in module '%s' (+0x%zX) [%s]",
                           code, modFileUtf8.c_str(), rva, modPathUtf8.c_str());
    }
    logger::flush();

    return g_prevExceptionFilter ? g_prevExceptionFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// Pure-SEH wrappers (no C++ destructors inside __try)
HMODULE sehLoadLibraryW(const wchar_t* path, DWORD* outLastErr, DWORD* outExc) noexcept
{
    __try {
        HMODULE h = LoadLibraryW(path);
        if (!h && outLastErr) *outLastErr = GetLastError();
        return h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outExc) *outExc = GetExceptionCode();
        return nullptr;
    }
}

bool sehCallGrmlInit(GrmlPluginInitFn fn, const GrmlHostApi* host, DWORD* outExc) noexcept
{
    __try {
        fn(host);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outExc) *outExc = GetExceptionCode();
        return false;
    }
}

bool sehCallPostScan(GrmlPostScanFn fn, void* userData, DWORD* outExc) noexcept
{
    __try {
        fn(userData);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outExc) *outExc = GetExceptionCode();
        return false;
    }
}

bool sehFreeLibrary(HMODULE mod, DWORD* outExc) noexcept
{
    __try {
        return FreeLibrary(mod) != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outExc) *outExc = GetExceptionCode();
        return false;
    }
}

// ── GrmlHostApi Implementation ───────────────────────────────────────────────
int32_t GRML_CALL hostRegisterPatternScan(const char* pattern, void** outAddr)
{
    if (!pattern || !*pattern || !outAddr) return 0;
    *outAddr = nullptr;

    std::lock_guard lock(g_supMutex);
    if (g_scanExecuted && g_gamePe.textBase && g_gamePe.textSize > 0) {
        // Late registration fallback (single-pattern pass)
        std::vector<AobPatternRequest> single;
        single.push_back({"plugin_late_aob", pattern, outAddr, false});
        runSinglePassAobScan(g_gamePe.textBase, g_gamePe.textSize, single);
        return (*outAddr != nullptr) ? 1 : 0;
    }

    char nameBuf[48]{};
    std::snprintf(nameBuf, sizeof(nameBuf), "plugin_aob_%zu", g_pendingScans.size());
    g_pendingScans.push_back({nameBuf, pattern, outAddr, false});
    return 1;
}

int32_t GRML_CALL hostRegisterPostScanCallback(GrmlPostScanFn cb, void* userData)
{
    if (!cb) return 0;
    std::lock_guard lock(g_supMutex);
    if (g_scanExecuted) {
        DWORD exc = 0;
        return sehCallPostScan(cb, userData, &exc) ? 1 : 0;
    }
    g_postScanCallbacks.push_back({cb, userData});
    return 1;
}

int32_t GRML_CALL hostRegisterSaveEvents(GrmlSaveEventFn onLoad, GrmlSaveEventFn onUnload, void* userData)
{
    save_events::registerLoadUnload(onLoad, onUnload, userData);
    return 1;
}

int32_t GRML_CALL hostRegisterSaveWriteEvent(GrmlSaveEventFn onWrite, void* userData)
{
    save_events::registerWrite(onWrite, userData);
    return 1;
}

void GRML_CALL hostLog(int32_t level, const char* pluginId, const char* fmt, ...)
{
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    logger::logMessageV(level, pluginId ? pluginId : "Plugin", fmt, args);
    va_end(args);
}

int32_t GRML_CALL hostGetGameInfo(GrmlGameInfo* outInfo)
{
    if (!outInfo || outInfo->struct_size < sizeof(GrmlGameInfo)) return 0;
    outInfo->abi_version    = GRML_API_VERSION;
    outInfo->image_base     = reinterpret_cast<uintptr_t>(g_gamePe.base);
    outInfo->image_size     = static_cast<uint32_t>(g_gamePe.imageSize);
    outInfo->pe_timestamp   = g_gamePe.timeDateStamp;
    outInfo->game_version   = g_gameVersion.c_str();
    outInfo->sha256         = g_gameSha256.c_str();
    outInfo->loader_version = GRML_BUILD_VERSION;
    return 1;
}

int32_t GRML_CALL hostGetPluginConfigPath(
    const char*    pluginId,
    const wchar_t* defaultIniPath,
    wchar_t*       outBuf,
    uint32_t       capWchars)
{
    if (!pluginId || !*pluginId || !outBuf || capWchars == 0 || g_dataRoot.empty()) return 0;

    std::string safeName;
    for (const char* p = pluginId; *p; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            safeName.push_back(c);
        } else {
            safeName.push_back('_');
        }
    }
    if (safeName.empty()) return 0;

    std::error_code ec;
    const std::filesystem::path cfgDir = g_dataRoot / L"configs";
    std::filesystem::create_directories(cfgDir, ec);

    const std::filesystem::path targetPath = cfgDir / utf8ToWide(safeName + ".ini");
    if (defaultIniPath && *defaultIniPath && !std::filesystem::exists(targetPath, ec)) {
        if (std::filesystem::exists(defaultIniPath, ec)) {
            std::filesystem::copy_file(defaultIniPath, targetPath,
                                       std::filesystem::copy_options::skip_existing, ec);
        }
    }

    const std::wstring& w = targetPath.wstring();
    if (w.size() + 1 > capWchars) return 0;
    std::wmemcpy(outBuf, w.c_str(), w.size() + 1);
    return 1;
}

int32_t GRML_CALL hostRegisterService(const char* name, uint32_t version, void* servicePtr)
{
    if (!name || !*name || !servicePtr) return 0;
    std::lock_guard lock(g_serviceMutex);
    for (auto& s : g_services) {
        if (s.name == name && s.version == version) {
            s.ptr = servicePtr;
            return 1;
        }
    }
    g_services.push_back({name, version, servicePtr});
    return 1;
}

void* GRML_CALL hostGetService(const char* name, uint32_t version)
{
    if (!name || !*name) return nullptr;
    std::lock_guard lock(g_serviceMutex);
    for (const auto& s : g_services) {
        if (s.name == name && s.version == version) return s.ptr;
    }
    return nullptr;
}

int32_t GRML_CALL hostResolveNamedOffset(const char* sectionName, uintptr_t* outValue)
{
    if (!sectionName || !*sectionName || !outValue) return 0;
    *outValue = 0;
    std::string want = sectionName;
    for (char& c : want) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    std::lock_guard lock(g_supMutex);
    for (const auto& e : g_resolvedOffsets) {
        if (e.def.name == want &&
            (e.status == offsets::OffsetStatus::Ok || e.status == offsets::OffsetStatus::Cached)) {
            *outValue = e.resolvedValue;
            return 1;
        }
    }
    return 0;
}

const char* GRML_CALL hostGetLanguage(void)
{
    return g_activeLanguage.empty() ? "en" : g_activeLanguage.c_str();
}

int32_t GRML_CALL hostLoadTranslations(const char* pluginId, const wchar_t* localesDir)
{
    if (!pluginId || !*pluginId) return 0;
    std::filesystem::path dir;
    if (localesDir && *localesDir) {
        dir = std::filesystem::path(localesDir);
    } else {
        std::lock_guard lock(g_i18nMutex);
        if (const auto it = g_pluginDirs.find(pluginId); it != g_pluginDirs.end()) {
            dir = it->second / L"locales";
        }
    }
    if (dir.empty()) return 0;

    TranslationMap dict;
    if (!loadModLocaleDictionary(dir, g_activeLanguage, dict)) {
        return 0;
    }

    std::lock_guard lock(g_i18nMutex);
    g_pluginTranslations[pluginId] = std::move(dict);
    return 1;
}

const char* GRML_CALL hostTranslate(const char* pluginId, const char* key)
{
    if (!key) return "";
    if (!pluginId || !*pluginId) return key;

    std::lock_guard lock(g_i18nMutex);
    const auto pit = g_pluginTranslations.find(pluginId);
    if (pit == g_pluginTranslations.end()) return key;
    const auto kit = pit->second.find(key);
    if (kit == pit->second.end()) return key;

    // Return a stable pointer from g_translationStringPool (std::deque never
    // invalidates references on push_back) so plugins can pass it directly to ImGui.
    g_translationStringPool.push_back(kit->second);
    return g_translationStringPool.back().c_str();
}

int32_t GRML_CALL wrsrIsInGame(void)
{
    return (g_inGameFlagPtr && *g_inGameFlagPtr != 0) ? 1 : 0;
}

void* GRML_CALL wrsrResolveRipTarget(const void* instr, uint32_t dispOff, uint32_t nextIpOff)
{
    return resolveRipRelativeDisp32(const_cast<void*>(instr), dispOff, nextIpOff);
}

int32_t GRML_CALL wrsrGetSymbol(const char* symbolId, void** outAddr)
{
    if (!symbolId || !outAddr) return 0;
    *outAddr = nullptr;
    if (std::strcmp(symbolId, "wrsr.resource_vector") == 0) *outAddr = g_resourceVectorAddr;
    else if (std::strcmp(symbolId, "wrsr.evaluate_dist_task") == 0) *outAddr = g_evalDistTaskAddr;
    else if (std::strcmp(symbolId, "wrsr.resource_get") == 0) *outAddr = g_resourceGetAddr;
    else if (std::strcmp(symbolId, "wrsr.in_game_flag") == 0) *outAddr = const_cast<uint8_t*>(g_inGameFlagPtr);
    return (*outAddr != nullptr) ? 1 : 0;
}

int32_t GRML_CALL wrsrGetOffsets(GrmlWrsrOffsets* outOffsets)
{
    if (!outOffsets || outOffsets->struct_size < sizeof(GrmlWrsrOffsets)) return 0;
    *outOffsets = g_wrsrOffsets;
    return 1;
}

int32_t GRML_CALL wrsrGetTextSection(const void** outBase, uint32_t* outSize)
{
    if (!g_gamePe.textBase || g_gamePe.textSize == 0) return 0;
    if (outBase) *outBase = g_gamePe.textBase;
    if (outSize) *outSize = static_cast<uint32_t>(g_gamePe.textSize);
    return 1;
}

const GrmlWrsrApi g_wrsrApi = {
    GRML_API_VERSION,
    sizeof(GrmlWrsrApi),
    &wrsrIsInGame,
    &wrsrResolveRipTarget,
    &wrsrGetSymbol,
    &wrsrGetOffsets,
    &wrsrGetTextSection,
};

GrmlHostApi g_hostApi = {
    GRML_API_VERSION,
    sizeof(GrmlHostApi),
    nullptr, // filled with overlay::uiApi() during bootstrap
    &hostRegisterPatternScan,
    &hostRegisterPostScanCallback,
    &hostRegisterSaveEvents,
    &hostRegisterSaveWriteEvent,
    &hostLog,
    &hostGetGameInfo,
    nullptr, // filled with hooks::hookApi() during bootstrap
    &g_wrsrApi,
    &hostGetPluginConfigPath,
    &hostRegisterService,
    &hostGetService,
    &hostResolveNamedOffset,
    &hostGetLanguage,
    &hostLoadTranslations,
    &hostTranslate,
};

std::wstring formatHotkeyLabelW(uint16_t vk)
{
    if (vk >= VK_F1 && vk <= VK_F12) {
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    }
    if (vk == VK_INSERT) return L"Insert";
    if (vk == VK_HOME) return L"Home";
    if (vk == VK_END) return L"End";
    if (vk == VK_PRIOR) return L"PageUp";
    if (vk == VK_NEXT) return L"PageDown";
    if (vk == VK_PAUSE) return L"Pause";
    if (vk == VK_SCROLL) return L"ScrollLock";
    if (vk == VK_OEM_3) return L"~";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    return L"F9";
}

// ── Safe Menu Version Line Tagging (RVA-free, disp32 repoint) ────────────────
void installMenuStatusTag(const std::string& wideFmtUtf8, uint16_t overlayVk)
{
    if (wideFmtUtf8.empty() || !g_gamePe.base || !g_gamePe.textBase || g_gamePe.textSize < 8) return;

    const std::wstring targetFmt = utf8ToWide(wideFmtUtf8);
    if (targetFmt.empty()) return;

    // Locate UTF-16LE format string in readable PE sections
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_gamePe.base);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_gamePe.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const size_t needleBytes = (targetFmt.size() + 1) * sizeof(wchar_t);
    const uint8_t* needlePtr = reinterpret_cast<const uint8_t*>(targetFmt.c_str());

    uintptr_t anchorAddr = 0;
    size_t anchorCount   = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
        const size_t span = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (span < needleBytes || sec[i].VirtualAddress >= g_gamePe.imageSize) continue;
        const uint8_t* begin = g_gamePe.base + sec[i].VirtualAddress;
        const uint8_t* end   = begin + span - needleBytes;
        for (const uint8_t* p = begin; p <= end; p += sizeof(wchar_t)) {
            if (std::memcmp(p, needlePtr, needleBytes) == 0) {
                anchorAddr = reinterpret_cast<uintptr_t>(p);
                ++anchorCount;
            }
        }
    }
    if (anchorCount != 1 || anchorAddr == 0) {
        logger::logMessage(GRML_LOG_WARN, "MenuStatus",
                           "[AOB MISS] menu_version_fmt_wide matches=%zu — disabling menu status tag", anchorCount);
        return;
    }

    // Find unique RIP-relative LEA referencing anchorAddr in .text
    uint8_t* dispPtr    = nullptr;
    uint8_t* insnEndPtr = nullptr;
    size_t   leaCount   = 0;
    const uint8_t* tEnd = g_gamePe.textBase + g_gamePe.textSize;
    for (const uint8_t* p = g_gamePe.textBase; p + 7 <= tEnd; ++p) {
        const uint8_t* q = p;
        if (*q >= 0x40 && *q <= 0x4F) ++q;
        if (q + 6 > tEnd || q[0] != 0x8D || (q[1] & 0xC7u) != 0x05u) continue;
        int32_t disp = 0;
        std::memcpy(&disp, q + 2, 4);
        const uintptr_t target = reinterpret_cast<uintptr_t>(q + 6) + static_cast<intptr_t>(disp);
        if (target == anchorAddr) {
            dispPtr    = const_cast<uint8_t*>(q + 2);
            insnEndPtr = const_cast<uint8_t*>(q + 6);
            ++leaCount;
            p = q + 5;
        }
    }
    if (leaCount != 1 || !dispPtr) {
        logger::logMessage(GRML_LOG_WARN, "MenuStatus",
                           "menu_version_fmt_wide LEA xref count=%zu — disabling menu status tag", leaCount);
        return;
    }

    const std::wstring tagged = targetFmt + L" | GRML " + utf8ToWide(GRML_BUILD_VERSION) +
                                L" [" + formatHotkeyLabelW(overlayVk) + L"]";
    const size_t allocBytes   = (tagged.size() + 1) * sizeof(wchar_t);

    // Allocate within +-2 GiB of insnEndPtr
    const uintptr_t rip   = reinterpret_cast<uintptr_t>(insnEndPtr);
    const uintptr_t reach = 0x70000000ull;
    uintptr_t low         = (rip > reach) ? (rip - reach) : 0x10000ull;
    const uintptr_t high  = (rip + reach > rip) ? (rip + reach) : ~uintptr_t(0);
    low                   = (low + 0xFFFFull) & ~0xFFFFull;

    void* storage = nullptr;
    for (uintptr_t cur = low; cur < high && !storage;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi))) break;
        const uintptr_t reg  = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next = reg + mbi.RegionSize;
        if (next <= cur) break;
        if (mbi.State == MEM_FREE) {
            const uintptr_t cand = (reg + 0xFFFFull) & ~0xFFFFull;
            if (cand >= low && cand + allocBytes <= next && cand + allocBytes < high) {
                storage = VirtualAlloc(reinterpret_cast<LPVOID>(cand), allocBytes,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            }
        }
        cur = next;
    }
    if (!storage) return;

    std::memcpy(storage, tagged.c_str(), allocBytes);
    DWORD oldProt = 0;
    VirtualProtect(storage, allocBytes, PAGE_READONLY, &oldProt);

    const intptr_t newDelta = reinterpret_cast<intptr_t>(storage) - reinterpret_cast<intptr_t>(insnEndPtr);
    if (newDelta < INT32_MIN || newDelta > INT32_MAX) {
        VirtualFree(storage, 0, MEM_RELEASE);
        return;
    }

    DWORD codeProt = 0;
    if (VirtualProtect(dispPtr, 4, PAGE_EXECUTE_READWRITE, &codeProt)) {
        const int32_t d32 = static_cast<int32_t>(newDelta);
        std::memcpy(dispPtr, &d32, 4);
        FlushInstructionCache(GetCurrentProcess(), dispPtr, 4);
        VirtualProtect(dispPtr, 4, codeProt, &codeProt);
        logger::logMessage(GRML_LOG_INFO, "MenuStatus", "Patched main-menu version tag safely (| GRML %s)",
                           GRML_BUILD_VERSION);
    }
}

uintptr_t resolveResourceGet(const std::string& anchorStr)
{
    if (anchorStr.empty() || !g_gamePe.base || !g_gamePe.textBase) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_gamePe.base);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_gamePe.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const size_t nBytes = anchorStr.size() + 1;

    uintptr_t anchorAddr = 0;
    size_t count = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
        const size_t span = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (span < nBytes || sec[i].VirtualAddress >= g_gamePe.imageSize) continue;
        const uint8_t* begin = g_gamePe.base + sec[i].VirtualAddress;
        const uint8_t* end   = begin + span - nBytes;
        for (const uint8_t* p = begin; p <= end; ++p) {
            if (std::memcmp(p, anchorStr.c_str(), nBytes) == 0) {
                anchorAddr = reinterpret_cast<uintptr_t>(p);
                ++count;
            }
        }
    }
    if (count != 1 || !anchorAddr) {
        logger::logMessage(GRML_LOG_WARN, "Resolver", "[AOB MISS] resource_get_anchor count=%zu", count);
        return 0;
    }

    uint32_t funcBegin = 0, funcEnd = 0;
    size_t xrefFuncs = 0;
    const uint8_t* tEnd = g_gamePe.textBase + g_gamePe.textSize;
    for (const uint8_t* p = g_gamePe.textBase; p + 7 <= tEnd; ++p) {
        const uint8_t* q = p;
        if (*q >= 0x40 && *q <= 0x4F) ++q;
        if (q + 6 > tEnd || (q[0] != 0x8D && q[0] != 0x8B) || (q[1] & 0xC7u) != 0x05u) continue;
        int32_t disp = 0;
        std::memcpy(&disp, q + 2, 4);
        if (reinterpret_cast<uintptr_t>(q + 6) + static_cast<intptr_t>(disp) != anchorAddr) continue;
        const uint32_t rva = static_cast<uint32_t>(p - g_gamePe.base);
        uint32_t b = 0, e = 0;
        if (lookupRuntimeFunctionRva(g_gamePe, rva, b, e)) {
            if (xrefFuncs == 0) {
                funcBegin = b;
                funcEnd   = e;
                xrefFuncs = 1;
            } else if (b != funcBegin) {
                xrefFuncs = 2;
                break;
            }
        }
    }
    if (xrefFuncs == 1 && funcBegin != 0) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(g_gamePe.base + funcBegin);
        logger::logMessage(GRML_LOG_INFO, "Resolver", "Resolved wrsr.resource_get @ RVA 0x%X (size 0x%X)",
                           funcBegin, funcEnd - funcBegin);
        return addr;
    }
    return 0;
}

uint16_t parseHotkeyVk(const std::string& rawName)
{
    std::string up;
    up.reserve(rawName.size());
    for (char c : rawName) {
        if (c == ' ' || c == '\t' || c == '_') continue;
        up.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c);
    }
    if (up.size() >= 2 && up[0] == 'F') {
        const int n = std::atoi(up.c_str() + 1);
        if (n >= 1 && n <= 12) return static_cast<uint16_t>(VK_F1 + (n - 1));
    }
    if (up == "INSERT" || up == "INS") return VK_INSERT;
    if (up == "HOME") return VK_HOME;
    if (up == "END") return VK_END;
    if (up == "PAGEUP" || up == "PGUP") return VK_PRIOR;
    if (up == "PAGEDOWN" || up == "PGDN") return VK_NEXT;
    if (up == "PAUSE") return VK_PAUSE;
    if (up == "SCROLLLOCK") return VK_SCROLL;
    if (up == "TILDE" || up == "`" || up == "~") return VK_OEM_3;
    if (up.size() == 1 && ((up[0] >= 'A' && up[0] <= 'Z') || (up[0] >= '0' && up[0] <= '9'))) {
        return static_cast<uint16_t>(up[0]);
    }
    return VK_F9;
}

} // namespace

bool bootstrap(const std::filesystem::path& loaderRootDir)
{
    if (g_bootstrapped.exchange(true)) return true;

    g_loaderRoot = loaderRootDir;
    const std::filesystem::path dataRoot = getAppDataDir(g_loaderRoot);
    g_dataRoot = dataRoot;
    logger::init(dataRoot / L"grml.log");
    logger::logMessage(GRML_LOG_INFO, "Core", "Glorious Republic Mod Loader (GRML v%s) starting...",
                       GRML_BUILD_VERSION);

    // Inspect SOVIET64.exe
    wchar_t exePathBuf[MAX_PATH * 4]{};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exePathBuf, _countof(exePathBuf));
    if (exeLen > 0 && exeLen < _countof(exePathBuf)) {
        g_gameExePath = std::filesystem::path(exePathBuf, exePathBuf + exeLen);
        g_gameSha256  = computeFileSha256(g_gameExePath);
    }
    inspectLoadedPe(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)), g_gamePe);

    // Load grml_offsets.ini (embedded baseline + external/OTA override)
    IniFile offsetsIni;
    uint64_t offsetsIniHash = 0;
    offsets::loadMergedOffsetsIni(g_loaderRoot, dataRoot, generated::kEmbeddedGrmlOffsetsIni,
                                  offsetsIni, offsetsIniHash);

    if (!g_gameSha256.empty()) {
        g_gameVersion = offsetsIni.getString("builds", g_gameSha256, "unknown");
    }
    if (g_gameVersion == "unknown" && !g_gameExePath.empty()) {
        const std::string peVer = readPeFileVersion(g_gameExePath);
        if (!peVer.empty()) g_gameVersion = peVer;
    }
    logger::logMessage(GRML_LOG_INFO, "Game",
                       "SOVIET64.exe version=%s TimeDateStamp=0x%08X SizeOfImage=0x%zX EntryPoint=0x%X SHA256=%s",
                       g_gameVersion.c_str(), g_gamePe.timeDateStamp, g_gamePe.imageSize,
                       g_gamePe.addressOfEntryPoint,
                       g_gameSha256.empty() ? "(unavailable)" : g_gameSha256.c_str());

    // Load grml.ini from AppData\Local\GRML
    IniFile grmlIni;
    const auto grmlIniPath = dataRoot / L"grml.ini";
    if (!grmlIni.loadFromFile(grmlIniPath)) {
        logger::logMessage(GRML_LOG_INFO, "Config", "No grml.ini found; using defaults");
    }
    {
        std::string lang = grmlIni.getString("general", "language", "auto");
        if (lang.empty() || _stricmp(lang.c_str(), "auto") == 0) {
            lang = detectWrsrGameLanguage(g_gameExePath.parent_path());
            if (lang.empty()) lang = "en";
        }
        g_activeLanguage = std::move(lang);
        logger::logMessage(GRML_LOG_INFO, "Config", "Active UI language: %s", g_activeLanguage.c_str());
    }

    const std::filesystem::path pluginsDir =
        (_wcsicmp(g_loaderRoot.filename().c_str(), L"core") == 0 && g_loaderRoot.has_parent_path())
            ? g_loaderRoot.parent_path() / L"plugins"
            : g_loaderRoot / L"plugins";
    overlay::setPluginsDirectory(pluginsDir);

    // Install centralized hooks, CrashGuard, Media VFS & DX11 F9 hub
    g_prevExceptionFilter = SetUnhandledExceptionFilter(&grmlUnhandledExceptionFilter);
    g_hostApi.ui    = overlay::uiApi();
    g_hostApi.hooks = hooks::hookApi();
    vfs_redirect::install(dataRoot, g_gameExePath.parent_path());
    save_events::install(dataRoot);
    const uint16_t overlayVk = parseHotkeyVk(grmlIni.getString("general", "overlay_hotkey", "F9"));
    overlay::install(overlayVk);

    // Resolve wrsr.resource_get & prepare Tesmio/RML compatibility host
    const uintptr_t resGetAddr = resolveResourceGet(offsetsIni.getString("signatures", "resource_get_anchor", ""));
    g_resourceGetAddr = reinterpret_cast<void*>(resGetAddr);
    compat::initialize(g_gameExePath.parent_path(), pluginsDir, g_gameVersion, g_gameSha256, resGetAddr);

    // Resolve [profile.<ver>] WRSR struct offsets from grml_offsets.ini (with recursive base= inheritance)
    offsets::resolveWrsrProfileOffsets(offsetsIni, g_gamePe, g_wrsrOffsets);

    // Prepare declarative grml_offsets.ini entries + PE-hash RVA cache check
    const std::filesystem::path offsetCachePath = dataRoot / L"grml_offset_cache.ini";
    const std::vector<offsets::OffsetDefinition> offsetDefs = offsets::collectOffsetDefinitions(offsetsIni);
    bool allOffsetsCached = false;
    std::vector<void*> declMatchSites(offsetDefs.size(), nullptr);
    {
        std::lock_guard lock(g_supMutex);
        allOffsetsCached = offsets::tryResolveFromCache(offsetCachePath, g_gamePe, offsetsIniHash,
                                                        offsetDefs, g_resolvedOffsets);
        for (size_t i = 0; i < g_resolvedOffsets.size(); ++i) {
            if (g_resolvedOffsets[i].status != offsets::OffsetStatus::Cached) {
                g_pendingScans.push_back({g_resolvedOffsets[i].def.name,
                                          g_resolvedOffsets[i].def.pattern,
                                          &declMatchSites[i],
                                          g_resolvedOffsets[i].def.required});
            }
        }
    }
    if (allOffsetsCached && !offsetDefs.empty()) {
        logger::logMessage(GRML_LOG_INFO, "Offsets",
                           "PE-hash RVA cache hit (%zu entries verified in O(1))", offsetDefs.size());
    }

    auto hasDeclSection = [&](const char* name) {
        for (const auto& d : offsetDefs) {
            if (d.name == name) return true;
        }
        return false;
    };

    // Register GRML's own internal AOB requests into the single-pass scanner
    void* inGameFlagSite    = nullptr;
    void* priceTableSite    = nullptr;
    void* evalDistSite      = nullptr;
    void* menuLoopSite      = nullptr;
    void* menuUiStateSite   = nullptr;
    void* loadSaveHdrSite   = nullptr;
    void* loadSaveTrigSite  = nullptr;
    void* introSkipSite     = nullptr;
    const std::string inGamePat       = hasDeclSection("in_game_flag") ? std::string{} : offsetsIni.getString("signatures", "in_game_flag_aob", "");
    const std::string priceTablePat   = hasDeclSection("resource_vector") ? std::string{} : offsetsIni.getString("signatures", "price_table_aob", "");
    const std::string evalDistPat     = hasDeclSection("evaluate_dist_task") ? std::string{} : offsetsIni.getString("signatures", "dispatch_check_aob", "");
    const std::string menuLoopPat     = offsetsIni.getString("signatures", "main_menu_loop_aob", "");
    const std::string menuUiStatePat  = offsetsIni.getString("signatures", "main_menu_uistate_aob", "");
    const std::string loadSaveHdrPat  = offsetsIni.getString("signatures", "load_save_header_aob", "");
    const std::string loadSaveTrigPat = offsetsIni.getString("signatures", "load_save_trigger_aob", "");
    const bool wantIntroSkip          = grmlIni.getBool("general", "skip_intro_logos", true);
    const std::string introSkipPat    = wantIntroSkip ? offsetsIni.getString("signatures", "intro_skip_aob", "") : std::string{};
    {
        std::lock_guard lock(g_supMutex);
        if (!inGamePat.empty()) {
            g_pendingScans.push_back({"in_game_flag_aob", inGamePat, &inGameFlagSite, false});
        }
        if (!priceTablePat.empty()) {
            g_pendingScans.push_back({"price_table_aob", priceTablePat, &priceTableSite, false});
        }
        if (!evalDistPat.empty()) {
            g_pendingScans.push_back({"dispatch_check_aob", evalDistPat, &evalDistSite, false});
        }
        if (!menuLoopPat.empty()) {
            g_pendingScans.push_back({"main_menu_loop_aob", menuLoopPat, &menuLoopSite, false});
        }
        if (!menuUiStatePat.empty()) {
            g_pendingScans.push_back({"main_menu_uistate_aob", menuUiStatePat, &menuUiStateSite, false});
        }
        if (!loadSaveHdrPat.empty()) {
            g_pendingScans.push_back({"load_save_header_aob", loadSaveHdrPat, &loadSaveHdrSite, false});
        }
        if (!loadSaveTrigPat.empty()) {
            g_pendingScans.push_back({"load_save_trigger_aob", loadSaveTrigPat, &loadSaveTrigSite, false});
        }
        if (!introSkipPat.empty()) {
            g_pendingScans.push_back({"intro_skip_aob", introSkipPat, &introSkipSite, false});
        }
    }

    // Discover & topologically sort plugins
    DiscoveryResult disc = discoverAllPlugins(g_loaderRoot, g_gameExePath, grmlIni);
    for (const auto& w : disc.warnings) {
        logger::logMessage(GRML_LOG_WARN, "Discovery", "%s", w.c_str());
    }
    std::vector<std::string> orderWarnings;
    std::vector<DiscoveredPlugin> ordered = resolveLoadOrder(disc.plugins, orderWarnings);
    for (const auto& w : orderWarnings) {
        logger::logMessage(GRML_LOG_WARN, "TopoSort", "%s", w.c_str());
    }

    logger::logMessage(GRML_LOG_INFO, "Supervisor", "Discovered %zu plugin(s); %zu enabled for load",
                       disc.plugins.size(), ordered.size());

    // Phase 1: SEH-protected LoadLibraryW + RmlPluginLoad / TsmPluginInit / GrmlPluginInit
    uint32_t orderIdx = 1;
    std::vector<LoadedPluginRecord> records;
    records.reserve(ordered.size());

    for (const auto& p : ordered) {
        LoadedPluginRecord rec;
        rec.meta      = p;
        rec.loadOrder = orderIdx++;

        if (p.blockedByDependencyOrConflict) {
            rec.failed = true;
            rec.status = p.warning.empty() ? "Blocked by dependency/conflict" : p.warning;
            records.push_back(std::move(rec));
            continue;
        }

        DWORD lastErr = 0, excCode = 0;
        HMODULE mod = sehLoadLibraryW(p.dllPath.c_str(), &lastErr, &excCode);
        if (!mod) {
            rec.failed = true;
            char buf[128]{};
            if (excCode != 0) {
                std::snprintf(buf, sizeof(buf), "LoadLibraryW exception 0x%08lX", excCode);
            } else {
                std::snprintf(buf, sizeof(buf), "LoadLibraryW failed (GetLastError=%lu)", lastErr);
            }
            rec.status = buf;
            logger::logMessage(GRML_LOG_ERROR, "Supervisor", "%s: %s (%s)",
                               p.id.c_str(), rec.status.c_str(), wideToUtf8(p.dllPath.wstring()).c_str());
            records.push_back(std::move(rec));
            continue;
        }
        rec.module = mod;

        bool ok = true;
        if (p.hasRmlQuery && p.hasRmlLoad) {
            ok = compat::invokeRmlPlugin(mod, rec.meta.id, rec.meta.name, rec.meta.version, rec.meta.author, rec.status);
        }
        if (ok && (p.hasTsmInit || p.hasTsmApiVer)) {
            ok = compat::invokeTesmioInit(mod, rec.meta.id, rec.meta.name, rec.meta.version, rec.status);
        }

        auto grmlInitFn = reinterpret_cast<GrmlPluginInitFn>(GetProcAddress(mod, "GrmlPluginInit"));
        if (ok && grmlInitFn) {
            {
                std::lock_guard lock(g_i18nMutex);
                g_pluginDirs[rec.meta.id] = rec.meta.dllPath.parent_path();
            }
            DWORD initExc = 0;
            if (!sehCallGrmlInit(grmlInitFn, &g_hostApi, &initExc)) {
                ok = false;
                char buf[96]{};
                std::snprintf(buf, sizeof(buf), "GrmlPluginInit exception 0x%08lX", initExc);
                rec.status = buf;
                logger::logMessage(GRML_LOG_ERROR, "Supervisor", "%s: %s", rec.meta.id.c_str(), buf);
            } else {
                rec.status = "Loaded (GRML)";
            }
        } else if (ok && rec.status.empty()) {
            rec.status = "Loaded (Raw DLL)";
        }

        rec.loaded = ok;
        rec.failed = !ok;
        if (!ok) {
            hooks::rollbackPluginHooks(rec.meta.id);
            compat::rollbackPluginHooks(rec.meta.id);
            DWORD freeExc = 0;
            sehFreeLibrary(mod, &freeExc);
            rec.module = nullptr;
        } else {
            logger::logMessage(GRML_LOG_INFO, "Supervisor", "[#%u] Loaded %s v%s (%s)",
                               rec.loadOrder, rec.meta.name.c_str(), rec.meta.version.c_str(),
                               rec.status.c_str());
            {
                std::lock_guard lock(g_crashModulesMutex);
                g_crashModules.push_back({mod, rec.meta.id, rec.meta.name, wideToUtf8(rec.meta.dllPath.wstring())});
            }
        }
        records.push_back(std::move(rec));
    }

    // Phase 2: Single-pass .text AOB scan across uncached grml_offsets.ini + plugin requests
    {
        std::vector<AobPatternRequest> scans;
        {
            std::lock_guard lock(g_supMutex);
            scans = g_pendingScans;
            g_scanExecuted = true;
        }
        if (!scans.empty() && g_gamePe.textBase && g_gamePe.textSize > 0) {
            const AobScanSummary sum = runSinglePassAobScan(g_gamePe.textBase, g_gamePe.textSize, scans);
            logger::logMessage(GRML_LOG_INFO, "AOBScanner",
                               "Single-pass .text scan complete: total=%zu unique=%zu missing=%zu ambiguous=%zu",
                               sum.totalPatterns, sum.uniqueMatches, sum.missingCount, sum.ambiguousCount);
            for (const auto& s : scans) {
                if (s.matchCount != 1) {
                    logger::logMessage(GRML_LOG_WARN, "AOBScanner",
                                       "[AOB MISS] Pattern '%s' matched %zu time(s) (expected 1) — dependent feature disabled",
                                       s.name.c_str(), s.matchCount);
                }
            }
        }

        // Finalize declarative offset entries & update PE-hash RVA cache on miss
        {
            std::lock_guard lock(g_supMutex);
            for (size_t i = 0; i < g_resolvedOffsets.size(); ++i) {
                auto& entry = g_resolvedOffsets[i];
                if (entry.status == offsets::OffsetStatus::Cached) continue;

                size_t foundCount = 0;
                for (const auto& s : scans) {
                    if (s.outAddr == &declMatchSites[i]) {
                        foundCount = s.matchCount;
                        break;
                    }
                }
                entry.matchCount = foundCount;
                const auto* site = static_cast<const uint8_t*>(declMatchSites[i]);
                uintptr_t resolvedVal = 0;
                if (foundCount == 1 && site &&
                    offsets::applyOffsetResolveRule(g_gamePe, site, entry.def, resolvedVal)) {
                    entry.status        = offsets::OffsetStatus::Ok;
                    entry.matchedRva    = static_cast<uint32_t>(site - g_gamePe.base);
                    entry.resolvedValue = resolvedVal;
                } else {
                    entry.status        = offsets::OffsetStatus::Missing;
                    entry.matchedRva    = 0;
                    entry.resolvedValue = 0;
                    logger::logMessage(GRML_LOG_WARN, "Offsets",
                                       "[OFFSET MISSING] Section '%s' matched %zu time(s) (expected 1) — marked MISSING",
                                       entry.def.name.c_str(), entry.matchCount);
                }
            }
            if (!allOffsetsCached && !g_resolvedOffsets.empty()) {
                if (offsets::writeOffsetCache(offsetCachePath, g_gamePe, offsetsIniHash, g_resolvedOffsets)) {
                    logger::logMessage(GRML_LOG_INFO, "Offsets", "Updated PE-hash RVA cache (grml_offset_cache.ini)");
                }
            }
            save_events::SessionDiagnosticsSnapshot snap;
            snap.gameVersion    = g_gameVersion;
            snap.peTimestamp    = g_gamePe.timeDateStamp;
            snap.peSizeOfImage  = static_cast<uint32_t>(g_gamePe.imageSize);
            snap.peEntryPoint   = g_gamePe.addressOfEntryPoint;
            snap.offsetsFnv1a64 = offsetsIniHash;
            snap.offsets        = g_resolvedOffsets;
            save_events::publishOffsetDiagnostics(std::move(snap));
        }
    }

    // Apply GRML internal targets from declarative sections (with legacy [signatures] fallback)
    uintptr_t namedVal = 0;
    if (hostResolveNamedOffset("in_game_flag", &namedVal) && namedVal != 0) {
        g_inGameFlagPtr = reinterpret_cast<const volatile uint8_t*>(namedVal);
        overlay::setInGameFlagPtr(g_inGameFlagPtr);
        logger::logMessage(GRML_LOG_INFO, "Offsets", "in_game_flag resolved -> 0x%p", g_inGameFlagPtr);
    } else if (inGameFlagSite) {
        const uint32_t dispOff   = static_cast<uint32_t>(offsetsIni.getInt("signatures", "in_game_flag_disp_off", 15));
        const uint32_t nextIpOff = static_cast<uint32_t>(offsetsIni.getInt("signatures", "in_game_flag_next_ip_off", 19));
        void* flagPtr = resolveRipRelativeDisp32(inGameFlagSite, dispOff, nextIpOff);
        g_inGameFlagPtr = static_cast<const volatile uint8_t*>(flagPtr);
        overlay::setInGameFlagPtr(g_inGameFlagPtr);
        logger::logMessage(GRML_LOG_INFO, "AOBScanner", "in_game_flag_aob resolved -> 0x%p", flagPtr);
    }

    if (hostResolveNamedOffset("resource_vector", &namedVal) && namedVal != 0) {
        g_resourceVectorAddr = reinterpret_cast<void*>(namedVal);
        uintptr_t strideVal = 0;
        if (hostResolveNamedOffset("resource_stride", &strideVal) && strideVal >= 0x200 && strideVal <= 0x1000) {
            g_wrsrOffsets.res_stride = static_cast<uint32_t>(strideVal);
        }
        logger::logMessage(GRML_LOG_INFO, "Offsets", "resource_vector resolved -> 0x%p (stride=0x%X)",
                           g_resourceVectorAddr, g_wrsrOffsets.res_stride);
    } else if (priceTableSite) {
        const uint32_t vecDisp   = static_cast<uint32_t>(offsetsIni.getInt("signatures", "price_table_vec_disp_off", 14));
        const uint32_t vecNextIp = static_cast<uint32_t>(offsetsIni.getInt("signatures", "price_table_vec_next_ip_off", 18));
        const uint32_t strideOff = static_cast<uint32_t>(offsetsIni.getInt("signatures", "price_table_stride_imm_off", 45));
        g_resourceVectorAddr = resolveRipRelativeDisp32(priceTableSite, vecDisp, vecNextIp);
        uint32_t immStride = 0;
        std::memcpy(&immStride, static_cast<const uint8_t*>(priceTableSite) + strideOff, sizeof(immStride));
        if (immStride >= 0x200 && immStride <= 0x1000) {
            g_wrsrOffsets.res_stride = immStride;
        }
        logger::logMessage(GRML_LOG_INFO, "AOBScanner", "price_table_aob resolved -> 0x%p (stride=0x%X)",
                           g_resourceVectorAddr, g_wrsrOffsets.res_stride);
    }

    if (hostResolveNamedOffset("evaluate_dist_task", &namedVal) && namedVal != 0) {
        g_evalDistTaskAddr = reinterpret_cast<void*>(namedVal);
        logger::logMessage(GRML_LOG_INFO, "Offsets", "evaluate_dist_task resolved -> 0x%p", g_evalDistTaskAddr);
    } else if (evalDistSite) {
        g_evalDistTaskAddr = evalDistSite;
        logger::logMessage(GRML_LOG_INFO, "AOBScanner", "dispatch_check_aob resolved -> 0x%p", g_evalDistTaskAddr);
    }
    if (menuLoopSite && menuUiStateSite && loadSaveHdrSite && loadSaveTrigSite) {
        overlay::DirectSaveLoadTargets dsl{};
        const uint32_t actDisp = static_cast<uint32_t>(offsetsIni.getInt("signatures", "main_menu_active_disp_off", 3));
        const uint32_t actNext = static_cast<uint32_t>(offsetsIni.getInt("signatures", "main_menu_active_next_ip_off", 7));
        const uint32_t objDisp = static_cast<uint32_t>(offsetsIni.getInt("signatures", "main_menu_obj_disp_off", 52));
        const uint32_t objNext = static_cast<uint32_t>(offsetsIni.getInt("signatures", "main_menu_obj_next_ip_off", 56));
        const uint32_t uiImm   = static_cast<uint32_t>(offsetsIni.getInt("signatures", "main_menu_uistate_imm_off", 2));
        const uint32_t reqDisp = static_cast<uint32_t>(offsetsIni.getInt("signatures", "load_save_req_disp_off", 11));
        const uint32_t reqNext = static_cast<uint32_t>(offsetsIni.getInt("signatures", "load_save_req_next_ip_off", 16));
        const uint32_t ngDisp  = static_cast<uint32_t>(offsetsIni.getInt("signatures", "load_save_newgame_disp_off", 23));
        const uint32_t ngNext  = static_cast<uint32_t>(offsetsIni.getInt("signatures", "load_save_newgame_next_ip_off", 28));

        dsl.menuActivePtr    = static_cast<const volatile uint8_t*>(resolveRipRelativeDisp32(menuLoopSite, actDisp, actNext));
        dsl.mainMenuObj      = static_cast<uint8_t*>(resolveRipRelativeDisp32(menuLoopSite, objDisp, objNext));
        std::memcpy(&dsl.uiStateOff, static_cast<const uint8_t*>(menuUiStateSite) + uiImm, sizeof(dsl.uiStateOff));
        dsl.loadSaveHeaderFn = loadSaveHdrSite;
        dsl.loadRequestedPtr = static_cast<volatile uint8_t*>(resolveRipRelativeDisp32(loadSaveTrigSite, reqDisp, reqNext));
        dsl.newGameFlagPtr   = static_cast<volatile uint8_t*>(resolveRipRelativeDisp32(loadSaveTrigSite, ngDisp, ngNext));

        overlay::setDirectSaveLoadTargets(dsl, g_gameExePath.parent_path());
        logger::logMessage(GRML_LOG_INFO, "AOBScanner",
                           "Direct-to-Save targets resolved (menuObj=0x%p loadFn=0x%p uiStateOff=0x%X)",
                           dsl.mainMenuObj, dsl.loadSaveHeaderFn, dsl.uiStateOff);
    } else {
        overlay::setDirectSaveLoadTargets({}, g_gameExePath.parent_path());
        logger::logMessage(GRML_LOG_WARN, "DirectLoad",
                           "One or more Direct-to-Save signatures missed (menuLoop=%d uiState=%d loadHdr=%d loadTrig=%d) — auto-load disabled",
                           menuLoopSite != nullptr, menuUiStateSite != nullptr,
                           loadSaveHdrSite != nullptr, loadSaveTrigSite != nullptr);
    }

    if (introSkipSite) {
        const uint32_t jeOff = static_cast<uint32_t>(offsetsIni.getInt("signatures", "intro_skip_je_off", 24));
        auto* patchAddr = static_cast<uint8_t*>(introSkipSite) + jeOff;
        if (patchAddr[0] == 0x74 && patchAddr[1] == 0x38) {
            DWORD oldProt = 0;
            if (VirtualProtect(patchAddr, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                patchAddr[0] = 0x90;
                patchAddr[1] = 0x90;
                VirtualProtect(patchAddr, 2, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), patchAddr, 2);
                logger::logMessage(GRML_LOG_INFO, "IntroSkip",
                                   "Patched intro logo timer gate @ 0x%p (devlogo/publogo skipped)", patchAddr);
            }
        }
    }

    // Phase 3: Post-scan callbacks & Tesmio TsmPluginStart
    {
        std::vector<PostScanEntry> cbs;
        {
            std::lock_guard lock(g_supMutex);
            cbs = g_postScanCallbacks;
        }
        for (const auto& entry : cbs) {
            DWORD exc = 0;
            if (!sehCallPostScan(entry.cb, entry.userData, &exc)) {
                logger::logMessage(GRML_LOG_ERROR, "Supervisor",
                                   "Post-scan callback faulted (0x%08lX)", exc);
            }
        }
    }
    for (auto& rec : records) {
        if (rec.loaded && rec.module && rec.meta.hasTsmStart) {
            if (!compat::invokeTesmioStart(rec.module, rec.meta.id, rec.status)) {
                hooks::rollbackPluginHooks(rec.meta.id);
                compat::rollbackPluginHooks(rec.meta.id);
                if (!rec.meta.hasGrmlInit && !rec.meta.hasRmlLoad) {
                    {
                        std::lock_guard lock(g_crashModulesMutex);
                        std::erase_if(g_crashModules, [&](const CrashModuleInfo& cm) {
                            return cm.module == rec.module;
                        });
                    }
                    DWORD freeExc = 0;
                    sehFreeLibrary(rec.module, &freeExc);
                    rec.module = nullptr;
                    rec.loaded = false;
                    rec.failed = true;
                    logger::logMessage(GRML_LOG_WARN, "Supervisor",
                                       "Unloaded failed plugin '%s' (%s)",
                                       rec.meta.id.c_str(), rec.status.c_str());
                }
            }
        }
        // Auto-register zero-code declarative UI schema (.ui.json / .ui.yaml / settingsUi)
        // OR auto-built INI settings UI for external/legacy mods without a custom GRML tab.
        if (rec.loaded && !rec.meta.hasGrmlInit && !overlay::hasRegisteredTabForPlugin(rec.meta.id)) {
            UiSchema schema = discoverModUiSchema(rec.meta.dllPath, rec.meta.manifestPath, rec.meta.name);
            if (schema.valid) {
                applyModUiTranslations(schema, rec.meta.dllPath, g_activeLanguage);
                const int32_t tid = overlay::registerExternalUiSchemaTab(
                    rec.meta.id, rec.meta.name, rec.meta.dllPath, rec.meta.manifestPath, schema);
                if (tid >= 0) {
                    logger::logMessage(GRML_LOG_INFO, "OverlayHub",
                                       "%s UI tab for '%s' -> %s (%zu controls)",
                                       schema.autoGenerated ? "Auto-built INI" : "Registered declarative",
                                       rec.meta.id.c_str(), schema.iniFile.c_str(), schema.controls.size());
                }
            }
        }
    }

    // Publish Loaded Mods table to F9 Mod Settings hub
    std::vector<overlay::LoadedModSummary> uiList;
    uiList.reserve(records.size());
    for (const auto& rec : records) {
        overlay::LoadedModSummary s;
        s.id        = rec.meta.id;
        s.name      = rec.meta.name;
        s.version   = rec.meta.version;
        s.author    = rec.meta.author;
        s.loaderApi = loaderKindDisplayName(rec.meta);
        s.loadOrder = rec.loadOrder;
        s.loaded    = rec.loaded;
        s.failed    = rec.failed;
        s.status    = rec.status;
        uiList.push_back(std::move(s));
    }
    overlay::publishLoadedMods(std::move(uiList));

    if (grmlIni.getBool("general", "show_menu_status", true)) {
        installMenuStatusTag(offsetsIni.getString("signatures", "menu_version_fmt_wide", ""), overlayVk);
    }

    {
        std::lock_guard lock(g_supMutex);
        g_loadedPlugins = std::move(records);
    }

    logger::logMessage(GRML_LOG_INFO, "Core", "GRML bootstrap complete");
    return true;
}

void shutdown()
{
    if (!g_bootstrapped.exchange(false)) return;

    logger::logMessage(GRML_LOG_INFO, "Core", "GRML shutting down — unloading plugins in reverse load order");
    overlay::shutdown();
    hooks::shutdown();
    save_events::shutdown();
    vfs_redirect::shutdown();
    compat::shutdown();
    SetUnhandledExceptionFilter(g_prevExceptionFilter);
    g_prevExceptionFilter = nullptr;
    {
        std::lock_guard lock(g_crashModulesMutex);
        g_crashModules.clear();
    }

    std::vector<LoadedPluginRecord> toUnload;
    {
        std::lock_guard lock(g_supMutex);
        toUnload = std::move(g_loadedPlugins);
        g_loadedPlugins.clear();
    }

    // Unload in strict reverse load order inside SEH guards
    for (auto it = toUnload.rbegin(); it != toUnload.rend(); ++it) {
        if (it->module) {
            DWORD exc = 0;
            if (!sehFreeLibrary(it->module, &exc)) {
                logger::logMessage(GRML_LOG_WARN, "Supervisor",
                                   "FreeLibrary faulted for '%s' (exc=0x%08lX)", it->meta.id.c_str(), exc);
            }
            it->module = nullptr;
        }
    }

    logger::shutdown();
}

} // namespace grml::supervisor
