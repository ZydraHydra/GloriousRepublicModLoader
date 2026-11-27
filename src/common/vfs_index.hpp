#pragma once
// ============================================================================
// vfs_index.hpp — GRML Asset Overrides (Media VFS) scanner & priority map
// ============================================================================
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace grml::vfs {

enum class PackSource {
    Local,
    Workshop,
};

struct PackInfo {
    std::string  id;           // e.g. "local:MyPack" or "ws:123456789"
    std::string  displayName;
    PackSource   source{PackSource::Local};
    std::wstring rootDir;
    std::wstring zipPath;
    bool         enabled{false};
    size_t       indexedFiles{0};
    size_t       totalFiles{0};
    size_t       conflictCount{0};
};

struct ConflictEntry {
    std::string              relativePath;
    std::string              winningPack;
    std::vector<std::string> overriddenPacks;
};

struct TransparentWStringHash {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::wstring_view sv) const noexcept
    {
        return std::hash<std::wstring_view>{}(sv);
    }

    [[nodiscard]] size_t operator()(const std::wstring& s) const noexcept
    {
        return std::hash<std::wstring_view>{}(s);
    }
};

using VfsMap = std::unordered_map<std::wstring, std::wstring, TransparentWStringHash, std::equal_to<>>;

class VfsIndex {
public:
    static VfsIndex& get() noexcept;

    // Scans local media_overrides\ and Steam Workshop 784150 media_soviet/override packs,
    // merges saved state from <loaderDir>\grml_vfs.ini, and builds the immutable VFS map.
    void build(const std::filesystem::path& loaderDir,
               const std::filesystem::path& gameDir,
               bool extractZips = true,
               const std::filesystem::path& workshopRootOverride = {},
               const std::filesystem::path& overridesRootOverride = {});

    // Persists updated settings + pack order/enabled flags to <loaderDir>\grml_vfs.ini.
    static bool saveConfig(const std::filesystem::path& loaderDir,
                           bool enabled,
                           const std::vector<std::string>& orderedIds,
                           const std::unordered_map<std::string, bool>& enabledMap);

    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }
    void setEnabled(bool on) noexcept { m_enabled.store(on, std::memory_order_relaxed); }

    [[nodiscard]] const VfsMap& map() const noexcept { return m_map; }
    [[nodiscard]] const std::vector<PackInfo>& packs() const noexcept { return m_packs; }
    [[nodiscard]] const std::vector<ConflictEntry>& conflicts() const noexcept { return m_conflicts; }
    [[nodiscard]] size_t indexedFileCount() const noexcept { return m_map.size(); }
    [[nodiscard]] size_t conflictCount() const noexcept { return m_conflicts.size(); }
    [[nodiscard]] bool isBuilt() const noexcept { return m_built; }

    [[nodiscard]] std::string serializeJson() const;

private:
    VfsIndex() = default;

    std::atomic<bool>          m_enabled{true};
    bool                       m_scanWorkshop{true};
    VfsMap                     m_map;
    std::vector<PackInfo>      m_packs;
    std::vector<ConflictEntry> m_conflicts;
    bool                       m_built{false};
};

} // namespace grml::vfs
