#pragma once
// ============================================================================
// save_events.hpp — Centralized CreateFileW save lifecycle hook for GRML
// ============================================================================
// Hooks KERNELBASE!CreateFileW (with KERNEL32 fallback) once on behalf of all
// plugins so individual mods do not install redundant CreateFileW detours.
// Fast-path follows HOOKING_AND_PERFORMANCE_RULES.md: zero heap allocations
// and zero locks for non-save file opens.
// ============================================================================
#include "grml_api.h"

#include <filesystem>

namespace grml::save_events {

bool install(const std::filesystem::path& loaderDir = {});
void shutdown();

void registerLoadUnload(GrmlSaveEventFn onLoad, GrmlSaveEventFn onUnload, void* userData);
void registerWrite(GrmlSaveEventFn onWrite, void* userData);

// Called by overlay_hub / runtime when in-game flag transitions 1 -> 0 (Main Menu)
void notifyInGameState(bool inGame);

} // namespace grml::save_events
