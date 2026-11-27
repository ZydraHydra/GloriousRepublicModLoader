#pragma once
// ============================================================================
// save_events.hpp — Centralized CreateFileW save lifecycle hook for GRML
// ============================================================================
// Hooks KERNELBASE!CreateFileW (with KERNEL32 fallback) once on behalf of all
// plugins so individual mods do not install redundant CreateFileW detours.
// Fast-path follows HOOKING_AND_PERFORMANCE_RULES.md: zero heap allocations
// and zero locks for non-save file opens.
// ============================================================================
#include "common/offset_resolver.hpp"
#include "grml_api.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace grml::save_events {

struct SessionDiagnosticsSnapshot {
    bool     hasData{false};
    std::string gameVersion;
    uint32_t    peTimestamp{0};
    uint32_t    peSizeOfImage{0};
    uint32_t    peEntryPoint{0};
    uint64_t    offsetsFnv1a64{0};
    std::vector<grml::offsets::ResolvedOffsetEntry> offsets;
};

bool install(const std::filesystem::path& loaderDir = {});
void shutdown();

void registerLoadUnload(GrmlSaveEventFn onLoad, GrmlSaveEventFn onUnload, void* userData);
void registerWrite(GrmlSaveEventFn onWrite, void* userData);
void publishOffsetDiagnostics(SessionDiagnosticsSnapshot snapshot);

// Called by overlay_hub / runtime when in-game flag transitions 1 -> 0 (Main Menu)
void notifyInGameState(bool inGame);

// One-shot Direct-to-Save auto-load API (lock-free fast-exit check for Present hook)
bool hasPendingAutoLoad() noexcept;
std::wstring consumePendingAutoLoad();

} // namespace grml::save_events
