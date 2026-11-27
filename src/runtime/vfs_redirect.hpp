#pragma once
// ============================================================================
// vfs_redirect.hpp — Zero-alloc, lock-free Media VFS runtime redirection hooks
// ============================================================================
#include <cstdint>
#include <filesystem>

namespace grml::vfs_redirect {

constexpr size_t kNormBufCap = 512;

// Builds VFS index from <loaderDir>\grml_vfs.ini + <gameDir> and installs
// CreateFileA, CreateFile2, GetFileAttributesW/A/ExW hooks (CreateFileW shares
// save_events.cpp's single hook via tryResolveReadOnlyW).
bool install(const std::filesystem::path& loaderDir,
             const std::filesystem::path& gameDir);

void shutdown();

// Zero-alloc hot-path lookup used by CreateFileW in save_events.cpp
const wchar_t* tryResolveReadOnlyW(const wchar_t* lpFileName,
                                   wchar_t (&normBuf)[kNormBufCap]) noexcept;

[[nodiscard]] uint64_t redirectHits() noexcept;
[[nodiscard]] bool     isHookActive() noexcept;

} // namespace grml::vfs_redirect
