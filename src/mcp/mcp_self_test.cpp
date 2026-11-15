// `PhoneMimi.exe --mcp-self-test`: checks the MCP pieces without a phone.

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>

#ifdef ENABLE_ANDROID
#include <opm/android/scrcpy_receiver.h>
#include <opm/android/scrcpy_util.h>
#include <opm/android/setup_hint.h>
#endif
#include <opm/mcp/frame_tap.h>
#include <opm/mcp/mcp_protocol.h>
#include <opm/mcp/mcp_server.h>
#include <opm/mcp/mcp_tools.h>
#include <opm/mcp/mcp_util.h>

using nlohmann::json;

namespace opm::mcp {

namespace {

int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fails;                                                           \
            std::cout << "FAIL " << __LINE__ << ": " #cond "\n";                 \
        }                                                                        \
    } while (0)

json call(const ToolTable& t, const json& msg) {
    std::string out = handle_jsonrpc(msg.dump(), t);
    return out.empty() ? json() : json::parse(out);
}

json rpc(const char* method, json params = json::object(), int id = 1) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

std::string text_of(const json& r) {
    std::string s;
    for (auto& c : r["result"]["content"])
        if (c["type"] == "text") s += c["text"].get<std::string>();
    return s;
}
bool is_err(const json& r) { return r["result"].value("isError", false); }

const char* const kDump =
    "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation=\"0\">"
    "<node index=\"0\" text=\"\" resource-id=\"\" class=\"android.widget.FrameLayout\" "
    "package=\"com.app\" content-desc=\"\" clickable=\"false\" bounds=\"[0,0][1080,2400]\">"
    "<node index=\"0\" text=\"\" resource-id=\"\" class=\"android.widget.LinearLayout\" "
    "package=\"com.app\" content-desc=\"\" clickable=\"false\" bounds=\"[0,0][1080,2400]\">"
    "<node index=\"0\" text=\"\xC4\x90\xC4\x83ng nh\xE1\xBA\xADp\" resource-id=\"com.app:id/login\" "
    "class=\"android.widget.Button\" package=\"com.app\" content-desc=\"\" clickable=\"true\" "
    "bounds=\"[100,2000][980,2160]\" />"
    "<node index=\"1\" text=\"A &amp; B&#10;C\" resource-id=\"\" class=\"android.widget.TextView\" "
    "package=\"com.app\" content-desc=\"\" clickable=\"false\" bounds=\"[0,100][1080,200]\" />"
    "</node></node></hierarchy>UI hierchary dumped to: /dev/tty\n";

// ---- Task 1 ----
void test_protocol() {
    ToolTable t = {{"echo", "Echo", {{"type", "object"}},
                    [](const json& a) { return text_result(a.value("s", "")); }},
                   {"boom", "Throws", {{"type", "object"}},
                    [](const json&) -> ToolResult { throw std::runtime_error("x"); }}};
    json r = call(t, rpc("initialize", {{"protocolVersion", "2025-03-26"}}));
    CHECK(r["result"]["protocolVersion"] == "2025-03-26");
    CHECK(r["result"]["capabilities"].contains("tools"));
    CHECK(r["result"]["serverInfo"]["name"] == "phonemimi");
    CHECK(r["id"] == 1);
    r = call(t, rpc("initialize", {{"protocolVersion", "1999-01-01"}}));
    CHECK(r["result"]["protocolVersion"] == "2025-06-18");
    CHECK(handle_jsonrpc(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", t).empty());
    r = call(t, rpc("tools/list"));
    CHECK(r["result"]["tools"].size() == 2);
    CHECK(r["result"]["tools"][0]["name"] == "echo");
    CHECK(r["result"]["tools"][0].contains("inputSchema"));
    r = call(t, rpc("tools/call", {{"name", "echo"}, {"arguments", {{"s", "hi"}}}}));
    CHECK(text_of(r) == "hi" && !is_err(r));
    CHECK(call(t, rpc("tools/call", {{"name", "nope"}}))["error"]["code"] == -32602);
    CHECK(call(t, rpc("bogus/method"))["error"]["code"] == -32601);
    CHECK(json::parse(handle_jsonrpc("{bad", t))["error"]["code"] == -32700);
    CHECK(json::parse(handle_jsonrpc("[]", t))["error"]["code"] == -32600);
    // Wrong types must answer with an error, never throw (review fix).
    CHECK(json::parse(handle_jsonrpc(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":5}})", t))
              ["error"]["code"] == -32602);
    CHECK(json::parse(handle_jsonrpc(R"({"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":5}})", t))
              .contains("error"));
    CHECK(is_err(call(t, rpc("tools/call", {{"name", "boom"}}))));

    HttpRequest q;
    q.method = "POST";
    q.path = "/mcp";
    q.body = rpc("ping").dump();
    CHECK(handle_http(q, "tok", 47290, t).status == 401);
    q.headers["authorization"] = "Bearer bad";
    CHECK(handle_http(q, "tok", 47290, t).status == 401);
    q.headers["authorization"] = "Bearer tok";
    CHECK(handle_http(q, "tok", 47290, t).status == 200);
    q.headers["origin"] = "http://evil.com";
    CHECK(handle_http(q, "tok", 47290, t).status == 403);
    q.headers["origin"] = "http://localhost:47290";
    CHECK(handle_http(q, "tok", 47290, t).status == 200);
    q.body = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    auto nr = handle_http(q, "tok", 47290, t);
    CHECK(nr.status == 202 && nr.body.empty());
    q.method = "GET";
    CHECK(handle_http(q, "tok", 47290, t).status == 405);
    q.method = "POST";
    q.path = "/other";
    CHECK(handle_http(q, "tok", 47290, t).status == 404);

    std::string a = make_token(), b = make_token();
    CHECK(a.size() == 32 && a != b);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(setup_command(47290, "ab") ==
          "claude mcp add --transport http phonemimi http://127.0.0.1:47290/mcp --header "
          "\"Authorization: Bearer ab\"");
}

// ---- Task 2 ----
void test_util() {
    Size s = shot_size({1080, 2400});
    CHECK(s.w == 576 && s.h == 1280);
    s = shot_size({2400, 1080});
    CHECK(s.w == 1280 && s.h == 576);
    s = shot_size({720, 1280});
    CHECK(s.w == 720 && s.h == 1280);
    s = shot_size({600, 400});
    CHECK(s.w == 600 && s.h == 400);
    CHECK(map_x(540, {1080, 2400}, {576, 1280}) == 288);

    std::vector<UiNode> n;
    std::string e;
    CHECK(parse_ui_dump(kDump, &n, &e));
    CHECK(n.size() == 4);
    if (n.size() == 4) {
        CHECK(n[2].text == "\xC4\x90\xC4\x83ng nh\xE1\xBA\xADp");
        CHECK(n[2].id == "login" && n[2].cls == "Button" && n[2].clickable);
        CHECK(n[2].depth == 2);
        CHECK(n[3].text == "A & B\nC");
    }
    std::string tree = format_ui_tree(n, {1080, 2400}, {576, 1280});
    CHECK(tree.find("id=login") != std::string::npos);
    CHECK(tree.find("[53,1067][523,1152]") != std::string::npos);
    CHECK(tree.find("LinearLayout") == std::string::npos);
    CHECK(find_nodes(n, {"\xC4\x90\xC4\x83ng", "", "", false}).size() == 1);
    CHECK(find_nodes(n, {"login", "", "", false}).empty());
    CHECK(find_nodes(n, {"", "login", "", true}).size() == 1);
    CHECK(find_nodes(n, {"nope", "", "", false}).empty());
    CHECK(!parse_ui_dump("ERROR: could not get idle state.\n", &n, &e));
    CHECK(e.find("idle") != std::string::npos);
    Size wm = parse_wm_size("Physical size: 1080x2400\n");
    CHECK(wm.w == 1080 && wm.h == 2400);
    wm = parse_wm_size("Physical size: 1080x2400\nOverride size: 720x1600\n");
    CHECK(wm.w == 720 && wm.h == 1600);
    CHECK(parse_wm_size("error").w == 0);
    CHECK(parse_ui_rotation(kDump) == 0);
    CHECK(parse_ui_rotation("<hierarchy rotation=\"1\"><node/></hierarchy>") == 1);

    const std::pair<const char*, int> keys[] = {
        {"back", 4}, {"home", 3}, {"recents", 187}, {"power", 26}, {"enter", 66},
        {"delete", 67}, {"forward_delete", 112}, {"tab", 61}, {"escape", 111}, {"menu", 82},
        {"search", 84}, {"volume_up", 24}, {"volume_down", 25}, {"volume_mute", 164},
        {"dpad_up", 19}, {"dpad_down", 20}, {"dpad_left", 21}, {"dpad_right", 22},
        {"dpad_center", 23}, {"wake", 224}, {"sleep", 223}, {"82", 82}, {"bogus", -1}};
    for (auto& k : keys) CHECK(keycode_for(k.first) == k.second);

    std::string big = truncate_output(std::string(70000, 'x'));
    CHECK(big.size() < 66000 && big.find("truncated") != std::string::npos);
    CHECK(base64("Man") == "TWFu" && base64("Ma") == "TWE=" && base64("M") == "TQ==");
}

// ---- Task 3 ----
void test_frame_tap() {
    FrameTap tap;
    Frame f;
    CHECK(!tap.get("a", &f, 0));
    std::vector<uint8_t> px(20 * 2, 7);   // 4x2, stride 20
    tap.offer_rgba("a", px.data(), 4, 2, 20);
    CHECK(tap.get("a", &f, 0));
    CHECK(f.w == 4 && f.h == 2 && f.rgba.size() == 32);
    std::fill(px.begin(), px.end(), uint8_t(9));
    tap.offer_rgba("a", px.data(), 4, 2, 20);    // within 250 ms, nobody waiting: ignored
    tap.get("a", &f, 0);
    CHECK(f.rgba[0] == 7);
    std::atomic<int> seen{0};
    std::thread th([&] {
        Frame g;
        if (tap.get("a", &g, 300)) seen = g.rgba[0];
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    tap.offer_rgba("a", px.data(), 4, 2, 20);
    th.join();
    CHECK(seen == 9);
    tap.drop("a");
    CHECK(!tap.get("a", &f, 0));
}

// ---- Task 4 ----
struct FakeHost : Host {
    std::vector<DeviceRow> rows;
    std::vector<Input> sent;
    std::map<std::string, std::pair<int, std::string>> adb_replies;
    std::vector<std::string> calls;
    bool send_ok = true;
    bool is_closing = false;
    Frame fr;
    bool closing() override { return is_closing; }
    int quits = 0;
    void quit() override { ++quits; }

    std::vector<DeviceRow> devices() override { return rows; }
    bool frame(const std::string& id, Frame* out, int) override {
        if (id != "A1" && id != "I1") return false;
        *out = fr;
        return true;
    }
    bool send(const std::string&, const Input& in) override {
        if (!send_ok) return false;
        sent.push_back(in);
        return true;
    }
    int adb(const std::string&, const std::vector<std::string>& args, std::string* out, int) override {
        std::string k;
        for (auto& a : args) k += (k.empty() ? "" : " ") + a;
        calls.push_back(k);
        auto it = adb_replies.find(k);
        if (it == adb_replies.end()) { if (out) out->clear(); return 0; }
        if (out) *out = it->second.second;
        return it->second.first;
    }
    std::string connect_usb() override { return "Mirroring usb"; }
    std::string connect_wifi(const std::string& a) override { return "Mirroring " + a; }
    media::OcrJobResult ocr(const Frame&) override {
        media::OcrJobResult r;
        r.ok = true;
        r.lines.push_back({"Hello world", 100, 200, 300, 40});
        return r;
    }
};

json tool(const ToolTable& t, const char* name, json args) {
    return call(t, rpc("tools/call", {{"name", name}, {"arguments", args}}));
}

void test_tools() {
    FakeHost h;
    h.rows = {{"A1", "Pixel", "android", true, true, true}, {"I1", "iPhone", "iphone", false, true, false}};
    h.fr.w = 1080;
    h.fr.h = 2400;
    h.fr.rgba.assign(size_t(1080) * 2400 * 4, 128);
    h.adb_replies["exec-out uiautomator dump /dev/tty"] = {0, kDump};
    ToolTable t = make_tools(h);
    CHECK(t.size() == 29);
    CHECK(!is_err(tool(t, "quit_app", json::object())) && h.quits == 1);

    json r = tool(t, "tap", {{"x", 288}, {"y", 640}});
    CHECK(!is_err(r) && h.sent.size() == 2);
    if (h.sent.size() == 2) {
        CHECK(h.sent[0].kind == Input::Touch && h.sent[0].action == 0 && h.sent[1].action == 1);
        CHECK(h.sent[0].x == 540 && h.sent[0].y == 1200);
    }
    h.sent.clear();
    r = tool(t, "tap", {{"text", "\xC4\x90\xC4\x83ng"}});
    CHECK(!is_err(r) && text_of(r).find("login") != std::string::npos);
    CHECK(!h.sent.empty() && h.sent[0].x == 540 && h.sent[0].y == 2080);
    r = tool(t, "tap", {{"text", "nope"}});
    CHECK(is_err(r) && text_of(r).find("login") != std::string::npos);

    h.sent.clear();
    tool(t, "type_text", {{"text", "xin ch\xC3\xA0o"}});
    CHECK(h.sent.size() == 1 && h.sent[0].kind == Input::Clipboard);
    h.sent.clear();
    tool(t, "type_text", {{"text", "hello"}, {"submit", true}});
    CHECK(h.sent.size() == 3 && h.sent[0].kind == Input::Text && h.sent[1].keycode == 66 &&
          h.sent[1].action == 0 && h.sent[2].action == 1);

    CHECK(is_err(tool(t, "press_key", {{"key", "bogus"}})));
    r = tool(t, "tap", {{"device", "I1"}, {"x", 1}, {"y", 1}});
    CHECK(is_err(r) && text_of(r).find("view-only") != std::string::npos);
    r = tool(t, "screenshot", {{"device", "I1"}});
    CHECK(!is_err(r) && r["result"]["content"][0]["type"] == "image");

    r = tool(t, "screenshot", {});
    CHECK(!is_err(r));
    if (!is_err(r)) {
        // Decode the PNG IHDR width/height from the base64 header.
        std::string b64 = r["result"]["content"][0]["data"];
        static const std::string al = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string raw;
        unsigned v = 0;
        int bits = 0;
        for (size_t i = 0; i < 40 && i < b64.size(); ++i) {
            v = (v << 6) | unsigned(al.find(b64[i]));
            bits += 6;
            if (bits >= 8) { bits -= 8; raw += char((v >> bits) & 0xFF); }
        }
        auto be = [&](size_t o) {
            return (unsigned char)raw[o] << 24 | (unsigned char)raw[o + 1] << 16 |
                   (unsigned char)raw[o + 2] << 8 | (unsigned char)raw[o + 3];
        };
        CHECK(raw.size() >= 24 && be(16) == 576u && be(20) == 1280u);
    }

    h.send_ok = false;
    r = tool(t, "tap", {{"x", 10}, {"y", 10}});
    CHECK(is_err(r) && text_of(r).find("connect_wifi") != std::string::npos);
    h.send_ok = true;

    h.adb_replies["exec-out uiautomator dump /dev/tty"] = {0, "ERROR: could not get idle state.\n"};
    for (int i = 1; i <= 8; ++i)
        h.adb_replies["exec-out cat /data/local/tmp/pm_ui_" + std::to_string(i) + ".xml"] = {1, "No such file"};
    r = tool(t, "ui_tree", {});
    CHECK(is_err(r) && text_of(r).find("ocr_screen") != std::string::npos);
    // Each fallback dump uses its own temp file and removes it (review minor).
    h.calls.clear();
    tool(t, "ui_tree", {});
    tool(t, "ui_tree", {});
    std::vector<std::string> files;
    int removed = 0;
    for (auto& c : h.calls) {
        if (c.rfind("shell uiautomator dump /data/local/tmp/pm_ui_", 0) == 0) files.push_back(c.substr(23));
        if (c.rfind("shell rm -f /data/local/tmp/pm_ui_", 0) == 0) ++removed;
    }
    CHECK(files.size() == 2 && files[0] != files[1] && removed == 2);
    h.adb_replies["exec-out uiautomator dump /dev/tty"] = {0, kDump};

    h.adb_replies["shell sleep 999"] = {-2, "partial"};
    r = tool(t, "adb_shell", {{"command", "sleep 999"}});
    CHECK(is_err(r) && text_of(r).find("timed out") != std::string::npos);

    r = tool(t, "wait_for", {{"text", "\xC4\x90\xC4\x83ng"}, {"timeout_ms", 1000}});
    CHECK(!is_err(r) && text_of(r).find("Found") == 0);
    r = tool(t, "wait_for", {{"text", "zzz"}, {"timeout_ms", 600}});
    CHECK(!is_err(r) && text_of(r).find("Not found") == 0);
    r = tool(t, "wait_for", {{"device", "I1"}, {"text", "hello"}, {"timeout_ms", 100}});
    CHECK(text_of(r).find("Found") == 0);

    r = tool(t, "ocr_screen", {});
    CHECK(text_of(r).find("Hello world [53,107,160,21]") == 0);

    // A dialog: the dump's root is only the dialog window, bounds are still
    // screen pixels, so taps must map with the display size (review fix).
    h.adb_replies["shell wm size"] = {0, "Physical size: 1080x2400\n"};
    h.adb_replies["exec-out uiautomator dump /dev/tty"] = {0,
        "<hierarchy rotation=\"0\"><node text=\"\" class=\"android.widget.FrameLayout\" "
        "bounds=\"[42,856][1038,1544]\"><node text=\"OK\" class=\"android.widget.Button\" "
        "clickable=\"true\" bounds=\"[800,1400][1000,1500]\" /></node></hierarchy>"};
    h.sent.clear();
    r = tool(t, "tap", {{"text", "OK"}, {"exact", true}});
    CHECK(!is_err(r) && !h.sent.empty() && h.sent[0].x == 900 && h.sent[0].y == 1450);
    r = tool(t, "ui_tree", {});
    CHECK(text_of(r).find("[427,747][533,800]") != std::string::npos);

    // wait_for must not claim "Gone" when it cannot look (review fix).
    r = tool(t, "wait_for", {{"device", "I1"}, {"id", "login"}, {"timeout_ms", 100}});
    CHECK(is_err(r));
    h.adb_replies["exec-out uiautomator dump /dev/tty"] = {0, "ERROR: could not get idle state.\n"};
    r = tool(t, "wait_for", {{"id", "login"}, {"gone", true}, {"timeout_ms", 1500}});
    CHECK(text_of(r).find("Gone") == std::string::npos);
    h.adb_replies["exec-out uiautomator dump /dev/tty"] = {0, kDump};

    // logcat filters: -t counts before filtering, so a wide window is read
    // and trimmed here (found on the real phone).
    h.adb_replies["logcat -d -v threadtime -t 20000 *:W"] = {0, "w1\nw2\nw3\n"};
    r = tool(t, "logcat", {{"level", "w"}, {"lines", 2}});
    CHECK(text_of(r) == "w2\nw3\n");

    // App exit: a long wait_for ends at once.
    h.is_closing = true;
    auto w0 = std::chrono::steady_clock::now();
    tool(t, "wait_for", {{"text", "zzz"}, {"timeout_ms", 30000}});
    CHECK(std::chrono::steady_clock::now() - w0 < std::chrono::seconds(3));
    h.is_closing = false;

    h.rows = {};
    r = tool(t, "tap", {{"x", 1}, {"y", 1}});
    CHECK(is_err(r) && text_of(r).find("connect_usb") != std::string::npos);
}

// ---- Task 5 ----
std::string roundtrip(SOCKET s, const std::string& req) {
    send(s, req.data(), int(req.size()), 0);
    std::string got;
    char b[4096];
    for (;;) {
        size_t he = got.find("\r\n\r\n");
        if (he != std::string::npos) {
            size_t cl = got.find("Content-Length: ");
            size_t len = cl == std::string::npos ? 0 : std::stoul(got.substr(cl + 16));
            if (got.size() >= he + 4 + len) return got;
        }
        int n = recv(s, b, sizeof(b), 0);
        if (n <= 0) return got;
        got.append(b, size_t(n));
    }
}

SOCKET dial(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(u_short(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD to = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    return s;
}

std::string post(const std::string& body, const std::string& auth) {
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n" +
           (auth.empty() ? std::string() : "Authorization: " + auth + "\r\n") +
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

void test_server() {
    McpServer srv(ToolTable{});
    CHECK(srv.start(0, "tok").empty());
    int port = srv.port();
    CHECK(port > 0);
    SOCKET s = dial(port);
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET) return;
    std::string init = rpc("initialize").dump();
    CHECK(roundtrip(s, post(init, "")).rfind("HTTP/1.1 401", 0) == 0);
    std::string r1 = roundtrip(s, post(init, "Bearer tok"));
    CHECK(r1.rfind("HTTP/1.1 200", 0) == 0 && r1.find("phonemimi") != std::string::npos);
    std::string r2 = roundtrip(s, post(rpc("ping", json::object(), 2).dump(), "Bearer tok"));
    CHECK(r2.rfind("HTTP/1.1 200", 0) == 0);
    closesocket(s);
    s = dial(port);
    std::string huge = "POST /mcp HTTP/1.1\r\nAuthorization: Bearer tok\r\nContent-Length: 2097152\r\n\r\n";
    CHECK(roundtrip(s, huge).rfind("HTTP/1.1 413", 0) == 0);
    closesocket(s);
    auto t0 = std::chrono::steady_clock::now();
    srv.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    s = dial(port);
    CHECK(s == INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
}

#ifdef ENABLE_ANDROID
// adb runs through the same spawner; cmd.exe stands in for adb (review fixes).
void test_spawn() {
    android::AdbController a;
    a.set_adb_path("cmd.exe");
    std::string out;
    auto t0 = std::chrono::steady_clock::now();
    int rc = a.run({"/c", "for /l %i in (0,0,1) do @echo yyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"}, &out, 1500);
    CHECK(rc == -2);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6));
    CHECK(!out.empty() && out.size() <= (8u << 20) + 8192);

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec) / fs::path(u8"pm_mcp_Dự án.txt");
    { std::ofstream(p) << "x"; }
    auto u8 = p.u8string();
    std::string path(u8.begin(), u8.end());
    out.clear();
    rc = a.run({"/c", "if", "exist", path, "echo", "YES"}, &out, 5000);
    CHECK(rc == 0 && out.find("YES") != std::string::npos);
    fs::remove(p, ec);
}

// 3.5.0: cable setup hint, device clipboard messages, audio resampler.
void test_android_helpers() {
    using namespace opm::android;
    UsbId x = parse_usb_id("USB\\VID_2717&PID_FF48\\fc7639210013");
    CHECK(x.vid == 0x2717 && x.pid == 0xFF48);
    CHECK(parse_usb_id("HID\\VID_046D&PID_C52B").vid == 0);
    CHECK(brand_for(0x04E8) == Brand::Samsung && brand_for(0x2717) == Brand::Xiaomi &&
          brand_for(0x22D9) == Brand::Oppo && brand_for(0x18D1) == Brand::Other);
    std::vector<UsbId> xiaomi = {{0x046D, 0xC52B}, {0x2717, 0xFF48}};
    auto st = decide_setup(xiaomi, false, false);
    CHECK(st.hint == SetupHint::DebuggingOff && st.brand == Brand::Xiaomi);
    CHECK(decide_setup(xiaomi, true, false).hint == SetupHint::None);
    CHECK(decide_setup(xiaomi, false, true).hint == SetupHint::Unauthorized);
    CHECK(decide_setup({{0x05AC, 0x12A8}}, false, false).hint == SetupHint::IPhoneCable);
    CHECK(decide_setup({{0x05AC, 0x024F}}, false, false).hint == SetupHint::None);  // Apple keyboard
    CHECK(decide_setup({}, false, false).hint == SetupHint::None);

    // clipboard "hi" + ack + UHID(2 bytes) + start of clipboard "abc"
    std::vector<uint8_t> m = {0, 0, 0, 0, 2, 'h', 'i', 1, 0, 0, 0, 0, 0, 0, 0, 7,
                              2, 0, 1, 0, 2, 9, 9, 0, 0, 0, 0, 3, 'a'};
    std::vector<std::string> clips;
    size_t used = parse_device_messages(m.data(), m.size(), &clips);
    CHECK(clips.size() == 1 && clips[0] == "hi" && used == 23);
    m.erase(m.begin(), m.begin() + (std::ptrdiff_t)used);
    m.push_back('b');
    m.push_back('c');
    clips.clear();
    used = parse_device_messages(m.data(), m.size(), &clips);
    CHECK(clips.size() == 1 && clips[0] == "abc" && used == m.size());
    uint8_t bad[] = {9, 0};
    CHECK(parse_device_messages(bad, 2, &clips) == SIZE_MAX);

    // 4800 frames of 48 kHz in two calls -> about 4410 frames of 44.1 kHz.
    PcmResampler48to441 rs;
    std::vector<int16_t> in(4800 * 2, 1000), out;
    rs.feed(in.data(), 2400, &out);
    rs.feed(in.data() + 4800, 2400, &out);
    CHECK(out.size() / 2 >= 4408 && out.size() / 2 <= 4411);
    CHECK(!out.empty() && out[0] == 1000 && out.back() == 1000);
}
#endif

} // namespace

int run_self_test() {
    test_protocol();
    test_util();
    test_frame_tap();
    test_tools();
    test_server();
#ifdef ENABLE_ANDROID
    test_spawn();
    test_android_helpers();
#endif
    if (g_fails == 0)
        std::cout << "mcp self-test: PASS (" << g_checks << " checks)\n";
    else
        std::cout << "mcp self-test: FAIL (" << g_fails << " of " << g_checks << ")\n";
    std::cout.flush();
    return g_fails == 0 ? 0 : 2;
}

} // namespace opm::mcp
