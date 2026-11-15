#pragma once

#include <cstdint>
#include <string>

namespace opm {

// User-configurable runtime settings. Persisted to
// %APPDATA%\PhoneMimi\settings.ini on Windows.
struct Settings {
    // Phone bezel colour (mid/dark range — clamped on load).
    uint8_t bezel_r = 28;
    uint8_t bezel_g = 28;
    uint8_t bezel_b = 30;

    // Screenshot behaviour
    bool screenshot_save_to_folder = true;
    bool screenshot_copy_to_clipboard = true;
    // When true, every screenshot/annotated capture is opened in Snagit
    // Editor (TechSmith Snagit must be installed). If the user disables
    // "Save to folder" but keeps this on, the image is written to a temp
    // file just so Snagit can pick it up.
    bool screenshot_open_in_snagit = false;

    // When true, screenshots/annotated captures are downscaled so the
    // image height matches screenshot_resize_height_cm when pasted into a
    // document at 96 DPI (Word's default for images without embedded
    // resolution metadata). The resized image is what gets saved/copied/
    // opened in Snagit; its filename gets a "-h<X>cm" suffix.
    bool  screenshot_resize_enabled = false;
    float screenshot_resize_height_cm = 10.0f;
    // When true (and resize is enabled), also save an unsuffixed copy of
    // the original, full-resolution image alongside the resized one.
    bool  screenshot_save_original_too = false;

    // Recording behaviour. format: 0 = MP4 (H.264), 1 = GIF.
    int  record_format = 0;
    int  record_max_duration_sec = 60;  // 0 = unlimited
    int  record_fps_mp4 = 30;
    int  record_fps_gif = 10;
    int  record_bitrate_kbps = 6000;

    // When true, the AirPlay/Cast/Miracast service name advertises as
    // "PhoneMimi by <COMPUTERNAME>" instead of
    // "PhoneMimi by MSEndpointMgr". Useful when multiple instances run
    // on the same network. Applied at next launch.
    bool use_computer_name = false;

    // Keep the app window above all other windows (no-focus topmost).
    // Applied immediately when toggled and again at startup.
    bool always_on_top = false;

    // UI accent colour: index into the renderer's palette (0 blue, 1 sky,
    // 2 violet, 3 green, 4 pink, 5 orange).
    int accent_color = 0;

    // MCP server for Claude Code (127.0.0.1 only). The token is made the
    // first time the switch is turned on.
    bool mcp_enabled = false;
    // Phone sound plays on the PC and, by default, on the phone too.
    bool android_mute_phone = false;
    // The one-time "drop files here" tip was shown.
    bool tip_drop_shown = false;
    int mcp_port = 47290;
    std::string mcp_token;

    // Start mirroring an Android phone as soon as it is plugged in over
    // USB (debugging must already be allowed on the phone).
    bool android_auto_usb = true;

    // Keep the on-screen keyboard of the phone hidden while mirroring (the
    // PC keyboard is used instead). Applies from the next connection.
    bool android_hide_soft_keyboard = true;

    // Send PC typing to the phone as soon as an Android phone is mirrored
    // (same as pressing Ctrl+K).
    bool android_keyboard_on = false;

    // Send typed text by pasting it instead of as key presses. Key presses
    // go through the phone's keyboard app, which may buffer whole words.
    bool android_type_via_paste = true;

    // Switch the phone's own display off while it is mirrored (Ctrl+O).
    bool android_screen_off = false;

    // Stream quality. Defaults ask for the most each side can deliver.
    // Android (scrcpy): longest side in pixels and frames per second,
    // 0 = no limit (the phone's native resolution / refresh rate).
    int android_max_size = 0;
    int android_max_fps = 0;
    int android_bitrate_mbps = 16;
    // iPhone (AirPlay): frame rate we advertise; iOS sends at most 60.
    int airplay_max_fps = 60;

    // Webcam drawer (TODO.md #9). Persisted so the user's "show my hands"
    // preference survives a restart. webcam_device_id is the MF symbolic
    // link returned by WebcamCapture::enumerate(); empty = system default.
    bool        webcam_drawer_open         = false;
    std::string webcam_device_id;
    bool        webcam_mirror_h            = true;   // selfie-style horizontal flip
    bool        webcam_include_in_recording = false; // composite in MP4/GIF (M5)

    // Returns the path to the settings file (creates the directory if needed).
    static std::string file_path();

    // Crash-guard marker for the webcam auto-start path. We touch a small
    // sentinel file just before opening the previously-selected camera at
    // launch, and remove it once a frame arrives. If the next launch finds
    // the marker still in place it means the prior run crashed inside the
    // capture stack and we should NOT retry that device automatically.
    static bool webcam_pending_exists();
    static void set_webcam_pending();
    static void clear_webcam_pending();

    // Load from disk. Missing/invalid file returns defaults.
    static Settings load();

    // Save to disk. Returns true on success.
    bool save() const;
};

} // namespace opm
