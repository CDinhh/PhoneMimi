#include <opm/mcp/mcp_protocol.h>

#include <cstdio>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <random>
#endif

#ifndef OPM_VERSION_MAJOR
#define OPM_VERSION_MAJOR 0
#define OPM_VERSION_MINOR 0
#define OPM_VERSION_PATCH 0
#endif

using nlohmann::json;

namespace opm::mcp {

namespace {

const char* const kInstructions =
    "PhoneMimi mirrors phones to this PC. Coordinates in every tool are "
    "screenshot pixels (long side at most 1280). Prefer ui_tree and taps by "
    "text/id over raw coordinates; take a screenshot to check a result; use "
    "wait_for after navigation instead of sleeping. iPhones (AirPlay) are "
    "view-only: screenshot and ocr_screen only.";

json rpc_error(const json& id, int code, const std::string& msg) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}

json rpc_result(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

std::string version_string() {
    char b[32];
    std::snprintf(b, sizeof(b), "%d.%d.%d", OPM_VERSION_MAJOR, OPM_VERSION_MINOR,
                  OPM_VERSION_PATCH);
    return b;
}

json dispatch(const json& msg, const ToolTable& tools) {
    json id = msg.contains("id") ? msg["id"] : json();
    if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string())
        return rpc_error(id, -32600, "Invalid request");
    const std::string method = msg["method"];
    const json params = msg.contains("params") && msg["params"].is_object() ? msg["params"]
                                                                          : json::object();

    if (method == "initialize") {
        std::string v = params.value("protocolVersion", "");
        if (v != "2025-03-26" && v != "2025-06-18") v = "2025-06-18";
        return rpc_result(id, {
            {"protocolVersion", v},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "phonemimi"}, {"version", version_string()}}},
            {"instructions", kInstructions},
        });
    }
    if (method == "ping") return rpc_result(id, json::object());
    if (method == "tools/list") {
        json list = json::array();
        for (auto& t : tools)
            list.push_back({{"name", t.name}, {"description", t.description},
                            {"inputSchema", t.input_schema}});
        return rpc_result(id, {{"tools", list}});
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        for (auto& t : tools) {
            if (t.name != name) continue;
            ToolResult r;
            try {
                r = t.run(params.value("arguments", json::object()));
            } catch (const std::exception& e) {
                r = text_result(std::string("Tool failed: ") + e.what(), true);
            } catch (...) {
                r = text_result("Tool failed", true);
            }
            return rpc_result(id, {{"content", r.content}, {"isError", r.is_error}});
        }
        return rpc_error(id, -32602, "Unknown tool: " + name);
    }
    return rpc_error(id, -32601, "Method not found: " + method);
}

bool origin_ok(const std::string& origin, int port) {
    if (origin.empty()) return true;
    const std::string p = ":" + std::to_string(port);
    for (const char* host : {"http://127.0.0.1", "http://localhost"})
        if (origin == host || origin == std::string(host) + p) return true;
    return false;
}

} // namespace

ToolResult text_result(const std::string& text, bool is_error) {
    ToolResult r;
    r.content.push_back({{"type", "text"}, {"text", text}});
    r.is_error = is_error;
    return r;
}

std::string handle_jsonrpc(const std::string& body, const ToolTable& tools) {
    json msg;
    try {
        msg = json::parse(body);
    } catch (...) {
        return rpc_error(nullptr, -32700, "Parse error").dump();
    }
    if (msg.is_array()) return rpc_error(nullptr, -32600, "Batches are not supported").dump();
    // A message without an id is a notification (or a response): no reply.
    if (msg.is_object() && !msg.contains("id")) return "";
    // Invalid UTF-8 in tool output must not throw out of dump().
    return dispatch(msg, tools).dump(-1, ' ', false, json::error_handler_t::replace);
}

HttpResponse handle_http(const HttpRequest& req, const std::string& token,
                         int port, const ToolTable& tools) {
    auto header = [&](const char* k) {
        auto it = req.headers.find(k);
        return it == req.headers.end() ? std::string() : it->second;
    };
    if (req.path != "/mcp") return {404, "text/plain", "Not found"};
    if (!origin_ok(header("origin"), port)) return {403, "text/plain", "Forbidden origin"};
    if (token.empty() || header("authorization") != "Bearer " + token)
        return {401, "text/plain", "Unauthorized"};
    if (req.method != "POST") return {405, "text/plain", "Method not allowed"};
    std::string out = handle_jsonrpc(req.body, tools);
    if (out.empty()) return {202, "", ""};
    return {200, "application/json", out};
}

std::string make_token() {
    unsigned char raw[16] = {};
#ifdef _WIN32
    BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    std::random_device rd;
    for (auto& b : raw) b = static_cast<unsigned char>(rd());
#endif
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (unsigned char b : raw) { s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}

std::string setup_command(int port, const std::string& token) {
    return "claude mcp add --transport http phonemimi http://127.0.0.1:" +
           std::to_string(port) + "/mcp --header \"Authorization: Bearer " + token + "\"";
}

} // namespace opm::mcp
