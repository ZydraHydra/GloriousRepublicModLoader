#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "launcher/http_server.hpp"
#include "launcher/launcher_api.hpp"
#include "common/ini_util.hpp"
#include "common/json_min.hpp"
#include "common/offset_resolver.hpp"
#include "common/pe_scanner.hpp"
#include "embedded_grml_offsets.hpp"
#include "embedded_ui_assets.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <commdlg.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace grml::http_server {

namespace {

SOCKET                g_listenSock = INVALID_SOCKET;
std::thread           g_acceptThread;
std::atomic<bool>     g_running{false};
std::filesystem::path g_loaderDir;
std::mutex            g_apiMutex;

uint32_t g_lastLaunchPid    = 0;
uint64_t g_lastLaunchMs     = 0;

struct CachedModRef {
    std::string id;
    std::string name;
    std::string dllPath;
};
std::vector<CachedModRef> g_cachedMods;

uint64_t nowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string urlDecode(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hexVal(in[i + 1]);
            const int lo = hexVal(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (in[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::unordered_map<std::string, std::string> parseQuery(std::string_view qs)
{
    std::unordered_map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < qs.size()) {
        const size_t amp = qs.find('&', pos);
        const std::string_view part = (amp == std::string_view::npos)
            ? qs.substr(pos)
            : qs.substr(pos, amp - pos);
        const size_t eq = part.find('=');
        if (eq != std::string_view::npos) {
            out[urlDecode(part.substr(0, eq))] = urlDecode(part.substr(eq + 1));
        } else if (!part.empty()) {
            out[urlDecode(part)] = "";
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return out;
}

std::vector<std::string> readLogTail(const std::filesystem::path& dataDir, size_t maxLines)
{
    std::string raw;
    if (!grml::readFileBytesW(dataDir / L"grml.log", raw)) return {};
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        std::string_view line(raw.data() + pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) lines.emplace_back(line);
        pos = eol + 1;
    }
    if (lines.size() > maxLines) {
        lines.erase(lines.begin(), lines.begin() + static_cast<ptrdiff_t>(lines.size() - maxLines));
    }
    return lines;
}

std::string readPlaylistsRawJson(const std::filesystem::path& dataDir)
{
    std::string raw;
    if (!grml::readFileBytesW(dataDir / L"grml_playlists.json", raw) || raw.empty()) {
        return "[]";
    }
    const auto parsed = grml::parseJson(raw);
    if (!parsed || !parsed->isArray()) return "[]";
    return raw;
}

std::string readSessionLastSave(const std::filesystem::path& dataDir)
{
    std::string raw;
    if (!grml::readFileBytesW(dataDir / L"grml_session.json", raw)) return {};
    const auto parsed = grml::parseJson(raw);
    if (!parsed || !parsed->isObject()) return {};
    if (const auto* s = parsed->get("last_loaded_save")) {
        return s->asString();
    }
    return {};
}

std::string findMostRecentSaveFolderFromGameDir(const std::filesystem::path& gameDir,
                                                const std::filesystem::path& dataDir)
{
    if (gameDir.empty()) return {};

    const std::filesystem::path savesRoot = gameDir / L"media_soviet" / L"save";
    std::error_code ec;
    std::filesystem::file_time_type bestTime{};
    std::string bestName;

    if (std::filesystem::exists(savesRoot, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(savesRoot, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::filesystem::path hdr = entry.path() / L"header.bin";
            if (!std::filesystem::exists(hdr, ec)) continue;
            const auto wt = std::filesystem::last_write_time(hdr, ec);
            if (!ec && (bestName.empty() || wt > bestTime)) {
                bestTime = wt;
                bestName = grml::wideToUtf8(entry.path().filename().wstring());
            }
        }
    }

    const std::string sessionSave = readSessionLastSave(dataDir);
    if (!sessionSave.empty() &&
        std::filesystem::exists(savesRoot / grml::utf8ToWide(sessionSave) / L"header.bin", ec)) {
        const auto sessTime = std::filesystem::last_write_time(dataDir / L"grml_session.json", ec);
        if (bestName.empty() || (!ec && sessTime > bestTime)) {
            return sessionSave;
        }
    }
    return bestName;
}

std::filesystem::path g_cachedGameDir;

std::string findMostRecentSaveFolder(const std::string& discoverJson, const std::filesystem::path& dataDir)
{
    const auto parsed = grml::parseJson(discoverJson);
    std::filesystem::path gameExe;
    if (parsed && parsed->isObject()) {
        if (const auto* g = parsed->get("gameExe")) {
            const std::string gs = g->asString();
            if (!gs.empty()) gameExe = std::filesystem::path(grml::utf8ToWide(gs));
        }
    }
    if (gameExe.empty() || !gameExe.has_parent_path()) return {};
    g_cachedGameDir = gameExe.parent_path();
    return findMostRecentSaveFolderFromGameDir(g_cachedGameDir, dataDir);
}

std::string toLowerAscii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::vector<int> parseSemverParts(std::string_view v)
{
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
    std::vector<int> parts;
    size_t i = 0;
    while (i < v.size()) {
        if (v[i] < '0' || v[i] > '9') {
            if (v[i] == '-') break;
            ++i;
            continue;
        }
        int num = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            num = num * 10 + (v[i] - '0');
            ++i;
        }
        parts.push_back(num);
    }
    return parts;
}

bool isNewerSemver(std::string_view candidate, std::string_view current)
{
    const auto a = parseSemverParts(candidate);
    const auto b = parseSemverParts(current);
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int av = (i < a.size()) ? a[i] : 0;
        const int bv = (i < b.size()) ? b[i] : 0;
        if (av != bv) return av > bv;
    }
    return false;
}

std::string checkGitHubReleaseJson()
{
    const std::string curVer = GRML_BUILD_VERSION;
    const std::wstring ua = L"GloriousRepublicModLoader/" + grml::utf8ToWide(curVer);

    HINTERNET hSession = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        return "{\"ok\":false,\"currentVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
               "\",\"error\":\"WinHttpOpen failed\"}";
    }
    WinHttpSetTimeouts(hSession, 4000, 4000, 4000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "{\"ok\":false,\"currentVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
               "\",\"error\":\"Failed to connect to GitHub\"}";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET",
        L"/repos/ZydraHydra/GloriousRepublicModLoader/releases/latest",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "{\"ok\":false,\"currentVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
               "\",\"error\":\"WinHttpOpenRequest failed\"}";
    }

    const wchar_t* hdrs = L"Accept: application/vnd.github+json\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, hdrs, static_cast<DWORD>(-1L),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (sent) sent = WinHttpReceiveResponse(hRequest, nullptr);

    DWORD statusCode = 0;
    DWORD szStatus   = sizeof(statusCode);
    if (sent) {
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &szStatus, WINHTTP_NO_HEADER_INDEX);
    }

    std::string body;
    if (sent && statusCode == 200) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (WinHttpReadData(hRequest, chunk.data(), avail, &read) && read > 0) {
                chunk.resize(read);
                body += chunk;
            } else {
                break;
            }
            if (body.size() > 512 * 1024) break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!sent) {
        return "{\"ok\":false,\"currentVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
               "\",\"error\":\"GitHub request failed\"}";
    }
    if (statusCode == 404) {
        return "{\"ok\":true,\"currentVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
               "\",\"latestVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
               "\",\"updateAvailable\":false,\"noReleases\":true,"
               "\"releaseUrl\":\"https://github.com/ZydraHydra/GloriousRepublicModLoader/releases\"}";
    }
    if (statusCode != 200) {
        return "{\"ok\":false,\"currentVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
               "\",\"error\":\"GitHub HTTP " + std::to_string(statusCode) + "\"}";
    }

    const auto parsed = grml::parseJson(body);
    if (!parsed || !parsed->isObject()) {
        return "{\"ok\":false,\"currentVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
               "\",\"error\":\"Invalid GitHub response\"}";
    }
    std::string tag = parsed->get("tag_name") ? parsed->get("tag_name")->asString() : "";
    std::string latest = tag;
    if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) latest.erase(0, 1);
    const std::string htmlUrl = parsed->get("html_url")
        ? parsed->get("html_url")->asString()
        : "https://github.com/ZydraHydra/GloriousRepublicModLoader/releases";
    const std::string relName = parsed->get("name") ? parsed->get("name")->asString() : tag;
    const bool newer = !latest.empty() && isNewerSemver(latest, curVer);

    return "{\"ok\":true,\"currentVersion\":\"" + grml::launcher_api::jsonEscape(curVer) +
           "\",\"latestVersion\":\"" + grml::launcher_api::jsonEscape(latest.empty() ? curVer : latest) +
           "\",\"tagName\":\"" + grml::launcher_api::jsonEscape(tag) +
           "\",\"releaseName\":\"" + grml::launcher_api::jsonEscape(relName) +
           "\",\"releaseUrl\":\"" + grml::launcher_api::jsonEscape(htmlUrl) +
           "\",\"updateAvailable\":" + std::string(newer ? "true" : "false") + "}";
}

std::string buildLocalesListJson(const std::filesystem::path& loaderDir,
                                 const std::filesystem::path& dataDir)
{
    struct LocaleEntry {
        std::string code;
        std::string name;
        std::string nativeName;
    };
    std::unordered_map<std::string, LocaleEntry> byCode;

    auto ingestJson = [&](std::string_view fallbackCode, std::string_view bytes) {
        const auto root = grml::parseJson(bytes);
        if (!root || !root->isObject()) return;
        std::string code(fallbackCode);
        std::string name(fallbackCode);
        std::string nativeName(fallbackCode);
        if (const auto* meta = root->get("_meta"); meta && meta->isObject()) {
            if (const auto* c = meta->get("code")) {
                const auto s = c->asString();
                if (!s.empty()) code = s;
            }
            if (const auto* n = meta->get("name")) {
                const auto s = n->asString();
                if (!s.empty()) name = s;
            }
            if (const auto* nn = meta->get("nativeName")) {
                const auto s = nn->asString();
                if (!s.empty()) nativeName = s;
            }
        }
        if (code.empty()) return;
        byCode[code] = LocaleEntry{code, name, nativeName};
    };

    for (const auto& a : grml::generated::embeddedUiAssets()) {
        const std::string_view p(a.urlPath);
        if (!p.starts_with("/locales/") || !p.ends_with(".json")) continue;
        const auto rel = p.substr(9); // after "/locales/"
        if (rel.find('/') != std::string_view::npos) continue;
        const std::string fallbackCode(rel.substr(0, rel.size() - 5));
        ingestJson(fallbackCode, std::string_view(reinterpret_cast<const char*>(a.data), a.size));
    }

    auto scanDir = [&](const std::filesystem::path& dir) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return;
        for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
            if (ec || !ent.is_regular_file(ec)) continue;
            if (ent.path().extension() != L".json") continue;
            std::string bytes;
            if (!grml::readFileBytesW(ent.path(), bytes)) continue;
            const std::string fallbackCode = grml::wideToUtf8(ent.path().stem().wstring());
            ingestJson(fallbackCode, bytes);
        }
    };

    scanDir(loaderDir / L"locales");
    scanDir(dataDir / L"locales");

    std::vector<LocaleEntry> list;
    list.reserve(byCode.size());
    for (auto& [k, v] : byCode) list.push_back(std::move(v));
    std::sort(list.begin(), list.end(), [](const LocaleEntry& a, const LocaleEntry& b) {
        if (a.code == "en" && b.code != "en") return true;
        if (b.code == "en" && a.code != "en") return false;
        return a.code < b.code;
    });

    std::string out = "{\"locales\":[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) out += ',';
        out += "{\"code\":\"" + grml::launcher_api::jsonEscape(list[i].code) + "\"";
        out += ",\"name\":\"" + grml::launcher_api::jsonEscape(list[i].name) + "\"";
        out += ",\"nativeName\":\"" + grml::launcher_api::jsonEscape(list[i].nativeName) + "\"}";
    }
    out += "]}";
    return out;
}

std::string updateOffsetsOtaJson(const std::filesystem::path& loaderDir,
                                 const std::filesystem::path& dataDir)
{
    grml::IniFile cfg;
    cfg.loadFromFile(dataDir / L"grml.ini");
    const std::string rawUrl = cfg.getString(
        "general",
        "offsets_ota_url",
        "https://raw.githubusercontent.com/ZydraHydra/GloriousRepublicModLoader/main/config/grml_offsets.ini");

    const std::wstring wideUrl = grml::utf8ToWide(rawUrl);
    URL_COMPONENTS uc{};
    uc.dwStructSize     = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength  = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &uc) ||
        uc.dwHostNameLength == 0 || uc.dwUrlPathLength == 0) {
        return "{\"ok\":false,\"error\":\"Invalid OTA URL\"}";
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    const std::wstring urlPath(uc.lpszUrlPath, uc.dwUrlPathLength);
    const bool isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    const std::wstring ua = L"GloriousRepublicModLoader/" + grml::utf8ToWide(GRML_BUILD_VERSION);
    HINTERNET hSession = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "{\"ok\":false,\"error\":\"WinHttpOpen failed\"}";
    WinHttpSetTimeouts(hSession, 4000, 4000, 4000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "{\"ok\":false,\"error\":\"Failed to connect to GitHub raw host\"}";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", urlPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "{\"ok\":false,\"error\":\"WinHttpOpenRequest failed\"}";
    }

    BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (sent) sent = WinHttpReceiveResponse(hRequest, nullptr);

    DWORD statusCode = 0;
    DWORD szStatus   = sizeof(statusCode);
    if (sent) {
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &szStatus, WINHTTP_NO_HEADER_INDEX);
    }

    std::string body;
    if (sent && statusCode == 200) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (WinHttpReadData(hRequest, chunk.data(), avail, &read) && read > 0) {
                chunk.resize(read);
                body += chunk;
            } else {
                break;
            }
            if (body.size() > 1024 * 1024) break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!sent) return "{\"ok\":false,\"error\":\"GitHub OTA request failed\"}";
    if (statusCode != 200) {
        return "{\"ok\":false,\"error\":\"GitHub OTA HTTP " + std::to_string(statusCode) + "\"}";
    }

    // Strip UTF-8 BOM if present
    if (body.size() >= 3 &&
        static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }

    grml::IniFile parsed;
    if (!parsed.loadFromMemory(body) || parsed.sectionOrder().empty()) {
        return "{\"ok\":false,\"error\":\"Downloaded grml_offsets.ini is empty or invalid\"}";
    }
    const auto defs = grml::offsets::collectOffsetDefinitions(parsed);
    if (defs.empty() && !parsed.sectionKeys("signatures") && !parsed.sectionKeys("builds")) {
        return "{\"ok\":false,\"error\":\"Downloaded file is not a valid grml_offsets.ini\"}";
    }

    grml::IniFile prevIni;
    uint64_t prevHash = 0;
    grml::offsets::loadMergedOffsetsIni(loaderDir, dataDir, grml::generated::kEmbeddedGrmlOffsetsIni,
                                        prevIni, prevHash);

    std::filesystem::path targetPath = loaderDir / L"grml_offsets.ini";
    if (!grml::writeFileBytesW(targetPath, body)) {
        targetPath = dataDir / L"grml_offsets.ini";
        if (!grml::writeFileBytesW(targetPath, body)) {
            return "{\"ok\":false,\"error\":\"Failed to write grml_offsets.ini\"}";
        }
    }

    // Invalidate PE-hash RVA cache so next launch or /api/state re-verifies cleanly
    DeleteFileW((dataDir / L"grml_offset_cache.ini").c_str());

    grml::IniFile newIni;
    uint64_t newHash = 0;
    grml::offsets::loadMergedOffsetsIni(loaderDir, dataDir, grml::generated::kEmbeddedGrmlOffsetsIni,
                                        newIni, newHash);
    const bool updated = (newHash != prevHash);

    char prevBuf[32]{}, newBuf[32]{};
    std::snprintf(prevBuf, sizeof(prevBuf), "0x%016llX", static_cast<unsigned long long>(prevHash));
    std::snprintf(newBuf, sizeof(newBuf), "0x%016llX", static_cast<unsigned long long>(newHash));

    return "{\"ok\":true,\"updated\":" + std::string(updated ? "true" : "false") +
           ",\"previousHash\":\"" + std::string(prevBuf) + "\"" +
           ",\"hash\":\"" + std::string(newBuf) + "\"" +
           ",\"entryCount\":" + std::to_string(defs.size()) +
           ",\"path\":\"" + grml::launcher_api::jsonEscape(grml::wideToUtf8(targetPath.wstring())) + "\"}";
}

bool winHttpPostForm(const wchar_t* host,
                     const wchar_t* path,
                     const std::string& formBody,
                     DWORD& outStatus,
                     std::string& outBody,
                     std::string& outError)
{
    outStatus = 0;
    outBody.clear();
    outError.clear();

    const std::wstring ua = L"GloriousRepublicModLoader/" + grml::utf8ToWide(GRML_BUILD_VERSION);
    HINTERNET hSession = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        outError = "WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        outError = "Failed to connect to Steam Web API";
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        outError = "WinHttpOpenRequest failed";
        return false;
    }

    const wchar_t* hdrs = L"Content-Type: application/x-www-form-urlencoded\r\n";
    BOOL sent = WinHttpSendRequest(
        hRequest, hdrs, static_cast<DWORD>(-1L),
        const_cast<char*>(formBody.data()),
        static_cast<DWORD>(formBody.size()),
        static_cast<DWORD>(formBody.size()),
        0);
    if (sent) sent = WinHttpReceiveResponse(hRequest, nullptr);

    if (sent) {
        DWORD szStatus = sizeof(outStatus);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &outStatus, &szStatus, WINHTTP_NO_HEADER_INDEX);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (WinHttpReadData(hRequest, chunk.data(), avail, &read) && read > 0) {
                chunk.resize(read);
                outBody += chunk;
            } else {
                break;
            }
            if (outBody.size() > 2 * 1024 * 1024) break;
        }
    } else {
        outError = "Steam Web API request failed";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return sent == TRUE;
}

std::string extractSteamCollectionId(std::string_view input)
{
    auto trimWs = [](std::string_view sv) {
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n'))
            sv.remove_prefix(1);
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n'))
            sv.remove_suffix(1);
        return sv;
    };
    input = trimWs(input);
    if (input.empty()) return {};

    // Check for ?id=123456 or &id=123456 in Steam Workshop URLs
    const size_t idPos = input.find("id=");
    if (idPos != std::string_view::npos) {
        std::string digits;
        for (size_t i = idPos + 3; i < input.size(); ++i) {
            if (input[i] >= '0' && input[i] <= '9') digits.push_back(input[i]);
            else break;
        }
        return digits;
    }

    // Otherwise require pure digits
    std::string digits;
    for (char c : input) {
        if (c < '0' || c > '9') return {};
        digits.push_back(c);
    }
    return digits;
}

std::string fetchSteamCollectionJson(const std::string& rawInput, int& outHttpStatus)
{
    outHttpStatus = 200;
    const std::string collectionId = extractSteamCollectionId(rawInput);
    if (collectionId.empty()) {
        outHttpStatus = 400;
        return "{\"ok\":false,\"error\":\"Invalid Steam Workshop collection URL or ID.\"}";
    }

    auto fetchCollectionChildren = [&](const std::string& colId,
                                       std::vector<std::string>& outItemIds,
                                       std::vector<std::string>& outSubCollectionIds,
                                       std::string& err) -> bool {
        DWORD status = 0;
        std::string respBody;
        const std::string form = "collectioncount=1&publishedfileids%5B0%5D=" + colId;
        if (!winHttpPostForm(L"api.steampowered.com",
                             L"/ISteamRemoteStorage/GetCollectionDetails/v1/",
                             form, status, respBody, err)) {
            return false;
        }
        if (status != 200) {
            err = "Steam API HTTP " + std::to_string(status);
            return false;
        }
        const auto parsed = grml::parseJson(respBody);
        const auto* responseObj = parsed ? parsed->get("response") : nullptr;
        const auto* detailsArr  = responseObj ? responseObj->get("collectiondetails") : nullptr;
        if (!detailsArr || !detailsArr->isArray() ||
            std::get<grml::JsonArray>(detailsArr->data).empty()) {
            err = "Unexpected Steam collection response.";
            return false;
        }
        const auto& entry = std::get<grml::JsonArray>(detailsArr->data)[0];
        const int resCode = entry.get("result") ? entry.get("result")->asInt(0) : 0;
        if (resCode != 1) {
            err = "Steam collection not found or is private (result " + std::to_string(resCode) + ").";
            return false;
        }
        const auto* children = entry.get("children");
        if (children && children->isArray()) {
            for (const auto& ch : std::get<grml::JsonArray>(children->data)) {
                if (!ch.isObject()) continue;
                const std::string childId = ch.get("publishedfileid") ? ch.get("publishedfileid")->asString() : "";
                const int fileType        = ch.get("filetype") ? ch.get("filetype")->asInt(0) : 0;
                if (childId.empty()) continue;
                if (fileType == 2) {
                    outSubCollectionIds.push_back(childId);
                } else {
                    outItemIds.push_back(childId);
                }
            }
        }
        return true;
    };

    std::vector<std::string> rawItemIds;
    std::vector<std::string> subCollectionIds;
    std::string err;
    if (!fetchCollectionChildren(collectionId, rawItemIds, subCollectionIds, err)) {
        outHttpStatus = 502;
        return "{\"ok\":false,\"error\":\"" + grml::launcher_api::jsonEscape(err) + "\"}";
    }

    // Resolve 1 level of nested sub-collections if present
    for (const auto& subId : subCollectionIds) {
        std::vector<std::string> ignoredNested;
        std::string subErr;
        fetchCollectionChildren(subId, rawItemIds, ignoredNested, subErr);
    }

    // Deduplicate while preserving order
    std::vector<std::string> workshopIds;
    std::unordered_set<std::string> seen;
    for (const auto& id : rawItemIds) {
        if (seen.insert(id).second) {
            workshopIds.push_back(id);
        }
    }

    // Fetch collection title via GetPublishedFileDetails
    std::string title;
    {
        DWORD status = 0;
        std::string respBody;
        std::string metaErr;
        const std::string form = "itemcount=1&publishedfileids%5B0%5D=" + collectionId;
        if (winHttpPostForm(L"api.steampowered.com",
                             L"/ISteamRemoteStorage/GetPublishedFileDetails/v1/",
                             form, status, respBody, metaErr) && status == 200) {
            const auto parsed = grml::parseJson(respBody);
            const auto* responseObj = parsed ? parsed->get("response") : nullptr;
            const auto* detailsArr  = responseObj ? responseObj->get("publishedfiledetails") : nullptr;
            if (detailsArr && detailsArr->isArray() &&
                !std::get<grml::JsonArray>(detailsArr->data).empty()) {
                const auto& item0 = std::get<grml::JsonArray>(detailsArr->data)[0];
                if (const auto* t = item0.get("title")) {
                    title = t->asString();
                }
            }
        }
    }
    if (title.empty()) {
        title = "Steam Collection " + collectionId;
    }

    const std::string colUrl = "https://steamcommunity.com/sharedfiles/filedetails/?id=" + collectionId;
    std::string out = "{\"ok\":true,\"collectionId\":\"" + grml::launcher_api::jsonEscape(collectionId) +
                      "\",\"collectionUrl\":\"" + grml::launcher_api::jsonEscape(colUrl) +
                      "\",\"title\":\"" + grml::launcher_api::jsonEscape(title) +
                      "\",\"workshopIds\":[";
    for (size_t i = 0; i < workshopIds.size(); ++i) {
        if (i > 0) out += ',';
        out += "\"" + grml::launcher_api::jsonEscape(workshopIds[i]) + "\"";
    }
    out += "]}";
    return out;
}

std::string buildSuspectsJson(const std::vector<std::string>& logTail,
                              const std::string& werJson,
                              const std::vector<CachedModRef>& mods)
{
    struct Suspect {
        std::string severity;
        std::string modId;
        std::string modName;
        std::string reason;
        std::string source;
        std::string faultingModule;
    };
    std::vector<Suspect> out;
    std::vector<std::string> seen;

    auto add = [&](Suspect s) {
        const std::string k = s.severity + "|" + s.modName + "|" + s.reason;
        for (const auto& x : seen) if (x == k) return;
        seen.push_back(k);
        out.push_back(std::move(s));
    };

    const std::regex cgRe(R"(\[CrashGuard\]\s+FATAL\s+(0x[0-9A-Fa-f]+)\s+in\s+(\S+)\s+\([^)]+\)\s+->\s+Suspect mod:\s+(\S+)\s+\(([^)]+)\)\s+\[([^\]]+)\])");
    const std::regex errRe(R"(^\[[^\]]+\]\s+\[(ERROR|FATAL)\]\s+\[([^\]]+)\]\s+(.*)$)");

    for (const auto& line : logTail) {
        std::smatch m;
        if (std::regex_search(line, m, cgRe)) {
            add({"crash", m[4].str(), m[3].str(),
                 "Unhandled exception " + m[1].str() + " in " + m[2].str(),
                 "CrashGuard", m[5].str()});
            continue;
        }
        if (std::regex_match(line, m, errRe)) {
            const std::string sev = (m[1].str() == "FATAL") ? "crash" : "error";
            const std::string tag = m[2].str();
            const std::string msg = m[3].str();
            const std::string msgLo = toLowerAscii(msg);
            std::string matchedId;
            std::string matchedName = tag;
            for (const auto& mod : mods) {
                const std::string modNameLo = toLowerAscii(mod.name);
                const std::string modIdLo   = toLowerAscii(mod.id);
                std::string dllBase = std::filesystem::path(grml::utf8ToWide(mod.dllPath)).filename().string();
                if ((!modNameLo.empty() && msgLo.find(modNameLo) != std::string::npos) ||
                    (!modIdLo.empty() && msgLo.find(modIdLo) != std::string::npos) ||
                    (!dllBase.empty() && msgLo.find(toLowerAscii(dllBase)) != std::string::npos)) {
                    matchedId   = mod.id;
                    matchedName = mod.name;
                    break;
                }
            }
            add({sev, matchedId, matchedName, msg, "grml.log", ""});
        }
    }

    const auto wer = grml::parseJson(werJson);
    if (wer && wer->isObject() && wer->get("found") && wer->get("found")->asBool(false)) {
        const std::string modName = wer->get("faultingModuleName") ? wer->get("faultingModuleName")->asString() : "";
        const std::string modPath = wer->get("faultingModulePath") ? wer->get("faultingModulePath")->asString() : "";
        const std::string excCode = wer->get("exceptionCode") ? wer->get("exceptionCode")->asString() : "";
        const std::string offset  = wer->get("faultOffset") ? wer->get("faultOffset")->asString() : "";
        const std::string modNameLo = toLowerAscii(modName);
        const std::string modPathLo = toLowerAscii(modPath);
        std::string matchedId;
        std::string dispName = modName;
        for (const auto& mod : mods) {
            const std::string dllBase = toLowerAscii(std::filesystem::path(grml::utf8ToWide(mod.dllPath)).filename().string());
            if ((!dllBase.empty() && dllBase == modNameLo) ||
                (!modPathLo.empty() && toLowerAscii(mod.dllPath) == modPathLo)) {
                matchedId = mod.id;
                dispName  = mod.name;
                break;
            }
        }
        add({"crash", matchedId, dispName,
             "Windows WER: " + excCode + " at " + modName + " +" + offset,
             "Windows Event Log", modPath.empty() ? modName : modPath});
    }

    std::string j = "[";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) j += ',';
        j += "{";
        j += "\"severity\":\"" + grml::launcher_api::jsonEscape(out[i].severity) + "\",";
        if (out[i].modId.empty()) j += "\"modId\":null,";
        else j += "\"modId\":\"" + grml::launcher_api::jsonEscape(out[i].modId) + "\",";
        j += "\"modName\":\"" + grml::launcher_api::jsonEscape(out[i].modName) + "\",";
        j += "\"reason\":\"" + grml::launcher_api::jsonEscape(out[i].reason) + "\",";
        j += "\"source\":\"" + grml::launcher_api::jsonEscape(out[i].source) + "\",";
        j += "\"faultingModule\":\"" + grml::launcher_api::jsonEscape(out[i].faultingModule) + "\"";
        j += "}";
    }
    j += "]";
    return j;
}

void updateCachedModsFromDiscoverJson(const std::string& discoverJson)
{
    const auto root = grml::parseJson(discoverJson);
    if (!root || !root->isObject()) return;
    const auto* modsArr = root->get("mods");
    if (!modsArr || !modsArr->isArray()) return;
    g_cachedMods.clear();
    for (const auto& item : std::get<grml::JsonArray>(modsArr->data)) {
        if (!item.isObject()) continue;
        CachedModRef m;
        m.id      = item.get("id") ? item.get("id")->asString() : "";
        m.name    = item.get("name") ? item.get("name")->asString() : "";
        m.dllPath = item.get("dllPath") ? item.get("dllPath")->asString() : "";
        if (!m.id.empty()) g_cachedMods.push_back(std::move(m));
    }
}

void sendHttpResponse(SOCKET sock, int statusCode, const char* statusText,
                      std::string_view contentType, std::string_view body)
{
    char hdr[512]{};
    const int hdrLen = std::snprintf(
        hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %.*s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        statusCode, statusText,
        static_cast<int>(contentType.size()), contentType.data(),
        body.size());
    send(sock, hdr, hdrLen, 0);
    size_t sent = 0;
    while (sent < body.size()) {
        const int chunk = static_cast<int>(std::min<size_t>(body.size() - sent, 65536));
        const int n = send(sock, body.data() + sent, chunk, 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

void handleClient(SOCKET client)
{
    std::string reqBuf;
    reqBuf.reserve(4096);
    char tmp[4096];
    size_t headerEnd = std::string::npos;
    for (;;) {
        const int n = recv(client, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        reqBuf.append(tmp, static_cast<size_t>(n));
        headerEnd = reqBuf.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (reqBuf.size() > 1024 * 1024) break;
    }
    if (headerEnd == std::string::npos) {
        closesocket(client);
        return;
    }

    const std::string_view hdrView(reqBuf.data(), headerEnd);
    const size_t firstEol = hdrView.find("\r\n");
    const std::string_view reqLine = (firstEol == std::string_view::npos) ? hdrView : hdrView.substr(0, firstEol);

    const size_t sp1 = reqLine.find(' ');
    const size_t sp2 = (sp1 != std::string_view::npos) ? reqLine.find(' ', sp1 + 1) : std::string_view::npos;
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
        sendHttpResponse(client, 400, "Bad Request", "text/plain", "Bad Request");
        closesocket(client);
        return;
    }

    const std::string method(reqLine.substr(0, sp1));
    const std::string_view target = reqLine.substr(sp1 + 1, sp2 - (sp1 + 1));
    const size_t qPos = target.find('?');
    const std::string path(qPos == std::string_view::npos ? target : target.substr(0, qPos));
    const auto query = parseQuery(qPos == std::string_view::npos ? std::string_view{} : target.substr(qPos + 1));

    size_t contentLength = 0;
    {
        const std::string hdrLo = toLowerAscii(std::string(hdrView));
        const size_t clPos = hdrLo.find("content-length:");
        if (clPos != std::string::npos) {
            contentLength = static_cast<size_t>(std::strtoull(hdrLo.c_str() + clPos + 15, nullptr, 10));
        }
    }
    std::string body = reqBuf.substr(headerEnd + 4);
    while (body.size() < contentLength && body.size() < 4 * 1024 * 1024) {
        const int n = recv(client, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        body.append(tmp, static_cast<size_t>(n));
    }
    if (body.size() > contentLength && contentLength > 0) {
        body.resize(contentLength);
    }

    const std::filesystem::path dataDir = grml::getAppDataDir(g_loaderDir);

    if (method == "GET" && path == "/api/state") {
        std::lock_guard lock(g_apiMutex);
        const auto it = query.find("game");
        const std::filesystem::path gameOverride = (it != query.end())
            ? std::filesystem::path(grml::utf8ToWide(it->second))
            : std::filesystem::path{};
        const std::string discoverJson = grml::launcher_api::buildDiscoverJson(g_loaderDir, gameOverride);
        updateCachedModsFromDiscoverJson(discoverJson);

        const auto logs = readLogTail(dataDir, 250);
        std::string logJson = "[";
        for (size_t i = 0; i < logs.size(); ++i) {
            if (i > 0) logJson += ',';
            logJson += "\"" + grml::launcher_api::jsonEscape(logs[i]) + "\"";
        }
        logJson += "]";

        // Parse grmlIni and vfs slices from discoverJson via json_min or extract top-level keys
        // Since discoverJson already contains "grmlIni" and "vfs", embed discovery and let JS/extract read them,
        // OR extract raw slices cleanly:
        const std::string playlistsJson = readPlaylistsRawJson(dataDir);
        const std::string latestSave = findMostRecentSaveFolder(discoverJson, dataDir);
        const std::string resp =
            "{\"discovery\":" + discoverJson +
            ",\"logTail\":" + logJson +
            ",\"playlists\":" + playlistsJson +
            ",\"latestSave\":\"" + grml::launcher_api::jsonEscape(latestSave) + "\"}";
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", resp);
    } else if (method == "GET" && path == "/api/latest-save") {
        std::lock_guard lock(g_apiMutex);
        std::filesystem::path gameDir = g_cachedGameDir;
        const auto it = query.find("game");
        if (it != query.end() && !it->second.empty()) {
            const std::filesystem::path gameOverride(grml::utf8ToWide(it->second));
            if (gameOverride.has_parent_path()) gameDir = gameOverride.parent_path();
        }
        const std::string latestSave = findMostRecentSaveFolderFromGameDir(gameDir, dataDir);
        const std::string resp =
            "{\"latestSave\":\"" + grml::launcher_api::jsonEscape(latestSave) + "\"}";
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", resp);
    } else if (method == "GET" && path == "/api/locales") {
        std::lock_guard lock(g_apiMutex);
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8",
                         buildLocalesListJson(g_loaderDir, dataDir));
    } else if (method == "GET" && path == "/api/check-update") {
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", checkGitHubReleaseJson());
    } else if (method == "POST" && path == "/api/update-offsets") {
        std::lock_guard lock(g_apiMutex);
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8",
                         updateOffsetsOtaJson(g_loaderDir, dataDir));
    } else if (method == "GET" && path == "/api/saves") {
        std::lock_guard lock(g_apiMutex);
        const auto it = query.find("game");
        const std::filesystem::path gameOverride = (it != query.end())
            ? std::filesystem::path(grml::utf8ToWide(it->second))
            : std::filesystem::path{};
        const std::string savesJson = grml::launcher_api::buildInspectSavesJson(g_loaderDir, gameOverride);
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", savesJson);
    } else if (method == "GET" && path == "/api/diagnostics") {
        std::lock_guard lock(g_apiMutex);
        const bool checkEvents  = query.count("checkEvents") && query.at("checkEvents") == "1";
        const bool checkProcess = checkEvents || (query.count("checkProcess") && query.at("checkProcess") == "1");
        const auto logs = readLogTail(dataDir, 250);
        std::vector<uint32_t> pids;
        if (checkProcess) pids = grml::launcher_api::getRunningSovietPids();
        const bool running = !pids.empty();
        bool lastPidAlive = false;
        for (uint32_t p : pids) if (p == g_lastLaunchPid) lastPidAlive = true;
        const bool exitedAfterLaunch = checkProcess && g_lastLaunchPid != 0 && !lastPidAlive &&
                                       ((nowMs() - g_lastLaunchMs) < 120000);
        bool hasCrashGuard = false;
        for (const auto& l : logs) if (l.find("[CrashGuard]") != std::string::npos) { hasCrashGuard = true; break; }
        std::string werJson = "{\"found\":false}";
        if (checkEvents || (exitedAfterLaunch && !hasCrashGuard)) {
            werJson = grml::launcher_api::buildCrashEventsJson();
        }
        const bool crashDetected = hasCrashGuard || exitedAfterLaunch || (werJson.find("\"found\":true") != std::string::npos);
        const std::string lastSave = crashDetected ? readSessionLastSave(dataDir) : std::string{};
        const std::string suspects = buildSuspectsJson(logs, werJson, g_cachedMods);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        char timeBuf[32]{};
        std::snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);

        std::string pidsJson = "[";
        for (size_t i = 0; i < pids.size(); ++i) {
            if (i > 0) pidsJson += ',';
            pidsJson += std::to_string(pids[i]);
        }
        pidsJson += "]";

        std::string logJson = "[";
        for (size_t i = 0; i < logs.size(); ++i) {
            if (i > 0) logJson += ',';
            logJson += "\"" + grml::launcher_api::jsonEscape(logs[i]) + "\"";
        }
        logJson += "]";

        std::string resp = "{";
        resp += "\"running\":" + std::string(running ? "true" : "false") + ",";
        resp += "\"pids\":" + pidsJson + ",";
        resp += "\"lastLaunchPid\":" + (g_lastLaunchPid ? std::to_string(g_lastLaunchPid) : "null") + ",";
        resp += "\"exitedUnexpectedly\":" + std::string(exitedAfterLaunch ? "true" : "false") + ",";
        resp += "\"crashDetected\":" + std::string(crashDetected ? "true" : "false") + ",";
        resp += "\"lastLoadedSave\":\"" + grml::launcher_api::jsonEscape(lastSave) + "\",";
        resp += "\"logTail\":" + logJson + ",";
        resp += "\"suspects\":" + suspects + ",";
        resp += "\"werCrash\":" + (werJson.find("\"found\":true") != std::string::npos ? werJson : "null") + ",";
        resp += "\"checkedAt\":\"" + std::string(timeBuf) + "\"";
        resp += "}";
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", resp);
    } else if (method == "POST" && path == "/api/config") {
        std::lock_guard lock(g_apiMutex);
        std::string outJson;
        const bool ok = grml::launcher_api::saveConfigFromJson(g_loaderDir, body, outJson);
        sendHttpResponse(client, ok ? 200 : 500, ok ? "OK" : "Error", "application/json; charset=utf-8", outJson);
    } else if (method == "POST" && path == "/api/vfs") {
        std::lock_guard lock(g_apiMutex);
        std::string outJson;
        const bool ok = grml::launcher_api::saveVfsFromJson(g_loaderDir, body, outJson);
        sendHttpResponse(client, ok ? 200 : 500, ok ? "OK" : "Error", "application/json; charset=utf-8", outJson);
    } else if (method == "GET" && path == "/api/mod-ini") {
        std::lock_guard lock(g_apiMutex);
        const auto it = query.find("iniPath");
        const std::filesystem::path iniPath = (it != query.end())
            ? std::filesystem::path(grml::utf8ToWide(it->second))
            : std::filesystem::path{};
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", grml::launcher_api::readIniJson(iniPath));
    } else if (method == "POST" && path == "/api/mod-ini") {
        std::lock_guard lock(g_apiMutex);
        std::string outJson;
        const bool ok = grml::launcher_api::saveIniFromJson(body, outJson);
        sendHttpResponse(client, ok ? 200 : 500, ok ? "OK" : "Error", "application/json; charset=utf-8", outJson);
    } else if (method == "POST" && path == "/api/generate-schema") {
        std::lock_guard lock(g_apiMutex);
        std::string outJson;
        const bool ok = grml::launcher_api::generateSchemaFromJson(body, outJson);
        sendHttpResponse(client, ok ? 200 : 500, ok ? "OK" : "Error", "application/json; charset=utf-8", outJson);
    } else if (method == "GET" && path == "/api/mod-image") {
        const auto it = query.find("path");
        const std::string imgPath = (it != query.end()) ? it->second : "";
        const std::string lo = toLowerAscii(imgPath);
        const char* mime = nullptr;
        if (lo.ends_with(".png")) mime = "image/png";
        else if (lo.ends_with(".webp")) mime = "image/webp";
        else if (lo.ends_with(".jpg") || lo.ends_with(".jpeg")) mime = "image/jpeg";
        std::string bytes;
        if (!mime || !grml::readFileBytesW(grml::utf8ToWide(imgPath), bytes)) {
            sendHttpResponse(client, 404, "Not Found", "text/plain", "Not Found");
        } else {
            sendHttpResponse(client, 200, "OK", mime, bytes);
        }
    } else if (method == "GET" && path == "/api/browse-game") {
        const auto it = query.find("current");
        std::wstring initialDir;
        if (it != query.end() && !it->second.empty()) {
            const std::filesystem::path p = grml::utf8ToWide(it->second);
            if (std::filesystem::exists(p.parent_path())) {
                initialDir = p.parent_path().wstring();
            }
        }
        wchar_t fileBuf[MAX_PATH * 4]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize     = sizeof(ofn);
        ofn.lpstrFilter     = L"SOVIET64.exe\0SOVIET64.exe\0Executables (*.exe)\0*.exe\0All Files\0*.*\0";
        ofn.lpstrFile       = fileBuf;
        ofn.nMaxFile        = _countof(fileBuf);
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.lpstrTitle      = L"Locate SOVIET64.exe";
        ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            const std::string chosen = grml::wideToUtf8(fileBuf);
            sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8",
                             "{\"ok\":true,\"path\":\"" + grml::launcher_api::jsonEscape(chosen) + "\"}");
        } else {
            sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", "{\"ok\":false,\"cancelled\":true}");
        }
    } else if (method == "POST" && path == "/api/open-url") {
        const auto root = grml::parseJson(body);
        const std::string openUrl = (root && root->get("url")) ? root->get("url")->asString() : "";
        if (openUrl.rfind("https://steamcommunity.com/", 0) != 0 &&
            openUrl.rfind("steam://url/CommunityFilePage/", 0) != 0 &&
            openUrl.rfind("https://github.com/ZydraHydra/GloriousRepublicModLoader", 0) != 0) {
            sendHttpResponse(client, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"Unsupported URL\"}");
        } else {
            auto isSteamRunning = []() -> bool {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap == INVALID_HANDLE_VALUE) return false;
                PROCESSENTRY32W pe{};
                pe.dwSize = sizeof(pe);
                bool found = false;
                if (Process32FirstW(snap, &pe)) {
                    do {
                        if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
                            found = true;
                            break;
                        }
                    } while (Process32NextW(snap, &pe));
                }
                CloseHandle(snap);
                return found;
            };

            std::string targetUrl = openUrl;
            bool openedInSteam = (openUrl.rfind("steam://", 0) == 0);
            if (openUrl.rfind("https://steamcommunity.com/", 0) == 0 &&
                openUrl.find("filedetails/") != std::string::npos) {
                const std::string wsId = extractSteamCollectionId(openUrl);
                if (!wsId.empty() && isSteamRunning()) {
                    targetUrl = "steam://url/CommunityFilePage/" + wsId;
                    openedInSteam = true;
                }
            }
            ShellExecuteW(nullptr, L"open", grml::utf8ToWide(targetUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            sendHttpResponse(client, 200, "OK", "application/json",
                             std::string("{\"ok\":true,\"openedInSteam\":") + (openedInSteam ? "true" : "false") + "}");
        }
    } else if (method == "POST" && path == "/api/playlists") {
        std::lock_guard lock(g_apiMutex);
        const auto req = grml::parseJson(body);
        const std::string action = (req && req->get("action")) ? req->get("action")->asString() : "";
        const std::string name   = (req && req->get("name")) ? req->get("name")->asString() : "";
        const auto deleteNames   = (req && req->get("names")) ? req->get("names")->asStringArray() : std::vector<std::string>{};
        std::unordered_set<std::string> deleteSet(deleteNames.begin(), deleteNames.end());

        struct PlaylistRecord {
            std::string name;
            std::vector<std::string> enabledIds;
            std::vector<std::string> workshopIds;
            std::string source;
            std::string collectionId;
            std::string collectionUrl;
        };

        const auto existing = grml::parseJson(readPlaylistsRawJson(dataDir));
        std::vector<PlaylistRecord> items;
        if (existing && existing->isArray()) {
            for (const auto& el : std::get<grml::JsonArray>(existing->data)) {
                if (!el.isObject()) continue;
                const std::string n = el.get("name") ? el.get("name")->asString() : "";
                if (n.empty() || n == name || deleteSet.count(n)) continue;
                PlaylistRecord rec;
                rec.name          = n;
                rec.enabledIds    = el.get("enabledIds") ? el.get("enabledIds")->asStringArray() : std::vector<std::string>{};
                rec.workshopIds   = el.get("workshopIds") ? el.get("workshopIds")->asStringArray() : std::vector<std::string>{};
                rec.source        = el.get("source") ? el.get("source")->asString() : "";
                rec.collectionId  = el.get("collectionId") ? el.get("collectionId")->asString() : "";
                rec.collectionUrl = el.get("collectionUrl") ? el.get("collectionUrl")->asString() : "";
                items.push_back(std::move(rec));
            }
        }
        if (action != "delete" && !name.empty()) {
            PlaylistRecord rec;
            rec.name          = name;
            rec.enabledIds    = (req && req->get("enabledIds")) ? req->get("enabledIds")->asStringArray() : std::vector<std::string>{};
            rec.workshopIds   = (req && req->get("workshopIds")) ? req->get("workshopIds")->asStringArray() : std::vector<std::string>{};
            rec.source        = (req && req->get("source")) ? req->get("source")->asString() : "";
            rec.collectionId  = (req && req->get("collectionId")) ? req->get("collectionId")->asString() : "";
            rec.collectionUrl = (req && req->get("collectionUrl")) ? req->get("collectionUrl")->asString() : "";
            items.push_back(std::move(rec));
        }

        auto appendJsonStrArr = [](std::string& dst, const std::vector<std::string>& arr) {
            dst += '[';
            for (size_t j = 0; j < arr.size(); ++j) {
                if (j > 0) dst += ',';
                dst += "\"" + grml::launcher_api::jsonEscape(arr[j]) + "\"";
            }
            dst += ']';
        };

        std::string out = "[\n";
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) out += ",\n";
            const auto& r = items[i];
            out += "  {\"name\":\"" + grml::launcher_api::jsonEscape(r.name) + "\",\"enabledIds\":";
            appendJsonStrArr(out, r.enabledIds);
            if (!r.workshopIds.empty()) {
                out += ",\"workshopIds\":";
                appendJsonStrArr(out, r.workshopIds);
            }
            if (!r.source.empty()) {
                out += ",\"source\":\"" + grml::launcher_api::jsonEscape(r.source) + "\"";
            }
            if (!r.collectionId.empty()) {
                out += ",\"collectionId\":\"" + grml::launcher_api::jsonEscape(r.collectionId) + "\"";
            }
            if (!r.collectionUrl.empty()) {
                out += ",\"collectionUrl\":\"" + grml::launcher_api::jsonEscape(r.collectionUrl) + "\"";
            }
            out += "}";
        }
        out += "\n]\n";
        grml::writeFileBytesW(dataDir / L"grml_playlists.json", out);
        sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true,\"playlists\":" + out + "}");
    } else if (method == "POST" && path == "/api/playlists/import-steam") {
        const auto req = grml::parseJson(body);
        const std::string colInput = (req && req->get("collection")) ? req->get("collection")->asString() : "";
        int status = 200;
        const std::string resJson = fetchSteamCollectionJson(colInput, status);
        sendHttpResponse(client, status, status == 200 ? "OK" : "Error",
                         "application/json; charset=utf-8", resJson);
    } else if (method == "POST" && path == "/api/launch") {
        std::lock_guard lock(g_apiMutex);
        const auto req = grml::parseJson(body);
        const std::string autoLoadSave = (req && req->get("autoLoadSave"))
            ? req->get("autoLoadSave")->asString()
            : std::string{};
        const std::filesystem::path sessionPath = dataDir / L"grml_session.json";
        if (!autoLoadSave.empty()) {
            const std::string sessionJson =
                "{\n  \"last_loaded_save\": \"\",\n  \"auto_load_save\": \"" +
                grml::launcher_api::jsonEscape(autoLoadSave) + "\"\n}\n";
            grml::writeFileBytesW(sessionPath, sessionJson);
        } else {
            DeleteFileW(sessionPath.c_str());
        }
        const std::filesystem::path gameExe = (req && req->get("gameExe"))
            ? std::filesystem::path(grml::utf8ToWide(req->get("gameExe")->asString()))
            : std::filesystem::path{};
        const auto tempDisabledIds = (req && req->get("tempDisabledIds"))
            ? req->get("tempDisabledIds")->asStringArray()
            : std::vector<std::string>{};
        const auto res = grml::launcher_api::launchGame(g_loaderDir, gameExe, tempDisabledIds);
        if (res.ok && res.processId != 0) {
            g_lastLaunchPid = res.processId;
            g_lastLaunchMs  = nowMs();
        }
        sendHttpResponse(client, res.ok ? 200 : 500, res.ok ? "OK" : "Error",
                         "application/json; charset=utf-8", res.json);
    } else if (method == "GET") {
        const std::string lookup = (path == "/") ? "/index.html" : path;
        // Allow external locale JSON files in %LOCALAPPDATA%\GRML\locales\ or <loaderDir>\locales\
        // to override or extend embedded translations without rebuilding.
        if (lookup.starts_with("/locales/") && lookup.ends_with(".json")) {
            const std::string rel = lookup.substr(9);
            if (!rel.empty() && rel.find('/') == std::string::npos &&
                rel.find('\\') == std::string::npos && rel.find("..") == std::string::npos) {
                const std::wstring wrel = grml::utf8ToWide(rel);
                std::string extBytes;
                if (grml::readFileBytesW(dataDir / L"locales" / wrel, extBytes) ||
                    grml::readFileBytesW(g_loaderDir / L"locales" / wrel, extBytes)) {
                    sendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", extBytes);
                    closesocket(client);
                    return;
                }
            }
        }
        const auto assets = grml::generated::embeddedUiAssets();
        const grml::generated::EmbeddedUiAsset* matched  = nullptr;
        const grml::generated::EmbeddedUiAsset* fallback = nullptr;
        for (const auto& a : assets) {
            if (lookup == a.urlPath) { matched = &a; break; }
            if (std::strcmp(a.urlPath, "/index.html") == 0) fallback = &a;
        }
        const auto* chosen = matched ? matched : fallback;
        if (!chosen) {
            sendHttpResponse(client, 404, "Not Found", "text/plain", "UI assets not embedded");
        } else {
            sendHttpResponse(client, 200, "OK", chosen->mimeType,
                             std::string_view(reinterpret_cast<const char*>(chosen->data), chosen->size));
        }
    } else {
        sendHttpResponse(client, 404, "Not Found", "text/plain", "Not Found");
    }

    closesocket(client);
}

void acceptLoop()
{
    while (g_running.load(std::memory_order_relaxed)) {
        SOCKET client = accept(g_listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        std::thread(handleClient, client).detach();
    }
}

} // namespace

bool start(const std::filesystem::path& loaderDir, uint16_t& outPort)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    g_loaderDir = loaderDir;
    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSock == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (bind(g_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(g_listenSock);
        g_listenSock = INVALID_SOCKET;
        return false;
    }
    if (listen(g_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(g_listenSock);
        g_listenSock = INVALID_SOCKET;
        return false;
    }

    int len = sizeof(addr);
    getsockname(g_listenSock, reinterpret_cast<sockaddr*>(&addr), &len);
    outPort = ntohs(addr.sin_port);

    g_running.store(true, std::memory_order_relaxed);
    g_acceptThread = std::thread(acceptLoop);
    return true;
}

void stop()
{
    g_running.store(false, std::memory_order_relaxed);
    if (g_listenSock != INVALID_SOCKET) {
        closesocket(g_listenSock);
        g_listenSock = INVALID_SOCKET;
    }
    if (g_acceptThread.joinable()) {
        g_acceptThread.join();
    }
    WSACleanup();
}

} // namespace grml::http_server
