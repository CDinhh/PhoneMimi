#pragma once

// Loopback HTTP server for MCP: 127.0.0.1 only, one thread per connection.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <opm/mcp/mcp_protocol.h>

namespace opm::mcp {

class McpServer {
public:
    explicit McpServer(ToolTable tools);
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // "" on success, otherwise why it could not listen. port 0 picks one.
    std::string start(int port, const std::string& token);
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return port_; }

private:
    void accept_loop_();
    void serve_(uintptr_t sock);

    ToolTable tools_;
    std::string token_;
    int port_ = 0;
    std::atomic<bool> running_{false};
    uintptr_t listen_ = ~uintptr_t(0);
    std::thread accept_thread_;
    std::mutex mu_;
    std::condition_variable idle_cv_;
    std::set<uintptr_t> clients_;
    int active_ = 0;
};

} // namespace opm::mcp
