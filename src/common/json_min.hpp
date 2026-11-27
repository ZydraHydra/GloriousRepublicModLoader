#pragma once
// ============================================================================
// json_min.hpp — Zero-dependency C++20 recursive-descent JSON parser
// ============================================================================
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace grml {

struct JsonValue;
using JsonArray  = std::vector<JsonValue>;
using JsonObject = std::unordered_map<std::string, JsonValue>;

struct JsonValue {
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
    Storage data{nullptr};

    bool isNull() const noexcept   { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool() const noexcept   { return std::holds_alternative<bool>(data); }
    bool isNumber() const noexcept { return std::holds_alternative<double>(data); }
    bool isString() const noexcept { return std::holds_alternative<std::string>(data); }
    bool isArray() const noexcept  { return std::holds_alternative<JsonArray>(data); }
    bool isObject() const noexcept { return std::holds_alternative<JsonObject>(data); }

    const JsonValue* get(std::string_view key) const noexcept;
    std::string      asString(std::string_view fallback = "") const;
    double           asNumber(double fallback = 0.0) const noexcept;
    int              asInt(int fallback = 0) const noexcept;
    bool             asBool(bool fallback = false) const noexcept;
    std::vector<std::string> asStringArray() const;
};

std::optional<JsonValue> parseJson(std::string_view input);

} // namespace grml
