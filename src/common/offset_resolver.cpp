#include "common/offset_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace grml::offsets {

namespace {

std::string lowerAscii(std::string_view sv)
{
    std::string out(sv);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

std::string_view trimView(std::string_view sv) noexcept
{
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

bool parseU32Auto(std::string_view text, uint32_t& out) noexcept
{
    text = trimView(text);
    if (text.empty()) return false;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) return false;
    uint32_t val = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), val, base);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) return false;
    out = val;
    return true;
}

bool parseU64Auto(std::string_view text, uint64_t& out) noexcept
{
    text = trimView(text);
    if (text.empty()) return false;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) return false;
    uint64_t val = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), val, base);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) return false;
    out = val;
    return true;
}

int64_t parseI64Auto(std::string_view text, int64_t fallback = 0) noexcept
{
    const std::string s(trimView(text));
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 0);
    if (end && end != s.c_str() && *end == '\0') {
        return static_cast<int64_t>(v);
    }
    return fallback;
}

bool listMatchesU32(std::string_view list, uint32_t target) noexcept
{
    size_t pos = 0;
    while (pos < list.size()) {
        const size_t comma = list.find(',', pos);
        const std::string_view token = trimView(
            comma == std::string_view::npos
                ? list.substr(pos)
                : list.substr(pos, comma - pos));
        uint32_t parsed = 0;
        if (!token.empty() && parseU32Auto(token, parsed) && parsed == target) {
            return true;
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return false;
}

bool isReservedSection(std::string_view secLower) noexcept
{
    return secLower == "signatures" ||
           secLower == "builds" ||
           secLower == "general" ||
           secLower.rfind("profile.", 0) == 0;
}

uint64_t computeCanonicalIniHash(const IniFile& ini)
{
    std::string canonical;
    canonical.reserve(4096);
    for (const auto& sec : ini.sectionOrder()) {
        canonical += '[';
        canonical += sec;
        canonical += "]\n";
        const auto* kv = ini.sectionKeys(sec);
        if (!kv) continue;
        std::vector<std::string> keys;
        keys.reserve(kv->size());
        for (const auto& [k, _] : *kv) {
            keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) {
            canonical += k;
            canonical += '=';
            canonical += kv->at(k);
            canonical += '\n';
        }
    }
    return fnv1a64(canonical);
}

bool collectProfileRecursive(const IniFile& ini,
                             std::string_view profileName,
                             int depth,
                             std::unordered_map<std::string, std::string>& out)
{
    if (depth > 8) return false;
    const std::string secName = "profile." + lowerAscii(profileName);
    const auto* kv = ini.sectionKeys(secName);
    if (!kv) return false;

    const auto baseIt = kv->find("base");
    if (baseIt != kv->end() && !baseIt->second.empty()) {
        if (!collectProfileRecursive(ini, baseIt->second, depth + 1, out)) {
            return false;
        }
    }
    for (const auto& [k, v] : *kv) {
        if (k != "base") {
            out[k] = v;
        }
    }
    return true;
}

} // namespace

const char* resolveModeToString(OffsetResolveMode mode) noexcept
{
    switch (mode) {
    case OffsetResolveMode::FunctionStart: return "function_start";
    case OffsetResolveMode::RipRel32:     return "rip_rel32";
    case OffsetResolveMode::CallRel32:    return "call_rel32";
    case OffsetResolveMode::Disp32:       return "disp32";
    }
    return "function_start";
}

OffsetResolveMode parseResolveMode(std::string_view text) noexcept
{
    const std::string lo = lowerAscii(trimView(text));
    if (lo == "rip_rel32") return OffsetResolveMode::RipRel32;
    if (lo == "call_rel32") return OffsetResolveMode::CallRel32;
    if (lo == "disp32") return OffsetResolveMode::Disp32;
    return OffsetResolveMode::FunctionStart;
}

const char* offsetStatusToString(OffsetStatus status) noexcept
{
    switch (status) {
    case OffsetStatus::Unscanned: return "UNSCANNED";
    case OffsetStatus::Ok:        return "OK";
    case OffsetStatus::Cached:    return "CACHED";
    case OffsetStatus::Missing:   return "MISSING";
    }
    return "MISSING";
}

bool loadMergedOffsetsIni(const std::filesystem::path& loaderDir,
                          const std::filesystem::path& dataDir,
                          std::string_view embeddedIni,
                          IniFile& outIni,
                          uint64_t& outHash)
{
    outIni = IniFile{};
    if (!embeddedIni.empty()) {
        outIni.loadFromMemory(embeddedIni);
    }

    std::error_code ec;
    if (!loaderDir.empty()) {
        const auto loaderIni = loaderDir / L"grml_offsets.ini";
        if (std::filesystem::exists(loaderIni, ec)) {
            IniFile ext;
            if (ext.loadFromFile(loaderIni)) {
                outIni.merge(ext);
            }
        }
    }
    if (!dataDir.empty()) {
        const auto otaIni = dataDir / L"grml_offsets.ini";
        if (std::filesystem::exists(otaIni, ec)) {
            const auto loaderIni = loaderDir.empty() ? std::filesystem::path{} : (loaderDir / L"grml_offsets.ini");
            if (loaderIni.empty() || !std::filesystem::equivalent(otaIni, loaderIni, ec)) {
                IniFile ota;
                if (ota.loadFromFile(otaIni)) {
                    outIni.merge(ota);
                }
            }
        }
    }

    outHash = computeCanonicalIniHash(outIni);
    return !outIni.sectionOrder().empty();
}

std::vector<OffsetDefinition> collectOffsetDefinitions(const IniFile& ini)
{
    std::vector<OffsetDefinition> defs;
    for (const auto& sec : ini.sectionOrder()) {
        if (isReservedSection(sec)) continue;
        const std::string pat = ini.getString(sec, "pattern", "");
        if (pat.empty()) continue;

        OffsetDefinition def;
        def.name          = sec;
        def.pattern       = pat;
        def.resolveMode   = parseResolveMode(ini.getString(sec, "resolve", "function_start"));
        def.opOffset      = static_cast<int32_t>(parseI64Auto(ini.getString(sec, "op_offset", "0"), 0));
        def.instrLen      = static_cast<int32_t>(parseI64Auto(ini.getString(sec, "instr_len", "0"), 0));
        def.extraOffset   = parseI64Auto(ini.getString(sec, "extra_offset", "0"), 0);
        def.category      = ini.getString(sec, "category", "General");
        if (def.category.empty()) def.category = "General";
        def.anchorXref    = ini.getString(sec, "anchor_xref", "");
        def.verifiedBuild = ini.getString(sec, "verified_build", "");
        def.required      = ini.getBool(sec, "required", false);
        defs.push_back(std::move(def));
    }
    return defs;
}

bool applyOffsetResolveRule(const PeModuleInfo& pe,
                            const uint8_t* matchAddr,
                            const OffsetDefinition& def,
                            uintptr_t& outValue) noexcept
{
    outValue = 0;
    if (!matchAddr || !pe.textBase || pe.textSize == 0) return false;
    const uint8_t* textEnd = pe.textBase + pe.textSize;
    if (matchAddr < pe.textBase || matchAddr >= textEnd) return false;

    const auto readBoundsOk = [&](int32_t off, size_t nBytes) noexcept -> bool {
        if (off < 0) return false;
        const size_t uOff = static_cast<size_t>(off);
        const size_t avail = static_cast<size_t>(textEnd - matchAddr);
        return uOff <= avail && nBytes <= (avail - uOff);
    };

    switch (def.resolveMode) {
    case OffsetResolveMode::FunctionStart: {
        const intptr_t addr = reinterpret_cast<intptr_t>(matchAddr) + static_cast<intptr_t>(def.extraOffset);
        if (addr <= 0) return false;
        outValue = static_cast<uintptr_t>(addr);
        return true;
    }
    case OffsetResolveMode::RipRel32:
    case OffsetResolveMode::CallRel32: {
        if (!readBoundsOk(def.opOffset, sizeof(int32_t)) || def.instrLen <= 0) {
            return false;
        }
        int32_t disp = 0;
        std::memcpy(&disp, matchAddr + def.opOffset, sizeof(disp));
        const intptr_t target = reinterpret_cast<intptr_t>(matchAddr) +
                                static_cast<intptr_t>(def.instrLen) +
                                static_cast<intptr_t>(disp) +
                                static_cast<intptr_t>(def.extraOffset);
        if (target <= 0) return false;
        outValue = static_cast<uintptr_t>(target);
        return true;
    }
    case OffsetResolveMode::Disp32: {
        if (!readBoundsOk(def.opOffset, sizeof(uint32_t))) {
            return false;
        }
        uint32_t disp = 0;
        std::memcpy(&disp, matchAddr + def.opOffset, sizeof(disp));
        const int64_t val = static_cast<int64_t>(disp) + def.extraOffset;
        if (val < 0) return false;
        outValue = static_cast<uintptr_t>(val);
        return true;
    }
    }
    return false;
}

bool tryResolveFromCache(const std::filesystem::path& cachePath,
                         const PeModuleInfo& pe,
                         uint64_t iniHash,
                         const std::vector<OffsetDefinition>& defs,
                         std::vector<ResolvedOffsetEntry>& outEntries)
{
    outEntries.clear();
    outEntries.reserve(defs.size());
    for (const auto& d : defs) {
        ResolvedOffsetEntry e;
        e.def    = d;
        e.status = OffsetStatus::Unscanned;
        outEntries.push_back(std::move(e));
    }

    if (defs.empty() || !pe.base || !pe.textBase || pe.textSize == 0) {
        return false;
    }

    IniFile cacheIni;
    if (!cacheIni.loadFromFile(cachePath)) {
        return false;
    }

    uint32_t cachedTs  = 0;
    uint32_t cachedImg = 0;
    uint32_t cachedEp  = 0;
    uint64_t cachedIni = 0;
    if (!parseU32Auto(cacheIni.getString("meta", "pe_timestamp", ""), cachedTs) ||
        !parseU32Auto(cacheIni.getString("meta", "pe_size_of_image", ""), cachedImg) ||
        !parseU32Auto(cacheIni.getString("meta", "pe_entry_point", ""), cachedEp) ||
        !parseU64Auto(cacheIni.getString("meta", "offsets_fnv1a64", ""), cachedIni)) {
        return false;
    }

    if (cachedTs != pe.timeDateStamp ||
        cachedImg != static_cast<uint32_t>(pe.imageSize) ||
        cachedEp != pe.addressOfEntryPoint ||
        cachedIni != iniHash) {
        return false;
    }

    bool allVerified = true;
    for (auto& entry : outEntries) {
        const std::string rvaStr = cacheIni.getString("rvas", entry.def.name, "");
        if (lowerAscii(rvaStr) == "missing") {
            entry.status        = OffsetStatus::Missing;
            entry.matchCount    = 0;
            entry.matchedRva    = 0;
            entry.resolvedValue = 0;
            continue;
        }
        uint32_t rva = 0;
        if (rvaStr.empty() || !parseU32Auto(rvaStr, rva) || rva >= pe.imageSize) {
            allVerified = false;
            continue;
        }
        const uint8_t* matchAddr = pe.base + rva;
        if (!verifyAobAtAddress(matchAddr, pe.textBase, pe.textSize, entry.def.pattern)) {
            allVerified = false;
            continue;
        }
        uintptr_t resolved = 0;
        if (!applyOffsetResolveRule(pe, matchAddr, entry.def, resolved)) {
            allVerified = false;
            continue;
        }
        entry.status        = OffsetStatus::Cached;
        entry.matchCount    = 1;
        entry.matchedRva    = rva;
        entry.resolvedValue = resolved;
    }

    return allVerified;
}

bool writeOffsetCache(const std::filesystem::path& cachePath,
                      const PeModuleInfo& pe,
                      uint64_t iniHash,
                      const std::vector<ResolvedOffsetEntry>& entries)
{
    if (cachePath.empty() || !pe.base) return false;
    char buf[64]{};
    std::string out = "; Auto-generated by GRML — PE-Hash RVA Offset Cache\n[meta]\n";
    std::snprintf(buf, sizeof(buf), "0x%08X", pe.timeDateStamp);
    out += "pe_timestamp = ";
    out += buf;
    out += "\n";

    std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(pe.imageSize));
    out += "pe_size_of_image = ";
    out += buf;
    out += "\n";

    std::snprintf(buf, sizeof(buf), "0x%X", pe.addressOfEntryPoint);
    out += "pe_entry_point = ";
    out += buf;
    out += "\n";

    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(iniHash));
    out += "offsets_fnv1a64 = ";
    out += buf;
    out += "\n\n[rvas]\n";

    for (const auto& entry : entries) {
        if ((entry.status == OffsetStatus::Ok || entry.status == OffsetStatus::Cached) && entry.matchedRva != 0) {
            std::snprintf(buf, sizeof(buf), "0x%X", entry.matchedRva);
            out += entry.def.name;
            out += " = ";
            out += buf;
            out += "\n";
        } else if (entry.status == OffsetStatus::Missing) {
            out += entry.def.name;
            out += " = MISSING\n";
        }
    }

    return writeFileBytesW(cachePath, out);
}

bool resolveWrsrProfileOffsets(const IniFile& ini,
                               const PeModuleInfo& pe,
                               GrmlWrsrOffsets& outOffsets)
{
    std::string matchedProf = "default";
    bool exactMatch = false;

    if (pe.timeDateStamp != 0) {
        for (const auto& sec : ini.sectionOrder()) {
            if (sec.rfind("profile.", 0) != 0 || sec == "profile.default") continue;
            const std::string tsList = ini.getString(sec, "pe_timestamp", "");
            if (tsList.empty() || !listMatchesU32(tsList, pe.timeDateStamp)) continue;
            const std::string szList = ini.getString(sec, "pe_size_of_image", "");
            if (!szList.empty() && pe.imageSize != 0 && !listMatchesU32(szList, static_cast<uint32_t>(pe.imageSize))) {
                continue;
            }
            matchedProf = sec.substr(8);
            exactMatch = true;
            break;
        }
    }

    std::unordered_map<std::string, std::string> merged;
    if (!collectProfileRecursive(ini, matchedProf, 0, merged)) {
        if (!collectProfileRecursive(ini, "default", 0, merged)) {
            return false;
        }
    }

    auto getOff = [&](const char* key, uint32_t def) -> uint32_t {
        const auto it = merged.find(key);
        if (it == merged.end()) return def;
        uint32_t v = def;
        return parseU32Auto(it->second, v) ? v : def;
    };

    outOffsets.abi_version               = GRML_API_VERSION;
    outOffsets.struct_size               = sizeof(GrmlWrsrOffsets);
    outOffsets.exact_profile_match       = exactMatch ? 1u : 0u;
    outOffsets.res_stride                = getOff("res_stride", 0x380);
    outOffsets.res_min_header_size       = getOff("res_min_header_size", 0x80);
    outOffsets.res_name_off              = getOff("res_name_off", 0x00);
    outOffsets.res_kind_off              = getOff("res_kind_off", 0x44);
    outOffsets.res_buy_usd_off           = getOff("res_buy_usd_off", 0x58);
    outOffsets.res_buy_rub_off           = getOff("res_buy_rub_off", 0x5C);
    outOffsets.res_sell_usd_off          = getOff("res_sell_usd_off", 0x60);
    outOffsets.res_sell_rub_off          = getOff("res_sell_rub_off", 0x64);
    outOffsets.task_res_off              = getOff("task_res_off", 0x00);
    outOffsets.task_src_bld_off          = getOff("task_src_bld_off", 0x10);
    outOffsets.task_dst_bld_off          = getOff("task_dst_bld_off", 0x48);
    outOffsets.task_score_src_double_off = getOff("task_score_src_double_off", 0x28);
    outOffsets.task_score_src_flag1_off  = getOff("task_score_src_flag1_off", 0x40);
    outOffsets.task_score_src_flag2_off  = getOff("task_score_src_flag2_off", 0x42);
    outOffsets.task_score_dst_double_off = getOff("task_score_dst_double_off", 0x60);
    outOffsets.task_score_dst_flag1_off  = getOff("task_score_dst_flag1_off", 0x78);
    outOffsets.task_score_dst_flag2_off  = getOff("task_score_dst_flag2_off", 0x7A);
    outOffsets.bld_desc_off              = getOff("bld_desc_off", 0x318);
    outOffsets.bld_faction_off           = getOff("bld_faction_off", 0x9EC);
    outOffsets.desc_type_off             = getOff("desc_type_off", 0x368);
    outOffsets.customs_building_type     = static_cast<int32_t>(getOff("customs_building_type", 20));
    outOffsets.faction_usd               = static_cast<int32_t>(getOff("faction_usd", 3));
    outOffsets.faction_rub               = static_cast<int32_t>(getOff("faction_rub", 4));
    return true;
}

} // namespace grml::offsets
