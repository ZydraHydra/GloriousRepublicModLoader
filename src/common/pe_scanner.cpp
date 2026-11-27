#include "common/pe_scanner.hpp"
#include "common/ini_util.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <sstream>

namespace grml {

bool parseAobPattern(std::string_view patStr,
                     std::vector<uint8_t>& bytes,
                     std::vector<uint8_t>& masks)
{
    bytes.clear();
    masks.clear();
    size_t i = 0;
    while (i < patStr.size()) {
        while (i < patStr.size() && (patStr[i] == ' ' || patStr[i] == '\t')) {
            ++i;
        }
        if (i >= patStr.size()) break;
        size_t j = i;
        while (j < patStr.size() && patStr[j] != ' ' && patStr[j] != '\t') {
            ++j;
        }
        const std::string_view tok = patStr.substr(i, j - i);
        i = j;

        if (tok == "?" || tok == "??") {
            bytes.push_back(0x00);
            masks.push_back(0x00);
        } else {
            uint8_t b = 0;
            auto res = std::from_chars(tok.data(), tok.data() + tok.size(), b, 16);
            if (res.ec != std::errc{} || res.ptr != tok.data() + tok.size()) {
                return false;
            }
            bytes.push_back(b);
            masks.push_back(0xFF);
        }
    }
    for (uint8_t m : masks) {
        if (m != 0) return !bytes.empty();
    }
    return false;
}

std::string readRegistryStringW(HKEY root, const wchar_t* subkey, const wchar_t* valueName)
{
    DWORD size = 0;
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < 2 || size > 64 * 1024) {
        return {};
    }
    std::wstring buf((size + 1) / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    if (!buf.empty() && buf.back() == L'\0') {
        buf.resize(std::wcslen(buf.c_str()));
    }
    return wideToUtf8(buf);
}

std::vector<std::filesystem::path> parseLibraryFoldersVdf(const std::filesystem::path& vdfPath)
{
    std::vector<std::filesystem::path> out;
    std::string raw;
    if (!readFileBytesW(vdfPath, raw)) {
        return out;
    }

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        const std::string_view line(raw.data() + pos, eol - pos);
        pos = eol + 1;

        const size_t q1 = line.find('"');
        if (q1 == std::string_view::npos) continue;
        const size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string_view::npos) continue;
        const std::string_view key = line.substr(q1 + 1, q2 - q1 - 1);

        const size_t q3 = line.find('"', q2 + 1);
        if (q3 == std::string_view::npos) continue;
        const size_t q4 = line.find('"', q3 + 1);
        if (q4 == std::string_view::npos) continue;
        const std::string_view val = line.substr(q3 + 1, q4 - q3 - 1);

        bool isNumericKey = !key.empty();
        for (char c : key) {
            if (c < '0' || c > '9') {
                isNumericKey = false;
                break;
            }
        }
        if (key == "path" || isNumericKey) {
            std::string unescaped;
            unescaped.reserve(val.size());
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == '\\') {
                    unescaped += '\\';
                    ++i;
                } else {
                    unescaped += val[i];
                }
            }
            if (!unescaped.empty()) {
                out.emplace_back(utf8ToWide(unescaped));
            }
        }
    }
    return out;
}

bool inspectLoadedPe(uint8_t* moduleBase, PeModuleInfo& out)
{
    out = {};
    if (!moduleBase) return false;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

    out.base                = moduleBase;
    out.imageSize           = nt->OptionalHeader.SizeOfImage;
    out.timeDateStamp       = nt->FileHeader.TimeDateStamp;
    out.addressOfEntryPoint = nt->OptionalHeader.AddressOfEntryPoint;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9]{};
        std::memcpy(name, sec[i].Name, 8);
        if (std::strcmp(name, ".text") == 0) {
            const uint32_t span = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            if (sec[i].VirtualAddress < out.imageSize && span <= out.imageSize - sec[i].VirtualAddress) {
                out.textBase = moduleBase + sec[i].VirtualAddress;
                out.textSize = span;
            }
        }
    }

    const auto& excDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.VirtualAddress && excDir.Size >= 12 &&
        excDir.VirtualAddress < out.imageSize &&
        excDir.Size <= out.imageSize - excDir.VirtualAddress) {
        out.runtimeFuncs     = moduleBase + excDir.VirtualAddress;
        out.runtimeFuncCount = excDir.Size / 12;
    }

    return out.textBase != nullptr && out.textSize > 0;
}

DiskPeExports inspectDiskPeExports(const std::filesystem::path& path)
{
    DiskPeExports result;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return result;
    }
    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return result;
    }
    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return result;
    }

    const auto* base = static_cast<const uint8_t*>(view);
    const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 && dos->e_lfanew < 0x100000) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE &&
            nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0 &&
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            result.isValidAmd64Dll = true;
            const auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (expDir.VirtualAddress && expDir.Size >= sizeof(IMAGE_EXPORT_DIRECTORY) &&
                expDir.VirtualAddress < nt->OptionalHeader.SizeOfImage) {
                const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expDir.VirtualAddress);
                if (exp->AddressOfNames && exp->AddressOfNames < nt->OptionalHeader.SizeOfImage) {
                    const auto* nameRvas = reinterpret_cast<const uint32_t*>(base + exp->AddressOfNames);
                    const uint32_t limit = (std::min<uint32_t>)(exp->NumberOfNames, 4096u);
                    for (uint32_t i = 0; i < limit; ++i) {
                        if (nameRvas[i] < nt->OptionalHeader.SizeOfImage) {
                            result.exports.emplace_back(reinterpret_cast<const char*>(base + nameRvas[i]));
                        }
                    }
                }
            }
        }
    }

    UnmapViewOfFile(view);
    CloseHandle(hMap);
    CloseHandle(hFile);
    return result;
}

std::string computeFileSha256(const std::filesystem::path& path)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return {};
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        CloseHandle(hFile);
        return {};
    }
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return {};
    }

    uint8_t buf[65536]{};
    DWORD read = 0;
    bool ok = true;
    while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0) {
        if (BCryptHashData(hHash, buf, read, 0) != 0) {
            ok = false;
            break;
        }
    }

    uint8_t digest[32]{};
    if (ok && BCryptFinishHash(hHash, digest, sizeof(digest), 0) == 0) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (size_t i = 0; i < 32; ++i) {
            out[i * 2]     = kHex[(digest[i] >> 4) & 0xF];
            out[i * 2 + 1] = kHex[digest[i] & 0xF];
        }
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return out;
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    CloseHandle(hFile);
    return {};
}

AobScanSummary runSinglePassAobScan(const uint8_t* scanBase,
                                    size_t scanSize,
                                    std::vector<AobPatternRequest>& requests)
{
    AobScanSummary summary{};
    summary.totalPatterns = requests.size();
    if (!scanBase || scanSize == 0 || requests.empty()) {
        for (auto& r : requests) {
            if (r.outAddr) *r.outAddr = nullptr;
        }
        summary.missingCount = requests.size();
        return summary;
    }

    struct CompiledPat {
        size_t               reqIdx{0};
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> masks;
        size_t               anchorOff{0};
        uint8_t              anchorByte{0};
    };

    std::vector<CompiledPat> compiled;
    compiled.reserve(requests.size());

    // Bucket by anchor byte at offset anchorOff for O(1) first-byte dispatch per offset
    std::vector<size_t> buckets[256];

    for (size_t i = 0; i < requests.size(); ++i) {
        requests[i].matchCount = 0;
        requests[i].firstMatch = nullptr;
        if (requests[i].outAddr) {
            *requests[i].outAddr = nullptr;
        }

        CompiledPat cp;
        cp.reqIdx = i;
        if (!parseAobPattern(requests[i].patternStr, cp.bytes, cp.masks) ||
            cp.bytes.size() > scanSize) {
            continue;
        }

        // Prefer the first non-wildcard byte as our anchor filter
        for (size_t k = 0; k < cp.masks.size(); ++k) {
            if (cp.masks[k] == 0xFF) {
                cp.anchorOff  = k;
                cp.anchorByte = cp.bytes[k];
                break;
            }
        }
        const size_t cpIdx = compiled.size();
        buckets[cp.anchorByte].push_back(cpIdx);
        compiled.push_back(std::move(cp));
    }

    // Single sequential pass over .text
    for (size_t pos = 0; pos < scanSize; ++pos) {
        const uint8_t b = scanBase[pos];
        const auto& candList = buckets[b];
        for (const size_t cpIdx : candList) {
            const auto& cp = compiled[cpIdx];
            if (pos < cp.anchorOff) {
                continue;
            }
            const size_t start = pos - cp.anchorOff;
            if (start + cp.bytes.size() > scanSize) {
                continue;
            }
            const uint8_t* ptr = scanBase + start;
            bool match = true;
            for (size_t j = 0; j < cp.bytes.size(); ++j) {
                if (cp.masks[j] && ptr[j] != cp.bytes[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                auto& req = requests[cp.reqIdx];
                if (req.matchCount == 0) {
                    req.firstMatch = const_cast<uint8_t*>(ptr);
                }
                ++req.matchCount;
            }
        }
    }

    for (auto& req : requests) {
        if (req.matchCount == 1) {
            ++summary.uniqueMatches;
            if (req.outAddr) {
                *req.outAddr = req.firstMatch;
            }
        } else if (req.matchCount == 0) {
            ++summary.missingCount;
            if (req.outAddr) {
                *req.outAddr = nullptr;
            }
        } else {
            ++summary.ambiguousCount;
            if (req.outAddr) {
                *req.outAddr = nullptr;
            }
        }
    }
    return summary;
}

void* resolveRipRelativeDisp32(void* site, uint32_t dispOff, uint32_t nextIpOff) noexcept
{
    if (!site) return nullptr;
    const auto* p = static_cast<const uint8_t*>(site);
    int32_t disp = 0;
    std::memcpy(&disp, p + dispOff, sizeof(disp));
    return const_cast<uint8_t*>(p + nextIpOff + disp);
}

bool verifyAobAtAddress(const uint8_t* addr,
                        const uint8_t* textBase,
                        size_t textSize,
                        std::string_view patStr) noexcept
{
    if (!addr || !textBase || textSize == 0 || addr < textBase) {
        return false;
    }
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> masks;
    if (!parseAobPattern(patStr, bytes, masks) || bytes.empty()) {
        return false;
    }
    const size_t offset = static_cast<size_t>(addr - textBase);
    if (offset > textSize || bytes.size() > textSize - offset) {
        return false;
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (masks[i] && addr[i] != bytes[i]) {
            return false;
        }
    }
    return true;
}

std::string readPeFileVersion(const std::filesystem::path& exePath)
{
    if (exePath.empty()) return {};
    DWORD dummy = 0;
    const DWORD sz = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (sz == 0 || sz > 1024 * 1024) return {};
    std::vector<uint8_t> buf(sz);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, sz, buf.data())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiLen) ||
        !ffi || ffiLen < sizeof(VS_FIXEDFILEINFO)) {
        return {};
    }
    if (ffi->dwSignature != 0xFEEF04BDu) return {};
    char out[64]{};
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u",
                  static_cast<unsigned>(HIWORD(ffi->dwFileVersionMS)),
                  static_cast<unsigned>(LOWORD(ffi->dwFileVersionMS)),
                  static_cast<unsigned>(HIWORD(ffi->dwFileVersionLS)),
                  static_cast<unsigned>(LOWORD(ffi->dwFileVersionLS)));
    return out;
}

uint64_t fnv1a64(std::string_view data) noexcept
{
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : data) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

bool lookupRuntimeFunctionRva(const PeModuleInfo& pe, uint32_t rva, uint32_t& outBegin, uint32_t& outEnd) noexcept
{
    if (!pe.runtimeFuncs || pe.runtimeFuncCount == 0) return false;
    uint32_t lo = 0;
    uint32_t hi = pe.runtimeFuncCount;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t b = 0;
        std::memcpy(&b, pe.runtimeFuncs + mid * 12u, 4);
        if (b <= rva) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) return false;
    const uint8_t* rf = pe.runtimeFuncs + (lo - 1u) * 12u;
    uint32_t b = 0, e = 0;
    std::memcpy(&b, rf, 4);
    std::memcpy(&e, rf + 4, 4);
    if (rva < b || rva >= e || b >= pe.imageSize || e > pe.imageSize) return false;
    outBegin = b;
    outEnd   = e;
    return true;
}

std::vector<std::filesystem::path> discoverSteamLibraries()
{
    std::vector<std::filesystem::path> roots;
    auto addUnique = [&](const std::filesystem::path& p) {
        if (p.empty()) return;
        std::error_code ec;
        std::filesystem::path norm = std::filesystem::weakly_canonical(p, ec);
        if (ec) norm = p.lexically_normal();
        for (const auto& existing : roots) {
            if (_wcsicmp(existing.c_str(), norm.c_str()) == 0) return;
        }
        if (std::filesystem::exists(norm, ec)) {
            roots.push_back(norm);
        }
    };

    const std::string hkcuSteam = readRegistryStringW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!hkcuSteam.empty()) addUnique(utf8ToWide(hkcuSteam));

    const std::string hklmSteam = readRegistryStringW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (!hklmSteam.empty()) addUnique(utf8ToWide(hklmSteam));

    addUnique(L"C:\\Program Files (x86)\\Steam");
    addUnique(L"C:\\Program Files\\Steam");

    const size_t initialCount = roots.size();
    for (size_t i = 0; i < initialCount; ++i) {
        const auto vdf = roots[i] / L"steamapps" / L"libraryfolders.vdf";
        for (const auto& lib : parseLibraryFoldersVdf(vdf)) {
            addUnique(lib);
        }
    }
    return roots;
}

std::filesystem::path discoverWrsrGameExe(const std::filesystem::path& baseDir,
                                          const std::filesystem::path& configuredPath)
{
    std::error_code ec;
    auto isExe = [&](const std::filesystem::path& p) -> bool {
        return !p.empty() && std::filesystem::is_regular_file(p, ec);
    };

    if (isExe(configuredPath)) return configuredPath;
    if (isExe(baseDir / L"SOVIET64.exe")) return baseDir / L"SOVIET64.exe";
    if (baseDir.has_parent_path() && isExe(baseDir.parent_path() / L"SOVIET64.exe")) {
        return baseDir.parent_path() / L"SOVIET64.exe";
    }

    for (const auto& lib : discoverSteamLibraries()) {
        const auto cand = lib / L"steamapps" / L"common" / L"SovietRepublic" / L"SOVIET64.exe";
        if (isExe(cand)) return cand;
    }
    return {};
}

std::filesystem::path workshopRootFromGameExe(const std::filesystem::path& gameExe)
{
    if (gameExe.empty()) return {};
    const auto gameDir = gameExe.parent_path();
    const auto common  = gameDir.parent_path();
    if (_wcsicmp(common.filename().c_str(), L"common") != 0) {
        return {};
    }
    const auto steamapps = common.parent_path();
    if (_wcsicmp(steamapps.filename().c_str(), L"steamapps") != 0) {
        return {};
    }
    return steamapps / L"workshop" / L"content" / L"784150";
}

} // namespace grml
