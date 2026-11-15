#include <opm/mcp/frame_tap.h>

#include <cstring>

#include <opm/media/decoder.h>

namespace opm::mcp {

void FrameTap::offer(const std::string& id, const media::VideoFrame& f) {
    offer_rgba(id, f.data, f.width, f.height, f.stride);
}

void FrameTap::offer_rgba(const std::string& id, const uint8_t* rgba, int w, int h,
                          int stride) {
    if (!rgba || w <= 0 || h <= 0 || stride < w * 4) return;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lk(mu_);
        Slot& s = slots_[id];
        if (s.waiting == 0 && s.frame.w > 0 &&
            now - s.last < std::chrono::milliseconds(250))
            return;
        s.frame.w = w;
        s.frame.h = h;
        s.frame.rgba.resize(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(s.frame.rgba.data() + size_t(y) * w * 4, rgba + size_t(y) * stride,
                        size_t(w) * 4);
        s.last = now;
        ++s.serial;
    }
    cv_.notify_all();
}

bool FrameTap::get(const std::string& id, Frame* out, int wait_ms) {
    std::unique_lock lk(mu_);
    auto it = slots_.find(id);
    if (it == slots_.end()) return false;
    unsigned seen = it->second.serial;
    ++it->second.waiting;
    cv_.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] {
        auto i = slots_.find(id);
        return i == slots_.end() || i->second.serial != seen;
    });
    it = slots_.find(id);
    if (it == slots_.end()) return false;
    --it->second.waiting;
    if (it->second.frame.w <= 0) return false;
    *out = it->second.frame;
    return true;
}

void FrameTap::size(const std::string& id, int* w, int* h) {
    std::lock_guard lk(mu_);
    auto it = slots_.find(id);
    *w = it == slots_.end() ? 0 : it->second.frame.w;
    *h = it == slots_.end() ? 0 : it->second.frame.h;
}

void FrameTap::drop(const std::string& id) {
    {
        std::lock_guard lk(mu_);
        slots_.erase(id);
    }
    cv_.notify_all();
}

} // namespace opm::mcp
