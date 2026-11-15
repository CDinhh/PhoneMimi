#include <opm/mcp/mcp_tools.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include <opm/mcp/mcp_util.h>

extern "C" int stbi_write_png_to_func(void (*func)(void*, void*, int), void* context,
                                      int w, int h, int comp, const void* data,
                                      int stride_in_bytes);

using nlohmann::json;

namespace opm::mcp {

namespace {

const char* const kNoAndroid =
    "No Android phone connected. Call connect_usb / connect_wifi or ask the user to connect one.";
const char* const kViewOnly =
    "iPhone is view-only over AirPlay: only screenshot and ocr_screen work.";

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

bool is_ascii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return (unsigned char)c < 0x80; });
}

std::string str_arg(const json& a, const char* k, const std::string& def = {}) {
    if (!a.contains(k) || a[k].is_null()) return def;
    if (a[k].is_string()) return a[k].get<std::string>();
    return a[k].dump();
}

int int_arg(const json& a, const char* k, int def) {
    if (a.contains(k) && a[k].is_number()) return a[k].get<int>();
    if (a.contains(k) && a[k].is_string()) {
        try { return std::stoi(a[k].get<std::string>()); } catch (...) {}
    }
    return def;
}

bool bool_arg(const json& a, const char* k, bool def) {
    return a.contains(k) && a[k].is_boolean() ? a[k].get<bool>() : def;
}

json schema(json props, std::vector<std::string> required = {}) {
    props["device"] = {{"type", "string"},
                       {"description", "Device id from list_devices. Default: the phone shown in PhoneMimi."}};
    json s = {{"type", "object"}, {"properties", props}};
    if (!required.empty()) s["required"] = required;
    return s;
}
json p_str(const char* d) { return {{"type", "string"}, {"description", d}}; }
json p_int(const char* d) { return {{"type", "integer"}, {"description", d}}; }
json p_bool(const char* d) { return {{"type", "boolean"}, {"description", d}}; }

struct Ctx {
    explicit Ctx(Host& h) : host(h) {}
    Host& host;

    // Picks the device a call targets, or sets *err.
    std::optional<DeviceRow> pick(const json& a, bool android_only, std::string* err) {
        auto rows = host.devices();
        if (rows.empty()) {
            *err = android_only ? kNoAndroid : "No phone connected.";
            return std::nullopt;
        }
        std::string want = str_arg(a, "device");
        if (!want.empty()) {
            for (auto& r : rows) {
                if (r.id != want) continue;
                if (android_only && r.kind != "android") { *err = kViewOnly; return std::nullopt; }
                return r;
            }
            std::string known;
            for (auto& r : rows) known += (known.empty() ? "" : ", ") + r.id;
            *err = "Unknown device " + want + ". Connected: " + known;
            return std::nullopt;
        }
        const DeviceRow* pickd = nullptr;
        for (auto& r : rows) if (r.shown) { pickd = &r; break; }
        if (!pickd) pickd = &rows[0];
        if (android_only && pickd->kind != "android") {
            pickd = nullptr;
            for (auto& r : rows) if (r.kind == "android") { pickd = &r; break; }
            if (!pickd) { *err = kNoAndroid; return std::nullopt; }
        }
        return *pickd;
    }

    // A device that has a stream and accepts input.
    std::optional<DeviceRow> pick_input(const json& a, std::string* err) {
        // An explicit iPhone gets the view-only message.
        auto d = pick(a, true, err);
        if (!d) return d;
        if (!d->mirrored || !d->control) {
            *err = "Device " + d->id + " is not being mirrored, so it cannot be controlled. "
                   "Call connect_usb / connect_wifi or ask the user to connect it in PhoneMimi.";
            return std::nullopt;
        }
        return d;
    }

    int adb(const DeviceRow& d, std::vector<std::string> args, std::string* out,
            int timeout_ms = 15000) {
        return host.adb(d.id, args, out, timeout_ms);
    }

    std::mutex wm_mu;
    std::map<std::string, Size> wm_sizes;   // natural display size per device

    // Display size in device pixels for the current rotation. The dump's
    // root is only the focused window (a dialog is smaller than the screen),
    // so it is the last resort.
    Size screen_size(const DeviceRow& d, const std::string& xml, const std::vector<UiNode>& nodes) {
        Size nat;
        {
            std::lock_guard lk(wm_mu);
            auto it = wm_sizes.find(d.id);
            if (it != wm_sizes.end()) nat = it->second;
        }
        if (nat.w <= 0) {
            std::string out;
            adb(d, {"shell", "wm", "size"}, &out, 5000);
            nat = parse_wm_size(out);
            if (nat.w > 0) {
                std::lock_guard lk(wm_mu);
                wm_sizes[d.id] = nat;
            }
        }
        if (nat.w <= 0) return ui_root_size(nodes);
        int rot = parse_ui_rotation(xml);
        bool portrait = nat.w < nat.h;
        Size s{std::min(nat.w, nat.h), std::max(nat.w, nat.h)};
        if (!portrait) std::swap(s.w, s.h);
        if (rot % 2) std::swap(s.w, s.h);
        return s;
    }

    // uiautomator dump, with the /sdcard fallback. `screen` gets the display
    // size the bounds refer to. budget_ms caps each adb step.
    bool ui_dump(const DeviceRow& d, std::vector<UiNode>* nodes, std::string* err,
                 Size* screen = nullptr, int budget_ms = 20000) {
        int step = std::clamp(budget_ms, 1000, 20000);
        std::string xml;
        adb(d, {"exec-out", "uiautomator", "dump", "/dev/tty"}, &xml, step);
        bool ok = parse_ui_dump(xml, nodes, err);
        if (!ok && !host.closing()) {
            std::string tmp;
            adb(d, {"shell", "uiautomator", "dump", "/sdcard/pm_ui.xml"}, &tmp, step);
            xml.clear();
            adb(d, {"exec-out", "cat", "/sdcard/pm_ui.xml"}, &xml, std::min(step, 10000));
            ok = parse_ui_dump(xml, nodes, err);
        }
        if (!ok) {
            *err = "uiautomator failed (" + *err +
                   "). The screen may be animating or playing video: retry, or use ocr_screen.";
            return false;
        }
        if (screen) *screen = screen_size(d, xml, *nodes);
        return true;
    }

    // Frame size of a mirrored device (0x0 if none).
    Size frame_size(const DeviceRow& d, int wait_ms = 0) {
        Frame f;
        if (!host.frame(d.id, &f, wait_ms)) return {};
        return {f.w, f.h};
    }

    // Tap space: from the stream if there is one, else from the screen size.
    Size shot_for(const DeviceRow& d, Size screen) {
        Size fs = frame_size(d);
        if (fs.w > 0) return shot_size(fs);
        return shot_size(screen);
    }

    std::string current_app(const DeviceRow& d) {
        std::string out;
        adb(d, {"shell", "dumpsys", "activity", "activities"}, &out, 10000);
        std::istringstream is(out);
        std::string line;
        while (std::getline(is, line)) {
            if (line.find("topResumedActivity") == std::string::npos &&
                line.find("mResumedActivity") == std::string::npos)
                continue;
            std::istringstream ls(line);
            std::string tok;
            while (ls >> tok)
                if (tok.find('/') != std::string::npos) {
                    while (!tok.empty() && (tok.back() == '}' || tok.back() == ')')) tok.pop_back();
                    return tok;
                }
        }
        return "unknown";
    }

    bool touch(const DeviceRow& d, int action, int x, int y) {
        Input in;
        in.kind = Input::Touch;
        in.action = action;
        in.x = x;
        in.y = y;
        return host.send(d.id, in);
    }

    bool key(const DeviceRow& d, int code) {
        Input in;
        in.kind = Input::Key;
        in.keycode = code;
        in.action = 0;
        if (!host.send(d.id, in)) return false;
        in.action = 1;
        return host.send(d.id, in);
    }

    // Resolves x/y or a selector to frame pixels. `what` describes the target.
    bool target(const DeviceRow& d, const json& a, int* fx, int* fy, std::string* what,
                std::string* err) {
        Size fs = frame_size(d);
        if (fs.w <= 0) { *err = "No video frame yet from " + d.id + "."; return false; }
        Selector sel{str_arg(a, "text"), str_arg(a, "id"), str_arg(a, "desc"),
                     bool_arg(a, "exact", false)};
        if (sel.text.empty() && sel.id.empty() && sel.desc.empty()) {
            if (!a.contains("x") || !a.contains("y")) {
                *err = "Give x and y (screenshot pixels), or text / id / desc.";
                return false;
            }
            Size shot = shot_size(fs);
            int x = int_arg(a, "x", 0), y = int_arg(a, "y", 0);
            if (x < 0 || y < 0 || x >= shot.w || y >= shot.h) {
                *err = "Point (" + std::to_string(x) + "," + std::to_string(y) +
                       ") is outside the " + std::to_string(shot.w) + "x" +
                       std::to_string(shot.h) + " screen.";
                return false;
            }
            *fx = map_x(x, shot, fs);
            *fy = map_y(y, shot, fs);
            *what = "(" + std::to_string(x) + "," + std::to_string(y) + ")";
            return true;
        }
        std::vector<UiNode> nodes;
        Size screen;
        if (!ui_dump(d, &nodes, err, &screen)) return false;
        auto hits = find_nodes(nodes, sel);
        int idx = int_arg(a, "index", 0);
        if (hits.empty() || idx < 0 || idx >= (int)hits.size()) {
            std::string cands;
            int n = 0;
            Size shot = shot_size(fs);
            for (auto& nd : nodes) {
                if (nd.text.empty() && nd.desc.empty() && nd.id.empty()) continue;
                if (n++ == 10) break;
                cands += "\n  " + std::string(nd.text.empty() ? "" : "\"" + nd.text + "\" ") +
                         (nd.desc.empty() ? "" : "desc=\"" + nd.desc + "\" ") +
                         (nd.id.empty() ? "" : "id=" + nd.id + " ") +
                         "at (" + std::to_string(map_x((nd.bounds.l + nd.bounds.r) / 2, screen, shot)) +
                         "," + std::to_string(map_y((nd.bounds.t + nd.bounds.b) / 2, screen, shot)) + ")";
            }
            *err = hits.empty() ? "No element matches." : "Only " + std::to_string(hits.size()) + " matches.";
            if (!cands.empty()) *err += " Some elements on screen:" + cands;
            return false;
        }
        const UiNode& n = *hits[idx];
        *fx = map_x((n.bounds.l + n.bounds.r) / 2, screen, fs);
        *fy = map_y((n.bounds.t + n.bounds.b) / 2, screen, fs);
        *what = n.cls + (n.text.empty() ? "" : " \"" + n.text + "\"") +
                (n.id.empty() ? "" : " id=" + n.id);
        if (hits.size() > 1)
            *what += " (match " + std::to_string(idx + 1) + " of " + std::to_string(hits.size()) + ")";
        return true;
    }
};

const char* const kLost =
    "The control channel to the phone is gone (it may have disconnected). "
    "Call list_devices, then connect_wifi or connect_usb.";

// Box-filter downscale of packed RGBA.
std::vector<uint8_t> downscale(const Frame& f, Size to) {
    std::vector<uint8_t> out(size_t(to.w) * to.h * 4);
    for (int y = 0; y < to.h; ++y) {
        int y0 = int((long long)y * f.h / to.h), y1 = std::max(y0 + 1, int((long long)(y + 1) * f.h / to.h));
        for (int x = 0; x < to.w; ++x) {
            int x0 = int((long long)x * f.w / to.w), x1 = std::max(x0 + 1, int((long long)(x + 1) * f.w / to.w));
            unsigned sum[4] = {0, 0, 0, 0}, n = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = &f.rgba[(size_t(sy) * f.w + sx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    ++n;
                }
            uint8_t* o = &out[(size_t(y) * to.w + x) * 4];
            for (int c = 0; c < 3; ++c) o[c] = uint8_t(sum[c] / n);
            o[3] = 255;
        }
    }
    return out;
}

void png_sink(void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size_t(size));
}

ToolResult image_result(const std::string& png, const std::string& text) {
    ToolResult r;
    r.content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", base64(png)}});
    r.content.push_back({{"type", "text"}, {"text", text}});
    return r;
}

ToolResult err(const std::string& e) { return text_result(e, true); }

std::string adb_fail(int rc, const std::string& out, int timeout_ms) {
    if (rc == -2) return "adb timed out after " + std::to_string(timeout_ms) + " ms.\n" + truncate_output(out);
    if (rc == -1) return "Could not run adb.";
    return "adb failed (exit " + std::to_string(rc) + "):\n" + truncate_output(out);
}

} // namespace

ToolTable make_tools(Host& host) {
    auto ctx = std::make_shared<Ctx>(host);
    ToolTable t;
    auto add = [&](const char* name, const char* desc, json sch,
                   std::function<ToolResult(Ctx&, const json&)> fn) {
        t.push_back({name, desc, std::move(sch), [ctx, fn, n = std::string(name)](const json& a) {
                         std::cout << "[MCP] " << n << " device=" << str_arg(a, "device", "default")
                                   << "\n";
                         return fn(*ctx, a);
                     }});
    };

    // ---- Seeing and reading ----
    add("list_devices", "List connected phones: id, name, kind (android/iphone), whether it is shown in PhoneMimi, mirrored (screenshots) and controllable, and the screenshot size.",
        {{"type", "object"}, {"properties", json::object()}},
        [](Ctx& c, const json&) {
            auto rows = c.host.devices();
            if (rows.empty()) return text_result("No phone connected. Android: call connect_usb / connect_wifi. iPhone: Screen Mirroring > PhoneMimi.");
            std::string s;
            for (auto& r : rows) {
                Size shot = shot_size(c.frame_size(r));
                s += r.id + " | " + r.name + " | " + r.kind + (r.shown ? " | shown" : "") +
                     (r.mirrored ? " | mirrored" : " | not mirrored") +
                     (r.control ? " | control" : "") +
                     (shot.w ? " | " + std::to_string(shot.w) + "x" + std::to_string(shot.h) +
                                   (shot.w > shot.h ? " landscape" : " portrait") : "") + "\n";
            }
            return text_result(s);
        });

    add("screenshot", "Screenshot of the phone as PNG. Its pixel grid is the coordinate space every other tool uses (long side at most 1280).",
        schema({{"max_side", p_int("Shrink the image so its long side is at most this (320..1280). Tap coordinates stay in the full screenshot space.")}}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick(a, false, &e);
            if (!d) return err(e);
            Frame f;
            if (!d->mirrored || !c.host.frame(d->id, &f, 250))
                return err(d->kind == "android"
                               ? "No video from " + d->id + ". Call connect_usb / connect_wifi, or ask the user to mirror it."
                               : "No video from the iPhone yet.");
            Size shot = shot_size({f.w, f.h});
            Size img = shot_size({f.w, f.h}, std::clamp(int_arg(a, "max_side", 1280), 320, 1280));
            auto px = (img.w == f.w && img.h == f.h) ? f.rgba : downscale(f, img);
            std::string png;
            if (!stbi_write_png_to_func(png_sink, &png, img.w, img.h, 4, px.data(), img.w * 4))
                return err("PNG encoding failed.");
            std::string txt = std::to_string(img.w) + "x" + std::to_string(img.h) + " screenshot space";
            if (img.w != shot.w)
                txt = std::to_string(img.w) + "x" + std::to_string(img.h) + " image; tap space is " +
                      std::to_string(shot.w) + "x" + std::to_string(shot.h) + " (multiply by " +
                      std::to_string(double(shot.w) / img.w).substr(0, 5) + ")";
            return image_result(png, txt + ", device " + d->id);
        });

    add("ui_tree", "Android UI elements on screen (uiautomator): class, text, content-desc, resource id, bounds in screenshot pixels, and flags. Use it to find what to tap.",
        schema({}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            std::vector<UiNode> nodes;
            Size screen;
            if (!c.ui_dump(*d, &nodes, &e, &screen)) return err(e);
            Size shot = c.shot_for(*d, screen);
            return text_result("App: " + c.current_app(*d) + "\nScreen: " + std::to_string(shot.w) +
                               "x" + std::to_string(shot.h) + "\n" +
                               truncate_output(format_ui_tree(nodes, screen, shot)));
        });

    add("ocr_screen", "Read the text on screen with Windows OCR. Works on iPhone too. Lines come with [x,y,w,h] in screenshot pixels.",
        schema({}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick(a, false, &e);
            if (!d) return err(e);
            Frame f;
            if (!c.host.frame(d->id, &f, 250)) return err("No video from " + d->id + ".");
            auto r = c.host.ocr(f);
            if (!r.ok) return err("OCR failed: " + r.error);
            Size fs{f.w, f.h}, shot = shot_size(fs);
            std::string s;
            for (auto& l : r.lines) {
                char b[96];
                std::snprintf(b, sizeof(b), " [%d,%d,%d,%d]\n", map_x(l.x, fs, shot), map_y(l.y, fs, shot),
                              map_x(l.w, fs, shot), map_y(l.h, fs, shot));
                s += l.text + b;
            }
            return text_result(s.empty() ? "No text found." : s);
        });

    add("current_app", "Foreground Android app as package/activity.", schema({}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            return text_result(c.current_app(*d));
        });

    // ---- Control ----
    json target_props = {
        {"x", p_int("X in screenshot pixels")}, {"y", p_int("Y in screenshot pixels")},
        {"text", p_str("Tap the element whose text contains this (case-insensitive)")},
        {"id", p_str("Element resource id (without the package prefix)")},
        {"desc", p_str("Element content-desc")},
        {"exact", p_bool("Match text/id/desc exactly")},
        {"index", p_int("Which match to use when several match (0-based)")}};

    auto gesture = [&](const char* name, const char* desc, int taps, int def_hold) {
        json props = target_props;
        if (def_hold > 100) props["duration_ms"] = p_int("How long to hold (default 600)");
        add(name, desc, schema(props), [taps, def_hold](Ctx& c, const json& a) {
            std::string e, what;
            auto d = c.pick_input(a, &e);
            if (!d) return err(e);
            int fx = 0, fy = 0;
            if (!c.target(*d, a, &fx, &fy, &what, &e)) return err(e);
            int hold = std::clamp(int_arg(a, "duration_ms", def_hold), 10, 10000);
            for (int i = 0; i < taps; ++i) {
                if (i) sleep_ms(120);
                if (!c.touch(*d, 0, fx, fy)) return err(kLost);
                sleep_ms(hold);
                if (!c.touch(*d, 1, fx, fy)) return err(kLost);
            }
            return text_result(std::string(taps == 2 ? "Double-tapped " : def_hold > 100 ? "Long-pressed " : "Tapped ") + what);
        });
    };
    gesture("tap", "Tap a point (x, y in screenshot pixels) or an element by text / id / desc.", 1, 40);
    gesture("long_press", "Long-press a point or an element.", 1, 600);
    gesture("double_tap", "Double-tap a point or an element.", 2, 40);

    add("swipe", "Swipe from (x1,y1) to (x2,y2), screenshot pixels.",
        schema({{"x1", p_int("")}, {"y1", p_int("")}, {"x2", p_int("")}, {"y2", p_int("")},
                {"duration_ms", p_int("Default 300")}}, {"x1", "y1", "x2", "y2"}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick_input(a, &e);
            if (!d) return err(e);
            Size fs = c.frame_size(*d), shot = shot_size(fs);
            if (fs.w <= 0) return err("No video frame yet.");
            int x1 = map_x(int_arg(a, "x1", 0), shot, fs), y1 = map_y(int_arg(a, "y1", 0), shot, fs);
            int x2 = map_x(int_arg(a, "x2", 0), shot, fs), y2 = map_y(int_arg(a, "y2", 0), shot, fs);
            int dur = std::clamp(int_arg(a, "duration_ms", 300), 16, 10000), steps = std::max(1, dur / 16);
            if (!c.touch(*d, 0, x1, y1)) return err(kLost);
            for (int i = 1; i <= steps; ++i) {
                sleep_ms(16);
                c.touch(*d, 2, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
            }
            if (!c.touch(*d, 1, x2, y2)) return err(kLost);
            return text_result("Swiped.");
        });

    add("scroll", "Scroll the content: direction down shows what is further down. amount 1 = about 60% of the screen.",
        schema({{"direction", {{"type", "string"}, {"enum", {"up", "down", "left", "right"}}}},
                {"amount", {{"type", "number"}, {"description", "Default 1"}}},
                {"x", p_int("Where to scroll (default centre)")}, {"y", p_int("")}}, {"direction"}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick_input(a, &e);
            if (!d) return err(e);
            Size fs = c.frame_size(*d), shot = shot_size(fs);
            if (fs.w <= 0) return err("No video frame yet.");
            std::string dir = str_arg(a, "direction", "down");
            double amt = a.contains("amount") && a["amount"].is_number() ? a["amount"].get<double>() : 1.0;
            amt = std::clamp(amt, 0.1, 3.0);
            int cx = map_x(int_arg(a, "x", shot.w / 2), shot, fs), cy = map_y(int_arg(a, "y", shot.h / 2), shot, fs);
            bool vert = dir == "up" || dir == "down";
            int len = int((vert ? fs.h : fs.w) * 0.6 * std::min(amt, 1.0));
            int sign = (dir == "down" || dir == "right") ? -1 : 1;   // finger moves against the content
            int reps = int(amt + 0.999);
            for (int r = 0; r < reps; ++r) {
                int sx = cx, sy = cy, ex = cx, ey = cy;
                if (vert) { sy = cy - sign * len / 2; ey = cy + sign * len / 2; }
                else { sx = cx - sign * len / 2; ex = cx + sign * len / 2; }
                sx = std::clamp(sx, 1, fs.w - 2); ex = std::clamp(ex, 1, fs.w - 2);
                sy = std::clamp(sy, 1, fs.h - 2); ey = std::clamp(ey, 1, fs.h - 2);
                if (!c.touch(*d, 0, sx, sy)) return err(kLost);
                for (int i = 1; i <= 18; ++i) {
                    sleep_ms(16);
                    c.touch(*d, 2, sx + (ex - sx) * i / 18, sy + (ey - sy) * i / 18);
                }
                c.touch(*d, 1, ex, ey);
                sleep_ms(250);
            }
            return text_result("Scrolled " + dir + ".");
        });

    add("type_text", "Type text into the focused field (any language). submit: press Enter after.",
        schema({{"text", p_str("")}, {"submit", p_bool("Press Enter after typing")}}, {"text"}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick_input(a, &e);
            if (!d) return err(e);
            std::string text = str_arg(a, "text");
            Input in;
            if (is_ascii(text)) {
                in.kind = Input::Text;
                for (size_t i = 0; i < text.size(); i += 300) {
                    in.text = text.substr(i, 300);
                    if (!c.host.send(d->id, in)) return err(kLost);
                }
            } else {
                in.kind = Input::Clipboard;
                in.text = text;
                if (!c.host.send(d->id, in)) return err(kLost);
            }
            if (bool_arg(a, "submit", false)) {
                sleep_ms(80);
                if (!c.key(*d, 66)) return err(kLost);
            }
            std::cout << "[MCP] type_text " << text.size() << " bytes\n";
            return text_result("Typed " + std::to_string(text.size()) + " bytes.");
        });

    add("press_key", "Press a key: back, home, recents, power, enter, delete, forward_delete, tab, escape, menu, search, volume_up, volume_down, volume_mute, dpad_up/down/left/right, dpad_center, wake, sleep, or an Android keycode number.",
        schema({{"key", p_str("Key name or keycode")}, {"long", p_bool("Long press")}}, {"key"}),
        [](Ctx& c, const json& a) {
            std::string e;
            std::string name = str_arg(a, "key");
            int code = keycode_for(name);
            if (code < 0) return err("Unknown key '" + name + "'.");
            if (bool_arg(a, "long", false)) {
                auto d = c.pick(a, true, &e);
                if (!d) return err(e);
                std::string out;
                int rc = c.adb(*d, {"shell", "input", "keyevent", "--longpress", std::to_string(code)}, &out);
                return rc == 0 ? text_result("Long-pressed " + name + ".") : err(adb_fail(rc, out, 15000));
            }
            auto d = c.pick_input(a, &e);
            if (!d) return err(e);
            if (!c.key(*d, code)) return err(kLost);
            return text_result("Pressed " + name + ".");
        });

    add("screen_power", "Turn the phone's own display off or on while mirroring continues.",
        schema({{"on", p_bool("")}}, {"on"}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick_input(a, &e);
            if (!d) return err(e);
            Input in;
            in.kind = Input::ScreenPower;
            in.action = bool_arg(a, "on", true) ? 1 : 0;
            if (!c.host.send(d->id, in)) return err(kLost);
            return text_result(in.action ? "Phone display on." : "Phone display off.");
        });

    add("wait_for", "Wait until an element (text / id / desc) appears, or disappears with gone=true. Polls every 500 ms.",
        schema({{"text", p_str("")}, {"id", p_str("")}, {"desc", p_str("")},
                {"gone", p_bool("Wait for it to disappear")},
                {"timeout_ms", p_int("Default 10000, max 60000")}}),
        [](Ctx& c, const json& a) {
            std::string e;
            auto d = c.pick(a, false, &e);
            if (!d) return err(e);
            Selector sel{str_arg(a, "text"), str_arg(a, "id"), str_arg(a, "desc"), false};
            if (sel.text.empty() && sel.id.empty() && sel.desc.empty()) return err("Give text, id or desc.");
            if (d->kind != "android" && sel.text.empty())
                return err("On an iPhone wait_for can only look for text (it uses OCR).");
            bool gone = bool_arg(a, "gone", false);
            int timeout = std::clamp(int_arg(a, "timeout_ms", 10000), 0, 60000);
            auto t0 = std::chrono::steady_clock::now();
            int ui_fails = 0;
            for (;;) {
                bool present = false, checked = false;
                std::vector<UiNode> nodes;
                std::string ue;
                int left = timeout - int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - t0).count());
                if (d->kind == "android" && ui_fails < 2 && c.ui_dump(*d, &nodes, &ue, nullptr, left)) {
                    present = !find_nodes(nodes, sel).empty();
                    checked = true;
                } else {
                    if (d->kind == "android") ++ui_fails;
                    Frame f;
                    if (!sel.text.empty() && c.host.frame(d->id, &f, 0)) {
                        auto r = c.host.ocr(f);
                        checked = r.ok;
                        for (auto& l : r.lines) {
                            std::string lt = l.text, w = sel.text;
                            for (auto& ch : lt) ch = char(std::tolower((unsigned char)ch));
                            for (auto& ch : w) ch = char(std::tolower((unsigned char)ch));
                            if (lt.find(w) != std::string::npos) { present = true; break; }
                        }
                    }
                }
                int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count());
                if (checked && present != gone)
                    return text_result(std::string(gone ? "Gone" : "Found") + " after " + std::to_string(ms) + " ms.");
                if (ms >= timeout || c.host.closing()) {
                    if (!checked && ui_fails >= 2 && sel.text.empty())
                        return err("Could not read the screen: uiautomator keeps failing and OCR cannot look up id/desc. Use text instead.");
                    return text_result(std::string(gone ? "Still there" : "Not found") + " after " + std::to_string(ms) + " ms.");
                }
                sleep_ms(std::min(500, std::max(1, timeout - ms)));
            }
        });

    // ---- Apps and dev chores ----
    add("list_apps", "Installed packages (third-party only by default).",
        schema({{"third_party", p_bool("Default true")}, {"filter", p_str("Substring to match")}}),
        [](Ctx& c, const json& a) {
            std::string e, out;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            std::vector<std::string> args = {"shell", "pm", "list", "packages"};
            if (bool_arg(a, "third_party", true)) args.push_back("-3");
            int rc = c.adb(*d, args, &out);
            if (rc != 0) return err(adb_fail(rc, out, 15000));
            std::string filter = str_arg(a, "filter");
            std::vector<std::string> pk;
            std::istringstream is(out);
            std::string line;
            while (std::getline(is, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.rfind("package:", 0) != 0) continue;
                line = line.substr(8);
                if (filter.empty() || line.find(filter) != std::string::npos) pk.push_back(line);
            }
            std::sort(pk.begin(), pk.end());
            std::string s;
            for (auto& p : pk) s += p + "\n";
            return text_result(s.empty() ? "No packages match." : truncate_output(s));
        });

    add("launch_app", "Start an app by package (optionally a specific activity). restart: force-stop it first.",
        schema({{"package", p_str("")}, {"activity", p_str("e.g. .MainActivity")},
                {"restart", p_bool("Force-stop first")}}, {"package"}),
        [](Ctx& c, const json& a) {
            std::string e, out;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            std::string pkg = str_arg(a, "package"), act = str_arg(a, "activity");
            if (bool_arg(a, "restart", false)) c.adb(*d, {"shell", "am", "force-stop", pkg}, &out);
            int rc = act.empty()
                         ? c.adb(*d, {"shell", "monkey", "-p", pkg, "-c", "android.intent.category.LAUNCHER", "1"}, &out)
                         : c.adb(*d, {"shell", "am", "start", "-n", pkg + "/" + act}, &out);
            if (rc != 0 || out.find("No activities found") != std::string::npos ||
                out.find("Error") != std::string::npos)
                return err("Could not launch " + pkg + ":\n" + truncate_output(out));
            return text_result("Launched " + pkg + ".");
        });

    auto simple_shell = [&](const char* name, const char* desc, const char* argname,
                            std::vector<std::string> pre, const char* done, const char* must) {
        add(name, desc, schema({{argname, p_str("")}}, {argname}),
            [pre, argname, done, must](Ctx& c, const json& a) {
                std::string e, out;
                auto d = c.pick(a, true, &e);
                if (!d) return err(e);
                auto args = pre;
                args.push_back(str_arg(a, argname));
                int rc = c.adb(*d, args, &out);
                if (rc != 0 || (*must && out.find(must) == std::string::npos))
                    return err(adb_fail(rc ? rc : 1, out, 15000));
                return text_result(std::string(done) + " " + str_arg(a, argname) + ".");
            });
    };
    simple_shell("stop_app", "Force-stop an app.", "package", {"shell", "am", "force-stop"}, "Stopped", "");
    simple_shell("clear_app_data", "Clear an app's data and cache (pm clear).", "package", {"shell", "pm", "clear"}, "Cleared", "Success");
    simple_shell("uninstall_app", "Uninstall an app.", "package", {"uninstall"}, "Uninstalled", "Success");

    add("install_apk", "Install an APK from a path on this PC.",
        schema({{"path", p_str("APK path on the PC")}, {"replace", p_bool("Default true")},
                {"grant_permissions", p_bool("Grant all runtime permissions")}}, {"path"}),
        [](Ctx& c, const json& a) {
            std::string e, out;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            std::string path = str_arg(a, "path");
            std::error_code ec;
            if (!std::filesystem::is_regular_file(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), ec))
                return err("No file at " + path);
            std::vector<std::string> args = {"install"};
            if (bool_arg(a, "replace", true)) args.push_back("-r");
            if (bool_arg(a, "grant_permissions", false)) args.push_back("-g");
            args.push_back(path);
            int rc = c.adb(*d, args, &out, 180000);
            if (rc != 0 || out.find("Success") == std::string::npos) return err(adb_fail(rc ? rc : 1, out, 180000));
            return text_result("Installed " + path + ".");
        });

    add("open_url", "Open a deep link or web URL on the phone (am start -a VIEW).",
        schema({{"url", p_str("")}}, {"url"}),
        [](Ctx& c, const json& a) {
            std::string e, out;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            std::string url = str_arg(a, "url");
            int rc = c.adb(*d, {"shell", "am start -a android.intent.action.VIEW -d " + sh_quote(url)}, &out);
            if (rc != 0 || out.find("Error") != std::string::npos) return err(adb_fail(rc ? rc : 1, out, 15000));
            return text_result("Opened " + url + ".");
        });

    add("logcat", "Recent log lines (logcat -d). Filter by package (its pid), level (V/D/I/W/E), tag, or since a timestamp ('MM-DD HH:MM:SS.mmm').",
        schema({{"lines", p_int("Default 200, max 2000")}, {"package", p_str("")}, {"level", p_str("V, D, I, W or E")},
                {"tag", p_str("")}, {"since", p_str("")}}),
        [](Ctx& c, const json& a) {
            std::string e, out;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            int lines = std::clamp(int_arg(a, "lines", 200), 1, 2000);
            std::vector<std::string> args = {"logcat", "-d", "-v", "threadtime"};
            std::string since = str_arg(a, "since");
            if (!since.empty()) { args.push_back("-T"); args.push_back(since); }
            else { args.push_back("-t"); args.push_back(std::to_string(lines)); }
            std::string pkg = str_arg(a, "package");
            if (!pkg.empty()) {
                std::string pid;
                c.adb(*d, {"shell", "pidof", pkg}, &pid);
                pid = pid.substr(0, pid.find_first_of(" \r\n"));
                if (pid.empty() || !std::isdigit((unsigned char)pid[0]))
                    return err(pkg + " is not running (no pid). Launch it first.");
                args.push_back("--pid=" + pid);
            }
            std::string level = str_arg(a, "level"), tag = str_arg(a, "tag");
            if (!level.empty()) level = std::string(1, char(std::toupper((unsigned char)level[0])));
            if (!tag.empty()) { args.push_back(tag + ":" + (level.empty() ? "V" : level)); args.push_back("*:S"); }
            else if (!level.empty()) args.push_back("*:" + level);
            int rc = c.adb(*d, args, &out, 20000);
            if (rc != 0) return err(adb_fail(rc, out, 20000));
            if (!since.empty()) {   // keep the last `lines`
                size_t pos = out.size();
                for (int n = 0; n <= lines && pos != std::string::npos && pos > 0; ++n)
                    pos = out.rfind('\n', pos - 1);
                if (pos != std::string::npos && pos > 0) out = out.substr(pos + 1);
            }
            return text_result(out.empty() ? "No log lines." : truncate_output(out));
        });

    add("clear_logcat", "Clear the log buffer (logcat -c).", schema({}),
        [](Ctx& c, const json& a) {
            std::string e, out;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            int rc = c.adb(*d, {"logcat", "-c"}, &out);
            return rc == 0 ? text_result("Log cleared.") : err(adb_fail(rc, out, 15000));
        });

    auto file_tool = [&](const char* name, const char* desc, bool push) {
        add(name, desc, schema({{"pc_path", p_str("Path on this PC")}, {"phone_path", p_str("Path on the phone, e.g. /sdcard/Download/x")}},
                               {"pc_path", "phone_path"}),
            [push](Ctx& c, const json& a) {
                std::string e, out;
                auto d = c.pick(a, true, &e);
                if (!d) return err(e);
                std::string pc = str_arg(a, "pc_path"), ph = str_arg(a, "phone_path");
                int rc = push ? c.adb(*d, {"push", pc, ph}, &out, 120000)
                              : c.adb(*d, {"pull", ph, pc}, &out, 120000);
                if (rc != 0) return err(adb_fail(rc, out, 120000));
                return text_result(truncate_output(out));
            });
    };
    file_tool("push_file", "Copy a file from this PC to the phone.", true);
    file_tool("pull_file", "Copy a file from the phone to this PC.", false);

    add("adb_shell", "Run a command in the phone's shell (adb shell). Returns exit code and output.",
        schema({{"command", p_str("")}, {"timeout_ms", p_int("Default 15000, max 120000")}}, {"command"}),
        [](Ctx& c, const json& a) {
            std::string e, out;
            auto d = c.pick(a, true, &e);
            if (!d) return err(e);
            int to = std::clamp(int_arg(a, "timeout_ms", 15000), 1000, 120000);
            int rc = c.adb(*d, {"shell", str_arg(a, "command")}, &out, to);
            if (rc == -2 || rc == -1) return err(adb_fail(rc, out, to));
            return text_result("exit " + std::to_string(rc) + "\n" + truncate_output(out), rc != 0);
        });

    add("connect_usb", "Mirror an Android phone plugged in with a USB cable (USB debugging on). It becomes the phone shown in PhoneMimi.",
        {{"type", "object"}, {"properties", json::object()}},
        [](Ctx& c, const json&) {
            std::string s = c.host.connect_usb();
            return text_result(s, s.rfind("Mirroring", 0) != 0);
        });

    add("connect_wifi", "Connect and mirror an Android phone over Wi-Fi. address = ip:port from the phone's Wireless debugging screen; without it, retries phones connected earlier. The phone must have been paired once in PhoneMimi.",
        {{"type", "object"}, {"properties", {{"address", p_str("ip:port")}}}},
        [](Ctx& c, const json& a) {
            std::string s = c.host.connect_wifi(str_arg(a, "address"));
            return text_result(s, s.rfind("Mirroring", 0) != 0);
        });

    return t;
}

} // namespace opm::mcp
