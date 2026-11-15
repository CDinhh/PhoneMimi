#pragma once

// Keeps a recent copy of each device's video frame for MCP screenshots.
// offer() runs on decoder threads for every frame, so it only copies at
// most every 250 ms, or right away when a get() is waiting for a fresh one.

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>

#include <opm/mcp/mcp_host.h>

namespace opm::media { struct VideoFrame; }

namespace opm::mcp {

class FrameTap {
public:
    void offer(const std::string& id, const media::VideoFrame& f);
    void offer_rgba(const std::string& id, const uint8_t* rgba, int w, int h, int stride);
    // Waits up to wait_ms for a fresh frame, then falls back to the last copy.
    bool get(const std::string& id, Frame* out, int wait_ms);
    void drop(const std::string& id);

private:
    struct Slot {
        Frame frame;
        std::chrono::steady_clock::time_point last{};
        int waiting = 0;
        unsigned serial = 0;
    };
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Slot> slots_;
};

} // namespace opm::mcp
