#include "common/ini_util.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace grml {

namespace {

std::string trimView(std::string_view sv)
{
    size_t b = 0;
    while (b < sv.size() && (sv[b] == ' ' || sv[b] == '\t' || sv[b] == '\r' || sv[b] == '\n')) {
        ++b;
    }
    size_t e = sv.size();
    while (e > b && (sv[e - 1] == ' ' || sv[e - 1] == '\t' || sv[e - 1] == '\r' || sv[e - 1] == '\n')) {
        --e;
    }
    return std::string(sv.substr(b, e - b));
}

std::string lowerAscii(std::string_view sv)
{
    std::string out(sv);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

} // namespace

std::wstring utf8ToWide(std::string_view s)
{
    if (s.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string wideToUtf8(std::wstring_view w)
{
    if (w.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

bool readFileBytesW(const std::filesystem::path& path, std::string& outBytes)
{
    outBytes.clear();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hFile, &sz) || sz.QuadPart < 0 || sz.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }
    outBytes.resize(static_cast<size_t>(sz.QuadPart));
    DWORD totalRead = 0;
    while (totalRead < static_cast<DWORD>(sz.QuadPart)) {
        DWORD chunk = 0;
        if (!ReadFile(hFile, outBytes.data() + totalRead,
                      static_cast<DWORD>(sz.QuadPart) - totalRead, &chunk, nullptr) ||
            chunk == 0) {
            break;
        }
        totalRead += chunk;
    }
    CloseHandle(hFile);
    outBytes.resize(totalRead);
    return totalRead == static_cast<DWORD>(sz.QuadPart);
}

bool writeFileBytesW(const std::filesystem::path& path, std::string_view bytes)
{
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD totalWritten = 0;
    while (totalWritten < static_cast<DWORD>(bytes.size())) {
        DWORD chunk = 0;
        if (!WriteFile(hFile, bytes.data() + totalWritten,
                       static_cast<DWORD>(bytes.size()) - totalWritten, &chunk, nullptr)) {
            CloseHandle(hFile);
            return false;
        }
        totalWritten += chunk;
    }
    CloseHandle(hFile);
    return true;
}

bool IniFile::loadFromMemory(std::string_view text)
{
    // Strip UTF-8 BOM if present
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    std::string currentSection = "general";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        std::string line = trimView(text.substr(pos, eol - pos));
        pos = eol + 1;

        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']' && line.size() >= 2) {
            currentSection = lowerAscii(trimView(std::string_view(line).substr(1, line.size() - 2)));
            if (m_sections.find(currentSection) == m_sections.end()) {
                m_sectionOrder.push_back(currentSection);
                m_sections.emplace(currentSection, SectionData{});
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trimView(std::string_view(line).substr(0, eq));
        const std::string val = trimView(std::string_view(line).substr(eq + 1));
        if (!key.empty()) {
            setString(currentSection, key, val);
        }
    }
    return true;
}

bool IniFile::loadFromFile(const std::filesystem::path& path)
{
    std::string raw;
    if (!readFileBytesW(path, raw)) {
        return false;
    }
    return loadFromMemory(raw);
}

bool IniFile::saveToFile(const std::filesystem::path& path) const
{
    std::string out;
    out.reserve(2048);
    for (size_t i = 0; i < m_sectionOrder.size(); ++i) {
        const auto& secName = m_sectionOrder[i];
        auto it = m_sections.find(secName);
        if (it == m_sections.end()) {
            continue;
        }
        if (i > 0) {
            out += '\n';
        }
        out += '[';
        out += secName;
        out += "]\n";
        for (const auto& k : it->second.keyOrder) {
            auto kvIt = it->second.kv.find(k);
            if (kvIt != it->second.kv.end()) {
                out += k;
                out += " = ";
                out += kvIt->second;
                out += '\n';
            }
        }
    }
    return writeFileBytesW(path, out);
}

void IniFile::merge(const IniFile& overlay)
{
    for (const auto& sec : overlay.m_sectionOrder) {
        auto it = overlay.m_sections.find(sec);
        if (it == overlay.m_sections.end()) {
            continue;
        }
        for (const auto& k : it->second.keyOrder) {
            auto kvIt = it->second.kv.find(k);
            if (kvIt != it->second.kv.end()) {
                setString(sec, k, kvIt->second);
            }
        }
    }
}

std::string IniFile::getString(std::string_view section, std::string_view key, std::string_view fallback) const
{
    const std::string secLower = lowerAscii(section);
    auto sIt = m_sections.find(secLower);
    if (sIt == m_sections.end()) {
        return std::string(fallback);
    }
    auto kIt = sIt->second.kv.find(std::string(key));
    if (kIt == sIt->second.kv.end()) {
        return std::string(fallback);
    }
    return kIt->second;
}

int IniFile::getInt(std::string_view section, std::string_view key, int fallback) const
{
    const std::string s = getString(section, key);
    if (s.empty()) {
        return fallback;
    }
    int v = fallback;
    auto res = std::from_chars(s.data(), s.data() + s.size(), v, 0);
    if (res.ec == std::errc{}) {
        return v;
    }
    return fallback;
}

double IniFile::getDouble(std::string_view section, std::string_view key, double fallback) const
{
    const std::string s = getString(section, key);
    if (s.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end && end != s.c_str()) {
        return v;
    }
    return fallback;
}

bool IniFile::getBool(std::string_view section, std::string_view key, bool fallback) const
{
    const std::string s = lowerAscii(getString(section, key));
    if (s.empty()) {
        return fallback;
    }
    if (s == "1" || s == "true" || s == "yes" || s == "on") {
        return true;
    }
    if (s == "0" || s == "false" || s == "no" || s == "off") {
        return false;
    }
    return fallback;
}

void IniFile::setString(std::string_view section, std::string_view key, std::string_view value)
{
    const std::string secLower = lowerAscii(section);
    auto& sec = m_sections[secLower];
    if (std::find(m_sectionOrder.begin(), m_sectionOrder.end(), secLower) == m_sectionOrder.end()) {
        m_sectionOrder.push_back(secLower);
    }
    const std::string keyStr(key);
    if (sec.kv.find(keyStr) == sec.kv.end()) {
        sec.keyOrder.push_back(keyStr);
    }
    sec.kv[keyStr] = std::string(value);
}

void IniFile::setInt(std::string_view section, std::string_view key, int value)
{
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%d", value);
    setString(section, key, buf);
}

void IniFile::setDouble(std::string_view section, std::string_view key, double value)
{
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.4f", value);
    setString(section, key, buf);
}

void IniFile::setBool(std::string_view section, std::string_view key, bool value)
{
    setString(section, key, value ? "1" : "0");
}

const std::unordered_map<std::string, std::string>* IniFile::sectionKeys(std::string_view section) const
{
    const std::string secLower = lowerAscii(section);
    auto it = m_sections.find(secLower);
    return (it != m_sections.end()) ? &it->second.kv : nullptr;
}

std::vector<std::string> IniFile::sectionOrder() const
{
    return m_sectionOrder;
}

bool updateIniFilePreservingComments(const std::filesystem::path& path,
                                     const std::vector<IniKeyUpdate>& updates)
{
    if (updates.empty()) {
        return true;
    }

    std::string raw;
    readFileBytesW(path, raw); // OK if missing; we'll build a new file

    // Preserve UTF-8 without BOM
    std::string_view view(raw);
    if (view.size() >= 3 &&
        static_cast<unsigned char>(view[0]) == 0xEF &&
        static_cast<unsigned char>(view[1]) == 0xBB &&
        static_cast<unsigned char>(view[2]) == 0xBF) {
        view.remove_prefix(3);
    }

    struct Pending {
        std::string section;
        std::string sectionLower;
        std::string key;
        std::string keyLower;
        std::string value;
        bool        applied{false};
    };

    std::vector<Pending> work;
    work.reserve(updates.size());
    for (const auto& u : updates) {
        if (u.key.empty()) continue;
        Pending p;
        p.section      = u.section.empty() ? std::string("settings") : u.section;
        p.sectionLower = lowerAscii(p.section);
        p.key          = u.key;
        p.keyLower     = lowerAscii(p.key);
        p.value        = u.value;
        work.push_back(std::move(p));
    }

    std::vector<std::string> lines;
    std::vector<std::string> lineSecLower;
    bool useCrlf = (view.find("\r\n") != std::string_view::npos);
    if (view.empty()) useCrlf = false;

    size_t pos = 0;
    std::string currentSecLower = "general";
    while (pos < view.size()) {
        size_t eol = view.find('\n', pos);
        const bool hasNl = (eol != std::string_view::npos);
        if (!hasNl) eol = view.size();
        std::string line(view.substr(pos, eol - pos));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pos = hasNl ? (eol + 1) : view.size();

        const std::string trimmed = trimView(line);
        if (!trimmed.empty() && trimmed.front() != ';' && trimmed.front() != '#') {
            if (trimmed.front() == '[' && trimmed.back() == ']' && trimmed.size() >= 2) {
                currentSecLower = lowerAscii(trimView(std::string_view(trimmed).substr(1, trimmed.size() - 2)));
            } else {
                const size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    const std::string k = trimView(std::string_view(line).substr(0, eq));
                    const std::string kLower = lowerAscii(k);
                    for (auto& w : work) {
                        if (!w.applied && w.sectionLower == currentSecLower && w.keyLower == kLower) {
                            // Keep original key prefix & spacing around '='
                            size_t afterEq = eq + 1;
                            while (afterEq < line.size() && (line[afterEq] == ' ' || line[afterEq] == '\t')) {
                                ++afterEq;
                            }
                            line = line.substr(0, afterEq) + w.value;
                            w.applied = true;
                            break;
                        }
                    }
                }
            }
        }

        lines.push_back(std::move(line));
        lineSecLower.push_back(currentSecLower);
    }

    // Insert remaining unapplied keys right after the last non-blank entry of matching sections,
    // or append new sections cleanly at the end.
    for (auto& w : work) {
        if (w.applied) continue;
        size_t insertAfter = std::string::npos;
        bool inTarget = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string tr = trimView(lines[i]);
            if (!tr.empty() && tr.front() == '[' && tr.back() == ']') {
                inTarget = (lineSecLower[i] == w.sectionLower);
                if (inTarget) insertAfter = i;
            } else if (inTarget && !tr.empty()) {
                insertAfter = i;
            }
        }

        const std::string newLine = w.key + " = " + w.value;
        if (insertAfter != std::string::npos) {
            lines.insert(lines.begin() + static_cast<ptrdiff_t>(insertAfter + 1), newLine);
            lineSecLower.insert(lineSecLower.begin() + static_cast<ptrdiff_t>(insertAfter + 1), w.sectionLower);
            w.applied = true;
        } else {
            if (!lines.empty() && !trimView(lines.back()).empty()) {
                lines.emplace_back();
                lineSecLower.push_back(w.sectionLower);
            }
            lines.push_back("[" + w.section + "]");
            lineSecLower.push_back(w.sectionLower);
            lines.push_back(newLine);
            lineSecLower.push_back(w.sectionLower);
            w.applied = true;
        }
    }

    const std::string nl = useCrlf ? "\r\n" : "\n";
    std::string out;
    out.reserve(raw.size() + 256);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += nl;
        out += lines[i];
    }
    if (!lines.empty()) out += nl;
    return writeFileBytesW(path, out);
}

std::filesystem::path getAppDataDir(const std::filesystem::path& fallbackDir)
{
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, _countof(buf));
    if (len > 0 && len < _countof(buf)) {
        std::filesystem::path dir = std::filesystem::path(buf, buf + len) / L"GRML";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    return fallbackDir;
}

} // namespace grml
