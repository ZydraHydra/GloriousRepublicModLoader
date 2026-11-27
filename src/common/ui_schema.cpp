#include "common/ui_schema.hpp"
#include "common/ini_util.hpp"
#include "common/json_min.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace grml {

namespace {

std::string lowerStr(std::string_view sv)
{
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trimStr(std::string_view sv)
{
    size_t b = 0;
    while (b < sv.size() && (sv[b] == ' ' || sv[b] == '\t' || sv[b] == '\r' || sv[b] == '\n')) ++b;
    size_t e = sv.size();
    while (e > b && (sv[e - 1] == ' ' || sv[e - 1] == '\t' || sv[e - 1] == '\r' || sv[e - 1] == '\n')) --e;
    return std::string(sv.substr(b, e - b));
}

std::string unquoteYaml(std::string s)
{
    s = trimStr(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

UiControlType parseControlType(std::string_view t)
{
    const std::string l = lowerStr(t);
    if (l == "bool" || l == "boolean" || l == "checkbox" || l == "toggle") return UiControlType::Bool;
    if (l == "int" || l == "integer") return UiControlType::Int;
    if (l == "float" || l == "number" || l == "double" || l == "slider") return UiControlType::Float;
    if (l == "select" || l == "enum" || l == "dropdown") return UiControlType::Select;
    if (l == "header" || l == "section" || l == "label") return UiControlType::Header;
    return UiControlType::String;
}

const char* controlTypeName(UiControlType t) noexcept
{
    switch (t) {
    case UiControlType::Bool:   return "bool";
    case UiControlType::Int:    return "int";
    case UiControlType::Float:  return "float";
    case UiControlType::String: return "string";
    case UiControlType::Select: return "select";
    case UiControlType::Header: return "header";
    }
    return "string";
}

std::string jsonScalarToString(const JsonValue* v)
{
    if (!v) return "";
    if (v->isString()) return v->asString();
    if (v->isBool()) return v->asBool() ? "1" : "0";
    if (v->isNumber()) {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%g", v->asNumber());
        return buf;
    }
    return "";
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

} // namespace

UiSchema parseUiSchemaFromJson(const JsonValue& root, const std::filesystem::path& sourceFile)
{
    UiSchema schema;
    schema.sourceFile = sourceFile;
    if (!root.isObject()) return schema;

    if (auto* v = root.get("tabTitle")) schema.tabTitle = v->asString();
    if (auto* v = root.get("tab_title")) schema.tabTitle = v->asString();
    if (auto* v = root.get("iniFile")) schema.iniFile = v->asString();
    if (auto* v = root.get("ini_file")) schema.iniFile = v->asString();
    if (auto* v = root.get("mainMenuAvailable")) schema.mainMenuAvailable = v->asBool(true);
    if (auto* v = root.get("main_menu_available")) schema.mainMenuAvailable = v->asBool(true);

    const JsonValue* ctrls = root.get("controls");
    if (!ctrls) ctrls = root.get("fields");
    if (!ctrls || !ctrls->isArray()) return schema;

    for (const auto& item : std::get<JsonArray>(ctrls->data)) {
        if (!item.isObject()) continue;
        UiControlDesc c;
        if (auto* v = item.get("type")) c.type = parseControlType(v->asString());
        if (auto* v = item.get("section")) c.section = v->asString();
        if (auto* v = item.get("key")) c.key = v->asString();
        if (auto* v = item.get("label")) c.label = v->asString();
        if (auto* v = item.get("tooltip")) c.tooltip = v->asString();
        if (auto* v = item.get("format")) c.format = v->asString();
        if (auto* v = item.get("min")) c.minValue = v->asNumber(0.0);
        if (auto* v = item.get("max")) c.maxValue = v->asNumber(1.0);
        if (auto* v = item.get("step")) c.step = v->asNumber(0.0);
        c.hasExplicitRange = (item.get("min") != nullptr || item.get("max") != nullptr);
        if (auto* v = item.get("default")) c.defaultValue = jsonScalarToString(v);
        if (auto* v = item.get("options")) c.options = v->asStringArray();

        if (c.label.empty()) c.label = c.key;
        if (c.type != UiControlType::Header && c.key.empty()) continue;
        schema.controls.push_back(std::move(c));
    }

    schema.valid = !schema.controls.empty();
    return schema;
}

UiSchema parseUiSchemaFromYaml(std::string_view yamlText, const std::filesystem::path& sourceFile)
{
    UiSchema schema;
    schema.sourceFile = sourceFile;

    // Strip UTF-8 BOM
    if (yamlText.size() >= 3 &&
        static_cast<unsigned char>(yamlText[0]) == 0xEF &&
        static_cast<unsigned char>(yamlText[1]) == 0xBB &&
        static_cast<unsigned char>(yamlText[2]) == 0xBF) {
        yamlText.remove_prefix(3);
    }

    bool inControls = false;
    bool inOptions  = false;
    UiControlDesc current{};
    bool hasCurrent = false;
    bool currentHadMinMax = false;

    auto flushCurrent = [&]() {
        if (!hasCurrent) return;
        if (current.label.empty()) current.label = current.key;
        current.hasExplicitRange = currentHadMinMax;
        if (current.type == UiControlType::Header || !current.key.empty()) {
            schema.controls.push_back(std::move(current));
        }
        current          = {};
        hasCurrent       = false;
        currentHadMinMax = false;
    };

    size_t pos = 0;
    while (pos < yamlText.size()) {
        size_t eol = yamlText.find('\n', pos);
        if (eol == std::string_view::npos) eol = yamlText.size();
        std::string_view rawLine = yamlText.substr(pos, eol - pos);
        pos = eol + 1;

        // Strip trailing \r and comments (# outside quotes)
        if (!rawLine.empty() && rawLine.back() == '\r') rawLine.remove_suffix(1);
        std::string lineStr;
        bool inS = false, inD = false;
        for (char c : rawLine) {
            if (c == '\'' && !inD) inS = !inS;
            else if (c == '"' && !inS) inD = !inD;
            else if (c == '#' && !inS && !inD) break;
            lineStr += c;
        }
        const std::string trimmed = trimStr(lineStr);
        if (trimmed.empty()) continue;

        size_t indent = 0;
        while (indent < lineStr.size() && lineStr[indent] == ' ') ++indent;

        if (indent == 0 && trimmed.rfind("- ", 0) != 0) {
            inOptions = false;
            const size_t colon = trimmed.find(':');
            if (colon == std::string::npos) continue;
            const std::string k = lowerStr(trimStr(std::string_view(trimmed).substr(0, colon)));
            const std::string v = unquoteYaml(trimmed.substr(colon + 1));
            if (k == "controls" || k == "fields") {
                inControls = true;
            } else {
                inControls = false;
                if (k == "tabtitle" || k == "tab_title") schema.tabTitle = v;
                else if (k == "inifile" || k == "ini_file") schema.iniFile = v;
                else if (k == "mainmenuavailable" || k == "main_menu_available") {
                    const std::string lv = lowerStr(v);
                    schema.mainMenuAvailable = (lv == "true" || lv == "1" || lv == "yes");
                }
            }
            continue;
        }

        if (!inControls) continue;

        if (trimmed.rfind("- ", 0) == 0 && !inOptions) {
            flushCurrent();
            hasCurrent = true;
            std::string rest = trimStr(std::string_view(trimmed).substr(2));
            const size_t colon = rest.find(':');
            if (colon != std::string::npos) {
                const std::string k = lowerStr(trimStr(std::string_view(rest).substr(0, colon)));
                const std::string v = unquoteYaml(rest.substr(colon + 1));
                if (k == "type") current.type = parseControlType(v);
                else if (k == "section") current.section = v;
                else if (k == "key") current.key = v;
                else if (k == "label") current.label = v;
            }
            continue;
        }

        if (!hasCurrent) continue;

        if (inOptions && trimmed.rfind("- ", 0) == 0) {
            current.options.push_back(unquoteYaml(trimmed.substr(2)));
            continue;
        }

        // Check if starting a new `- ` item at lower indent even when previous had `options`
        if (trimmed.rfind("- ", 0) == 0) {
            inOptions = false;
            flushCurrent();
            hasCurrent = true;
            std::string rest = trimStr(std::string_view(trimmed).substr(2));
            const size_t colon = rest.find(':');
            if (colon != std::string::npos) {
                const std::string k = lowerStr(trimStr(std::string_view(rest).substr(0, colon)));
                const std::string v = unquoteYaml(rest.substr(colon + 1));
                if (k == "type") current.type = parseControlType(v);
                else if (k == "section") current.section = v;
                else if (k == "key") current.key = v;
                else if (k == "label") current.label = v;
            }
            continue;
        }

        inOptions = false;
        const size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;
        const std::string k = lowerStr(trimStr(std::string_view(trimmed).substr(0, colon)));
        const std::string v = unquoteYaml(trimmed.substr(colon + 1));

        if (k == "type") current.type = parseControlType(v);
        else if (k == "section") current.section = v;
        else if (k == "key") current.key = v;
        else if (k == "label") current.label = v;
        else if (k == "tooltip") current.tooltip = v;
        else if (k == "format") current.format = v;
        else if (k == "min") { current.minValue = std::strtod(v.c_str(), nullptr); currentHadMinMax = true; }
        else if (k == "max") { current.maxValue = std::strtod(v.c_str(), nullptr); currentHadMinMax = true; }
        else if (k == "step") current.step = std::strtod(v.c_str(), nullptr);
        else if (k == "default") {
            const std::string lv = lowerStr(v);
            if (lv == "true") current.defaultValue = "1";
            else if (lv == "false") current.defaultValue = "0";
            else current.defaultValue = v;
        } else if (k == "options") {
            if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
                std::string inner = v.substr(1, v.size() - 2);
                size_t p = 0;
                while (p < inner.size()) {
                    size_t c = inner.find(',', p);
                    if (c == std::string::npos) c = inner.size();
                    std::string opt = unquoteYaml(inner.substr(p, c - p));
                    if (!opt.empty()) current.options.push_back(std::move(opt));
                    p = c + 1;
                }
            } else {
                inOptions = true;
            }
        }
    }
    flushCurrent();
    schema.valid = !schema.controls.empty();
    return schema;
}

namespace {

std::string normalizeAlnumLower(std::string_view sv)
{
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out += static_cast<char>(std::tolower(uc));
        }
    }
    return out;
}

bool equalsInsensitiveW(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

bool isIgnoredSystemIni(std::wstring_view filename)
{
    return equalsInsensitiveW(filename, L"grml.ini") ||
           equalsInsensitiveW(filename, L"grml_offsets.ini") ||
           equalsInsensitiveW(filename, L"desktop.ini") ||
           equalsInsensitiveW(filename, L"imgui.ini");
}

std::string prettifyKeyLabel(std::string_view key)
{
    std::string words;
    words.reserve(key.size() + 8);
    char prev = '\0';
    for (size_t i = 0; i < key.size(); ++i) {
        const char c = key[i];
        if (c == '_' || c == '-' || c == '.') {
            if (!words.empty() && words.back() != ' ') words += ' ';
            prev = c;
            continue;
        }
        const bool isUp   = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool prevLo = std::islower(static_cast<unsigned char>(prev)) != 0;
        const bool prevDg = std::isdigit(static_cast<unsigned char>(prev)) != 0;
        if (isUp && (prevLo || prevDg) && !words.empty() && words.back() != ' ') {
            words += ' ';
        }
        words += c;
        prev = c;
    }

    bool capNext = true;
    for (char& c : words) {
        if (c == ' ') {
            capNext = true;
        } else if (capNext) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capNext = false;
        }
    }
    return words.empty() ? std::string(key) : words;
}

bool isBannerComment(std::string_view text)
{
    bool hasAlnum = false;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            hasAlnum = true;
            break;
        }
    }
    return !hasAlnum;
}

bool isIntString(std::string_view s)
{
    if (s.empty()) return false;
    size_t i = (s.front() == '-') ? 1u : 0u;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool isFloatString(std::string_view s)
{
    if (s.empty()) return false;
    size_t i = (s.front() == '-') ? 1u : 0u;
    if (i >= s.size()) return false;
    bool seenDot = false;
    bool seenDig = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '.') {
            if (seenDot) return false;
            seenDot = true;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            seenDig = true;
        } else {
            return false;
        }
    }
    return seenDot && seenDig;
}

bool hasSubstring(std::string_view hayLower, std::string_view needleLower)
{
    return hayLower.find(needleLower) != std::string_view::npos;
}

bool endsWithAscii(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool tryParseNumberAt(std::string_view s, size_t pos, double& outVal, size_t& outNext)
{
    if (pos >= s.size()) return false;
    size_t i = pos;
    if (s[i] == '-') ++i;
    bool seenDigit = false;
    bool seenDot   = false;
    while (i < s.size()) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            seenDigit = true;
            ++i;
        } else if (s[i] == '.' && !seenDot && i + 1 < s.size() && s[i + 1] != '.' &&
                   std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            seenDot = true;
            ++i;
        } else {
            break;
        }
    }
    if (!seenDigit) return false;
    const std::string token(s.substr(pos, i - pos));
    outVal  = std::strtod(token.c_str(), nullptr);
    outNext = i;
    return std::isfinite(outVal);
}

// Recognizes explicit range hints such as "Range 0..500", "[1..60]", "(0 - 1)"
bool tryExtractRangeFromComment(std::string_view comment, double& outMin, double& outMax)
{
    const std::string low = lowerStr(comment);
    for (size_t i = 0; i < low.size(); ++i) {
        const bool hasNumStart = std::isdigit(static_cast<unsigned char>(low[i])) ||
                                 (low[i] == '-' && i + 1 < low.size() && std::isdigit(static_cast<unsigned char>(low[i + 1])));
        if (!hasNumStart) continue;
        if (i > 0 && std::isalnum(static_cast<unsigned char>(low[i - 1]))) continue;

        double a = 0.0;
        size_t afterA = 0;
        if (!tryParseNumberAt(low, i, a, afterA)) continue;

        size_t sep = afterA;
        while (sep < low.size() && low[sep] == ' ') ++sep;
        bool matchedSep = false;
        if (sep + 1 < low.size() && low[sep] == '.' && low[sep + 1] == '.') {
            sep += 2;
            matchedSep = true;
        } else if (sep < low.size() && low[sep] == '-') {
            // Only treat `A - B` as a range if `range` keyword appeared or enclosed in parens/brackets
            const bool paren = (i > 0 && (low[i - 1] == '(' || low[i - 1] == '[' || low[i - 1] == ' '));
            const bool rangeWord = (i >= 6 && low.substr(0, i).find("range") != std::string::npos);
            if (paren || rangeWord) {
                ++sep;
                matchedSep = true;
            }
        }
        if (!matchedSep) continue;

        while (sep < low.size() && low[sep] == ' ') ++sep;
        double b = 0.0;
        size_t afterB = 0;
        if (!tryParseNumberAt(low, sep, b, afterB)) continue;
        if (b > a) {
            outMin = a;
            outMax = b;
            return true;
        }
    }
    return false;
}

bool looksLikeQuantityOrMultiModeInt(std::string_view keyLower, std::string_view commentLower)
{
    static constexpr const char* kIntKeySuffixes[] = {
        "_m", "_km", "_t", "_kg", "_s", "_ms", "_sec", "_min", "_pct", "_percent",
        "_count", "_max", "_min", "_limit", "_radius", "_distance", "_delay",
        "_interval", "_seconds", "_minutes", "_mode", "_index", "_currency", "_slots", "_points"
    };
    for (const char* suf : kIntKeySuffixes) {
        if (endsWithAscii(keyLower, suf)) return true;
    }
    if (keyLower == "mode" || keyLower == "distance" || keyLower == "radius" ||
        keyLower == "limit" || keyLower == "count" || keyLower == "interval" || keyLower == "port") {
        return true;
    }

    static constexpr const char* kBoolKeyPrefixes[] = {
        "enable", "is_", "use_", "allow_", "disable_", "show_", "hide_", "clear_", "deterministic_", "debug"
    };
    if (keyLower == "enabled") return false;
    for (const char* pre : kBoolKeyPrefixes) {
        if (keyLower.rfind(pre, 0) == 0) {
            // Explicit multi-mode options in comment (2 = / 3 =) override boolean prefix
            return hasSubstring(commentLower, "2 =") || hasSubstring(commentLower, "2=") ||
                   hasSubstring(commentLower, "range ");
        }
    }

    return hasSubstring(commentLower, "2 =") || hasSubstring(commentLower, "2=") ||
           hasSubstring(commentLower, "3 =") || hasSubstring(commentLower, "3=") ||
           hasSubstring(commentLower, "range ") || hasSubstring(commentLower, "(m)") ||
           hasSubstring(commentLower, "(t)") || hasSubstring(commentLower, "metres") ||
           hasSubstring(commentLower, "meters") || hasSubstring(commentLower, "seconds") ||
           hasSubstring(commentLower, "percent") || hasSubstring(commentLower, "..");
}

UiControlDesc makeAutoIniControl(std::string_view section,
                                 std::string_view key,
                                 std::string_view value,
                                 std::string_view tooltip)
{
    UiControlDesc c;
    c.section          = std::string(section);
    c.key              = std::string(key);
    c.label            = prettifyKeyLabel(key);
    c.tooltip          = std::string(tooltip);
    c.hasExplicitRange = false;

    const std::string valTrim = trimStr(value);
    const std::string valLow  = lowerStr(valTrim);
    const std::string keyLow  = lowerStr(key);
    const std::string tipLow  = lowerStr(tooltip);

    if (valLow == "true" || valLow == "false" || valLow == "yes" ||
        valLow == "no" || valLow == "on" || valLow == "off") {
        c.type         = UiControlType::Bool;
        c.defaultValue = (valLow == "true" || valLow == "yes" || valLow == "on") ? "1" : "0";
        return c;
    }

    if ((valTrim == "0" || valTrim == "1") && !looksLikeQuantityOrMultiModeInt(keyLow, tipLow)) {
        c.type         = UiControlType::Bool;
        c.defaultValue = valTrim;
        return c;
    }

    if (isIntString(valTrim)) {
        c.type         = UiControlType::Int;
        c.defaultValue = valTrim;
        const long long n = std::strtoll(valTrim.c_str(), nullptr, 10);

        double rMin = 0.0, rMax = 0.0;
        if (tryExtractRangeFromComment(tooltip, rMin, rMax) &&
            n >= static_cast<long long>(rMin) && n <= static_cast<long long>(rMax)) {
            c.minValue         = std::floor(rMin);
            c.maxValue         = std::ceil(rMax);
            c.hasExplicitRange = true;
        } else {
            c.minValue = (n < 0) ? static_cast<double>((std::min<long long>)(n * 2, -100)) : 0.0;
            const long long scaled = (n >= 0 ? n : -n) * 4;
            c.maxValue = static_cast<double>((std::max<long long>)(100, scaled));
        }
        c.step = 1.0;
        return c;
    }

    if (isFloatString(valTrim)) {
        c.type         = UiControlType::Float;
        c.defaultValue = valTrim;
        const double f = std::strtod(valTrim.c_str(), nullptr);

        double rMin = 0.0, rMax = 0.0;
        if (tryExtractRangeFromComment(tooltip, rMin, rMax) && f >= rMin && f <= rMax) {
            c.minValue         = rMin;
            c.maxValue         = rMax;
            c.hasExplicitRange = true;
        } else {
            c.minValue = (f < 0.0) ? f * 2.0 : 0.0;
            c.maxValue = (std::max)(10.0, std::ceil(std::fabs(f) * 4.0));
        }
        c.step = (c.maxValue - c.minValue <= 2.5) ? 0.05 : 0.1;
        return c;
    }

    c.type         = UiControlType::String;
    c.defaultValue = valTrim;
    return c;
}

} // namespace

std::filesystem::path findModIniPath(const std::filesystem::path& dllPath,
                                     const std::filesystem::path& manifestPath,
                                     const UiSchema* schema)
{
    std::error_code ec;
    const auto dir = dllPath.parent_path();

    // 1. Explicit schema iniFile hint
    if (schema && schema->valid && !schema->iniFile.empty()) {
        std::filesystem::path cand = utf8ToWide(schema->iniFile);
        if (cand.is_relative()) cand = dir / cand;
        return cand;
    }

    // 2. Explicit manifest "iniFile" / "ini_file" / "ini" / "config" field
    if (!manifestPath.empty() && std::filesystem::is_regular_file(manifestPath, ec)) {
        std::string raw;
        if (readFileBytesW(manifestPath, raw)) {
            if (auto j = parseJson(raw)) {
                const JsonValue* iniVal = j->get("iniFile");
                if (!iniVal) iniVal = j->get("ini_file");
                if (!iniVal) iniVal = j->get("ini");
                if (!iniVal) iniVal = j->get("config");
                if (iniVal && !iniVal->asString().empty()) {
                    std::filesystem::path cand = utf8ToWide(iniVal->asString());
                    if (cand.is_relative()) cand = dir / cand;
                    return cand;
                }
            }
        }
    }

    // 3. Exact <DllStem>.ini next to DLL or in config/<DllStem>.ini
    const auto stemIni = dir / (dllPath.stem().wstring() + L".ini");
    if (std::filesystem::is_regular_file(stemIni, ec)) return stemIni;
    const auto cfgStemIni = dir / L"config" / (dllPath.stem().wstring() + L".ini");
    if (std::filesystem::is_regular_file(cfgStemIni, ec)) return cfgStemIni;

    // 4. Scan *.ini inside `dir` (ignoring GRML/system INIs)
    std::vector<std::filesystem::path> dirInis;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto ext = entry.path().extension().wstring();
        if (!equalsInsensitiveW(ext, L".ini")) continue;
        if (isIgnoredSystemIni(entry.path().filename().wstring())) continue;
        dirInis.push_back(entry.path());
    }

    // Unambiguous single .ini in this mod folder (e.g. CrossroadsManager/roadworks.ini or pack faixas.ini)
    if (dirInis.size() == 1) {
        return dirInis.front();
    }

    // Multiple INIs in one folder (e.g. mod pack): match only by normalized stem against DLL/manifest stem
    if (dirInis.size() > 1) {
        const std::string dllNorm = normalizeAlnumLower(wideToUtf8(dllPath.stem().wstring()));
        const std::string manNorm = manifestPath.empty()
                                        ? std::string{}
                                        : normalizeAlnumLower(wideToUtf8(manifestPath.stem().stem().wstring()));
        for (const auto& p : dirInis) {
            const std::string iniNorm = normalizeAlnumLower(wideToUtf8(p.stem().wstring()));
            if (!iniNorm.empty() && (iniNorm == dllNorm || (!manNorm.empty() && iniNorm == manNorm))) {
                return p;
            }
        }
    }

    return stemIni;
}

UiSchema buildUiSchemaFromIniFile(const std::filesystem::path& iniPath,
                                  const std::string& fallbackTabTitle)
{
    UiSchema schema;
    std::error_code ec;
    if (iniPath.empty() || !std::filesystem::is_regular_file(iniPath, ec)) {
        return schema;
    }

    std::string raw;
    if (!readFileBytesW(iniPath, raw) || raw.empty()) {
        return schema;
    }

    std::string_view view(raw);
    if (view.size() >= 3 &&
        static_cast<unsigned char>(view[0]) == 0xEF &&
        static_cast<unsigned char>(view[1]) == 0xBB &&
        static_cast<unsigned char>(view[2]) == 0xBF) {
        view.remove_prefix(3);
    }

    schema.autoGenerated     = true;
    schema.sourceFile        = iniPath;
    schema.iniFile           = wideToUtf8(iniPath.filename().wstring());
    schema.tabTitle          = fallbackTabTitle.empty()
                                   ? wideToUtf8(iniPath.stem().wstring())
                                   : fallbackTabTitle;
    schema.mainMenuAvailable = true;

    std::string currentSection = "Settings";
    bool emittedCurrentHeader  = false;
    std::string pendingComment;
    size_t fieldCount = 0;

    auto ensureHeader = [&]() {
        if (emittedCurrentHeader) return;
        UiControlDesc hdr;
        hdr.type    = UiControlType::Header;
        hdr.section = currentSection;
        hdr.label   = "[" + currentSection + "]";
        schema.controls.push_back(std::move(hdr));
        emittedCurrentHeader = true;
    };

    size_t pos = 0;
    while (pos < view.size()) {
        size_t eol = view.find('\n', pos);
        if (eol == std::string_view::npos) eol = view.size();
        std::string line = trimStr(view.substr(pos, eol - pos));
        pos = eol + 1;

        if (line.empty()) {
            pendingComment.clear();
            continue;
        }
        if (line.front() == ';' || line.front() == '#') {
            std::string ctext = trimStr(std::string_view(line).substr(1));
            if (!ctext.empty() && !isBannerComment(ctext)) {
                if (!pendingComment.empty()) pendingComment += '\n';
                pendingComment += ctext;
            }
            continue;
        }
        if (line.front() == '[' && line.back() == ']' && line.size() >= 2) {
            currentSection       = trimStr(std::string_view(line).substr(1, line.size() - 2));
            emittedCurrentHeader = false;
            pendingComment.clear();
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            pendingComment.clear();
            continue;
        }
        const std::string key = trimStr(std::string_view(line).substr(0, eq));
        const std::string val = trimStr(std::string_view(line).substr(eq + 1));
        if (key.empty()) {
            pendingComment.clear();
            continue;
        }

        if (pendingComment.size() > 420) {
            pendingComment.resize(417);
            pendingComment += "...";
        }

        ensureHeader();
        schema.controls.push_back(makeAutoIniControl(currentSection, key, val, pendingComment));
        ++fieldCount;
        pendingComment.clear();
    }

    schema.valid = (fieldCount > 0);
    return schema;
}

UiSchema discoverModUiSchema(const std::filesystem::path& dllPath,
                             const std::filesystem::path& manifestPath,
                             const std::string& fallbackTabTitle)
{
    const auto dir  = dllPath.parent_path();
    const auto stem = dllPath.stem().wstring();
    std::error_code ec;

    auto finalizeExplicit = [&](UiSchema s) {
        s.autoGenerated = false;
        if (s.iniFile.empty()) {
            const auto iniP = findModIniPath(dllPath, manifestPath, nullptr);
            if (!iniP.empty()) {
                s.iniFile = wideToUtf8(iniP.filename().wstring());
            }
        }
        return s;
    };

    // 1. Standalone sidecar `<Mod>.ui.json`
    {
        const auto p = dir / (stem + L".ui.json");
        std::string raw;
        if (std::filesystem::is_regular_file(p, ec) && readFileBytesW(p, raw)) {
            if (auto j = parseJson(raw)) {
                auto s = parseUiSchemaFromJson(*j, p);
                if (s.valid) return finalizeExplicit(std::move(s));
            }
        }
    }

    // 2. Standalone sidecar `<Mod>.ui.yaml` or `.ui.yml`
    for (const wchar_t* ext : {L".ui.yaml", L".ui.yml"}) {
        const auto p = dir / (stem + ext);
        std::string raw;
        if (std::filesystem::is_regular_file(p, ec) && readFileBytesW(p, raw)) {
            auto s = parseUiSchemaFromYaml(raw, p);
            if (s.valid) return finalizeExplicit(std::move(s));
        }
    }

    // 3. Inline `"settingsUi"` inside `*.grml.json` / `*.rml.json`
    if (!manifestPath.empty() && std::filesystem::is_regular_file(manifestPath, ec)) {
        std::string raw;
        if (readFileBytesW(manifestPath, raw)) {
            if (auto j = parseJson(raw)) {
                const JsonValue* uiObj = j->get("settingsUi");
                if (!uiObj) uiObj = j->get("settings_ui");
                if (uiObj) {
                    auto s = parseUiSchemaFromJson(*uiObj, manifestPath);
                    if (s.valid) return finalizeExplicit(std::move(s));
                }
            }
        }
    }

    // 4. Fallback: Scan existing .ini file and auto-build UI controls
    const std::filesystem::path iniPath = findModIniPath(dllPath, manifestPath, nullptr);
    if (!iniPath.empty() && std::filesystem::is_regular_file(iniPath, ec)) {
        UiSchema autoSchema = buildUiSchemaFromIniFile(
            iniPath, fallbackTabTitle.empty() ? wideToUtf8(stem) : fallbackTabTitle);
        if (autoSchema.valid) {
            return autoSchema;
        }
    }

    return {};
}

std::string serializeUiSchemaJson(const UiSchema& schema)
{
    if (!schema.valid) return "null";
    std::string out = "{";
    out += "\"autoGenerated\":" + std::string(schema.autoGenerated ? "true" : "false") + ",";
    out += "\"tabTitle\":\"" + jsonEscape(schema.tabTitle) + "\",";
    out += "\"iniFile\":\"" + jsonEscape(schema.iniFile) + "\",";
    out += "\"mainMenuAvailable\":" + std::string(schema.mainMenuAvailable ? "true" : "false") + ",";
    out += "\"sourceFile\":\"" + jsonEscape(wideToUtf8(schema.sourceFile.wstring())) + "\",";
    out += "\"controls\":[";
    for (size_t i = 0; i < schema.controls.size(); ++i) {
        if (i > 0) out += ',';
        const auto& c = schema.controls[i];
        char numBuf[160]{};
        std::snprintf(numBuf, sizeof(numBuf), "\"min\":%g,\"max\":%g,\"step\":%g,\"hasExplicitRange\":%s,",
                      c.minValue, c.maxValue, c.step, c.hasExplicitRange ? "true" : "false");
        out += "{";
        out += "\"type\":\"" + std::string(controlTypeName(c.type)) + "\",";
        out += "\"section\":\"" + jsonEscape(c.section) + "\",";
        out += "\"key\":\"" + jsonEscape(c.key) + "\",";
        out += "\"label\":\"" + jsonEscape(c.label) + "\",";
        out += "\"tooltip\":\"" + jsonEscape(c.tooltip) + "\",";
        out += "\"format\":\"" + jsonEscape(c.format) + "\",";
        out += numBuf;
        out += "\"default\":\"" + jsonEscape(c.defaultValue) + "\",";
        out += "\"options\":[";
        for (size_t j = 0; j < c.options.size(); ++j) {
            if (j > 0) out += ',';
            out += "\"" + jsonEscape(c.options[j]) + "\"";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

} // namespace grml
