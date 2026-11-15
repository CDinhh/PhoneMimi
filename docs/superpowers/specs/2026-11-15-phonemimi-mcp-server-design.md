# PhoneMimi MCP server: design

Date: 2026-11-15. Status: approved approach, design under review.

## Intent

Định builds phone apps (Flutter, React Native, native). They want Claude
(Claude Code on the same PC) to work the phone the way a developer does:
see the screen, read what is on it, tap, swipe, type, press keys, start and
stop apps, install builds, read logs, and run a test flow by itself after a
code change. "Whatever a developer needs, it has."

PhoneMimi already mirrors the phone and already drives Android through the
scrcpy control channel, so it is the natural place to expose this to Claude
through an MCP server.

## Goals

- Claude Code connects with one `claude mcp add` command, copied from the
  app.
- Claude can see the phone (screenshot), read it (UI tree on Android, OCR on
  any phone), control it (touch, swipe, scroll, keys, text), and do the
  usual Android dev chores (apps, APKs, deep links, logcat, files, shell).
- Works on the phone shown in the window and on any other connected phone,
  picked by a `device` argument.
- Fast: screenshots come from the live mirrored stream, input goes through
  the scrcpy control channel, not `adb input`.

## Non-goals

- Controlling an iPhone. AirPlay is receive-only; Apple gives no input
  channel. iPhone gets `screenshot` and `ocr_screen` only.
- Access from other machines. The server listens on 127.0.0.1 only.
- MCP resources, prompts, sampling, SSE streaming, sessions. Tools only,
  plain JSON responses.
- Screen recording, emulator management, iOS build tooling.

## Anti-goals (a failure even if it works)

- Anything that pops, raises or focuses a window on the PC. Định games in
  fullscreen; an MCP call must never steal focus.
- Blocking the SDL render/event thread. Every MCP request runs off it.
- A web page in the browser being able to drive the phone (DNS rebinding /
  CSRF against localhost).
- Server on by default.

## Constraints

- C++20, Windows, no local toolchain; builds run on GitHub Actions
  (`release.yml`). `app\` holds the committed portable build.
- No JSON library in the tree today. Code and log strings stay ASCII.
- Logging: `std::cout` with a `[MCP] ` prefix.
- Settings: new fields in `opm::Settings` with defaults, parser and writer in
  `src/settings.cpp`.
- No automated test suite; the closest is `--srp-self-test`.

## Approaches considered

1. **MCP inside PhoneMimi.exe (picked).** A small HTTP server on
   127.0.0.1 in the app speaks MCP Streamable HTTP. Uses the live decoded
   frames, the scrcpy control channel, the bundled adb and the Windows OCR
   wrapper directly. One exe, nothing else to install. Cost: the app must be
   running; one vendored header-only JSON library.
2. A separate MCP program in Python or Node that talks to PhoneMimi over a
   local socket. Faster to write, but needs a runtime installed and two
   programs kept in sync. Rejected: Định wants a lean machine.
3. An adb-only MCP that does not need the app. Already exists in the wild;
   slow screenshots (`screencap`), no use of PhoneMimi. Rejected.

## Design

### Transport

- MCP **Streamable HTTP**, single endpoint `POST http://127.0.0.1:<port>/mcp`.
  Each POST body is one JSON-RPC message; the reply is
  `Content-Type: application/json` with the JSON-RPC response. Notifications
  (no `id`) get `202 Accepted` with an empty body. `GET /mcp` returns
  `405`. Anything else returns `404`.
- Methods: `initialize`, `notifications/initialized`, `ping`, `tools/list`,
  `tools/call`. Unknown method: JSON-RPC error `-32601`.
- `initialize` replies with `protocolVersion` equal to the client's if it is
  one we know (`2025-03-26`, `2025-06-18`), otherwise `2025-06-18`;
  `capabilities: {tools: {}}`; `serverInfo: {name: "phonemimi", version:
  OPM_VERSION}`; and short `instructions` (below).
- No session id. Stateless per request.
- Tool failures are a normal result with `isError: true` and a text message
  saying what went wrong and what to try (e.g. "No Android phone connected.
  Call connect_usb or ask the user to connect one."). Protocol errors
  (bad JSON, unknown tool) use JSON-RPC errors.
- Built on the existing `opm::network::TcpServer` if it fits; otherwise a
  minimal blocking accept loop on its own thread. One worker thread per
  connection; keep-alive supported; request bodies capped at 1 MB.

### Security

- Off by default (`mcp_enabled=false`).
- Bind 127.0.0.1 only.
- Bearer token: 32 random hex chars generated on first enable with
  `BCryptGenRandom`, stored as `mcp_token` in settings.ini. Every request
  must carry `Authorization: Bearer <token>`; otherwise `401`.
- If an `Origin` header is present and is not `http://127.0.0.1[:port]` or
  `http://localhost[:port]`, reply `403`.
- Every tool call is logged: `[MCP] tap device=... x=... y=...` (never the
  token; `type_text` logs the length, not the text).

### Settings and UI

- New settings: `mcp_enabled` (bool, false), `mcp_port` (int, 47290),
  `mcp_token` (string, empty until first enable).
- Settings > Phone tab, new rows using the existing panel kit:
  - toggle "Let Claude control phones (MCP)";
  - when on: a line "Listening on 127.0.0.1:47290" (or the bind error), and
    a pill button "Copy Claude Code command" that copies
    `claude mcp add --transport http phonemimi http://127.0.0.1:47290/mcp --header "Authorization: Bearer <token>"`
    to the clipboard and shows a short "Copied" state on the pill.
- Turning the toggle on starts the server immediately; off stops it. No
  restart needed.
- README: a short "Use with Claude Code" section with the same command and
  the tool list. Version history row when the version is bumped.

### Coordinates

One coordinate space for every tool: **screenshot pixels**. `screenshot`
returns the phone screen scaled so its long side is at most 1280 px (never
upscaled), and says the size in its text part. `tap`, `swipe` etc. take x/y
in that space. `ui_tree` and `ocr_screen` report bounds in that same space.
The server maps screenshot space to the scrcpy video frame for input, and
device pixels (uiautomator) to screenshot space for bounds. The size is
fixed per device orientation, so Claude can reuse coordinates across calls
without taking a new screenshot.

### Frames

- Each Android session's video callback (and the AirPlay one) keeps the
  latest frame available to MCP: a throttled copy (at most 4 per second) of
  the RGBA frame into a per-device buffer, plus a "fresh frame wanted" flag
  that makes the next frame get copied immediately.
- `screenshot` sets the flag, waits up to 250 ms for a fresh frame, and
  falls back to the cached copy (the phone sends no frames while the screen
  is still). Inactive Android sessions decode too, so this works for phones
  not shown in the window.
- PNG encoding with the stb writer already in the tree.

### Devices

- A device is identified by its serial (Android, e.g. `R5CT...` or
  `192.168.1.50:41235`) or AirPlay source id (iPhone).
- Every tool takes an optional `device`. Empty means the phone currently
  shown in the window; if that is an iPhone and the tool is Android-only,
  fall back to the first connected Android phone; if none, error.
- MCP never switches which phone is shown in the window, except
  `connect_usb` / `connect_wifi`, which behave exactly like plugging the phone in (the new
  phone is shown). It still never raises or focuses the window.
- Phones adb can see but PhoneMimi is not mirroring appear in
  `list_devices` with `mirrored: false`. adb-based tools (apps, logcat,
  files, shell, `current_app`, `ui_tree`) work on them; `screenshot`,
  `ocr_screen` and input tools need a mirroring session and say so in the
  error ("call connect_usb / connect_wifi or ask the user to connect it"). (Builder call.)

### Tools

Android unless marked "any". All take optional `device`.

**Seeing and reading**
- `list_devices` (any): serial/id, name/model, kind (`android`/`iphone`),
  shown in window (bool), control ready (bool), screenshot size, rotation.
- `screenshot` (any): PNG image content + text "WxH, screenshot space".
  Optional `max_side` (320..1280, default 1280).
- `ui_tree`: `uiautomator dump` (via `adb exec-out uiautomator dump
  /dev/tty`, fall back to dumping to `/sdcard` and `cat`), parsed into a
  compact indented list, one line per useful node: text, content-desc,
  resource-id (package prefix stripped), class short name, bounds in
  screenshot space, and flags (clickable, scrollable, checked, focused,
  enabled=false). Nodes with no text/desc/id and not clickable are folded.
  Also returns the foreground package/activity. Error text when uiautomator
  fails (animations) suggests retrying or using `ocr_screen`.
- `ocr_screen` (any): Windows OCR on the current frame; returns lines with
  bounds in screenshot space. Needs the existing `run_ocr_rgba` extended or
  wrapped to return line boxes, not just joined text.
- `current_app`: foreground package and activity
  (`dumpsys activity activities` / `dumpsys window`).

**Control**
- `tap`: either `x,y`, or a selector `text` / `id` / `desc` (substring,
  case-insensitive; `exact: true` for exact) with optional `index`. A
  selector resolves through `ui_tree` and taps the centre of the match;
  the reply says what was tapped. No match: error listing up to 10
  nearby candidates.
- `long_press`: same targeting, `duration_ms` (default 600).
- `double_tap`: same targeting.
- `swipe`: `x1,y1,x2,y2`, `duration_ms` (default 300). Touch down, moves
  every ~16 ms, up.
- `scroll`: `direction` up/down/left/right, optional `x,y` (default centre),
  `amount` (default 1 = about 60 % of the screen), done as a swipe so it
  works in apps that ignore mouse-wheel events.
- `type_text`: types into the focused field. ASCII through `send_text`;
  anything else through the clipboard paste path PhoneMimi already uses.
  Optional `submit: true` presses Enter after.
- `press_key`: `key` one of back, home, recents, power, enter, delete,
  forward_delete, tab, escape, menu, search, volume_up, volume_down,
  volume_mute, dpad_up/down/left/right, dpad_center, wake, sleep, or a
  numeric Android keycode. Optional `long: true`.
- `screen_power`: `on` bool, phone display on/off while mirroring.
- `wait_for`: `text` (or `id`/`desc`), `timeout_ms` (default 10000, max
  60000), `gone: false`; polls `ui_tree` (falls back to OCR on Android if
  uiautomator keeps failing, always OCR on iPhone) about every 500 ms.
  Returns found/not found and elapsed time.

**Apps and dev chores**
- `list_apps`: packages, `third_party` (default true), optional `filter`.
- `launch_app`: `package`, optional `activity`; without activity uses
  `monkey -p <pkg> -c android.intent.category.LAUNCHER 1` (or
  `cmd package resolve-activity`). Optional `restart: true` force-stops
  first.
- `stop_app`: `am force-stop`.
- `clear_app_data`: `pm clear`.
- `install_apk`: local `path` on the PC, `replace` default true, optional
  `grant_permissions`. Uses `adb install`.
- `uninstall_app`: `package`.
- `open_url`: deep link or web URL via `am start -a VIEW -d`.
- `logcat`: `lines` (default 200, max 2000), optional `package` (resolved to
  pid with `pidof`), `level` (V/D/I/W/E), `tag`, `since` (logcat `-T`
  timestamp). Uses `logcat -d`.
- `clear_logcat`.
- `push_file` / `pull_file`: PC path <-> phone path.
- `adb_shell`: `command`, `timeout_ms` (default 15000, max 120000); returns
  exit code and output (truncated at 64 KB with a note).
- `connect_usb`: runs the existing USB connect path and returns its status.
- `connect_wifi`: optional `address` (`ip:port` from the phone's Wireless
  debugging screen). Runs `adb connect` and starts mirroring, like the Add
  Android panel's Connect. Without `address`, retries the addresses of
  phones mirrored earlier in this run (wireless links drop often). Pairing
  (code or QR) stays in the app UI: it needs the user at the phone. Like
  `connect_usb`, the newly connected phone is shown in the window.

### Server instructions (sent in `initialize`)

A few lines: coordinates are screenshot pixels; prefer `ui_tree` +
selector taps over raw coordinates; take a `screenshot` to check results;
`wait_for` after navigation instead of sleeping; iPhone is view-only.

### Code layout

- `lib/nlohmann/json.hpp` (vendored, MIT), added to
  `installer/THIRD_PARTY_LICENSES.txt` and the README table.
- `include/opm/mcp/mcp_server.h`, `src/mcp/mcp_server.cpp`: HTTP, auth,
  JSON-RPC dispatch.
- `src/mcp/mcp_tools.cpp`: tool table (name, description, input schema,
  handler).
- The server talks to the app through a narrow interface the App
  implements (list devices, latest frame, send input to a serial, run adb,
  connect USB); tools never touch `App` internals directly.
- CMake option `ENABLE_MCP` (ON), with Android-only tools under
  `ENABLE_ANDROID`.
- `PhoneMimi.exe --mcp-self-test`: feeds canned requests (initialize,
  tools/list, a call with a bad token, unknown method, bad JSON, the
  uiautomator XML parser on a sample dump, coordinate mapping) through the
  dispatcher without a network or phone; prints PASS/FAIL. Add a step to
  `release.yml` that runs it on the built exe and fails the job on FAIL.

### Version

Bump to 3.4.0, add README and in-app version history rows.

## Build and check

No local toolchain on this PC. Every check is a manual run of
`gh workflow run release.yml -R CDinhh/PhoneMimi --ref feat/mcp-server`
(about 4 minutes). `app\` is updated by downloading the
`PhoneMimi-<ver>-portable` artifact and committing it, as for 3.3.6.
Final check on the real phone: Claude Code connects with the copied
command and runs a short flow (screenshot, ui_tree, tap by text,
type_text, launch_app, logcat).

## Left to the builder

- Exact HTTP parsing details, threading of connections, timeouts.
- Exact text format of `ui_tree` and `list_devices` output, as long as it is
  compact and every node line carries bounds.
- How the frame copy and fresh-frame wait are synchronised.
- Exact UI placement within the Phone tab, following the existing rows.
