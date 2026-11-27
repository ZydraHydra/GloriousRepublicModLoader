#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace grml::mcp {

struct HttpMcpResponse {
    int         statusCode{200};
    std::string statusText{"OK"};
    std::string contentType{"application/json; charset=utf-8"};
    std::string sessionId;
    std::string body;
};

HttpMcpResponse handlePost(const std::filesystem::path& loaderDir,
                           std::string_view sessionIdHeader,
                           std::string_view body);
bool validateSession(std::string_view sessionId);
void closeSession(std::string_view sessionId);

} // namespace grml::mcp
