#pragma once

// MCP (Model Context Protocol) over Streamable HTTP, the pure part: HTTP
// routing, auth, and JSON-RPC dispatch. No sockets here, so
// `--mcp-self-test` can drive it directly.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace opm::mcp {

struct HttpRequest {
    std::string method, path;
    std::map<std::string, std::string> headers;   // lower-case keys
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type;
    std::string body;
};

struct ToolResult {
    nlohmann::json content = nlohmann::json::array();
    bool is_error = false;
};
ToolResult text_result(const std::string& text, bool is_error = false);

struct Tool {
    std::string name, description;
    nlohmann::json input_schema;
    std::function<ToolResult(const nlohmann::json& args)> run;
};
using ToolTable = std::vector<Tool>;

// Returns the JSON-RPC response text, or "" for a notification.
std::string handle_jsonrpc(const std::string& body, const ToolTable& tools);
HttpResponse handle_http(const HttpRequest& req, const std::string& token,
                         int port, const ToolTable& tools);

std::string make_token();                                    // 32 hex chars
std::string setup_command(int port, const std::string& token);

int run_self_test();                                         // 0 pass, 2 fail

} // namespace opm::mcp
