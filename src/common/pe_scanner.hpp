#pragma once
// ============================================================================
// pe_scanner.hpp — PE inspection, SHA-256 & single-pass .text AOB scanner
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace grml {

struct PeModuleInfo {
    uint8_t* base{nullptr};
    size_t   imageSize{0};
    uint32_t timeDateStamp{0};
    uint8_t* textBase{nullptr};
    size_t   textSize{0};
    uint8_t* runtimeFuncs{nullptr};
    uint32_t runtimeFuncCount{0};
};

struct AobPatternRequest {
    std::string name;
    std::string patternStr;
    void**      outAddr{nullptr};
    bool        required{false};

    // Populated by runSinglePassAobScan:
    size_t matchCount{0};
    void*  firstMatch{nullptr};
};

struct AobScanSummary {
    size_t totalPatterns{0};
    size_t uniqueMatches{0};
    size_t missingCount{0};
    size_t ambiguousCount{0};
};

// Inspects a loaded PE32+ (AMD64) image in memory.
bool inspectLoadedPe(uint8_t* moduleBase, PeModuleInfo& out);

// Reads an on-disk PE file and checks AMD64 DLL flag + exports list.
struct DiskPeExports {
    bool                     isValidAmd64Dll{false};
    std::vector<std::string> exports;
};
DiskPeExports inspectDiskPeExports(const std::filesystem::path& path);

// Computes lowercase hex SHA-256 of a file using BCrypt (wide-path safe).
std::string computeFileSha256(const std::filesystem::path& path);

// Executes a single sequential pass over [scanBase, scanBase + scanSize)
// evaluating all patterns in `requests` simultaneously. Sets *outAddr ONLY
// when matchCount == 1; otherwise sets *outAddr = nullptr.
AobScanSummary runSinglePassAobScan(const uint8_t* scanBase,
                                    size_t scanSize,
                                    std::vector<AobPatternRequest>& requests);

// Resolves RIP-relative disp32 target from an instruction site.
void* resolveRipRelativeDisp32(void* site, uint32_t dispOff, uint32_t nextIpOff) noexcept;

// Looks up `.pdata` RUNTIME_FUNCTION bounds for a given RVA.
bool lookupRuntimeFunctionRva(const PeModuleInfo& pe, uint32_t rva, uint32_t& outBegin, uint32_t& outEnd) noexcept;

// Steam / WRSR wide-path discovery helpers (app 784150)
std::vector<std::filesystem::path> discoverSteamLibraries();
std::filesystem::path discoverWrsrGameExe(const std::filesystem::path& baseDir,
                                          const std::filesystem::path& configuredPath);
std::filesystem::path workshopRootFromGameExe(const std::filesystem::path& gameExe);

} // namespace grml
