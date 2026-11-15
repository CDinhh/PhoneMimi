#include <opm/mcp/mcp_server.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace opm::mcp {

namespace {

constexpr size_t kMaxBody = 1 << 20;

const char* reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default: return "Error";
    }
}

bool send_all(SOCKET s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = send(s, data.data() + off, int(std::min<size_t>(data.size() - off, 1 << 16)), 0);
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

bool send_response(SOCKET s, const HttpResponse& r, bool keep_alive) {
    std::string h = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    if (!r.content_type.empty()) h += "Content-Type: " + r.content_type + "\r\n";
    h += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    h += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    return send_all(s, h + r.body);
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

McpServer::McpServer(ToolTable tools) : tools_(std::move(tools)) {}

McpServer::~McpServer() { stop(); }

std::string McpServer::start(int port, const std::string& token) {
    stop();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return "Could not create a socket.";
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 8) != 0) {
        closesocket(s);
        return "Port " + std::to_string(port) + " is in use. Pick another one or close the program using it.";
    }
    int len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    token_ = token;
    listen_ = uintptr_t(s);
    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop_(); });
    std::cout << "[MCP] listening on 127.0.0.1:" << port_ << "\n";
    return "";
}

void McpServer::stop() {
    if (!running_.exchange(false)) return;
    closesocket(SOCKET(listen_));
    listen_ = ~uintptr_t(0);
    if (accept_thread_.joinable()) accept_thread_.join();
    std::unique_lock lk(mu_);
    for (auto c : clients_) shutdown(SOCKET(c), SD_BOTH);
    // The app cancels adb and tool loops before this on exit, so workers end
    // fast. shortcut: a worker still busy after 10 s is left behind; only a
    // tool that ignores Host::closing() could hit that.
    idle_cv_.wait_for(lk, std::chrono::seconds(10), [this] { return active_ == 0; });
    std::cout << "[MCP] stopped\n";
}

void McpServer::accept_loop_() {
    while (running_) {
        SOCKET c = accept(SOCKET(listen_), nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!running_) break;
            Sleep(50);
            continue;
        }
        DWORD to = 30000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        {
            std::lock_guard lk(mu_);
            clients_.insert(uintptr_t(c));
            ++active_;
        }
        std::thread([this, c] {
            serve_(uintptr_t(c));
            {
                // Forget the handle before closing it: a reused handle
                // must never be shut down by stop().
                std::lock_guard lk(mu_);
                clients_.erase(uintptr_t(c));
            }
            closesocket(c);
            std::lock_guard lk(mu_);
            --active_;
            idle_cv_.notify_all();
        }).detach();
    }
}

void McpServer::serve_(uintptr_t sock) {
    SOCKET s = SOCKET(sock);
    std::string buf;
    char tmp[8192];
    while (running_) {
        size_t hdr_end;
        while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
            if (buf.size() > 64 * 1024) {
                send_response(s, {400, "text/plain", "Headers too large"}, false);
                return;
            }
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) return;
            buf.append(tmp, size_t(n));
        }
        HttpRequest req;
        size_t line_end = buf.find("\r\n");
        {
            std::string line = buf.substr(0, line_end);
            size_t a = line.find(' '), b = line.find(' ', a + 1);
            if (a == std::string::npos || b == std::string::npos) {
                send_response(s, {400, "text/plain", "Bad request line"}, false);
                return;
            }
            req.method = line.substr(0, a);
            req.path = line.substr(a + 1, b - a - 1);
            if (auto q = req.path.find('?'); q != std::string::npos) req.path.resize(q);
        }
        size_t pos = line_end + 2;
        while (pos < hdr_end) {
            size_t e = buf.find("\r\n", pos);
            std::string h = buf.substr(pos, e - pos);
            pos = e + 2;
            size_t colon = h.find(':');
            if (colon == std::string::npos) continue;
            req.headers[lower(trim(h.substr(0, colon)))] = trim(h.substr(colon + 1));
        }
        size_t body_len = 0;
        if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
            try { body_len = std::stoull(it->second); } catch (...) { body_len = kMaxBody + 1; }
        }
        if (body_len > kMaxBody) {
            send_response(s, {413, "text/plain", "Body too large"}, false);
            return;
        }
        buf.erase(0, hdr_end + 4);
        while (buf.size() < body_len) {
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) return;
            buf.append(tmp, size_t(n));
        }
        req.body = buf.substr(0, body_len);
        buf.erase(0, body_len);

        bool keep = lower(req.headers["connection"]) != "close";
        HttpResponse r = handle_http(req, token_, port_, tools_);
        if (!send_response(s, r, keep) || !keep) return;
    }
}

} // namespace opm::mcp
