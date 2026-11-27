#include "runtime/hook_manager.hpp"
#include "runtime/ring_logger.hpp"

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace grml::hooks {

namespace {

struct ManagedHook {
    std::string pluginId;
    std::string hookName;
    void*       target{nullptr};
};

std::mutex               g_hookMutex;
std::vector<ManagedHook> g_managedHooks;

void ensureMinHookInitialized() noexcept
{
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        logger::logMessage(GRML_LOG_ERROR, "HookMgr", "MH_Initialize failed (%d)", static_cast<int>(st));
    }
}

void recordHook(const char* pluginId, const char* hookName, void* target)
{
    std::lock_guard lock(g_hookMutex);
    g_managedHooks.push_back({
        pluginId ? pluginId : "unknown",
        hookName ? hookName : "unnamed",
        target
    });
}

int32_t GRML_CALL apiCreateHook(
    const char* pluginId,
    const char* hookName,
    void*       target,
    void*       detour,
    void**      outOriginal)
{
    if (!target || !detour || !outOriginal) return 0;
    ensureMinHookInitialized();

    const MH_STATUS st = MH_CreateHook(target, detour, outOriginal);
    if (st != MH_OK) {
        logger::logMessage(GRML_LOG_ERROR, "HookMgr",
                           "[%s] MH_CreateHook('%s', 0x%p) failed (%d)",
                           pluginId ? pluginId : "?", hookName ? hookName : "?",
                           target, static_cast<int>(st));
        return 0;
    }

    recordHook(pluginId, hookName, target);
    logger::logMessage(GRML_LOG_INFO, "HookMgr",
                       "[%s] Created hook '%s' @ 0x%p",
                       pluginId ? pluginId : "?", hookName ? hookName : "?", target);
    return 1;
}

int32_t GRML_CALL apiCreateApiHook(
    const char*    pluginId,
    const char*    hookName,
    const wchar_t* moduleName,
    const char*    procName,
    void*          detour,
    void**         outOriginal)
{
    if (!moduleName || !procName || !detour || !outOriginal) return 0;
    ensureMinHookInitialized();

    void* target = nullptr;
    const MH_STATUS st = MH_CreateHookApiEx(moduleName, procName, detour, outOriginal, &target);
    if (st != MH_OK || !target) {
        logger::logMessage(GRML_LOG_ERROR, "HookMgr",
                           "[%s] MH_CreateHookApiEx('%s', %s) failed (%d)",
                           pluginId ? pluginId : "?", hookName ? hookName : "?",
                           procName, static_cast<int>(st));
        return 0;
    }

    recordHook(pluginId, hookName, target);
    logger::logMessage(GRML_LOG_INFO, "HookMgr",
                       "[%s] Created API hook '%s' (%s) @ 0x%p",
                       pluginId ? pluginId : "?", hookName ? hookName : "?", procName, target);
    return 1;
}

int32_t GRML_CALL apiEnableHook(void* target)
{
    if (!target) return 0;
    return (MH_EnableHook(target) == MH_OK) ? 1 : 0;
}

int32_t GRML_CALL apiDisableHook(void* target)
{
    if (!target) return 0;
    return (MH_DisableHook(target) == MH_OK) ? 1 : 0;
}

int32_t GRML_CALL apiRemoveHook(void* target)
{
    if (!target) return 0;
    MH_DisableHook(target);
    const MH_STATUS st = MH_RemoveHook(target);
    {
        std::lock_guard lock(g_hookMutex);
        std::erase_if(g_managedHooks, [target](const ManagedHook& h) { return h.target == target; });
    }
    return (st == MH_OK) ? 1 : 0;
}

const GrmlHookApi g_hookApi = {
    GRML_API_VERSION,
    sizeof(GrmlHookApi),
    &apiCreateHook,
    &apiCreateApiHook,
    &apiEnableHook,
    &apiDisableHook,
    &apiRemoveHook,
};

} // namespace

const GrmlHookApi* hookApi() noexcept
{
    return &g_hookApi;
}

void rollbackPluginHooks(const std::string& pluginId)
{
    std::vector<ManagedHook> mine;
    {
        std::lock_guard lock(g_hookMutex);
        for (auto it = g_managedHooks.begin(); it != g_managedHooks.end();) {
            if (it->pluginId == pluginId) {
                mine.push_back(*it);
                it = g_managedHooks.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto it = mine.rbegin(); it != mine.rend(); ++it) {
        MH_DisableHook(it->target);
        MH_RemoveHook(it->target);
        logger::logMessage(GRML_LOG_WARN, "HookMgr",
                           "Rolled back hook '%s' for '%s'",
                           it->hookName.c_str(), pluginId.c_str());
    }
}

void shutdown()
{
    std::vector<ManagedHook> all;
    {
        std::lock_guard lock(g_hookMutex);
        all = std::move(g_managedHooks);
        g_managedHooks.clear();
    }
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        MH_DisableHook(it->target);
        MH_RemoveHook(it->target);
    }
}

} // namespace grml::hooks
