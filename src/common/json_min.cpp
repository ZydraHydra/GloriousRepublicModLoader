#include "common/json_min.hpp"

#include <cstdlib>

namespace grml {

const JsonValue* JsonValue::get(std::string_view key) const noexcept
{
    if (!isObject()) {
        return nullptr;
    }
    const auto& obj = std::get<JsonObject>(data);
    auto it = obj.find(std::string(key));
    return (it != obj.end()) ? &it->second : nullptr;
}

std::string JsonValue::asString(std::string_view fallback) const
{
    return isString() ? std::get<std::string>(data) : std::string(fallback);
}

double JsonValue::asNumber(double fallback) const noexcept
{
    return isNumber() ? std::get<double>(data) : fallback;
}

int JsonValue::asInt(int fallback) const noexcept
{
    return isNumber() ? static_cast<int>(std::get<double>(data)) : fallback;
}

bool JsonValue::asBool(bool fallback) const noexcept
{
    return isBool() ? std::get<bool>(data) : fallback;
}

std::vector<std::string> JsonValue::asStringArray() const
{
    std::vector<std::string> out;
    if (!isArray()) {
        return out;
    }
    for (const auto& item : std::get<JsonArray>(data)) {
        if (item.isString()) {
            out.push_back(item.asString());
        }
    }
    return out;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view s) : m_s(s) {}

    std::optional<JsonValue> parseRoot()
    {
        // Strip UTF-8 BOM if present
        if (m_s.size() >= 3 &&
            static_cast<unsigned char>(m_s[0]) == 0xEF &&
            static_cast<unsigned char>(m_s[1]) == 0xBB &&
            static_cast<unsigned char>(m_s[2]) == 0xBF) {
            m_pos += 3;
        }
        auto v = parseValue(0);
        if (!v) {
            return std::nullopt;
        }
        skipWs();
        return v;
    }

private:
    std::string_view m_s;
    size_t           m_pos{0};

    void skipWs() noexcept
    {
        while (m_pos < m_s.size()) {
            const char c = m_s[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    std::optional<JsonValue> parseValue(int depth)
    {
        if (depth > 64) {
            return std::nullopt;
        }
        skipWs();
        if (m_pos >= m_s.size()) {
            return std::nullopt;
        }
        const char c = m_s[m_pos];
        if (c == '{') return parseObject(depth + 1);
        if (c == '[') return parseArray(depth + 1);
        if (c == '"') {
            auto str = parseString();
            if (!str) return std::nullopt;
            return JsonValue{std::move(*str)};
        }
        if (c == 't' && m_s.substr(m_pos, 4) == "true") {
            m_pos += 4;
            return JsonValue{true};
        }
        if (c == 'f' && m_s.substr(m_pos, 5) == "false") {
            m_pos += 5;
            return JsonValue{false};
        }
        if (c == 'n' && m_s.substr(m_pos, 4) == "null") {
            m_pos += 4;
            return JsonValue{nullptr};
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parseNumber();
        }
        return std::nullopt;
    }

    std::optional<std::string> parseString()
    {
        if (m_pos >= m_s.size() || m_s[m_pos] != '"') {
            return std::nullopt;
        }
        ++m_pos;
        std::string out;
        while (m_pos < m_s.size()) {
            const char c = m_s[m_pos++];
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (m_pos >= m_s.size()) return std::nullopt;
                const char esc = m_s[m_pos++];
                switch (esc) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (m_pos + 4 > m_s.size()) return std::nullopt;
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = m_s[m_pos++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return std::nullopt;
                    }
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default:
                    return std::nullopt;
                }
            } else {
                out += c;
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseNumber()
    {
        const size_t start = m_pos;
        if (m_s[m_pos] == '-') ++m_pos;
        while (m_pos < m_s.size()) {
            const char c = m_s[m_pos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++m_pos;
            } else {
                break;
            }
        }
        const std::string numStr(m_s.substr(start, m_pos - start));
        char* endPtr = nullptr;
        const double v = std::strtod(numStr.c_str(), &endPtr);
        if (!endPtr || endPtr == numStr.c_str()) {
            return std::nullopt;
        }
        return JsonValue{v};
    }

    std::optional<JsonValue> parseArray(int depth)
    {
        ++m_pos; // '['
        JsonArray arr;
        skipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == ']') {
            ++m_pos;
            return JsonValue{std::move(arr)};
        }
        while (m_pos < m_s.size()) {
            auto val = parseValue(depth);
            if (!val) return std::nullopt;
            arr.push_back(std::move(*val));
            skipWs();
            if (m_pos < m_s.size() && m_s[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_s.size() && m_s[m_pos] == ']') {
                ++m_pos;
                return JsonValue{std::move(arr)};
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseObject(int depth)
    {
        ++m_pos; // '{'
        JsonObject obj;
        skipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == '}') {
            ++m_pos;
            return JsonValue{std::move(obj)};
        }
        while (m_pos < m_s.size()) {
            skipWs();
            auto key = parseString();
            if (!key) return std::nullopt;
            skipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != ':') return std::nullopt;
            ++m_pos;
            auto val = parseValue(depth);
            if (!val) return std::nullopt;
            obj.insert_or_assign(std::move(*key), std::move(*val));
            skipWs();
            if (m_pos < m_s.size() && m_s[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_s.size() && m_s[m_pos] == '}') {
                ++m_pos;
                return JsonValue{std::move(obj)};
            }
            return std::nullopt;
        }
        return std::nullopt;
    }
};

} // namespace

std::optional<JsonValue> parseJson(std::string_view input)
{
    Parser p(input);
    return p.parseRoot();
}

} // namespace grml
