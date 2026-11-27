#include "launcher/injector.hpp"
#include "common/ini_util.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace grml::launcher {

namespace {

bool isModuleLoadedRemote(DWORD pid, const wchar_t* moduleNameLower, uintptr_t& outBase)
{
    outBase = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleNameLower) == 0) {
                outBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

uint32_t findLocalExportRva(const std::filesystem::path& dllPath, const char* exportName)
{
    HMODULE hMod = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!hMod) return 0;
    FARPROC fn = GetProcAddress(hMod, exportName);
    uint32_t rva = 0;
    if (fn) {
        rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(hMod));
    }
    FreeLibrary(hMod);
    return rva;
}

std::vector<wchar_t> buildSanitizedChildEnvIfNeeded(const std::vector<std::string>& tempDisabledIds)
{
    auto getEnvStr = [](const wchar_t* name) -> std::wstring {
        wchar_t buf[256]{};
        const DWORD n = GetEnvironmentVariableW(name, buf, _countof(buf));
        return (n > 0 && n < _countof(buf)) ? std::wstring(buf, n) : std::wstring{};
    };
    auto isForeign = [](const std::wstring& v) {
        return !v.empty() && v != L"784150";
    };

    const std::wstring appId     = getEnvStr(L"SteamAppId");
    const std::wstring gameId    = getEnvStr(L"SteamGameId");
    const std::wstring overlayId = getEnvStr(L"SteamOverlayGameId");
    if (!isForeign(appId) && !isForeign(gameId) && !isForeign(overlayId) && tempDisabledIds.empty()) {
        return {};
    }

    LPWCH rawEnv = GetEnvironmentStringsW();
    if (!rawEnv) return {};

    std::vector<std::wstring> entries;
    for (const wchar_t* p = rawEnv; *p;) {
        const size_t len = std::wcslen(p);
        std::wstring entry(p, len);
        p += len + 1;

        const size_t eq = entry.find(L'=');
        const std::wstring key = (eq != std::wstring::npos) ? entry.substr(0, eq) : entry;
        if (_wcsicmp(key.c_str(), L"SteamAppId") == 0 ||
            _wcsicmp(key.c_str(), L"SteamGameId") == 0 ||
            _wcsicmp(key.c_str(), L"SteamOverlayGameId") == 0 ||
            _wcsicmp(key.c_str(), L"GRML_TEMP_DISABLED_MODS") == 0) {
            continue;
        }
        entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(rawEnv);

    if (!tempDisabledIds.empty()) {
        std::string joined;
        for (size_t i = 0; i < tempDisabledIds.size(); ++i) {
            if (i > 0) joined += ',';
            joined += tempDisabledIds[i];
        }
        entries.push_back(L"GRML_TEMP_DISABLED_MODS=" + grml::utf8ToWide(joined));
    }

    std::vector<wchar_t> block;
    for (const auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

} // namespace

LaunchResult launchAndInjectWrsr(const std::filesystem::path& loaderDir,
                                 const std::filesystem::path& gameExe,
                                 const std::vector<std::string>& tempDisabledIds)
{
    LaunchResult res{};
    std::error_code ec;

    const auto runtimeDll = loaderDir / L"grml_runtime.dll";
    if (!std::filesystem::is_regular_file(runtimeDll, ec)) {
        res.message = "grml_runtime.dll not found next to GloriousRepublicModLoader.exe";
        return res;
    }
    if (!std::filesystem::is_regular_file(gameExe, ec)) {
        res.message = "SOVIET64.exe not found at configured path";
        return res;
    }

    const uint32_t bootstrapRva = findLocalExportRva(runtimeDll, "GrmlBootstrap");
    if (bootstrapRva == 0) {
        res.message = "Failed to locate GrmlBootstrap export in grml_runtime.dll";
        return res;
    }

    std::wstring cmdLine = L"\"" + gameExe.wstring() + L"\"";
    std::wstring workDir = gameExe.parent_path().wstring();

    std::vector<wchar_t> envBlock = buildSanitizedChildEnvIfNeeded(tempDisabledIds);
    DWORD flags = NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED;
    LPVOID envPtr = nullptr;
    if (!envBlock.empty()) {
        flags |= CREATE_UNICODE_ENVIRONMENT;
        envPtr = envBlock.data();
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(gameExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                        flags, envPtr, workDir.c_str(), &si, &pi)) {
        char buf[128]{};
        std::snprintf(buf, sizeof(buf), "CreateProcessW failed (GetLastError=%lu)", GetLastError());
        res.message = buf;
        return res;
    }

    auto failClosed = [&](const std::string& reason) -> LaunchResult {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        res.ok      = false;
        res.message = reason;
        return res;
    };

    // Resume primary thread so Windows LdrpInitializeProcess initializes kernel32.dll
    ResumeThread(pi.hThread);

    uintptr_t remoteKernel32 = 0;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (isModuleLoadedRemote(pi.dwProcessId, L"kernel32.dll", remoteKernel32) && remoteKernel32 != 0) {
            break;
        }
        Sleep(25);
    }
    if (remoteKernel32 == 0) {
        return failClosed("Timed out waiting for kernel32.dll in SOVIET64.exe");
    }

    HMODULE localKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoadLib  = GetProcAddress(localKernel32, "LoadLibraryW");
    if (!localKernel32 || !localLoadLib) {
        return failClosed("Unable to resolve kernel32!LoadLibraryW");
    }
    const uintptr_t remoteLoadLib =
        remoteKernel32 + (reinterpret_cast<uintptr_t>(localLoadLib) - reinterpret_cast<uintptr_t>(localKernel32));

    // Allocate & write wide path to grml_runtime.dll in SOVIET64.exe
    const std::wstring dllPathW = runtimeDll.wstring();
    const size_t dllBytes       = (dllPathW.size() + 1) * sizeof(wchar_t);
    LPVOID remoteDllPath = VirtualAllocEx(pi.hProcess, nullptr, dllBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteDllPath || !WriteProcessMemory(pi.hProcess, remoteDllPath, dllPathW.c_str(), dllBytes, nullptr)) {
        return failClosed("VirtualAllocEx/WriteProcessMemory failed for grml_runtime.dll path");
    }

    HANDLE hLoadThread = CreateRemoteThread(
        pi.hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoadLib),
        remoteDllPath, 0, nullptr);
    if (!hLoadThread) {
        VirtualFreeEx(pi.hProcess, remoteDllPath, 0, MEM_RELEASE);
        return failClosed("CreateRemoteThread(LoadLibraryW) failed");
    }
    const DWORD loadWait = WaitForSingleObject(hLoadThread, 20000);
    CloseHandle(hLoadThread);
    if (loadWait == WAIT_OBJECT_0) {
        VirtualFreeEx(pi.hProcess, remoteDllPath, 0, MEM_RELEASE);
    } else {
        return failClosed("Timed out waiting for remote LoadLibraryW(grml_runtime.dll)");
    }

    uintptr_t remoteRuntimeBase = 0;
    if (!isModuleLoadedRemote(pi.dwProcessId, L"grml_runtime.dll", remoteRuntimeBase) || remoteRuntimeBase == 0) {
        return failClosed("grml_runtime.dll failed to load into SOVIET64.exe");
    }

    // Invoke GrmlBootstrap(remoteLoaderRootW)
    const std::wstring rootW = loaderDir.wstring();
    const size_t rootBytes   = (rootW.size() + 1) * sizeof(wchar_t);
    LPVOID remoteRoot = VirtualAllocEx(pi.hProcess, nullptr, rootBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteRoot || !WriteProcessMemory(pi.hProcess, remoteRoot, rootW.c_str(), rootBytes, nullptr)) {
        return failClosed("Failed to write loader root path to SOVIET64.exe");
    }

    const uintptr_t remoteBootstrap = remoteRuntimeBase + bootstrapRva;
    HANDLE hBootThread = CreateRemoteThread(
        pi.hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBootstrap),
        remoteRoot, 0, nullptr);
    if (!hBootThread) {
        VirtualFreeEx(pi.hProcess, remoteRoot, 0, MEM_RELEASE);
        return failClosed("CreateRemoteThread(GrmlBootstrap) failed");
    }
    const DWORD bootWait = WaitForSingleObject(hBootThread, 30000);
    DWORD bootExitCode   = 1;
    GetExitCodeThread(hBootThread, &bootExitCode);
    CloseHandle(hBootThread);
    if (bootWait == WAIT_OBJECT_0) {
        VirtualFreeEx(pi.hProcess, remoteRoot, 0, MEM_RELEASE);
    } else {
        return failClosed("Timed out waiting for GrmlBootstrap");
    }

    if (bootExitCode != 0) {
        return failClosed("GrmlBootstrap returned failure status");
    }

    res.ok        = true;
    res.processId = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    char okBuf[160]{};
    std::snprintf(okBuf, sizeof(okBuf), "Launched SOVIET64.exe (PID %u) with GRML active.", res.processId);
    res.message = okBuf;
    return res;
}

} // namespace grml::launcher
