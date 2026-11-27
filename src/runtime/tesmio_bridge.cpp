#include "runtime/tesmio_bridge.hpp"
#include "runtime/ring_logger.hpp"
#include "common/ini_util.hpp"
#include "common/pe_scanner.hpp"

#include <MinHook.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace grml::compat {

namespace {

struct ServiceKey {
    std::string name;
    uint32_t    version{0};
    bool operator==(const ServiceKey& o) const noexcept { return version == o.version && name == o.name; }
};

struct ServiceEntry {
    std::string providerId;
    void*       ptr{nullptr};
};

struct OwnedInlineHook {
    std::string pluginId;
    void*       target{nullptr};
};

struct OwnedIatHook {
    std::string pluginId;
    void**      slot{nullptr};
    void*       original{nullptr};
    void*       detour{nullptr};
};

std::mutex                       g_compatMutex;
std::string                      g_activePluginId;
std::string                      g_gameVersion;
std::string                      g_gameSha256;
std::filesystem::path            g_gameBaseDir;
std::filesystem::path            g_pluginsDir;
std::string                      g_baseDirUtf8;
std::string                      g_pluginDirUtf8;
std::string                      g_vfsRootUtf8;
uintptr_t                        g_resourceGetAddr{0};
std::vector<ServiceKey>          g_serviceKeys;
std::vector<ServiceEntry>        g_serviceVals;
std::vector<OwnedInlineHook>     g_inlineHookTargets;
std::vector<OwnedIatHook>        g_iatHooks;

using RmlQueryFn   = int(__cdecl*)(uint32_t, RmlPluginDescriptor*);
using RmlLoadFn    = int(__cdecl*)(const RmlHostApi*);
using TsmApiVerFn  = unsigned(__cdecl*)(void);
using TsmInitFn    = int(__cdecl*)(const TsmHost*, TsmPluginInfo*);
using TsmStartFn   = int(__cdecl*)(void);

struct RmlPluginCtx {
    char pluginId[128]{};
};

// ── RML API v1 Callbacks ─────────────────────────────────────────────────────
void __cdecl rmlHostLog(void* ctx, RmlLogLevel level, const char* message)
{
    const auto* c = static_cast<const RmlPluginCtx*>(ctx);
    const char* id = (c && c->pluginId[0]) ? c->pluginId : "RMLPlugin";
    logger::logMessage(static_cast<int32_t>(level), id, "%s", message ? message : "");
}

int __cdecl rmlHostGetGameInfo(void*, RmlGameInfo* outInfo)
{
    if (!outInfo || outInfo->struct_size < sizeof(RmlGameInfo)) return 0;
    outInfo->version        = g_gameVersion.c_str();
    outInfo->sha256         = g_gameSha256.c_str();
    outInfo->image_base     = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    outInfo->loader_api     = 1u;
    outInfo->loader_version = GRML_BUILD_VERSION;
    return 1;
}

int __cdecl rmlHostGetSymbol(void*, const char* symbolId, RmlSymbolInfo* outInfo)
{
    if (!symbolId || !outInfo || outInfo->struct_size < sizeof(RmlSymbolInfo)) return 0;
    if (std::strcmp(symbolId, "wrsr.resource_get") == 0 && g_resourceGetAddr != 0) {
        outInfo->id         = "wrsr.resource_get";
        outInfo->address    = g_resourceGetAddr;
        outInfo->confidence = RML_SYMBOL_CONFIRMED;
        outInfo->resolver   = "GRML single-pass anchor + .pdata resolver";
        return 1;
    }
    return 0;
}

uint32_t __cdecl rmlHostGetSymbolCount(void*)
{
    return (g_resourceGetAddr != 0) ? 1u : 0u;
}

int __cdecl rmlHostGetSymbolByIndex(void* ctx, uint32_t index, RmlSymbolInfo* outInfo)
{
    if (index == 0 && g_resourceGetAddr != 0) {
        return rmlHostGetSymbol(ctx, "wrsr.resource_get", outInfo);
    }
    return 0;
}

int __cdecl rmlHostProvideService(void* ctx, const char* name, uint32_t version, void* service)
{
    if (!name || !*name || !service) return 0;
    const auto* c = static_cast<const RmlPluginCtx*>(ctx);
    const std::string owner = (c && c->pluginId[0]) ? c->pluginId : "grml";
    std::lock_guard lock(g_compatMutex);
    for (size_t i = 0; i < g_serviceKeys.size(); ++i) {
        if (g_serviceKeys[i].name == name && g_serviceKeys[i].version == version) {
            g_serviceVals[i] = {owner, service};
            return 1;
        }
    }
    g_serviceKeys.push_back({name, version});
    g_serviceVals.push_back({owner, service});
    return 1;
}

void* __cdecl rmlHostConsumeService(void*, const char* name, uint32_t version, RmlServiceInfo* outInfo)
{
    if (!name || !*name) return nullptr;
    std::lock_guard lock(g_compatMutex);
    for (size_t i = 0; i < g_serviceKeys.size(); ++i) {
        if (g_serviceKeys[i].name == name && g_serviceKeys[i].version == version) {
            if (outInfo && outInfo->struct_size >= sizeof(RmlServiceInfo)) {
                outInfo->name        = g_serviceKeys[i].name.c_str();
                outInfo->version     = g_serviceKeys[i].version;
                outInfo->provider_id = g_serviceVals[i].providerId.c_str();
            }
            return g_serviceVals[i].ptr;
        }
    }
    return nullptr;
}

// ── Tesmio API v4 Callbacks ──────────────────────────────────────────────────
void tsmLog(const char* fmt, ...)
{
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    logger::logMessageV(GRML_LOG_INFO, "TesmioCompat", fmt, args);
    va_end(args);
}

int tsmReadablePtr(const void* p, size_t n)
{
    if (!p) return 0;
    if (n == 0) return 1;
    const auto* cur = static_cast<const uint8_t*>(p);
    const auto* end = cur + n;
    if (end < cur) return 0;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(cur, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        const auto* next = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= cur) return 0;
        cur = next;
    }
    return 1;
}

void** tsmFindIatSlot(void* module, const char* dll, const char* fn)
{
    if (!module || !dll || !fn) return nullptr;
    auto* base = static_cast<uint8_t*>(module);
    auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;

    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        const char* modName = reinterpret_cast<const char*>(base + imp->Name);
        if (_stricmp(modName, dll) != 0) continue;
        const DWORD oft = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        auto* orig = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + oft);
        auto* iat  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; orig->u1.AddressOfData; ++orig, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(orig->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), fn) == 0) {
                return reinterpret_cast<void**>(&iat->u1.Function);
            }
        }
    }
    return nullptr;
}

int tsmPatchIat(void* module, const char* dll, const char* fn, void* detour, void** original, const char* label)
{
    void** slot = tsmFindIatSlot(module, dll, fn);
    if (!slot || !detour) return 0;
    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) return 0;
    void* prev = *slot;
    *slot      = detour;
    VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
    if (original) *original = prev;
    {
        std::lock_guard lock(g_compatMutex);
        g_iatHooks.push_back({g_activePluginId, slot, prev, detour});
    }
    logger::logMessage(GRML_LOG_INFO, "TesmioCompat", "IAT hook installed: %s (%s!%s)",
                       label ? label : fn, dll, fn);
    return 1;
}

int tsmInstallInlineHook(void* target, void* detour, void** trampoline,
                         const unsigned char* expect, size_t stolen, const char* label)
{
    if (!target || !detour || !trampoline || stolen < 14) return 0;
    if (expect && !tsmReadablePtr(target, stolen)) return 0;
    if (expect && std::memcmp(target, expect, stolen) != 0) {
        logger::logMessage(GRML_LOG_ERROR, "TesmioCompat",
                           "Inline hook '%s' refused: expected bytes mismatch", label ? label : "unnamed");
        return 0;
    }

    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return 0;

    if (MH_CreateHook(target, detour, trampoline) != MH_OK) {
        logger::logMessage(GRML_LOG_ERROR, "TesmioCompat",
                           "MH_CreateHook failed for '%s'", label ? label : "unnamed");
        return 0;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return 0;
    }

    {
        std::lock_guard lock(g_compatMutex);
        g_inlineHookTargets.push_back({g_activePluginId, target});
    }
    logger::logMessage(GRML_LOG_INFO, "TesmioCompat", "Inline hook installed: %s", label ? label : "unnamed");
    return 1;
}

unsigned char* tsmAllocNear(unsigned char* anchor, size_t size)
{
    if (!anchor || !size || size > 16u * 1024u * 1024u) return nullptr;
    const uintptr_t rip   = reinterpret_cast<uintptr_t>(anchor);
    const uintptr_t reach = 0x70000000ull;
    uintptr_t low         = (rip > reach) ? (rip - reach) : 0x10000ull;
    const uintptr_t high  = (rip + reach > rip) ? (rip + reach) : ~uintptr_t(0);
    low                   = (low + 0xFFFFull) & ~0xFFFFull;

    for (uintptr_t p = low; p < high;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
        const uintptr_t region = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next   = region + mbi.RegionSize;
        if (next <= p) break;
        if (mbi.State == MEM_FREE) {
            const uintptr_t cand = (region + 0xFFFFull) & ~0xFFFFull;
            if (cand >= low && cand + size > cand && cand + size <= next && cand + size <= high) {
                auto* mem = static_cast<unsigned char*>(
                    VirtualAlloc(reinterpret_cast<LPVOID>(cand), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
                if (mem) return mem;
            }
        }
        p = next;
    }
    return nullptr;
}

long tsmFaultFilter(const char* what, void* ep)
{
    const auto* pointers = static_cast<EXCEPTION_POINTERS*>(ep);
    const DWORD code = (pointers && pointers->ExceptionRecord) ? pointers->ExceptionRecord->ExceptionCode : 0;
    logger::logMessage(GRML_LOG_ERROR, "TesmioCompat",
                       "SEH fault inside '%s' (code=0x%08lX)", what ? what : "unknown", code);
    return EXCEPTION_EXECUTE_HANDLER;
}

std::filesystem::path resolveIniPath(const char* iniName)
{
    if (!iniName || !*iniName) return {};
    std::filesystem::path p = utf8ToWide(iniName);
    if (p.is_absolute()) return p;
    if (std::filesystem::exists(g_gameBaseDir / p)) return g_gameBaseDir / p;
    if (std::filesystem::exists(g_pluginsDir / p)) return g_pluginsDir / p;
    return g_pluginsDir / p.filename();
}

int tsmConfigInt(const char* iniName, const char* section, const char* key, int fallback)
{
    IniFile ini;
    if (!ini.loadFromFile(resolveIniPath(iniName))) return fallback;
    return ini.getInt(section ? section : "", key ? key : "", fallback);
}

int tsmConfigString(const char* iniName, const char* section, const char* key,
                    char* out, int outSize, const char* fallback)
{
    if (!out || outSize <= 0) return 0;
    IniFile ini;
    std::string val = fallback ? fallback : "";
    if (ini.loadFromFile(resolveIniPath(iniName))) {
        val = ini.getString(section ? section : "", key ? key : "", val);
    }
    std::snprintf(out, static_cast<size_t>(outSize), "%s", val.c_str());
    return static_cast<int>(std::strlen(out));
}

int tsmProvide(const char* service, unsigned version, const void* iface)
{
    if (!service || !iface) return 0;
    return rmlHostProvideService(nullptr, service, version, const_cast<void*>(iface));
}

const void* tsmConsume(const char* service, unsigned version)
{
    return rmlHostConsumeService(nullptr, service, version, nullptr);
}

TsmHost g_tsmHost{};

// Pure-SEH wrappers (no local C++ objects with destructors)
int sehRmlQuery(RmlQueryFn fn, RmlPluginDescriptor* desc, DWORD* outCode) noexcept
{
    __try {
        return fn(1u, desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return -1;
    }
}

int sehRmlLoad(RmlLoadFn fn, const RmlHostApi* host, DWORD* outCode) noexcept
{
    __try {
        return fn(host);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return -1;
    }
}

int sehTsmApiVer(TsmApiVerFn fn, DWORD* outCode) noexcept
{
    __try {
        return static_cast<int>(fn());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return -1;
    }
}

int sehTsmInit(TsmInitFn fn, const TsmHost* host, TsmPluginInfo* info, DWORD* outCode) noexcept
{
    __try {
        return fn(host, info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return -1;
    }
}

int sehTsmStart(TsmStartFn fn, DWORD* outCode) noexcept
{
    __try {
        return fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return -1;
    }
}

} // namespace

void initialize(const std::filesystem::path& gameBaseDir,
                const std::filesystem::path& pluginsDir,
                const std::string& gameVersion,
                const std::string& gameSha256,
                uintptr_t resourceGetAddr)
{
    g_gameBaseDir     = gameBaseDir;
    g_pluginsDir      = pluginsDir;
    g_gameVersion     = gameVersion;
    g_gameSha256      = gameSha256;
    g_resourceGetAddr = resourceGetAddr;
    g_baseDirUtf8     = wideToUtf8(gameBaseDir.wstring());
    g_pluginDirUtf8   = wideToUtf8(pluginsDir.wstring());
    g_vfsRootUtf8     = wideToUtf8((gameBaseDir / L"media_soviet").wstring());

    const auto exeRange = []() {
        PeModuleInfo pe{};
        inspectLoadedPe(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)), pe);
        return pe;
    }();

    g_tsmHost                   = {};
    g_tsmHost.apiVersion        = 4u;
    g_tsmHost.structSize        = sizeof(TsmHost);
    g_tsmHost.exeModule         = GetModuleHandleW(nullptr);
    g_tsmHost.exeBase           = exeRange.base;
    g_tsmHost.exeSize           = exeRange.imageSize;
    g_tsmHost.engineModule      = GetModuleHandleW(L"C3DDLL64.dll");
    g_tsmHost.baseDir           = g_baseDirUtf8.c_str();
    g_tsmHost.pluginDir         = g_pluginDirUtf8.c_str();
    g_tsmHost.log               = &tsmLog;
    g_tsmHost.findIatSlot       = &tsmFindIatSlot;
    g_tsmHost.patchIat          = &tsmPatchIat;
    g_tsmHost.installInlineHook = &tsmInstallInlineHook;
    g_tsmHost.allocNear         = &tsmAllocNear;
    g_tsmHost.readablePtr       = &tsmReadablePtr;
    g_tsmHost.faultFilter       = &tsmFaultFilter;
    g_tsmHost.configInt         = &tsmConfigInt;
    g_tsmHost.configString      = &tsmConfigString;
    g_tsmHost.provide           = &tsmProvide;
    g_tsmHost.consume           = &tsmConsume;
    g_tsmHost.vfsRoot           = g_vfsRootUtf8.c_str();
}

void rollbackPluginHooks(const std::string& pluginId)
{
    if (pluginId.empty()) return;
    std::lock_guard lock(g_compatMutex);

    size_t rolledInline = 0;
    for (auto it = g_inlineHookTargets.begin(); it != g_inlineHookTargets.end();) {
        if (it->pluginId == pluginId) {
            MH_DisableHook(it->target);
            MH_RemoveHook(it->target);
            it = g_inlineHookTargets.erase(it);
            ++rolledInline;
        } else {
            ++it;
        }
    }

    size_t rolledIat = 0;
    for (auto it = g_iatHooks.begin(); it != g_iatHooks.end();) {
        if (it->pluginId == pluginId) {
            if (it->slot && *it->slot == it->detour) {
                DWORD oldProt = 0;
                if (VirtualProtect(it->slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    *it->slot = it->original;
                    VirtualProtect(it->slot, sizeof(void*), oldProt, &oldProt);
                }
            }
            it = g_iatHooks.erase(it);
            ++rolledIat;
        } else {
            ++it;
        }
    }

    if (rolledInline > 0 || rolledIat > 0) {
        logger::logMessage(GRML_LOG_WARN, "TesmioCompat",
                           "Rolled back %zu inline + %zu IAT hook(s) for failed plugin '%s'",
                           rolledInline, rolledIat, pluginId.c_str());
    }
}

void shutdown()
{
    std::lock_guard lock(g_compatMutex);
    for (auto it = g_inlineHookTargets.rbegin(); it != g_inlineHookTargets.rend(); ++it) {
        MH_DisableHook(it->target);
        MH_RemoveHook(it->target);
    }
    g_inlineHookTargets.clear();
    for (auto it = g_iatHooks.rbegin(); it != g_iatHooks.rend(); ++it) {
        if (it->slot && *it->slot == it->detour) {
            DWORD oldProt = 0;
            if (VirtualProtect(it->slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                *it->slot = it->original;
                VirtualProtect(it->slot, sizeof(void*), oldProt, &oldProt);
            }
        }
    }
    g_iatHooks.clear();
}

bool invokeRmlPlugin(HMODULE mod,
                     const std::string& pluginId,
                     std::string& ioName,
                     std::string& ioVersion,
                     std::string& ioAuthor,
                     std::string& outStatus)
{
    auto queryFn = reinterpret_cast<RmlQueryFn>(GetProcAddress(mod, "RmlPluginQuery"));
    auto loadFn  = reinterpret_cast<RmlLoadFn>(GetProcAddress(mod, "RmlPluginLoad"));
    if (!queryFn || !loadFn) {
        return false;
    }

    RmlPluginDescriptor desc{};
    desc.struct_size = sizeof(desc);
    DWORD exc = 0;
    const int qRes = sehRmlQuery(queryFn, &desc, &exc);
    if (qRes <= 0) {
        outStatus = (qRes < 0) ? "RmlPluginQuery exception" : "RmlPluginQuery declined";
        return false;
    }
    if (desc.name && *desc.name) ioName = desc.name;
    if (desc.version && *desc.version) ioVersion = desc.version;
    if (desc.author && *desc.author) ioAuthor = desc.author;

    static std::vector<std::unique_ptr<RmlPluginCtx>> s_contexts;
    auto ctx = std::make_unique<RmlPluginCtx>();
    std::snprintf(ctx->pluginId, sizeof(ctx->pluginId), "%s", pluginId.c_str());

    RmlHostApi host{};
    host.struct_size         = sizeof(RmlHostApi);
    host.api_version         = 1u;
    host.plugin_context      = ctx.get();
    host.log                 = &rmlHostLog;
    host.get_game_info       = &rmlHostGetGameInfo;
    host.get_symbol          = &rmlHostGetSymbol;
    host.get_symbol_count    = &rmlHostGetSymbolCount;
    host.get_symbol_by_index = &rmlHostGetSymbolByIndex;
    host.provide_service     = &rmlHostProvideService;
    host.consume_service     = &rmlHostConsumeService;
    s_contexts.push_back(std::move(ctx));

    exc = 0;
    const int lRes = sehRmlLoad(loadFn, &host, &exc);
    if (lRes <= 0) {
        outStatus = (lRes < 0) ? "RmlPluginLoad SEH exception" : "RmlPluginLoad failed";
        return false;
    }
    outStatus = "Loaded (RML v1)";
    return true;
}

bool invokeTesmioInit(HMODULE mod,
                      const std::string& pluginId,
                      std::string& ioName,
                      std::string& ioVersion,
                      std::string& outStatus)
{
    auto apiVerFn = reinterpret_cast<TsmApiVerFn>(GetProcAddress(mod, "TsmPluginApiVersion"));
    auto initFn   = reinterpret_cast<TsmInitFn>(GetProcAddress(mod, "TsmPluginInit"));
    if (!initFn) return false;

    if (apiVerFn) {
        DWORD exc = 0;
        const int ver = sehTsmApiVer(apiVerFn, &exc);
        if (ver < 3 || ver > 4) {
            outStatus = "Unsupported Tesmio API version";
            return false;
        }
    }

    TsmPluginInfo info{};
    DWORD exc = 0;
    {
        std::lock_guard lock(g_compatMutex);
        g_activePluginId = pluginId;
    }
    const int rc = sehTsmInit(initFn, &g_tsmHost, &info, &exc);
    {
        std::lock_guard lock(g_compatMutex);
        g_activePluginId.clear();
    }
    if (rc != 0) {
        outStatus = "TsmPluginInit failed";
        logger::logMessage(GRML_LOG_WARN, "TesmioCompat", "%s TsmPluginInit returned %d (exc=0x%08lX)",
                           pluginId.c_str(), rc, exc);
        return false;
    }
    if (info.name && *info.name) ioName = info.name;
    if (info.version && *info.version) ioVersion = info.version;
    outStatus = "Loaded (Tesmio v4)";
    return true;
}

bool invokeTesmioStart(HMODULE mod,
                       const std::string& pluginId,
                       std::string& outStatus)
{
    auto startFn = reinterpret_cast<TsmStartFn>(GetProcAddress(mod, "TsmPluginStart"));
    if (!startFn) return true;
    DWORD exc = 0;
    {
        std::lock_guard lock(g_compatMutex);
        g_activePluginId = pluginId;
    }
    const int rc = sehTsmStart(startFn, &exc);
    {
        std::lock_guard lock(g_compatMutex);
        g_activePluginId.clear();
    }
    if (rc != 0) {
        outStatus = "TsmPluginStart failed";
        logger::logMessage(GRML_LOG_WARN, "TesmioCompat", "%s TsmPluginStart returned %d (exc=0x%08lX)",
                           pluginId.c_str(), rc, exc);
        return false;
    }
    return true;
}

} // namespace grml::compat
