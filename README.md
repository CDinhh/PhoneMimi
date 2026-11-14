# PhoneMimi

> **Fork notes (v0.7.0)** - this fork of [MSEndpointMgr/1PhoneMirror](https://github.com/MSEndpointMgr/1PhoneMirror) adds, for Android:
> pairing by **QR code**, mirroring over a **USB cable**, and **PC control** (mouse = touch, wheel = scroll,
> `Ctrl+B/H/E/P` = Back/Home/Recents/Power, `Ctrl+K` = keyboard to phone, `Ctrl+V` = paste PC clipboard).
> Download the MSI or portable zip from [Releases](https://github.com/CDinhh/1PhoneMirror/releases/latest)
> (unsigned: SmartScreen shows *More info > Run anyway*). iPhone/iPad stays view-only (AirPlay over Wi-Fi).
> GPL-3.0, see [LICENSE](LICENSE).

An open-source screen-mirroring receiver for Windows that lets an iPhone or
an Android phone show up inside a phone-shaped window on your PC — no app
installed on the phone, no cables.

- **iOS / iPadOS / macOS** — native AirPlay (Screen Mirroring), with optional PIN pairing.
- **Android** — Wireless Debugging via the bundled `adb` + `scrcpy-server.jar`.
- **Miracast** — Wi-Fi Direct receiver (experimental, Windows-only).
- **Capture** — one-click screenshots (PNG) and screen recording (MP4 or GIF) straight into a phone-framed picture or clip — perfect for documentation.

Multiple phones can stay paired at once and appear as small dots in the
bottom bezel; left-click switches the active source, right-click opens a
menu to disconnect.

## Screenshots

<table>
  <tr>
    <td align="center"><img src="docs/screenshots/01-waiting.png" height="280" alt="Waiting for a connection"></td>
    <td align="center"><img src="docs/screenshots/02-info-panel.png" height="280" alt="Info panel"></td>
  </tr>
  <tr>
    <td><b>Idle screen</b> — quick reminders for AirPlay (iOS) and Wireless debugging (Android), with the framed phone window ready to receive.</td>
    <td><b>Info panel</b> (<code>I</code>) — version, shortcuts, network requirements, and a one-click "Copy network test script" for IT validation.</td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screenshots/03-settings-panel.png" height="280" alt="Settings panel"></td>
    <td align="center"><img src="docs/screenshots/04-log-viewer.png" height="280" alt="Log viewer"></td>
  </tr>
  <tr>
    <td><b>Settings panel</b> (<code>S</code>) — bezel colour swatches, screenshot/clipboard toggles, computer-name identity, and the MP4 / GIF recording-format selector.</td>
    <td><b>Log viewer</b> (<code>L</code>) — live activity drawer slides out to the right, perfect for debugging AirPlay handshakes or Android pairing.</td>
  </tr>
  <tr>
    <td align="center" colspan="2"><img src="docs/screenshots/05-Webcam-viewer-for-showing-phone-handling.png" height="280" alt="Webcam drawer for showing phone handling"></td>
  </tr>
  <tr>
    <td colspan="2"><b>Webcam drawer</b> — pop out a live webcam feed alongside the mirrored phone to demonstrate hands-on gestures, taps, and physical buttons during demos or training sessions.</td>
  </tr>
</table>

## Architecture

The receiver runs entirely on the PC. AirPlay (iOS) and scrcpy (Android)
streams are decoded with FFmpeg and rendered through SDL2 inside the
phone-shaped window. Recording captures the same RGBA frames straight
into MP4 (libx264) or animated GIF.

## Install

Grab the MSI or the portable zip from
[Releases](https://github.com/CDinhh/1PhoneMirror/releases/latest),
or build a portable copy yourself:

```powershell
.\package.ps1 -Portable    # -> app\PhoneMimi.exe
```

## Connecting a phone

### iPhone / iPad / Mac (AirPlay)
1. PC and phone on the **same Wi-Fi**.
2. Phone: **Control Center → Screen Mirroring → PhoneMimi**.
3. If PIN pairing is enabled, type the PIN shown on the PC.

### Android (Wireless Debugging)
1. Phone: **Settings → Developer options → Wireless debugging** → enable.
2. Tap **Pair device with pairing code** — note the **IP : port** and **6-digit code**.
3. PC: in PhoneMimi press **A** — the dialog pre-fills your PC's `/24`
   subnet (e.g. `192.168.0.`); type the phone's last octet, the pair port,
   and the PIN, then **Connect**. The phone shows up via mDNS within a
   second or two and starts mirroring.

A new pairing code is required each time the pair-with-code screen is
opened. Once paired, the device serial sticks until you forget it on
the phone.

### Miracast (Android, experimental)
1. PC: leave Miracast enabled (default).
2. Phone: **Quick Settings → Cast / Smart View / Screen Cast** → pick the PC.

## Capture

| Output | How |
|--------|-----|
| **Screenshot (PNG)** | `Ctrl+S`, or click the white circle in the menu/bezel. Save to `Pictures\PhoneMimi\`, copy to clipboard, and/or open directly in **Snagit Editor** (any combination, toggled in Settings). |
| **Video (MP4)** | `Ctrl+R`, or click the red circle in the menu/bezel. H.264 via libx264, 30 fps by default. |
| **Animated GIF** | Switch format in Settings → record as above. Auto-downscaled to 480 px wide. |
| **Delayed recording** | Right-click the record button → **Start in 5 s**. Frosty countdown overlays the screen. |
| **Timed clip** | Right-click → **Record 5 s / 10 s / 15 s** — auto-stops at the chosen duration. |

All output lands in your `Pictures\PhoneMimi\` folder; click the folder
button on the menu (or use the Windows file dialog) to open it.

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `F` | Toggle fullscreen |
| `M` | Toggle island menu |
| `L` | Toggle log viewer |
| `A` | Open Android pair / connect dialog |
| `I` | Toggle info panel |
| `V` | Toggle version history |
| `W` | Toggle webcam drawer (slides down from the phone bottom) |
| `U` | Mute / unmute audio output |
| `S` | Toggle settings panel (bezel colour, screenshots, recording format) |
| `Ctrl+S` | Screenshot — save to Pictures folder and/or clipboard (per Settings) |
| `Ctrl+Shift+S` | Screenshot with annotation tools |
| `Ctrl+Shift+T` | OCR — copy text from a region of the phone screen |
| `Ctrl+R` | Start / stop screen recording (MP4 or GIF, per Settings) |
| `Ctrl+0` | Reset window to default size |
| `Ctrl+1` … `Ctrl+9` | Switch to device 1 … 9 in the bottom-bezel picker |
| `Ctrl+C` | (Log viewer) Copy entire log to clipboard |
| `Ctrl+X` | (Log viewer) Clear log |
| `Esc` | Close panel / quit |

## Version history

| Version | Date | Highlights |
|---------|------|------------|
| **0.6.2 (pre-release)** | 30.09.2026 | AirPlay audio receiver with mute/unmute control and pause-safe mirroring. Pending validation before the Winget submission. |
| **0.6.1** | 30.09.2026 | Webcam startup now **fails safe**: invalid camera buffers stop capture with an error instead of crashing the app, and a failed webcam launch no longer traps the app into auto-reopening the same bad camera on next start. The first click on an inactive window now works for bezel interactions such as drag and close. |
| **0.6.0** | 29.08.2026 | The installer and application are now **digitally signed** with Azure Trusted Signing (Public Trust), so Windows SmartScreen and Defender recognise PhoneMimi as coming from a verified publisher instead of an unknown one. Every bundled executable and DLL is signed and timestamped. |
| **0.5.2** | 27.08.2026 | New **Statistics** drawer (`T`) — anonymous iOS/Android session, screenshot, annotation, OCR and recording counts, plus a scrollable event log and one-click CSV export. Screenshots can now be **resized to an exact height in cm** (with a print-quality DPI tag so they stay sharp when pasted into Word/PowerPoint). |
| **0.5.1** | 01.06.2026 | Bezel side buttons: action button, better sizing & visibility. |
| **0.5.0** | 21.05.2026 | **Webcam drawer** (`W`) — a landscape camera panel that slides out of the phone's bottom bezel, matching the log panel's drawer style. Lists Media Foundation cameras (right-click the bottom `W` button to pick one), live preview at the camera's native resolution, optional horizontal mirror. Screenshots (`Ctrl+S`) and screen recordings (`Ctrl+R`) capture the phone + webcam as one framed composite — WYSIWYG. In-drawer **screenshot button** (left) saves the webcam frame on its own (`<date> <time> Webcam.png`) and right-edge **camera-switch dots** flip between connected cameras. Crash-guard skips webcam auto-start after a failed launch so a bad camera can't lock the app. Version-history panel (`V`) scrollbar is now click-draggable. |
| **0.4.2** | 20.05.2026 | New capture file naming convention `<date> <time> <function>` (e.g. `2026-05-20 14-32-07 Screenshot.png`, `2026-05-20 14-32-07 Recording.mp4`) so screenshots and recordings sort chronologically in Explorer. |
| **0.4.1** | 18.05.2026 | **Open screenshots in Snagit Editor** — new third output toggle alongside *Save to folder* and *Copy to clipboard*. Auto-detects any installed Snagit version (registry + Program Files); shows a warning toast if Snagit isn't installed when enabled. |
| **0.4.0** | 18.05.2026 | **Multi-device keyboard switcher** (`Ctrl+1`…`Ctrl+9`) jumps between connected phones in the bottom-bezel picker. New **Always keep window on top** setting. AirPlay sources now label as **Apple 1/2/…** instead of *Device 1/2/…*. Tooltip shows the shortcut. |
| **0.3.9** | 17.05.2026 | Small telemetry in settings. |
| **0.3.8** | 16.05.2026 | Tuned for larger screens from macOS. Compact bezel icons, panels and overlays when mirroring iPad/Mac sources; window now reshapes when switching between connected devices. |
| **0.3.7** | 15.05.2026 | **OCR text capture** (`Ctrl+Shift+T`): right-click any region of the mirrored phone screen to copy the recognised text straight to the Windows clipboard. Powered by `Windows.Media.Ocr`. |
| **0.3.6** | 14.05.2026 | **Screenshot annotation** (`Ctrl+Shift+S`): arrow, rectangle, highlight, pixelate and text tools, 5-colour palette, line-width slider, undo, save (PNG + clipboard). |
| **0.3.5** | 13.05.2026 | Version check and UI tunings. In-app GitHub update check, Info panel "About" section, polished footer typography. |
| **0.3.4** | 13.05.2026 | New Android discovery routine for easy connect. |
| **0.3.3** | 12.05.2026 | Right-click the resize grip to reset the window to its default size. |
| **0.3.2** | 11.05.2026 | User interface fixes. |
| **0.3.1** | 10.05.2026 | **Phone frame on recordings** — MP4 and GIF now include the phone bezel/rounded corners just like screenshots. GIF gets per-frame optimal palette via `palettegen`+`paletteuse` with **genuine transparent rounded corners**. |
| **0.3.0** | 10.05.2026 | **Screen recording** — MP4 (H.264) and GIF, `Ctrl+R`, right-click for 5 s delay or 5 / 10 / 15 s timed clips. Frosty countdown overlay, REC chip while recording. Format selector in Settings. New bezel record button alongside the screenshot button. |
| **0.2.5** | 09.05.2026 | Info panel: copy network-test PowerShell script (with MDM check). |
| **0.2.4** | 08.05.2026 | Refined bezel toggles and auto-collapse on connect. |
| **0.2.3** | 08.05.2026 | Better Android experience and quicker screenshots. |
| **0.2.2** | 08.05.2026 | Settings: identify as computer name on the network. Bezel device dots show play/pause icon per source. |
| **0.2.1** | 07.05.2026 | Settings panel (`S`): bezel colour + screenshot options. |
| **0.2.0** | 06.05.2026 | Android mirroring (Wireless debugging, press `A`). |
| **0.1.6** | 06.05.2026 | Multiple iOS devices stay paired — switch from bezel dots. |
| **0.1.5** | 06.05.2026 | AirPlay PIN pairing for trusted-device security. |
| **0.1.4** | 06.05.2026 | Slide-out log viewer with live filtering (press `L`). |
| **0.1.2** | 05.05.2026 | Dynamic island menu in top bezel for quick actions. |
| **0.1.1** | 05.05.2026 | Screenshot capture to clipboard and Pictures folder. |
| **0.1.0** | 05.05.2026 | iOS AirPlay support. |
| **0.0.5** | 04.05.2026 | Initial ideas and implementation. |

The full version log is also available in-app — press `V`.

## Known app quirks

A few iOS apps interact with AirPlay in unusual ways. These are app-side
behaviours, not PhoneMimi bugs, but they're worth knowing about:

- **TripIt** — when launched, allocates a separate landscape `1920×1080`
  AirPlay surface and routes that to the receiver instead of mirroring
  the on-phone view. The result is a small "second-monitor" canvas that
  doesn't match what you see on the phone and isn't interactive.
  PhoneMimi keeps the phone frame stable across this canvas switch
  (no auto-rotate to landscape) and letterboxes the surface inside the
  phone screen rect, but the content is whatever TripIt chose to send.

If you spot another app that misbehaves, please open an issue with a
screenshot and the relevant lines from the in-app log (`L`).

## Project status

| Component | State |
|-----------|-------|
| AirPlay 2 — RTSP + mDNS + FairPlay + multi-source | Working |
| AirPlay PIN pairing | Working |
| Android — ADB pair/connect + scrcpy v3 stream | Working |
| Screenshots (PNG) | Working |
| Screen recording (MP4 H.264, GIF) | Working |
| Miracast — WinRT receiver | Experimental |
| Hardware-accelerated decode (DXVA2 / D3D11VA) | Planned |
| System tray + autostart | Planned |
| Touch input forwarding (Miracast UIBC) | Planned |

## References

| Resource | What it provides |
|----------|------------------|
| [UxPlay](https://github.com/antimof/UxPlay) | AirPlay 2 receiver — FairPlay crypto, stream handling |
| [RPiPlay](https://github.com/FD-/RPiPlay) | Simpler AirPlay receiver — good FairPlay reference |
| [scrcpy](https://github.com/Genymobile/scrcpy) | The server-side JAR pushed to Android devices |
| [Windows.Media.Miracast](https://learn.microsoft.com/en-us/uwp/api/windows.media.miracast) | WinRT Miracast API docs |
| [Bonjour SDK](https://developer.apple.com/bonjour/) | DNS-SD for AirPlay discovery on Windows |

## Code signing

Releases are currently **unsigned** while a code-signing certificate is
being arranged. Windows SmartScreen may therefore warn on first launch;
verify the published SHA-256 on the GitHub Release page before installing.

## Privacy

No telemetry. The only outbound request is the update check against
the GitHub Releases API.

## License

PhoneMimi is licensed under **GPL-3.0** — see [LICENSE](LICENSE).

### Third-party components

The Windows installer bundles the following components, each under its own
license:

| Component | Version | License | Role |
|-----------|---------|---------|------|
| [FFmpeg](https://ffmpeg.org/) | vcpkg `gpl,x264` build | GPL-2.0+ (compatible with GPL-3.0) | H.264 / AAC decoding + recording (dynamic link) |
| [libx264](https://www.videolan.org/developers/x264.html) | bundled with FFmpeg | GPL-2.0+ | H.264 encoder for MP4 recording |
| [SDL2](https://www.libsdl.org/) | vcpkg | zlib | Window, input, rendering |
| [OpenSSL](https://www.openssl.org/) | 3.x | Apache-2.0 | TLS for AirPlay handshake |
| [scrcpy-server.jar](https://github.com/Genymobile/scrcpy) | 3.0 | Apache-2.0 | Pushed to Android device, runs there |
| [adb.exe](https://developer.android.com/tools/adb) | Android Platform Tools | Apache-2.0 + AOSP | Pair / connect to Android over Wi-Fi |
| [stb_image](https://github.com/nothings/stb) | header-only | MIT / Public Domain | PNG / JPG decode for the app icon |
| Microsoft VC++ Runtime | 14.x | Microsoft Redistributable License | C/C++ runtime DLLs |

All components are GPL-3.0 compatible.
