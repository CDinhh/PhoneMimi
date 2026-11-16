# Android mirroring over Tailscale (remote phone)

## Intent
The owner's Android phone (Redmi Note 12, "phone server" running Termux +
Shizuku) sits at home. The owner wants PhoneMimi on a laptop elsewhere to
mirror it over Tailscale, with nobody touching the phone.

## Findings (spike, 2026-11-16)
- `adb connect 100.123.69.72:<tls port>` over Tailscale works.
- On this ROM, SELinux blocks `getprop service.adb.tls.port` for the shell
  user, so a phone-side "report the port" endpoint would have to scrape
  logcat. Fragile.
- `adb tcpip 5555` gives a fixed port that does not depend on Wi-Fi or the
  Wireless-debugging toggle; `adb connect 100.123.69.72:5555` works. It is
  lost on phone reboot (Shizuku is lost on reboot too, so a PC is needed
  then anyway).
- `adb tcpip` restarts adbd and kills `shizuku_server`; it can be restarted
  remotely over the same 5555 link with `libshizuku.so`.
- The phone already trusts this PC's adb key; a new laptop either pairs once
  in person or reuses the same `~/.android/adbkey`.

## Approaches
1. Discord `/adb` to read the port + manual connect. No phone code, all manual.
2. Phone endpoint reports the current TLS port. Needs logcat scraping
   (getprop denied), code on both sides.
3. Fixed port 5555 via `adb tcpip` (chosen). No phone code; the only gap is
   in PhoneMimi: it cannot connect to `ip:port` without pairing.

Chosen 3: simplest and the most robust (survives Wi-Fi changes); 2 rested
on reading the port, which this ROM blocks.

## Design (PhoneMimi)
- Android panel: if Pair port and Pairing code are empty, Connect uses
  IP + Connect port directly (`adb connect`, no pairing) through the
  existing `App::android_connect_wifi`. Pair port / code stop being
  required; Connect port becomes required when they are empty.
- Remember the last `ip:port` typed in the panel in `settings.ini`
  (`android_last_wifi`) and pre-fill IP + Connect port with it when the
  panel opens, so reconnecting is one click.
- No code on the phone. One-time setup (after each phone reboot):
  `adb tcpip 5555`, then restart Shizuku. Documented in the vault note.

## Non-goals
- Auto-reconnect when the remote link drops (reconnect is one click).
- iPhone/AirPlay, Cast, Miracast over Tailscale (mDNS / Wi-Fi Direct).
- Running `adb tcpip` from inside PhoneMimi (it kills Shizuku).
