#pragma once
// ============================================================================
// ini_util.hpp — UTF-8 (BOM-safe) INI reader/writer using wide Win32 paths
// ============================================================================
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace grml {

class IniFile {
public:
    bool loadFromMemory(std::string_view text);
    bool loadFromFile(const std::filesystem::path& path);
    bool saveToFile(const std::filesystem::path& path) const;

    void merge(const IniFile& overlay);

    std::string getString(std::string_view section, std::string_view key, std::string_view fallback = "") const;
    int         getInt(std::string_view section, std::string_view key, int fallback = 0) const;
    double      getDouble(std::string_view section, std::string_view key, double fallback = 0.0) const;
    bool        getBool(std::string_view section, std::string_view key, bool fallback = false) const;

    void setString(std::string_view section, std::string_view key, std::string_view value);
    void setInt(std::string_view section, std::string_view key, int value);
    void setDouble(std::string_view section, std::string_view key, double value);
    void setBool(std::string_view section, std::string_view key, bool value);

    const std::unordered_map<std::string, std::string>* sectionKeys(std::string_view section) const;
    std::vector<std::string> sectionOrder() const;

private:
    struct SectionData {
        std::vector<std::string>                     keyOrder;
        std::unordered_map<std::string, std::string> kv;
    };
    std::vector<std::string>                      m_sectionOrder;
    std::unordered_map<std::string, SectionData>  m_sections;
};

// Wide / UTF-8 conversion helpers (CP_UTF8, safe for non-ASCII Windows paths)
std::wstring utf8ToWide(std::string_view s);
std::string  wideToUtf8(std::wstring_view w);

// Win32 wide file I/O helpers (no ANSI truncation)
bool readFileBytesW(const std::filesystem::path& path, std::string& outBytes);
bool writeFileBytesW(const std::filesystem::path& path, std::string_view bytes);

struct IniKeyUpdate {
    std::string section;
    std::string key;
    std::string value;
};

// Updates key=value pairs in-place inside an existing .ini file while preserving
// comments (; / #), blank lines, and original section/key casing.
bool updateIniFilePreservingComments(const std::filesystem::path& path,
                                     const std::vector<IniKeyUpdate>& updates);

} // namespace grml
