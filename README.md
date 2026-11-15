# PhoneMimi

Mirror your iPhone or Android phone to Windows, inside a phone-shaped
window. Nothing to install on the phone.

- **iPhone / iPad / Mac**: AirPlay Screen Mirroring, with optional PIN pairing.
- **Android**: plug in a USB cable and it starts by itself, or connect over
  Wi-Fi (pairing code or QR code). Control the phone from the PC: mouse =
  touch, wheel = scroll, type on the PC keyboard, paste with `Ctrl+V`,
  side buttons for volume and power, phone screen off while mirroring.
- **Capture**: screenshots (PNG, with annotation and OCR) and recordings
  (MP4 or GIF), framed like a phone.
- **Webcam drawer**: show your camera next to the phone, in recordings too.
- Several phones can stay connected; switch between them from the dots in
  the bottom bezel.
- **Claude Code control (MCP)**: let Claude see, read and drive your phones
  to test the app you are building.

## Download

Get the latest version from [Releases](https://github.com/CDinhh/PhoneMimi/releases/latest):
the **portable zip** (unzip and run `PhoneMimi.exe`) or the **MSI** installer.
The same portable build is also in the [`app/`](app) folder of this repository.
Builds are unsigned, so SmartScreen may ask you to confirm
(*More info > Run anyway*).

## Connecting a phone

### iPhone / iPad / Mac (AirPlay)
1. PC and phone on the **same Wi-Fi**.
2. Phone: **Control Center > Screen Mirroring > PhoneMimi**.
3. If PIN pairing is on, type the PIN shown on the PC.

### Android over USB
1. Phone: **Developer options > USB debugging** on.
2. Plug the phone in and allow the PC on the phone. Mirroring starts by
   itself (Settings > Phone > Connect over USB automatically).

### Android over Wi-Fi
1. Phone: **Developer options > Wireless debugging** on.
2. PC: press **A**. Pick the phone under *Nearby*, or type the IP, pair port
   and pairing code from **Pair device with pairing code**, then **Connect**.
   **QR code** pairs by scanning instead.

## Use with Claude Code (MCP)

PhoneMimi has a built-in MCP server so [Claude Code](https://claude.com/claude-code)
can work your phone while you build an app: take screenshots, read the screen,
tap, swipe, type, start apps, install builds and read logs.

1. **Settings > Phone > Claude control (MCP)**: turn it on.
2. Press **Copy command** and run it in a terminal. It looks like:
   ```
   claude mcp add --transport http phonemimi http://127.0.0.1:47290/mcp --header "Authorization: Bearer <token>"
   ```
3. Start Claude Code and ask it to, say, "open my app and check the login screen".

The server only listens on this PC (127.0.0.1) and needs the token from the
command. It is off until you turn it on.

| Group | Tools |
|-------|-------|
| See and read | `list_devices`, `screenshot`, `ui_tree`, `ocr_screen`, `current_app` |
| Control | `tap`, `long_press`, `double_tap`, `swipe`, `scroll`, `type_text`, `press_key`, `screen_power`, `wait_for` |
| Apps and dev | `list_apps`, `launch_app`, `stop_app`, `clear_app_data`, `install_apk`, `uninstall_app`, `open_url`, `logcat`, `clear_logcat`, `push_file`, `pull_file`, `adb_shell` |
| Connect | `connect_usb`, `connect_wifi` |

Android phones can be controlled over USB or Wi-Fi. iPhones (AirPlay) are
view-only: `screenshot` and `ocr_screen` work, input does not.

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `A` | Add an Android phone |
| `S` | Settings |
| `M` | Island menu |
| `L` / `T` | Log / statistics drawer |
| `W` | Webcam drawer |
| `V` | Version history |
| `I` | Info panel |
| `U` | Mute / unmute PC audio |
| `F` | Fullscreen |
| `Ctrl+K` | Type on the phone from the PC keyboard |
| `Ctrl+V` | Paste the PC clipboard on the phone |
| `Ctrl+B` / `Ctrl+H` / `Ctrl+E` / `Ctrl+P` | Back / Home / Recents / Power |
| `Ctrl+O` | Phone screen off / on (keeps mirroring) |
| `Ctrl+S` | Screenshot |
| `Ctrl+Shift+S` | Screenshot with annotation |
| `Ctrl+Shift+T` | Copy text from the screen (OCR) |
| `Ctrl+R` | Start / stop recording (right-click the record button for a delay or a timed clip) |
| `Ctrl+1` ... `Ctrl+9` | Switch to phone 1 ... 9 |
| `Ctrl+0` | Reset the window size |
| `Esc` | Close a panel / quit |

## Building

Windows, Visual Studio 2022, CMake.

```powershell
.\scripts\setup_deps.ps1      # first time: vcpkg, FFmpeg, SDL2, OpenSSL
.\scripts\build.ps1           # -> build\Release\PhoneMimi.exe
.\package.ps1 -Portable       # build + copy everything to app\
```

GitHub Actions (`release.yml`, run by hand) builds the same portable folder
and an MSI.

## Version history

| Version | Date | Highlights |
|---------|------|------------|
| **3.4.0** | 15.11.2026 | Claude Code can see and drive your phones: built-in MCP server (Settings > Phone). |
| **3.3.6** | 15.11.2026 | New look: Lucide icons, Plus Jakarta Sans, six accent colours, new switches and segmented choices, redesigned Add Android panel, penguin waiting screen. |
| **0.7.0** | 14.11.2026 | PhoneMimi: Android over USB (starts by itself), typing and pasting to the phone, phone side buttons, phone screen off, stream quality settings, tabbed Settings, no telemetry. |

The same list is in the app: press `V`.

## Privacy

No telemetry. The only network request PhoneMimi makes on its own is the
update check against this repository's GitHub releases. The MCP server, when
you turn it on, listens on 127.0.0.1 only.

## License

GPL-3.0, see [LICENSE](LICENSE).

### Third-party components

| Component | License | Role |
|-----------|---------|------|
| [FFmpeg](https://ffmpeg.org/) with libx264 | GPL-2.0+ | Video decoding, MP4 / GIF recording |
| [SDL2](https://www.libsdl.org/) | zlib | Window, input, rendering |
| [OpenSSL](https://www.openssl.org/) | Apache-2.0 | AirPlay handshake |
| [scrcpy-server](https://github.com/Genymobile/scrcpy) 3.0 | Apache-2.0 | Runs on the Android phone |
| [adb](https://developer.android.com/tools/adb) | Apache-2.0 | Talks to the Android phone |
| [stb_image](https://github.com/nothings/stb) | MIT / Public Domain | Image decoding and encoding |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | MCP server JSON |
| [Lucide](https://lucide.dev/) icons | ISC | Icon font |
| [Plus Jakarta Sans](https://github.com/tokotype/PlusJakartaSans) | OFL-1.1 | UI font |
| Microsoft VC++ Runtime | Microsoft redistributable | C/C++ runtime |

AirPlay support builds on [UxPlay](https://github.com/antimof/UxPlay) and
[RPiPlay](https://github.com/FD-/RPiPlay).

Based on [MSEndpointMgr/1PhoneMirror](https://github.com/MSEndpointMgr/1PhoneMirror) (GPL-3.0).
