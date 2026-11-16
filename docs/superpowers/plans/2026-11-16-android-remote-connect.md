# Android Connect-Without-Pairing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the Android panel connect to `IP:Connect port` without pairing, and remember the last address, so a Tailscale phone (`100.123.69.72:5555`) is one click away.

**Architecture:** Renderer's `android_submit` relaxes validation when Pair port + Pairing code are empty; App's connect handler routes that case to the existing `App::android_connect_wifi(ip:port)`. The last address is kept in `Settings::android_last_wifi`, written by the renderer (it owns `settings_` and saves it whole) and used to pre-fill the panel.

**Tech Stack:** C++20, SDL2, existing `opm::Settings` key=value file.

**Spec:** `docs/superpowers/specs/2026-11-16-android-remote-tailscale-design.md`

## Global Constraints
- Strings stay ASCII; log lines keep the `[App]` / `[Android]` prefix.
- Settings field must default to empty so existing users are unaffected.
- No blocking on the SDL thread: the connect stays on the detached worker in `android_submit`.
- No automated test suite: verification is a Release build + a real connect over Tailscale.

## Review Focus
- Pair port filled but PIN empty (or the reverse): still the pairing path, with the existing "PIN must be 6 digits" error.
- Connect-only with empty Connect port: clear error, no adb call.
- `android_last_wifi` without a colon or garbage in settings.ini: ignored, panel falls back to subnet prefix.
- Settings saved by another panel after a connect must not wipe `android_last_wifi` (renderer owns the write).
- MCP `android_connect_wifi` log line must not claim "(MCP)" when called from the panel.

---

### Task 1: Connect-only path + remembered address

**Files:**
- Modify: `include/opm/settings.h` (add `std::string android_last_wifi;` near the Android fields)
- Modify: `src/settings.cpp` (parse + write `android_last_wifi`)
- Modify: `src/app.cpp:359-362` (empty PIN and pair port -> `android_connect_wifi(ip + ":" + cport)`)
- Modify: `src/android/android_dialog.cpp:538` (log prefix "(Wi-Fi)" instead of "(MCP)")
- Modify: `src/media/renderer.cpp` `android_submit` (validation + save `android_last_wifi`), `show_android_panel` (pre-fill)

**Interfaces:**
- Consumes: `std::string App::android_connect_wifi(const std::string& addr)` (existing; returns "Mirroring <serial>" on success).
- Produces: `Settings::android_last_wifi` ("ip:port" or empty).

- [ ] **Step 1:** Settings field + parse/write, default empty.
- [ ] **Step 2:** `android_submit`: if `android_port_` and `android_pin_` are both empty -> require `valid_ipv4(android_ip_)` and non-empty `android_connect_port_` ("Connect port required."), else keep the existing pair checks. Before spawning the worker, set `settings_.android_last_wifi = ip + ":" + cport` (only when cport non-empty) and `settings_.save()`.
- [ ] **Step 3:** `show_android_panel`: if `android_ip_` is empty and `android_last_wifi` has a colon, split it into `android_ip_` / `android_connect_port_` (before the subnet-prefix fallback).
- [ ] **Step 4:** App handler: `if (pin.empty() && port.empty()) return android_connect_wifi(ip + ":" + cport);`
- [ ] **Step 5:** Build: `.\scripts\build.ps1` -> exit 0.
- [ ] **Step 6:** Real test: run Release exe, open panel (`A`), IP `100.123.69.72`, Connect port `5555`, pair fields empty, Connect -> log shows `Mirroring 100.123.69.72:5555`; reopen panel -> fields pre-filled; restart app -> still pre-filled.
- [ ] **Step 7:** Commit, `.\package.ps1 -Portable`, commit `app\`, merge to `main`, push.
