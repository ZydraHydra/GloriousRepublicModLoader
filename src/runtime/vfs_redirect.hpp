#pragma once
// ============================================================================
// vfs_redirect.hpp — Zero-alloc, lock-free Media VFS runtime redirection hooks
// ============================================================================
#include <cstdint>
#include <filesystem>

namespace grml::vfs_redirect {

constexpr size_t kNormBufCap = 512;

struct DevPathOverrides {
    bool                  enabled{false};
    std::filesystem::path saveRoot;        // overrides media_soviet\save
    std::filesystem::path workshopRoot;    // overrides workshop\content\784150
    std::filesystem::path workshopWipRoot; // overrides media_soviet\workshop_wip
    std::filesystem::path overridesRoot;   // overrides media_overrides
};

// Builds VFS index from <loaderDir>\grml_vfs.ini + <gameDir> and installs
// CreateFileA, CreateFile2, GetFileAttributesW/A/ExW hooks (CreateFileW shares
// save_events.cpp's single hook via tryResolveReadOnlyW / tryResolveDevPathW).
// When devOverrides.enabled is true with at least one path, also hooks
// FindFirstFile*/CreateDirectory*/DeleteFile*/RemoveDirectory*/MoveFile* for
// transparent read+write+enumerate redirection.
bool install(const std::filesystem::path& loaderDir,
             const std::filesystem::path& gameDir,
             const DevPathOverrides& devOverrides = {});

void shutdown();

// Zero-alloc hot-path lookup used by CreateFileW in save_events.cpp
const wchar_t* tryResolveReadOnlyW(const wchar_t* lpFileName,
                                   wchar_t (&normBuf)[kNormBufCap]) noexcept;

// Zero-alloc hot-path dev-mode path override lookup (read + write + enumerate)
const wchar_t* tryResolveDevPathW(const wchar_t* lpFileName,
                                  wchar_t (&outBuf)[kNormBufCap],
                                  bool* outIsSavePath = nullptr) noexcept;

[[nodiscard]] uint64_t redirectHits() noexcept;
[[nodiscard]] uint64_t devRedirectHits() noexcept;
[[nodiscard]] bool     isHookActive() noexcept;
[[nodiscard]] bool     isDevRedirectActive() noexcept;

} // namespace grml::vfs_redirect
