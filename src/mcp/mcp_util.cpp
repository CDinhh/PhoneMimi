#include <opm/mcp/mcp_util.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace opm::mcp {

Size shot_size(Size src, int max_side) {
    int m = std::max(src.w, src.h);
    if (m <= 0) return {};
    if (m <= max_side) return src;
    double s = double(max_side) / m;
    return {std::max(1, int(src.w * s + 0.5)), std::max(1, int(src.h * s + 0.5))};
}

int map_x(int x, Size from, Size to) {
    return from.w > 0 ? int((long long)x * to.w * 2 / from.w + 1) / 2 : x;
}
int map_y(int y, Size from, Size to) {
    return from.h > 0 ? int((long long)y * to.h * 2 / from.h + 1) / 2 : y;
}

namespace {

void put_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63));
        out += char(0x80 | (cp & 63));
    } else {
        out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63));
        out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
    }
}

std::string decode_entities(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
        std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e.size() > 1 && e[0] == '#') {
            unsigned cp = (e[1] == 'x' || e[1] == 'X')
                              ? unsigned(std::strtoul(e.c_str() + 2, nullptr, 16))
                              : unsigned(std::strtoul(e.c_str() + 1, nullptr, 10));
            put_utf8(out, cp);
        } else { out += s[i]; continue; }
        i = semi;
    }
    return out;
}

// Attributes of one start tag: from just after "<node" to the closing '>'.
std::map<std::string, std::string> parse_attrs(const std::string& tag) {
    std::map<std::string, std::string> a;
    size_t i = 0;
    while (i < tag.size()) {
        while (i < tag.size() && std::isspace((unsigned char)tag[i])) ++i;
        size_t eq = tag.find('=', i);
        if (eq == std::string::npos) break;
        std::string key = tag.substr(i, eq - i);
        while (!key.empty() && std::isspace((unsigned char)key.back())) key.pop_back();
        size_t q = tag.find('"', eq);
        if (q == std::string::npos) break;
        size_t qe = tag.find('"', q + 1);
        if (qe == std::string::npos) break;
        a[key] = decode_entities(tag.substr(q + 1, qe - q - 1));
        i = qe + 1;
    }
    return a;
}

Rect parse_bounds(const std::string& b) {
    Rect r;
    std::sscanf(b.c_str(), "[%d,%d][%d,%d]", &r.l, &r.t, &r.r, &r.b);
    return r;
}

std::string lower_ascii(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool matches(const std::string& have, const std::string& want, bool exact) {
    if (want.empty()) return true;
    if (exact) return have == want;
    return lower_ascii(have).find(lower_ascii(want)) != std::string::npos;
}

} // namespace

bool parse_ui_dump(const std::string& xml, std::vector<UiNode>* out, std::string* err) {
    out->clear();
    if (xml.find("<hierarchy") == std::string::npos) {
        std::string e = xml.substr(0, 200);
        while (!e.empty() && std::isspace((unsigned char)e.back())) e.pop_back();
        if (err) *err = e.empty() ? "empty UI dump" : e;
        return false;
    }
    int depth = 0;
    size_t i = 0;
    while ((i = xml.find('<', i)) != std::string::npos) {
        if (xml.compare(i, 7, "</node>") == 0) { --depth; i += 7; continue; }
        if (xml.compare(i, 5, "<node") != 0) { ++i; continue; }
        size_t end = i + 5;
        bool in_q = false;
        while (end < xml.size() && (in_q || xml[end] != '>')) {
            if (xml[end] == '"') in_q = !in_q;
            ++end;
        }
        if (end >= xml.size()) break;
        bool self_close = xml[end - 1] == '/';
        auto a = parse_attrs(xml.substr(i + 5, end - i - 5 - (self_close ? 1 : 0)));
        UiNode n;
        n.text = a["text"];
        n.desc = a["content-desc"];
        n.id = a["resource-id"];
        if (auto p = n.id.find(":id/"); p != std::string::npos) n.id = n.id.substr(p + 4);
        n.cls = a["class"];
        if (auto p = n.cls.rfind('.'); p != std::string::npos) n.cls = n.cls.substr(p + 1);
        n.pkg = a["package"];
        n.bounds = parse_bounds(a["bounds"]);
        n.clickable = a["clickable"] == "true" || a["long-clickable"] == "true";
        n.scrollable = a["scrollable"] == "true";
        n.checked = a["checked"] == "true";
        n.focused = a["focused"] == "true";
        n.enabled = a["enabled"] != "false";
        n.depth = depth;
        out->push_back(std::move(n));
        if (!self_close) ++depth;
        i = end + 1;
    }
    if (out->empty()) {
        if (err) *err = "UI dump had no nodes";
        return false;
    }
    return true;
}

Size ui_root_size(const std::vector<UiNode>& nodes) {
    if (nodes.empty()) return {};
    return {nodes[0].bounds.r, nodes[0].bounds.b};
}

int parse_ui_rotation(const std::string& xml) {
    size_t h = xml.find("<hierarchy");
    if (h == std::string::npos) return 0;
    size_t r = xml.find("rotation=\"", h);
    if (r == std::string::npos || r > xml.find('>', h)) return 0;
    int v = std::atoi(xml.c_str() + r + 10);
    return v >= 0 && v <= 3 ? v : 0;
}

Size parse_wm_size(const std::string& out) {
    Size phys, over;
    auto grab = [&](const char* key, Size* s) {
        size_t p = out.find(key);
        if (p != std::string::npos) std::sscanf(out.c_str() + p + std::strlen(key), "%dx%d", &s->w, &s->h);
    };
    grab("Physical size:", &phys);
    grab("Override size:", &over);
    return over.w > 0 && over.h > 0 ? over : phys;
}

std::string format_ui_tree(const std::vector<UiNode>& nodes, Size root, Size shot) {
    std::string out;
    for (auto& n : nodes) {
        if (n.text.empty() && n.desc.empty() && n.id.empty() && !n.clickable && !n.scrollable)
            continue;
        out.append(size_t(std::min(n.depth, 20)) * 2, ' ');
        out += n.cls.empty() ? "node" : n.cls;
        if (!n.text.empty()) out += " \"" + n.text + "\"";
        if (!n.desc.empty()) out += " desc=\"" + n.desc + "\"";
        if (!n.id.empty()) out += " id=" + n.id;
        char b[80];
        std::snprintf(b, sizeof(b), " [%d,%d][%d,%d]", map_x(n.bounds.l, root, shot),
                      map_y(n.bounds.t, root, shot), map_x(n.bounds.r, root, shot),
                      map_y(n.bounds.b, root, shot));
        out += b;
        if (n.clickable) out += " clickable";
        if (n.scrollable) out += " scrollable";
        if (n.checked) out += " checked";
        if (n.focused) out += " focused";
        if (!n.enabled) out += " disabled";
        out += '\n';
    }
    return out;
}

std::vector<const UiNode*> find_nodes(const std::vector<UiNode>& nodes, const Selector& s) {
    std::vector<const UiNode*> r;
    if (s.text.empty() && s.id.empty() && s.desc.empty()) return r;
    for (auto& n : nodes)
        if (matches(n.text, s.text, s.exact) && matches(n.id, s.id, s.exact) &&
            matches(n.desc, s.desc, s.exact))
            r.push_back(&n);
    return r;
}

int keycode_for(const std::string& name) {
    static const std::map<std::string, int> k = {
        {"back", 4}, {"home", 3}, {"recents", 187}, {"power", 26}, {"enter", 66},
        {"delete", 67}, {"forward_delete", 112}, {"tab", 61}, {"escape", 111},
        {"menu", 82}, {"search", 84}, {"volume_up", 24}, {"volume_down", 25},
        {"volume_mute", 164}, {"dpad_up", 19}, {"dpad_down", 20}, {"dpad_left", 21},
        {"dpad_right", 22}, {"dpad_center", 23}, {"wake", 224}, {"sleep", 223},
    };
    if (!name.empty() && std::all_of(name.begin(), name.end(),
                                     [](char c) { return c >= '0' && c <= '9'; }))
        return name.size() <= 4 ? std::atoi(name.c_str()) : -1;
    auto it = k.find(lower_ascii(name));
    return it == k.end() ? -1 : it->second;
}

std::string truncate_output(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max) + "\n[output truncated: " + std::to_string(s.size()) +
           " bytes, showing the first " + std::to_string(max) + "]";
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 |
                     (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i < in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned char)in[i + 1] << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

} // namespace opm::mcp
