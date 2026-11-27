// ============================================================================
// dllmain.cpp — grml_runtime.dll entry point & GrmlBootstrap export
// ============================================================================
#include <windows.h>

#include <filesystem>

#include "runtime/plugin_supervisor.hpp"

namespace {

HMODULE g_selfModule = nullptr;

std::filesystem::path getSelfDirectory()
{
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(g_selfModule, buf, _countof(buf));
    if (len == 0 || len >= _countof(buf)) return {};
    return std::filesystem::path(buf, buf + len).parent_path();
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = hDll;
        DisableThreadLibraryCalls(hDll);
    } else if (reason == DLL_PROCESS_DETACH) {
        grml::supervisor::shutdown();
    }
    return TRUE;
}

extern "C" __declspec(dllexport) DWORD WINAPI GrmlBootstrap(LPVOID parameter)
{
    std::filesystem::path root;
    if (parameter) {
        const auto* wideRoot = static_cast<const wchar_t*>(parameter);
        if (*wideRoot) {
            root = std::filesystem::path(wideRoot);
        }
    }
    if (root.empty()) {
        root = getSelfDirectory();
    }
    return grml::supervisor::bootstrap(root) ? 0u : 1u;
}

// Backwards-compatible alias for any external tool expecting RML's bootstrap name
extern "C" __declspec(dllexport) DWORD WINAPI RmlBootstrap(LPVOID parameter)
{
    return GrmlBootstrap(parameter);
}

extern "C" __declspec(dllexport) DWORD WINAPI RmlBootstrapCommit(LPVOID /*parameter*/)
{
    return 0u;
}
