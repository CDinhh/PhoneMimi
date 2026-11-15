// App as the MCP host: what the Claude Code tools can see and do.

#include <opm/app.h>
#include <opm/mcp/mcp_tools.h>
#include <opm/media/ocr.h>
#include <opm/settings.h>

#include <thread>

namespace opm {

void App::mcp_init_() {
    mcp_ = std::make_unique<mcp::McpServer>(mcp::make_tools(*this));
    // Start/stop off the UI thread: stop() may wait for a tool call to end.
    auto apply = [this](bool on, int port, const std::string& token) {
        std::thread([this, on, port, token] {
            std::lock_guard lk(mcp_mutex_);
            if (!on) {
                mcp_->stop();
                renderer_.set_mcp_status("");
                return;
            }
            std::string e = mcp_->start(port, token);
            renderer_.set_mcp_status(e.empty() ? "Listening on 127.0.0.1:" + std::to_string(port) : e);
        }).detach();
    };
    renderer_.set_mcp_handler(apply);
    Settings s = Settings::load();
    if (s.mcp_enabled && !s.mcp_token.empty()) apply(true, s.mcp_port, s.mcp_token);
}

std::vector<mcp::DeviceRow> App::devices() {
    std::vector<mcp::DeviceRow> rows;
#ifdef ENABLE_ANDROID
    {
        std::lock_guard lk(scrcpy_mutex_);
        bool android_shown = active_source_.load() == static_cast<int>(Source::Android);
        for (auto& s : scrcpy_sessions_) {
            if (!s) continue;
            mcp::DeviceRow r;
            r.id = s->serial;
            r.name = s->model.empty() ? s->serial : s->model;
            r.kind = "android";
            r.shown = android_shown && s->serial == active_android_serial_;
            r.mirrored = true;
            r.control = s->receiver && s->receiver->control_ready();
            rows.push_back(r);
        }
    }
    // Phones adb sees but PhoneMimi does not mirror. `adb devices` costs a
    // process spawn, so it is cached for 3 s.
    std::vector<mcp::DeviceRow> extra;
    {
        std::lock_guard lk(adb_rows_mutex_);
        auto now = std::chrono::steady_clock::now();
        if (now - adb_rows_at_ > std::chrono::seconds(3)) {
            adb_rows_.clear();
            for (auto& d : adb_.list_devices()) {
                if (d.state != "device") continue;
                mcp::DeviceRow r;
                r.id = d.serial;
                r.name = d.model.empty() ? d.serial : d.model;
                r.kind = "android";
                adb_rows_.push_back(r);
            }
            adb_rows_at_ = now;
        }
        extra = adb_rows_;
    }
    for (auto& e : extra) {
        bool known = false;
        for (auto& r : rows) known |= r.id == e.id;
        if (!known) rows.push_back(e);
    }
#endif
#ifdef ENABLE_AIRPLAY
    if (config_.enable_airplay) {
        for (auto& s : airplay_.list_sources()) {
            if (!s.streaming) continue;
            mcp::DeviceRow r;
            r.id = "iphone";
            r.name = s.name;
            r.kind = "iphone";
            r.shown = active_source_.load() == static_cast<int>(Source::AirPlay);
            r.mirrored = true;
            rows.push_back(r);
            break;
        }
    }
#endif
    return rows;
}

bool App::frame(const std::string& id, mcp::Frame* out, int wait_ms) {
    return frame_tap_.get(id, out, wait_ms);
}

bool App::send(const std::string& id, const mcp::Input& in) {
#ifdef ENABLE_ANDROID
    std::lock_guard lk(scrcpy_mutex_);
    android::ScrcpyReceiver* rx = nullptr;
    for (auto& s : scrcpy_sessions_)
        if (s && s->receiver && s->serial == id) { rx = s->receiver.get(); break; }
    if (!rx || !rx->control_ready()) return false;
    switch (in.kind) {
        case mcp::Input::Touch: {
            // Coordinates refer to the frames MCP sees (they follow rotation).
            int w = 0, h = 0;
            frame_tap_.size(id, &w, &h);
            if (w <= 0) { w = rx->video_width(); h = rx->video_height(); }
            rx->send_touch(in.action, in.x, in.y, w, h);
            break;
        }
        case mcp::Input::Key:         rx->send_key(in.action, in.keycode); break;
        case mcp::Input::Text:        rx->send_text(in.text); break;
        case mcp::Input::Clipboard:   rx->send_clipboard(in.text, /*paste=*/true); break;
        case mcp::Input::ScreenPower: rx->send_display_power(in.action != 0); break;
    }
    return true;
#else
    (void)id; (void)in;
    return false;
#endif
}

int App::adb(const std::string& serial, const std::vector<std::string>& args,
             std::string* out, int timeout_ms) {
#ifdef ENABLE_ANDROID
    std::vector<std::string> a;
    if (!serial.empty()) a = {"-s", serial};
    a.insert(a.end(), args.begin(), args.end());
    return adb_.run(a, out, timeout_ms);
#else
    (void)serial; (void)args; (void)out; (void)timeout_ms;
    return -1;
#endif
}

std::string App::connect_usb() {
#ifdef ENABLE_ANDROID
    return android_connect_usb();
#else
    return "Android support is not built in.";
#endif
}

std::string App::connect_wifi(const std::string& addr) {
#ifdef ENABLE_ANDROID
    return android_connect_wifi(addr);
#else
    (void)addr;
    return "Android support is not built in.";
#endif
}

media::OcrJobResult App::ocr(const mcp::Frame& f) {
    return media::run_ocr_rgba(f.rgba.data(), f.w, f.h);
}

} // namespace opm
