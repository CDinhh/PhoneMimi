# PhoneMimi MCP Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An MCP server inside PhoneMimi.exe that lets Claude Code see, read and drive connected phones and do Android dev chores.

**Architecture:** A small HTTP/1.1 server on 127.0.0.1 speaks MCP Streamable HTTP (plain JSON replies). Pure pieces (HTTP routing, JSON-RPC, uiautomator parsing, coordinate mapping, key names) are tested by `--mcp-self-test`. Tools talk to the app only through an `opm::mcp::Host` interface that `App` implements; frames come from a throttled per-device `FrameTap` fed by the existing video callbacks.

**Tech Stack:** C++20, Winsock, nlohmann/json 3.11.3 (vendored single header), existing scrcpy control channel, `AdbController`, Windows.Media.Ocr, stb_image_write.

**Spec:** `docs/superpowers/specs/2026-11-15-phonemimi-mcp-server-design.md`

## Global Constraints

- Listen on 127.0.0.1 only; default port 47290; off by default (`mcp_enabled=false`).
- Every request needs `Authorization: Bearer <mcp_token>` (32 hex chars from `BCryptGenRandom`), else 401. A present `Origin` that is not `http://127.0.0.1[:port]` / `http://localhost[:port]` gets 403.
- Never raise, show or focus a window from MCP code. Never block the SDL thread.
- Coordinates in every tool are screenshot pixels: long side at most 1280, never upscaled.
- Code and log strings ASCII; log with `std::cout << "[MCP] "`; never log the token or typed text (log its length).
- Protocol versions known: `2025-03-26`, `2025-06-18` (default reply `2025-06-18`). Server name `phonemimi`.
- No local toolchain: build check = `gh workflow run release.yml -R CDinhh/PhoneMimi --ref feat/mcp-server`, then download the portable artifact and run `PhoneMimi.exe --mcp-self-test` (expects `mcp self-test: PASS`, exit 0).
- Version 3.4.0.

## Review Focus

1. uiautomator fails during animations/video (`ERROR: could not get idle state`): tool returns `isError` with "retry or use ocr_screen"; `wait_for` falls back to OCR. Test in Task 2/4.
2. Phone rotates or the stream size changes mid-session: screenshot space comes from the current frame every call. Test in Task 2.
3. Phone drops mid-call (Wi-Fi): `send_input` returns false, tool says "call connect_wifi"; nothing crashes. Test in Task 4.
4. Vietnamese / non-ASCII: `type_text` uses the clipboard paste path; XML entities and UTF-8 in dumps decode. Test in Task 2/4.
5. Hanging or huge adb output (`top`, long logcat): adb runs with a timeout and kills the process; tool output truncated at 64 KB with a note. Test in Task 4.

---

### Task 1: JSON-RPC + HTTP core and the self-test

**Files:**
- Create: `lib/nlohmann/json.hpp` (v3.11.3 single_include), `include/opm/mcp/mcp_protocol.h`, `src/mcp/mcp_protocol.cpp`, `src/mcp/mcp_self_test.cpp`
- Modify: `CMakeLists.txt` (option `ENABLE_MCP` ON, sources, `ENABLE_MCP=1`, `lib` include dir), `src/main.cpp` (`--mcp-self-test`, help line), `.github/workflows/release.yml` (self-test step after "Build MSI"), `installer/THIRD_PARTY_LICENSES.txt`

**Interfaces (produces):**
```cpp
namespace opm::mcp {
struct HttpRequest  { std::string method, path; std::map<std::string,std::string> headers; /*lower-case keys*/ std::string body; };
struct HttpResponse { int status = 200; std::string content_type; std::string body; };
struct ToolResult   { nlohmann::json content = nlohmann::json::array(); bool is_error = false; };
ToolResult text_result(const std::string& text, bool is_error = false);
struct Tool { std::string name, description; nlohmann::json input_schema;
              std::function<ToolResult(const nlohmann::json& args)> run; };
using ToolTable = std::vector<Tool>;
std::string handle_jsonrpc(const std::string& body, const ToolTable& tools); // "" = notification
HttpResponse handle_http(const HttpRequest& req, const std::string& token, int port, const ToolTable& tools);
std::string make_token();                                   // 32 hex
std::string setup_command(int port, const std::string& token);
int run_self_test();                                        // 0 pass, 2 fail
}
```

- [ ] **Step 1: Write self-test cases** in `mcp_self_test.cpp` (a `CHECK(cond)` macro that prints `FAIL <line>: <expr>` and counts; end prints `mcp self-test: PASS` or `mcp self-test: FAIL (n)`):
  - `initialize` with `protocolVersion:"2025-03-26"` echoes it; with `"1999-01-01"` returns `"2025-06-18"`; result has `capabilities.tools` and `serverInfo.name=="phonemimi"`.
  - `notifications/initialized` -> `handle_jsonrpc` returns `""`; `handle_http` returns 202 empty body.
  - `tools/list` with a one-tool table lists `name`, `description`, `inputSchema`.
  - `tools/call` unknown tool -> error code `-32602`; unknown method -> `-32601`; body `"{bad"` -> `-32700`; a JSON array body -> `-32600`.
  - `tools/call` on a tool that throws -> result `isError:true` (not a crash).
  - `handle_http`: no auth -> 401; wrong token -> 401; `Origin: http://evil.com` with right token -> 403; `Origin: http://localhost:47290` -> 200; `GET /mcp` -> 405; `POST /other` -> 404.
  - `make_token()` length 32, all hex, two calls differ; `setup_command(47290,"ab")` == `claude mcp add --transport http phonemimi http://127.0.0.1:47290/mcp --header "Authorization: Bearer ab"`.
- [ ] **Step 2: Implement** `mcp_protocol.cpp` to the interface. `tools/call` result JSON: `{content, isError}`. Responses keep the request `id`. `initialize` includes `instructions` (the spec's few lines).
- [ ] **Step 3: Wire** `main.cpp`: `--mcp-self-test` returns `opm::mcp::run_self_test()` before anything touches settings/network (next to `--srp-self-test`). CI step:
  ```yaml
  - name: MCP self-test
    shell: pwsh
    run: |
      $p = Start-Process dist\stage\PhoneMimi.exe -ArgumentList '--mcp-self-test' -Wait -PassThru -NoNewWindow -RedirectStandardOutput mcp.txt
      Get-Content mcp.txt
      if ($p.ExitCode -ne 0) { exit 1 }
  ```
- [ ] **Step 4: Run CI**; expect the "MCP self-test" step green with `mcp self-test: PASS`.
- [ ] **Step 5: Commit** `MCP: JSON-RPC/HTTP core with self-test`.

### Task 2: Pure helpers: screen space, UI dump parser, keys, OCR boxes

**Files:**
- Create: `include/opm/mcp/mcp_util.h`, `src/mcp/mcp_util.cpp`
- Modify: `include/opm/media/ocr.h`, `src/media/ocr.cpp` (line boxes), `src/mcp/mcp_self_test.cpp`

**Interfaces (produces):**
```cpp
namespace opm::mcp {
struct Size { int w = 0, h = 0; };
struct Rect { int l = 0, t = 0, r = 0, b = 0; };
Size shot_size(Size src, int max_side = 1280);        // keeps aspect, never upscales
int  map_x(int x, Size from, Size to);                // x * to.w / from.w, rounded
int  map_y(int y, Size from, Size to);
struct UiNode { std::string text, desc, id, cls, pkg; Rect bounds;
                bool clickable=false, scrollable=false, checked=false, focused=false, enabled=true; int depth=0; };
// Returns false and sets *err on non-XML / uiautomator error text.
bool parse_ui_dump(const std::string& xml, std::vector<UiNode>* out, std::string* err);
Size ui_root_size(const std::vector<UiNode>& nodes);  // bounds of the first node
std::string format_ui_tree(const std::vector<UiNode>& nodes, Size shot); // bounds mapped root->shot
struct Selector { std::string text, id, desc; bool exact = false; };
std::vector<const UiNode*> find_nodes(const std::vector<UiNode>& nodes, const Selector& s);
int keycode_for(const std::string& name);             // -1 unknown; digits parse as a code
std::string truncate_output(const std::string& s, size_t max = 65536);
}
// ocr.h: struct OcrLine { std::string text; int x, y, w, h; };  OcrJobResult gains std::vector<OcrLine> lines;
```

- [ ] **Step 1: Self-test cases:**
  - `shot_size({1080,2400})=={576,1280}`; `shot_size({2400,1080})=={1280,576}` (rotation); `shot_size({720,1280})=={720,1280}`; `shot_size({600,400})=={600,400}`.
  - `map_x(540,{1080,2400},{576,1280})==288`.
  - A sample dump (inline string, 4 nodes: root `[0,0][1080,2400]`, a FrameLayout with no text, a Button `text="Đăng nhập"` `resource-id="com.app:id/login"` `clickable="true"` `bounds="[100,2000][980,2160]"`, a TextView `text="A &amp; B&#10;C"`) parses to 4 nodes; button text is UTF-8 "Đăng nhập", id stripped to `login`, TextView text `"A & B\nC"`.
  - `format_ui_tree` output contains `login` and `[53,1067][523,1152]` and omits the textless non-clickable FrameLayout line.
  - `find_nodes` `{text:"Đăng"}` (substring, ASCII-only case folding) finds the button; `{text:"login"}` finds nothing (text, not id); `{id:"login",exact:true}` finds it; `{text:"nope"}` empty.
  - `parse_ui_dump("ERROR: could not get idle state.")` returns false with err containing `idle`.
  - `keycode_for`: back 4, home 3, recents 187, power 26, enter 66, delete 67, forward_delete 112, tab 61, escape 111, menu 82, search 84, volume_up 24, volume_down 25, volume_mute 164, dpad_up 19, dpad_down 20, dpad_left 21, dpad_right 22, dpad_center 23, wake 224, sleep 223, `"82"` 82, `"bogus"` -1.
  - `truncate_output(std::string(70000,'x')).size() < 66000` and ends with a note containing `truncated`.
- [ ] **Step 2: Implement.** Hand-rolled attribute scanner over `<node .../>` (track depth by `<node` / `</node>`); decode `&amp; &lt; &gt; &quot; &apos; &#N; &#xN;` to UTF-8. Folding of a node line: skip if no text, desc, id and not clickable/scrollable. Line format: `<indent>Class "text" desc="..." id=... [l,t][r,b] clickable scrollable checked focused disabled`.
- [ ] **Step 3: OCR boxes:** in `run_ocr_rgba`, for each line, union the `BoundingRect()` of its words into `OcrLine`; keep `text` joined as today.
- [ ] **Step 4: Run CI**; `mcp self-test: PASS`.
- [ ] **Step 5: Commit** `MCP: screen space, UI dump parser, key names, OCR line boxes`.

### Task 3: Host interface, FrameTap, adb with timeout

**Files:**
- Create: `include/opm/mcp/mcp_host.h`, `include/opm/mcp/frame_tap.h`, `src/mcp/frame_tap.cpp`
- Modify: `include/opm/android/scrcpy_receiver.h`, `src/android/scrcpy_receiver.cpp` (`spawn_capture` timeout + public `run`), `src/mcp/mcp_self_test.cpp`

**Interfaces (produces):**
```cpp
namespace opm::mcp {
struct Frame { std::vector<uint8_t> rgba; int w = 0, h = 0; };   // packed RGBA
struct DeviceRow { std::string id, name, kind; /*"android"|"iphone"*/ bool shown=false, mirrored=false, control=false; };
struct Input { enum Kind { Touch, Key, Text, Clipboard, ScreenPower } kind = Touch;
               int action = 0; int x = 0, y = 0; int keycode = 0; std::string text; }; // x,y in frame pixels
class Host { public: virtual ~Host() = default;
  virtual std::vector<DeviceRow> devices() = 0;
  virtual bool frame(const std::string& id, Frame* out, int wait_ms) = 0;   // false: no stream
  virtual bool send(const std::string& id, const Input& in) = 0;            // false: no control channel
  virtual int  adb(const std::string& serial, const std::vector<std::string>& args,
                   std::string* out, int timeout_ms) = 0;                   // -2 timeout; serial "" = no -s
  virtual std::string connect_usb() = 0;
  virtual std::string connect_wifi(const std::string& addr) = 0;
  virtual media::OcrJobResult ocr(const Frame& f) = 0; };
class FrameTap { public:
  void offer(const std::string& id, const media::VideoFrame& f);  // copies if >=250 ms since last copy or fresh wanted
  bool get(const std::string& id, Frame* out, int wait_ms);        // sets fresh wanted, waits, falls back to cache
  void drop(const std::string& id); };
}
// AdbController: int run(const std::vector<std::string>& args, std::string* out, int timeout_ms);
```

- [ ] **Step 1: Self-test cases** for `FrameTap` (build `media::VideoFrame`s with a small owned buffer; if `VideoFrame`'s destructor frees `data`, allocate it the way the decoder does):
  - no offer -> `get` false; one offer of a 4x2 frame with stride 20 -> `get` true, `w==4,h==2`, `rgba.size()==32`, row padding removed.
  - second offer within 250 ms is ignored unless a `get` is waiting: start `get(id, &f, 200)` on a thread, offer a new frame 20 ms later, the thread sees the new pixels.
  - `drop` -> `get` false.
- [ ] **Step 2: Implement `FrameTap`** (mutex + condition_variable, map id -> {Frame, last_copy, fresh_wanted}).
- [ ] **Step 3: adb timeout:** add `int timeout_ms` (default 10000 = current behaviour) to `spawn_capture`; read with `PeekNamedPipe` polling so a hanging child is killed with `TerminateProcess` at the deadline and returns -2; output keeps what was read. Public `AdbController::run` forwards to it. Existing callers unchanged.
- [ ] **Step 4: Run CI**; PASS.
- [ ] **Step 5: Commit** `MCP: host interface, frame tap, adb timeout`.

### Task 4: The tools

**Files:**
- Create: `src/mcp/mcp_tools.cpp`, `include/opm/mcp/mcp_tools.h`
- Modify: `src/mcp/mcp_self_test.cpp` (FakeHost)

**Interfaces:** consumes Tasks 1-3. Produces `ToolTable make_tools(Host& host);`

Device resolution (one helper): `device` arg if given (must be in `devices()`), else the `shown` row; Android-only tools fall back to the first Android row; none -> error "No Android phone connected. Call connect_usb / connect_wifi or ask the user to connect one." Input/screenshot tools on a row with `mirrored=false` -> error naming connect_usb / connect_wifi. Android-only tools on an iPhone -> error "iPhone is view-only over AirPlay".

Screen space: `shot = shot_size({frame.w, frame.h})` from the current frame (mirrored) or from `ui_root_size` (adb-only). Input maps shot -> frame with `map_x/map_y`.

Tools and behaviour (names exact, schemas JSON Schema objects with the spec's args/defaults): `list_devices`, `screenshot` (PNG via `stbi_write_png_to_func`, content `[{type:"image",mimeType:"image/png",data:<base64>},{type:"text",text:"WxH screenshot space"}]`; `max_side` smaller only shrinks the image and the text states the tap space), `ui_tree` (`exec-out uiautomator dump /dev/tty`, then fallback `shell uiautomator dump /sdcard/pm_ui.xml` + `exec-out cat`; strip trailing "UI hierchary dumped to" line; prepend `current_app`), `ocr_screen`, `current_app` (`shell dumpsys activity activities`, first `topResumedActivity`/`mResumedActivity` line, `pkg/activity`), `tap`, `long_press`, `double_tap`, `swipe`, `scroll`, `type_text`, `press_key`, `screen_power`, `wait_for`, `list_apps`, `launch_app`, `stop_app`, `clear_app_data`, `install_apk`, `uninstall_app`, `open_url`, `logcat`, `clear_logcat`, `push_file`, `pull_file`, `adb_shell`, `connect_usb`, `connect_wifi`.

Timings: tap down/up 40 ms apart; double_tap 2 taps 120 ms apart; long_press default 600 ms; swipe moves every 16 ms; scroll `amount` 1 = 60 % of the axis from the centre (or x,y), `down` = finger moves up; `press_key long:true` uses `shell input keyevent --longpress <code>`; `type_text` ASCII via `Text` in 300-byte chunks, otherwise `Clipboard`; `submit` sends Enter (66) down/up. adb timeouts: default 15000, `install_apk` 180000, `adb_shell` user value max 120000. Outputs through `truncate_output`. Every call logs `[MCP] <tool> device=<id>`.

- [ ] **Step 1: Self-test with a FakeHost** (records `send` calls; one Android device `A1` mirrored with a 1080x2400 frame; one iPhone `I1`; scripted `adb` replies keyed by joined args):
  - `tap {x:288,y:640}` -> sends Touch down then up at frame (540,1200).
  - `tap {text:"Đăng"}` with the Task 2 sample dump -> touch at the button centre in frame pixels (540,2080); reply mentions `login`.
  - `tap {text:"nope"}` -> `isError`, reply lists candidates.
  - `type_text {text:"xin chào"}` -> one Clipboard input; `type_text {text:"hello",submit:true}` -> Text then Key 66 down/up.
  - `press_key {key:"bogus"}` -> `isError`.
  - `tap` on `I1` -> `isError` containing `view-only`; `screenshot` on `I1` -> image content.
  - FakeHost `send` returns false -> `tap` `isError` containing `connect_wifi`.
  - `ui_tree` when adb returns `ERROR: could not get idle state.` both times -> `isError` containing `ocr_screen`.
  - `adb_shell` when FakeHost adb returns -2 -> `isError` containing `timed out`.
  - `wait_for {text:"Đăng",timeout_ms:1000}` -> found; `{text:"zzz",timeout_ms:600}` -> not found, not an error crash, elapsed reported.
  - `screenshot` on `A1` returns a PNG whose header decodes to 576x1280 (check bytes 16..23 of the PNG IHDR).
- [ ] **Step 2: Implement `make_tools`.**
- [ ] **Step 3: Run CI**; PASS.
- [ ] **Step 4: Commit** `MCP: tools`.

### Task 5: Socket server

**Files:**
- Create: `include/opm/mcp/mcp_server.h`, `src/mcp/mcp_server.cpp`
- Modify: `src/mcp/mcp_self_test.cpp`

**Interfaces (produces):**
```cpp
namespace opm::mcp {
class McpServer { public:
  explicit McpServer(ToolTable tools); ~McpServer();          // dtor stops
  std::string start(int port, const std::string& token);      // "" ok, else error text
  void stop(); bool running() const; int port() const; };     // port() = bound port (start(0) picks one)
}
```

- [ ] **Step 1: Self-test:** `start(0, "tok")` binds 127.0.0.1; over a real loopback socket: POST `/mcp` without auth -> `HTTP/1.1 401`; with auth and an `initialize` body -> 200 with `phonemimi`; two requests on one keep-alive connection both answered; a 2 MB body -> 413; `stop()` returns within 1 s and a new connect fails.
- [ ] **Step 2: Implement.** Accept thread; one thread per connection; parse request line, headers, `Content-Length` body (cap 1 MB); `Connection: close` honoured; 30 s idle timeout via `SO_RCVTIMEO`; `stop()` closes the listen socket and all open client sockets, joins threads. Log `[MCP] listening on 127.0.0.1:<port>` / bind error.
- [ ] **Step 3: Run CI**; PASS.
- [ ] **Step 4: Commit** `MCP: loopback HTTP server`.

### Task 6: App integration and Settings UI

**Files:**
- Create: `src/app_mcp.cpp`
- Modify: `include/opm/app.h`, `src/app.cpp`, `src/android/android_dialog.cpp`, `include/opm/settings.h`, `src/settings.cpp`, `include/opm/media/renderer.h`, `src/media/renderer.cpp`, `CMakeLists.txt`

**Interfaces:**
- `App` privately implements `mcp::Host` in `src/app_mcp.cpp`; members `mcp::FrameTap frame_tap_; std::unique_ptr<mcp::McpServer> mcp_;` plus `std::string App::android_connect_wifi(const std::string& addr)` (drop sessions for that ip, `adb_.connect`, `start_android_session_`; empty addr retries remembered addresses `std::vector<std::string> wifi_history_`).
- Settings: `bool mcp_enabled=false; int mcp_port=47290; std::string mcp_token;` parse (`mcp_port` clamped 1024..65535) and write.
- Renderer: `using McpFn = std::function<std::string(bool on, int port, const std::string& token)>; void set_mcp_handler(McpFn);` returns status text shown under the toggle.

- [ ] **Step 1: Frames:** in the Android video callback in `start_android_session_` call `frame_tap_.offer(captured, frame)` before routing; in the AirPlay video callback (`src/app.cpp`) offer under id `iphone`; `frame_tap_.drop(serial)` in `android_disconnect`.
- [ ] **Step 2: Host methods:** `devices()` = mirrored Android sessions (`control` = `control_ready()`), then adb-only devices from `adb_.list_devices()` not mirrored, then `iphone` if AirPlay is streaming; `shown` from `active_source_` / `active_android_serial_`. `send` finds the session by serial under `scrcpy_mutex_` and calls `send_touch` (frame size from the session's `video_width/height`), `send_key`, `send_text`, `send_clipboard(text,true)`, `send_display_power`. `ocr` calls `media::run_ocr_rgba`.
- [ ] **Step 3: Lifecycle:** in `App::init`, `Settings::load()`; if `mcp_enabled` start the server; set the renderer handler (start/stop on toggle, return `"Listening on 127.0.0.1:<port>"` or the error). `App::shutdown` stops MCP first.
- [ ] **Step 4: Settings UI (Phone tab):** after "Phone screen off": `settings_toggle_mcp_btn_ = toggle("Claude control (MCP)", settings_.mcp_enabled);` When on, a status line (grey, existing `text` helper) and a `ui_pill_` "Copy Claude Code command" (`settings_mcp_copy_btn_`) that calls `SDL_SetClipboardText(mcp::setup_command(...))` and shows "Copied" for 1.5 s. Toggling on with an empty token sets `mcp_token = mcp::make_token()` then saves, then calls the handler. Rows must fit the card (existing `rows` count / note position updated); labels go through `fit_text_px`.
- [ ] **Step 5: Run CI**; PASS; download the portable build, run it, enable the toggle, copy the command; `curl -s -X POST http://127.0.0.1:47290/mcp -H "Authorization: Bearer <token>" -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'` lists 28 tools.
- [ ] **Step 6: Commit** `MCP: app host, frame capture, connect_wifi, settings toggle`.

### Task 7: Version, docs, release build, live check

**Files:** `CMakeLists.txt` (3.4.0), `README.md` ("Use with Claude Code" section + version row + nlohmann/json row), `src/media/renderer.cpp` (in-app version history row 3.4.0), `AGENTS.md` (MCP module line in layout table), `app\` (portable build)

- [ ] **Step 1:** Bump, docs, history row: `3.4.0 | 15.11.2026 | Claude Code can see and drive your phones (MCP server, Settings > Phone).`
- [ ] **Step 2: Run CI**, download portable into `app\`, `--mcp-self-test` and `--srp-self-test` pass.
- [ ] **Step 3: Live check** with the user's phone: `claude mcp add ...`, then `list_devices`, `screenshot`, `ui_tree`, `tap {text:...}`, `type_text`, `launch_app`, `logcat`. Note anything that fails.
- [ ] **Step 4: Commit** `PhoneMimi 3.4.0: MCP server` and `app: portable 3.4.0`.
