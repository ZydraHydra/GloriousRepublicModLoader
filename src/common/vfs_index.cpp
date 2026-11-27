#include "common/vfs_index.hpp"
#include "common/ini_util.hpp"
#include "common/pe_scanner.hpp"
#include "common/zip_reader.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace grml::vfs {

namespace {

bool dirExists(const std::wstring& path) noexcept
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

bool fileExists(const std::wstring& path) noexcept
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

std::wstring toLowerWide(std::wstring s)
{
    for (wchar_t& ch : s) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        } else if (ch == L'/') {
            ch = L'\\';
        }
    }
    return s;
}

std::wstring normalizeRelKey(const std::wstring& rel)
{
    std::wstring out;
    out.reserve(rel.size());
    for (wchar_t ch : rel) {
        if (ch == L'/') ch = L'\\';
        else if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        if (ch == L'\\' && !out.empty() && out.back() == L'\\') continue;
        out.push_back(ch);
    }
    return out;
}

std::wstring sanitizeIdForPath(const std::string& id)
{
    std::wstring out;
    out.reserve(id.size());
    for (char c : id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'_');
        }
    }
    return out;
}

std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

std::string parseWorkshopTitle(const std::wstring& workshopDir)
{
    std::string content;
    if (!readFileBytesW(workshopDir + L"\\workshopconfig.ini", content)) return {};
    const size_t pos = content.find("$ITEM_NAME");
    if (pos == std::string::npos) return {};
    const size_t q1 = content.find('"', pos);
    if (q1 == std::string::npos) return {};
    const size_t q2 = content.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 <= q1 + 1) return {};
    return content.substr(q1 + 1, q2 - (q1 + 1));
}

size_t countFilesRecursive(const std::wstring& baseDir)
{
    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((baseDir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    size_t count = 0;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            count += countFilesRecursive(baseDir + L"\\" + ffd.cFileName);
        } else {
            ++count;
        }
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
    return count;
}

void collectFilesRecursive(const std::wstring& baseDir,
                           const std::wstring& relPrefix,
                           std::vector<std::pair<std::wstring, std::wstring>>& outFiles)
{
    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((baseDir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
        const std::wstring fullPath = baseDir + L"\\" + ffd.cFileName;
        const std::wstring relPath  = relPrefix + L"\\" + ffd.cFileName;
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            collectFilesRecursive(fullPath, relPath, outFiles);
        } else {
            outFiles.emplace_back(normalizeRelKey(relPath), fullPath);
        }
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
}

std::wstring findFirstMediaZipInDir(const std::wstring& dir, size_t& outZipCount)
{
    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((dir + L"\\*.zip").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return {};
    std::wstring matched;
    do {
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const std::wstring zipPath = dir + L"\\" + ffd.cFileName;
        const size_t cnt = zip::countMediaEntries(zipPath);
        if (cnt > 0) {
            matched     = zipPath;
            outZipCount = cnt;
            break;
        }
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
    return matched;
}

void scanLocalPacks(const std::wstring& overridesDir, std::vector<PackInfo>& discovered)
{
    if (overridesDir.empty() || !dirExists(overridesDir)) return;

    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((overridesDir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
        if (ffd.cFileName[0] == L'.') continue;

        const std::wstring fullItem = overridesDir + L"\\" + ffd.cFileName;
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            const bool hasMedia    = dirExists(fullItem + L"\\media_soviet");
            const bool hasOverride = dirExists(fullItem + L"\\override");
            size_t zipCount = 0;
            std::wstring zipFile;
            if (!hasMedia && !hasOverride) {
                zipFile = findFirstMediaZipInDir(fullItem, zipCount);
                if (zipFile.empty()) continue;
            }
            const std::string nameUtf8 = wideToUtf8(ffd.cFileName);
            PackInfo info;
            info.id          = "local:" + nameUtf8;
            info.displayName = nameUtf8;
            info.source      = PackSource::Local;
            info.rootDir     = fullItem;
            info.zipPath     = zipFile;
            info.totalFiles  = !zipFile.empty()
                                   ? zipCount
                                   : ((hasMedia ? countFilesRecursive(fullItem + L"\\media_soviet") : 0) +
                                      (hasOverride ? countFilesRecursive(fullItem + L"\\override") : 0));
            discovered.push_back(std::move(info));
        } else {
            const std::wstring nameLower = toLowerWide(ffd.cFileName);
            if (nameLower.size() > 4 && nameLower.compare(nameLower.size() - 4, 4, L".zip") == 0) {
                const size_t zipCount = zip::countMediaEntries(fullItem);
                if (zipCount > 0) {
                    const std::string nameUtf8 = wideToUtf8(ffd.cFileName);
                    PackInfo info;
                    info.id          = "local:" + nameUtf8;
                    info.displayName = nameUtf8;
                    info.source      = PackSource::Local;
                    info.rootDir     = overridesDir;
                    info.zipPath     = fullItem;
                    info.totalFiles  = zipCount;
                    discovered.push_back(std::move(info));
                }
            }
        }
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
}

void scanWorkshopPacks(const std::filesystem::path& wsRootPath, std::vector<PackInfo>& discovered)
{
    const std::wstring wsRoot = wsRootPath.wstring();
    if (wsRoot.empty() || !dirExists(wsRoot)) return;

    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((wsRoot + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;

        const std::wstring packRoot    = wsRoot + L"\\" + ffd.cFileName;
        const bool         hasMedia    = dirExists(packRoot + L"\\media_soviet");
        const bool         hasOverride = dirExists(packRoot + L"\\override");
        size_t             zipCount    = 0;
        std::wstring       zipFile;
        if (!hasMedia && !hasOverride) {
            zipFile = findFirstMediaZipInDir(packRoot, zipCount);
            if (zipFile.empty()) continue;
        }

        const std::string wsIdUtf8 = wideToUtf8(ffd.cFileName);
        const std::string title    = parseWorkshopTitle(packRoot);
        PackInfo info;
        info.id          = "ws:" + wsIdUtf8;
        info.displayName = title.empty() ? ("Workshop " + wsIdUtf8) : (title + " [" + wsIdUtf8 + "]");
        info.source      = PackSource::Workshop;
        info.rootDir     = packRoot;
        info.zipPath     = zipFile;
        info.totalFiles  = !zipFile.empty()
                               ? zipCount
                               : ((hasMedia ? countFilesRecursive(packRoot + L"\\media_soviet") : 0) +
                                  (hasOverride ? countFilesRecursive(packRoot + L"\\override") : 0));
        discovered.push_back(std::move(info));
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
}

} // namespace

VfsIndex& VfsIndex::get() noexcept
{
    static VfsIndex instance;
    return instance;
}

void VfsIndex::build(const std::filesystem::path& loaderDir,
                     const std::filesystem::path& gameDir,
                     bool extractZips,
                     const std::filesystem::path& workshopRootOverride,
                     const std::filesystem::path& overridesRootOverride)
{
    m_map.clear();
    m_packs.clear();
    m_conflicts.clear();

    const auto iniPath = loaderDir / L"grml_vfs.ini";
    IniFile cfg;
    cfg.loadFromFile(iniPath);

    m_enabled.store(cfg.getBool("settings", "enabled", true), std::memory_order_relaxed);
    m_scanWorkshop = true;

    std::vector<std::string> configuredOrder;
    if (const auto* prio = cfg.sectionKeys("Priority")) {
        std::vector<std::pair<int, std::string>> numbered;
        numbered.reserve(prio->size());
        for (const auto& [k, v] : *prio) {
            if (!v.empty()) {
                numbered.emplace_back(std::atoi(k.c_str()), v);
            }
        }
        std::sort(numbered.begin(), numbered.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (auto& [idx, val] : numbered) {
            configuredOrder.push_back(std::move(val));
        }
    }

    const std::wstring overridesDirW = !overridesRootOverride.empty()
        ? overridesRootOverride.wstring()
        : (gameDir.empty() ? std::wstring{} : (gameDir / L"media_overrides").wstring());
    const std::filesystem::path wsRoot = !workshopRootOverride.empty()
        ? workshopRootOverride
        : (gameDir.empty() ? workshopRootFromGameExe({}) : workshopRootFromGameExe(gameDir / L"SOVIET64.exe"));

    std::vector<PackInfo> discovered;
    if (!overridesDirW.empty()) {
        scanLocalPacks(overridesDirW, discovered);
    }
    if (!wsRoot.empty()) {
        scanWorkshopPacks(wsRoot, discovered);
    }

    std::sort(discovered.begin(), discovered.end(), [](const PackInfo& a, const PackInfo& b) {
        return a.id < b.id;
    });

    std::unordered_map<std::string, size_t> idToIndex;
    for (size_t i = 0; i < discovered.size(); ++i) {
        idToIndex[discovered[i].id] = i;
    }

    std::vector<bool> placed(discovered.size(), false);
    std::vector<PackInfo> ordered;
    ordered.reserve(discovered.size());

    for (const auto& id : configuredOrder) {
        const auto it = idToIndex.find(id);
        if (it != idToIndex.end() && !placed[it->second]) {
            placed[it->second] = true;
            ordered.push_back(std::move(discovered[it->second]));
        }
    }
    for (size_t i = 0; i < discovered.size(); ++i) {
        if (!placed[i]) {
            ordered.push_back(std::move(discovered[i]));
        }
    }

    // Opt-in default: packs start disabled unless explicitly enabled in grml_vfs.ini
    for (auto& p : ordered) {
        p.enabled = cfg.getBool("Enabled", p.id, false);
    }

    m_packs = std::move(ordered);

    std::unordered_map<std::wstring, size_t, TransparentWStringHash, std::equal_to<>> winnerPackIdx;
    std::unordered_map<std::wstring, size_t, TransparentWStringHash, std::equal_to<>> conflictIdxByRel;

    for (size_t packIdx = 0; packIdx < m_packs.size(); ++packIdx) {
        auto& pack = m_packs[packIdx];
        if (!pack.enabled) continue;

        std::vector<std::pair<std::wstring, std::wstring>> files;
        if (!pack.zipPath.empty()) {
            if (!extractZips) continue;
            const std::wstring cacheDir =
                overridesDirW + L"\\.vfs_cache\\" + sanitizeIdForPath(pack.id);
            zip::extractMediaEntries(pack.zipPath, cacheDir, files);
        } else {
            const std::wstring mediaSub    = pack.rootDir + L"\\media_soviet";
            const std::wstring overrideSub = pack.rootDir + L"\\override";
            if (dirExists(mediaSub)) collectFilesRecursive(mediaSub, L"media_soviet", files);
            if (dirExists(overrideSub)) collectFilesRecursive(overrideSub, L"media_soviet", files);
        }

        pack.totalFiles = files.size();
        for (auto& [relKey, fullTarget] : files) {
            auto existing = m_map.find(relKey);
            if (existing == m_map.end()) {
                m_map.emplace(relKey, std::move(fullTarget));
                winnerPackIdx.emplace(std::move(relKey), packIdx);
                ++pack.indexedFiles;
            } else {
                const size_t winIdx = winnerPackIdx[relKey];
                if (winIdx == packIdx) continue;
                ++pack.conflictCount;
                auto cIt = conflictIdxByRel.find(relKey);
                if (cIt == conflictIdxByRel.end()) {
                    ++m_packs[winIdx].conflictCount;
                    ConflictEntry entry;
                    entry.relativePath = wideToUtf8(relKey);
                    entry.winningPack  = m_packs[winIdx].displayName;
                    entry.overriddenPacks.push_back(pack.displayName);
                    conflictIdxByRel.emplace(relKey, m_conflicts.size());
                    m_conflicts.push_back(std::move(entry));
                } else {
                    m_conflicts[cIt->second].overriddenPacks.push_back(pack.displayName);
                }
            }
        }
    }

    m_built = true;
}

bool VfsIndex::saveConfig(const std::filesystem::path& loaderDir,
                          bool enabled,
                          const std::vector<std::string>& orderedIds,
                          const std::unordered_map<std::string, bool>& enabledMap)
{
    IniFile cfg;
    cfg.setBool("settings", "enabled", enabled);

    for (size_t i = 0; i < orderedIds.size(); ++i) {
        char keyBuf[32]{};
        std::snprintf(keyBuf, sizeof(keyBuf), "%zu", i);
        cfg.setString("Priority", keyBuf, orderedIds[i]);
        const auto it = enabledMap.find(orderedIds[i]);
        const bool isOn = (it != enabledMap.end()) ? it->second : false;
        cfg.setBool("Enabled", orderedIds[i], isOn);
    }
    return cfg.saveToFile(loaderDir / L"grml_vfs.ini");
}

std::string VfsIndex::serializeJson() const
{
    std::string out = "{";
    out += "\"enabled\":" + std::string(isEnabled() ? "true" : "false") + ",";
    out += "\"scanWorkshop\":" + std::string(m_scanWorkshop ? "true" : "false") + ",";
    out += "\"indexedFiles\":" + std::to_string(m_map.size()) + ",";
    out += "\"conflictCount\":" + std::to_string(m_conflicts.size()) + ",";
    out += "\"packs\":[";
    for (size_t i = 0; i < m_packs.size(); ++i) {
        if (i > 0) out += ',';
        const auto& p = m_packs[i];
        out += "{";
        out += "\"id\":\"" + jsonEscape(p.id) + "\",";
        out += "\"displayName\":\"" + jsonEscape(p.displayName) + "\",";
        out += "\"source\":\"" + std::string(p.source == PackSource::Workshop ? "Workshop" : "Local") + "\",";
        out += "\"rootDir\":\"" + jsonEscape(wideToUtf8(p.rootDir)) + "\",";
        out += "\"zipPath\":\"" + jsonEscape(wideToUtf8(p.zipPath)) + "\",";
        out += "\"enabled\":" + std::string(p.enabled ? "true" : "false") + ",";
        out += "\"indexedFiles\":" + std::to_string(p.indexedFiles) + ",";
        out += "\"totalFiles\":" + std::to_string(p.totalFiles) + ",";
        out += "\"conflictCount\":" + std::to_string(p.conflictCount);
        out += "}";
    }
    out += "],\"conflicts\":[";
    const size_t maxC = (std::min<size_t>)(m_conflicts.size(), 100);
    for (size_t i = 0; i < maxC; ++i) {
        if (i > 0) out += ',';
        const auto& c = m_conflicts[i];
        out += "{";
        out += "\"relativePath\":\"" + jsonEscape(c.relativePath) + "\",";
        out += "\"winningPack\":\"" + jsonEscape(c.winningPack) + "\",";
        out += "\"overriddenPacks\":[";
        for (size_t j = 0; j < c.overriddenPacks.size(); ++j) {
            if (j > 0) out += ',';
            out += "\"" + jsonEscape(c.overriddenPacks[j]) + "\"";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

} // namespace grml::vfs
