#pragma once
// ============================================================================
// offset_resolver.hpp — Declarative grml_offsets.ini parser, resolver & cache
// ============================================================================
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/ini_util.hpp"
#include "common/pe_scanner.hpp"
#include "grml_api.h"

namespace grml::offsets {

enum class OffsetResolveMode {
    FunctionStart,
    RipRel32,
    CallRel32,
    Disp32,
};

enum class OffsetStatus {
    Unscanned,
    Ok,
    Cached,
    Missing,
};

struct OffsetDefinition {
    std::string       name;
    std::string       pattern;
    OffsetResolveMode resolveMode{OffsetResolveMode::FunctionStart};
    int32_t           opOffset{0};
    int32_t           instrLen{0};
    int64_t           extraOffset{0};
    std::string       category{"General"};
    std::string       anchorXref;
    std::string       verifiedBuild;
    bool              required{false};
};

struct ResolvedOffsetEntry {
    OffsetDefinition def;
    OffsetStatus     status{OffsetStatus::Unscanned};
    size_t           matchCount{0};
    uint32_t         matchedRva{0};
    uintptr_t        resolvedValue{0};
};

const char*       resolveModeToString(OffsetResolveMode mode) noexcept;
OffsetResolveMode parseResolveMode(std::string_view text) noexcept;
const char*       offsetStatusToString(OffsetStatus status) noexcept;

// Loads embedded grml_offsets.ini, merges loaderDir/grml_offsets.ini and
// dataDir/grml_offsets.ini (OTA override), and computes a canonical 64-bit
// FNV-1a hash of the effective INI contents.
bool loadMergedOffsetsIni(const std::filesystem::path& loaderDir,
                          const std::filesystem::path& dataDir,
                          std::string_view embeddedIni,
                          IniFile& outIni,
                          uint64_t& outHash);

// Collects all declarative offset sections (sections containing `pattern`).
std::vector<OffsetDefinition> collectOffsetDefinitions(const IniFile& ini);

// Applies `resolve` rule (function_start, rip_rel32, call_rel32, disp32)
// with bounds checks against `pe`.
bool applyOffsetResolveRule(const PeModuleInfo& pe,
                            const uint8_t* matchAddr,
                            const OffsetDefinition& def,
                            uintptr_t& outValue) noexcept;

// Checks grml_offset_cache.ini against PE COFF fingerprint + INI FNV-1a hash.
// Verifies pattern bytes/mask at module_base + cached_rva in O(1).
// Returns true iff ALL definitions were verified from cache.
bool tryResolveFromCache(const std::filesystem::path& cachePath,
                         const PeModuleInfo& pe,
                         uint64_t iniHash,
                         const std::vector<OffsetDefinition>& defs,
                         std::vector<ResolvedOffsetEntry>& outEntries);

// Persists PE fingerprint, INI hash, and uniquely matched RVAs to grml_offset_cache.ini.
bool writeOffsetCache(const std::filesystem::path& cachePath,
                      const PeModuleInfo& pe,
                      uint64_t iniHash,
                      const std::vector<ResolvedOffsetEntry>& entries);

// Resolves [profile.<ver>] WRSR struct offsets with recursive `base = <parent>`
// inheritance and comma-separated pe_timestamp / pe_size_of_image lists.
bool resolveWrsrProfileOffsets(const IniFile& ini,
                               const PeModuleInfo& pe,
                               GrmlWrsrOffsets& outOffsets);

} // namespace grml::offsets
