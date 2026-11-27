#pragma once
// ============================================================================
// tesmio_bridge.hpp — Backwards-compatible RML API v1 & Tesmio API v4 host
// ============================================================================
#include <cstdint>
#include <filesystem>
#include <string>

#include <windows.h>

namespace grml::compat {

// RML API v1 ABI structs (clean-room binary-compatible layout)
enum RmlLogLevel : int32_t {
    RML_LOG_TRACE = 0,
    RML_LOG_INFO  = 1,
    RML_LOG_WARN  = 2,
    RML_LOG_ERROR = 3,
};

enum RmlSymbolConfidence : int32_t {
    RML_SYMBOL_UNRESOLVED   = 0,
    RML_SYMBOL_PROBABLE     = 1,
    RML_SYMBOL_CONFIRMED    = 2,
    RML_SYMBOL_AMBIGUOUS    = 3,
    RML_SYMBOL_INCOMPATIBLE = 4,
    RML_SYMBOL_OBSERVED     = 5,
};

struct RmlPluginDescriptor {
    uint32_t    struct_size;
    uint32_t    descriptor_version;
    uint32_t    api_min;
    uint32_t    api_max;
    const char* id;
    const char* name;
    const char* version;
    const char* author;
};

struct RmlSymbolInfo {
    uint32_t            struct_size;
    const char*         id;
    uintptr_t           address;
    RmlSymbolConfidence confidence;
    const char*         resolver;
};

struct RmlGameInfo {
    uint32_t    struct_size;
    const char* version;
    const char* sha256;
    uintptr_t   image_base;
    uint32_t    loader_api;
    const char* loader_version;
};

struct RmlServiceInfo {
    uint32_t    struct_size;
    const char* name;
    uint32_t    version;
    const char* provider_id;
};

struct RmlHostApi {
    uint32_t struct_size;
    uint32_t api_version;
    void*    plugin_context;
    void(__cdecl* log)(void* plugin_context, RmlLogLevel level, const char* message);
    int(__cdecl* get_game_info)(void* plugin_context, RmlGameInfo* out_info);
    int(__cdecl* get_symbol)(void* plugin_context, const char* symbol_id, RmlSymbolInfo* out_info);
    uint32_t(__cdecl* get_symbol_count)(void* plugin_context);
    int(__cdecl* get_symbol_by_index)(void* plugin_context, uint32_t index, RmlSymbolInfo* out_info);
    int(__cdecl* provide_service)(void* plugin_context, const char* name, uint32_t version, void* service);
    void*(__cdecl* consume_service)(void* plugin_context, const char* name, uint32_t version, RmlServiceInfo* out_info);
};

// TesmioLoader API v4 ABI structs
struct TsmHost {
    unsigned       apiVersion;
    unsigned       structSize;
    void*          exeModule;
    unsigned char* exeBase;
    size_t         exeSize;
    void*          engineModule;
    const char*    baseDir;
    const char*    pluginDir;
    void (*log)(const char* fmt, ...);
    void** (*findIatSlot)(void* module, const char* dll, const char* fn);
    int (*patchIat)(void* module, const char* dll, const char* fn, void* detour, void** original, const char* label);
    int (*installInlineHook)(void* target, void* detour, void** trampoline, const unsigned char* expect, size_t stolen, const char* label);
    unsigned char* (*allocNear)(unsigned char* anchor, size_t size);
    int (*readablePtr)(const void* p, size_t n);
    long (*faultFilter)(const char* what, void* exceptionPointers);
    int (*configInt)(const char* iniName, const char* section, const char* key, int fallback);
    int (*configString)(const char* iniName, const char* section, const char* key, char* out, int outSize, const char* fallback);
    int (*provide)(const char* service, unsigned version, const void* iface);
    const void* (*consume)(const char* service, unsigned version);
    const char* vfsRoot;
};
static_assert(sizeof(TsmHost) == 0x98, "Tesmio API v4 TsmHost size mismatch");

struct TsmPluginInfo {
    const char* name;
    const char* version;
};

void initialize(const std::filesystem::path& gameBaseDir,
                const std::filesystem::path& pluginsDir,
                const std::string& gameVersion,
                const std::string& gameSha256,
                uintptr_t resourceGetAddr);

void shutdown();
void rollbackPluginHooks(const std::string& pluginId);

// SEH-protected lifecycle invokers (returns 1 on success, 0 on failure/exception)
bool invokeRmlPlugin(HMODULE mod,
                     const std::string& pluginId,
                     std::string& ioName,
                     std::string& ioVersion,
                     std::string& ioAuthor,
                     std::string& outStatus);

bool invokeTesmioInit(HMODULE mod,
                      const std::string& pluginId,
                      std::string& ioName,
                      std::string& ioVersion,
                      std::string& outStatus);

bool invokeTesmioStart(HMODULE mod,
                       const std::string& pluginId,
                       std::string& outStatus);

} // namespace grml::compat
