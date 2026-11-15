#pragma once

// What the MCP tools need from the app. App implements it; the self-test
// uses a fake.

#include <cstdint>
#include <string>
#include <vector>

#include <opm/media/ocr.h>

namespace opm::mcp {

struct Frame {
    std::vector<uint8_t> rgba;   // packed RGBA, no row padding
    int w = 0, h = 0;
};

struct DeviceRow {
    std::string id, name, kind;  // kind: "android" | "iphone"
    bool shown = false;          // the phone on screen in the window
    bool mirrored = false;       // a live stream exists (screenshots)
    bool control = false;        // input can be sent
};

struct Input {
    enum Kind { Touch, Key, Text, Clipboard, ScreenPower } kind = Touch;
    int action = 0;              // Touch 0 down 1 up 2 move; Key 0 down 1 up; ScreenPower 0 off 1 on
    int x = 0, y = 0;            // frame pixels
    int keycode = 0;
    std::string text;
};

class Host {
public:
    virtual ~Host() = default;
    virtual std::vector<DeviceRow> devices() = 0;
    // Latest frame of a mirrored device; false when there is no stream.
    virtual bool frame(const std::string& id, Frame* out, int wait_ms) = 0;
    // False when the device has no control channel (or dropped).
    virtual bool send(const std::string& id, const Input& in) = 0;
    // `adb -s serial <args>` (no -s when serial is empty). -2 = timed out.
    virtual int adb(const std::string& serial, const std::vector<std::string>& args,
                    std::string* out, int timeout_ms) = 0;
    virtual std::string connect_usb() = 0;
    virtual std::string connect_wifi(const std::string& addr) = 0;
    virtual media::OcrJobResult ocr(const Frame& f) = 0;
};

} // namespace opm::mcp
