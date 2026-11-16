#include <opm/media/renderer.h>
#ifdef ENABLE_MCP
#include <opm/mcp/mcp_protocol.h>
#endif
#include <opm/log_buffer.h>
#include <opm/config.h>
#include <opm/network/update_check.h>
#include <opm/usage_log.h>
#ifdef _WIN32
#include <opm/media/ocr.h>
#include <qrcodegen/qrcodegen.hpp>
#endif
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include <SDL.h>
#include <SDL_syswm.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#pragma comment(lib, "comctl32.lib")
#endif

extern "C" {
    int stbi_write_png(const char* filename, int w, int h, int comp,
                       const void* data, int stride_in_bytes);
    int stbi_write_png_to_func(void (*func)(void*, void*, int), void* context,
                               int w, int h, int comp, const void* data,
                               int stride_in_bytes);
    unsigned char* stbi_load(const char* filename, int* x, int* y,
                             int* channels_in_file, int desired_channels);
    unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y,
                                         int* channels_in_file, int desired_channels);
    void stbi_image_free(void* retval_from_stbi_load);
}

// Bilinear downscale/upscale of an RGBA8 buffer. `src_stride` is in bytes
// (may include row padding); the output is always tightly packed (dw*4).
static std::vector<uint8_t> resize_rgba_bilinear(const uint8_t* src, int sw, int sh,
                                                  int src_stride, int dw, int dh) {
    std::vector<uint8_t> dst((size_t)dw * dh * 4, 0);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return dst;
    const float x_ratio = (float)sw / (float)dw;
    const float y_ratio = (float)sh / (float)dh;
    for (int y = 0; y < dh; ++y) {
        float sy = (y + 0.5f) * y_ratio - 0.5f;
        int y0 = (int)std::floor(sy);
        float fy = sy - (float)y0;
        int y0c = std::clamp(y0, 0, sh - 1);
        int y1c = std::clamp(y0 + 1, 0, sh - 1);
        for (int x = 0; x < dw; ++x) {
            float sx = (x + 0.5f) * x_ratio - 0.5f;
            int x0 = (int)std::floor(sx);
            float fx = sx - (float)x0;
            int x0c = std::clamp(x0, 0, sw - 1);
            int x1c = std::clamp(x0 + 1, 0, sw - 1);
            const uint8_t* p00 = src + (size_t)y0c * src_stride + (size_t)x0c * 4;
            const uint8_t* p10 = src + (size_t)y0c * src_stride + (size_t)x1c * 4;
            const uint8_t* p01 = src + (size_t)y1c * src_stride + (size_t)x0c * 4;
            const uint8_t* p11 = src + (size_t)y1c * src_stride + (size_t)x1c * 4;
            uint8_t* d = dst.data() + (size_t)y * dw * 4 + (size_t)x * 4;
            for (int c = 0; c < 4; ++c) {
                float v0 = p00[c] + (p10[c] - p00[c]) * fx;
                float v1 = p01[c] + (p11[c] - p01[c]) * fx;
                float v  = v0 + (v1 - v0) * fy;
                d[c] = (uint8_t)std::clamp(v, 0.0f, 255.0f);
            }
        }
    }
    return dst;
}

#ifdef _WIN32
// Locate TechSmith Snagit Editor on this machine. Checks the published
// App Paths registry key first (works for any installed major version),
// then falls back to scanning Program Files for "Snagit <ver>" folders.
// Returns an empty string if Snagit is not installed.
static std::string find_snagit_editor() {
    const HKEY roots[]  = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (HKEY root : roots) {
        for (REGSAM view : views) {
            HKEY hk = nullptr;
            if (RegOpenKeyExA(root,
                    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\SnagitEditor.exe",
                    0, KEY_READ | view, &hk) == ERROR_SUCCESS) {
                char buf[MAX_PATH] = {0};
                DWORD cb = sizeof(buf);
                DWORD type = 0;
                LONG r = RegQueryValueExA(hk, nullptr, nullptr, &type,
                                          (LPBYTE)buf, &cb);
                RegCloseKey(hk);
                if (r == ERROR_SUCCESS && cb > 0) {
                    std::string path(buf);
                    if (!path.empty() && std::filesystem::exists(path)) return path;
                }
            }
        }
    }
    const char* env_keys[] = { "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432" };
    for (const char* k : env_keys) {
        char buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableA(k, buf, sizeof(buf));
        if (n == 0 || n >= sizeof(buf)) continue;
        std::filesystem::path techsmith = std::filesystem::path(buf) / "TechSmith";
        std::error_code ec;
        if (!std::filesystem::is_directory(techsmith, ec)) continue;
        for (auto& entry : std::filesystem::directory_iterator(techsmith, ec)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().string();
            if (name.rfind("Snagit", 0) != 0) continue;
            auto candidate = entry.path() / "SnagitEditor.exe";
            if (std::filesystem::exists(candidate, ec)) {
                return candidate.string();
            }
        }
    }
    return {};
}

// Open `file_path` in Snagit Editor. Returns true if Snagit was found
// and launched. ShellExecute with the explicit editor path is the most
// reliable invocation (the editor accepts a single file argument).
static bool open_in_snagit(const std::string& file_path) {
    std::string editor = find_snagit_editor();
    if (editor.empty()) {
        std::cout << "[Snagit] SnagitEditor.exe not found on this machine\n";
        return false;
    }
    std::string params = "\"" + file_path + "\"";
    HINSTANCE r = ShellExecuteA(nullptr, "open", editor.c_str(),
                                params.c_str(), nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        std::cout << "[Snagit] ShellExecute failed (code "
                  << (INT_PTR)r << ") for " << editor << "\n";
        return false;
    }
    std::cout << "[Snagit] Opened " << file_path << " in " << editor << "\n";
    return true;
}
#else
static inline std::string find_snagit_editor() { return {}; }
static inline bool open_in_snagit(const std::string&) { return false; }
#endif

#ifdef _WIN32
static constexpr UINT_PTR kRendererWindowSubclassId = 1;

static LRESULT CALLBACK renderer_window_subclass_proc(HWND hwnd, UINT msg,
                                                      WPARAM wparam, LPARAM lparam,
                                                      UINT_PTR subclass_id,
                                                      DWORD_PTR ref_data) {
    if (msg == WM_MOUSEACTIVATE) {
        // Borderless SDL windows on Windows can activate with the first
        // click but still eat that click, which forces a second click for
        // drag/close/bezel interactions. Return MA_ACTIVATE so the click
        // that activated the window is also delivered to SDL.
        return MA_ACTIVATE;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

static void install_renderer_window_subclass(SDL_Window* window) {
    if (!window) return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) return;
    SetWindowSubclass(info.info.win.window,
                      renderer_window_subclass_proc,
                      kRendererWindowSubclassId,
                      0);
}

static void remove_renderer_window_subclass(SDL_Window* window) {
    if (!window) return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) return;
    RemoveWindowSubclass(info.info.win.window,
                         renderer_window_subclass_proc,
                         kRendererWindowSubclassId);
}
#endif

// ---------------------------------------------------------------------------
// GDI text helpers → SDL_Texture  (Segoe UI, anti-aliased)
// ---------------------------------------------------------------------------
#ifdef _WIN32

// Fonts embedded in the exe (resources/phonemimi.rc), registered privately
// for this process at startup. Plus Jakarta Sans ships one family name per
// weight (nameID 1), so the face is picked by weight; Lucide is the icon font.
static bool g_jakarta_ok = false;
static bool g_lucide_ok = false;

static bool load_rcdata(int id, const void** data, DWORD* size) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!res) return false;
    HGLOBAL h = LoadResource(mod, res);
    if (!h) return false;
    *data = LockResource(h);
    *size = SizeofResource(mod, res);
    return *data && *size > 0;
}

static int load_embedded_fonts() {
    static const int ids[] = {201, 202, 203, 204, 205};
    int ok = 0;
    for (int id : ids) {
        const void* data = nullptr; DWORD size = 0; DWORD n = 0;
        if (load_rcdata(id, &data, &size) &&
            AddFontMemResourceEx(const_cast<void*>(data), size, nullptr, &n) && n > 0) {
            ++ok;
            if (id == 201) g_lucide_ok = true;
        }
    }
    g_jakarta_ok = ok - (g_lucide_ok ? 1 : 0) == 4;
    std::cout << "[Renderer] Embedded fonts: " << ok << "/5\n";
    return ok;
}

// UI font for `weight` (FW_* value). `face` overrides the family (icon
// fonts, "Segoe UI Symbol", ...).
static HFONT make_ui_font(int font_height, int weight, const wchar_t* face = nullptr) {
    const wchar_t* fam = face;
    int w = weight;
    if (!fam && g_jakarta_ok) {
        if (weight >= 800)      { fam = L"Plus Jakarta Sans ExtraBold"; w = FW_NORMAL; }
        else if (weight >= 700) { fam = L"Plus Jakarta Sans";           w = FW_BOLD; }
        else if (weight >= 600) { fam = L"Plus Jakarta Sans SemiBold";  w = FW_NORMAL; }
        else                    { fam = L"Plus Jakarta Sans Medium";    w = FW_NORMAL; }
    }
    if (!fam) fam = L"Segoe UI";
    return CreateFontW(-font_height, 0, 0, 0, w, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, fam);
}

// Measure-only helper used by word-wrap layout. Returns the same width
// that make_text_texture would produce (advance width + overhang +
// right-padding) without going through SDL.
static int measure_text_w(const std::string& text, int font_height, int weight = 500) {
    if (text.empty()) return 0;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return 0;
    HFONT font = make_ui_font(font_height, weight);
    HFONT old_font = (HFONT)SelectObject(hdc, font);
    SIZE sz = {0, 0};
    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &sz);
    TEXTMETRICA tm;
    int extra = 0;
    if (GetTextMetricsA(hdc, &tm)) extra = tm.tmOverhang;
    extra += (font_height + 3) / 4;
    SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
    return sz.cx + extra;
}

// ANSI version
static SDL_Texture* make_text_texture(SDL_Renderer* renderer, const std::string& text,
                                       int font_height, uint8_t cr, uint8_t cg, uint8_t cb,
                                       int* out_w, int* out_h, int weight = 500) {
    if (text.empty()) return nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return nullptr;

    HFONT font = make_ui_font(font_height, weight);
    HFONT old_font = (HFONT)SelectObject(hdc, font);

    // UTF-8 in, UTF-16 to GDI, so Vietnamese and phone names with accents draw.
    std::wstring wtext;
    if (int wn = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0); wn > 0) {
        wtext.resize((size_t)wn);
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wtext.data(), wn);
    }
    SIZE sz;
    GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz);
    if (sz.cx <= 0 || sz.cy <= 0) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    // Add right-padding so glyph overhang (italic / ClearType subpixel /
    // last-char ABC overhang) is not clipped. GetTextExtentPoint32 reports
    // advance widths only, not the actual ink bounds of the last glyph.
    {
        TEXTMETRICW tm;
        if (GetTextMetricsW(hdc, &tm)) sz.cx += tm.tmOverhang;
        sz.cx += (font_height + 3) / 4;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz.cx;
    bmi.bmiHeader.biHeight = -sz.cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    HBITMAP old_bm = (HBITMAP)SelectObject(hdc, hbm);

    memset(bits, 0, sz.cx * sz.cy * 4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, 0, 0, wtext.c_str(), (int)wtext.size());
    GdiFlush();

    // ClearType renders sub-pixel RGB. Combine into luminance-based alpha.
    auto* src = static_cast<uint8_t*>(bits);
    std::vector<uint8_t> rgba(sz.cx * sz.cy * 4);
    for (int i = 0; i < sz.cx * sz.cy; i++) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        uint8_t alpha = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        rgba[i * 4 + 0] = cr;
        rgba[i * 4 + 1] = cg;
        rgba[i * 4 + 2] = cb;
        rgba[i * 4 + 3] = alpha;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, sz.cx, sz.cy);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), sz.cx * 4);
    }
    *out_w = sz.cx;
    *out_h = sz.cy;

    SelectObject(hdc, old_bm);
    SelectObject(hdc, old_font);
    DeleteObject(hbm);
    DeleteObject(font);
    DeleteDC(hdc);
    return tex;
}

// Wide/Unicode version (for ♫, ©, etc.). Optional font_name lets
// callers pick e.g. "Segoe UI Symbol"; nullptr = the UI font.
static SDL_Texture* make_text_texture_w(SDL_Renderer* renderer, const std::wstring& text,
                                          int font_height, uint8_t cr, uint8_t cg, uint8_t cb,
                                          int* out_w, int* out_h,
                                          const wchar_t* font_name = nullptr, int weight = 500) {
    if (text.empty()) return nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return nullptr;

    HFONT font = make_ui_font(font_height, weight, font_name);
    HFONT old_font = (HFONT)SelectObject(hdc, font);

    SIZE sz;
    GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &sz);
    if (sz.cx <= 0 || sz.cy <= 0) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    // Add right-padding so glyph overhang (italic / ClearType subpixel /
    // last-char ABC overhang) is not clipped. GetTextExtentPoint32 reports
    // advance widths only, not the actual ink bounds of the last glyph.
    {
        TEXTMETRICW tm;
        if (GetTextMetricsW(hdc, &tm)) sz.cx += tm.tmOverhang;
        sz.cx += (font_height + 3) / 4;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz.cx;
    bmi.bmiHeader.biHeight = -sz.cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    HBITMAP old_bm = (HBITMAP)SelectObject(hdc, hbm);

    memset(bits, 0, sz.cx * sz.cy * 4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, 0, 0, text.c_str(), (int)text.size());
    GdiFlush();

    auto* src = static_cast<uint8_t*>(bits);
    std::vector<uint8_t> rgba(sz.cx * sz.cy * 4);
    for (int i = 0; i < sz.cx * sz.cy; i++) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        uint8_t alpha = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        rgba[i * 4 + 0] = cr;
        rgba[i * 4 + 1] = cg;
        rgba[i * 4 + 2] = cb;
        rgba[i * 4 + 3] = alpha;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, sz.cx, sz.cy);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), sz.cx * 4);
    }
    *out_w = sz.cx;
    *out_h = sz.cy;

    SelectObject(hdc, old_bm);
    SelectObject(hdc, old_font);
    DeleteObject(hbm);
    DeleteObject(font);
    DeleteDC(hdc);
    return tex;
}

// Word-wrapped version of make_text_texture_w. Lays out `text` inside a
// box no wider than `max_pixel_width` using DrawTextW(DT_WORDBREAK) and
// returns a single texture covering all wrapped lines. Used for the
// description rows in the Version History panel so long blurbs reflow
// instead of being shrunk to fit on one line.
static SDL_Texture* make_text_texture_w_wrapped(SDL_Renderer* renderer,
                                                const std::wstring& text,
                                                int font_height,
                                                uint8_t cr, uint8_t cg, uint8_t cb,
                                                int max_pixel_width,
                                                int* out_w, int* out_h,
                                                const wchar_t* font_name = nullptr, int weight = 500) {
    if (text.empty() || max_pixel_width <= 0) return nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return nullptr;

    HFONT font = make_ui_font(font_height, weight, font_name);
    HFONT old_font = (HFONT)SelectObject(hdc, font);

    // CALCRECT lays out wrapped text and returns the bounding box of the
    // longest line. DrawText may modify its buffer, so work on a mutable
    // copy. We measure without DT_CENTER so the returned width matches the
    // longest wrapped line (DT_CENTER + DT_CALCRECT can return the full
    // input width on some Windows versions), then draw with DT_CENTER so
    // any shorter continuation lines are centred inside that box. The
    // Version panel centres the whole texture again at draw time, so this
    // makes every wrapped line render centred.
    std::wstring buf = text;
    RECT rc = {0, 0, max_pixel_width, 0};
    DrawTextW(hdc, buf.data(), (int)buf.size(), &rc,
              DT_WORDBREAK | DT_NOPREFIX | DT_LEFT | DT_CALCRECT);
    UINT fmt = DT_WORDBREAK | DT_NOPREFIX | DT_CENTER;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    // Right-pad for glyph overhang (same rationale as the single-line path).
    {
        TEXTMETRICW tm;
        if (GetTextMetricsW(hdc, &tm)) w += tm.tmOverhang;
        w += (font_height + 3) / 4;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return nullptr;
    }
    HBITMAP old_bm = (HBITMAP)SelectObject(hdc, hbm);

    memset(bits, 0, w * h * 4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT draw_rc = {0, 0, w, h};
    DrawTextW(hdc, buf.data(), (int)buf.size(), &draw_rc, fmt);
    GdiFlush();

    auto* src = static_cast<uint8_t*>(bits);
    std::vector<uint8_t> rgba(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        uint8_t alpha = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        rgba[i * 4 + 0] = cr;
        rgba[i * 4 + 1] = cg;
        rgba[i * 4 + 2] = cb;
        rgba[i * 4 + 3] = alpha;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), w * 4);
    }
    *out_w = w;
    *out_h = h;

    SelectObject(hdc, old_bm);
    SelectObject(hdc, old_font);
    DeleteObject(hbm);
    DeleteObject(font);
    DeleteDC(hdc);
    return tex;
}

// Cheap GDI text measurement (no texture creation). Includes the same
// overhang/ClearType padding used by make_text_texture so the result matches
// what will actually be rasterised.
static int measure_text_width_a(const std::string& text, int font_height, int weight = 500) {
    if (text.empty()) return 0;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return 0;
    HFONT font = make_ui_font(font_height, weight);
    HFONT old_font = (HFONT)SelectObject(hdc, font);
    SIZE sz{};
    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &sz);
    TEXTMETRICA tm{};
    if (GetTextMetricsA(hdc, &tm)) sz.cx += tm.tmOverhang;
    sz.cx += (font_height + 3) / 4;
    SelectObject(hdc, old_font);
    DeleteObject(font);
    DeleteDC(hdc);
    return sz.cx;
}

#endif // _WIN32

static void png_write_vec(void* context, void* data, int size) {
    auto* vec = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<const uint8_t*>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

// Standard (zlib/PNG) CRC-32, needed to hand-build the pHYs chunk below.
static uint32_t png_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

// Inserts a pHYs chunk (pixels-per-meter, both axes) right after IHDR so
// Office reads the image's true DPI instead of assuming 96 — lets resized
// screenshots carry far more pixels per cm without growing on the page.
static void png_set_dpi(std::vector<uint8_t>& png, int dpi) {
    if (png.size() < 33 || dpi <= 0) return;
    uint32_t ppm = (uint32_t)std::lround(dpi / 0.0254);
    std::vector<uint8_t> chunk;
    auto push_u32 = [&](uint32_t v) {
        chunk.push_back((uint8_t)(v >> 24)); chunk.push_back((uint8_t)(v >> 16));
        chunk.push_back((uint8_t)(v >> 8));  chunk.push_back((uint8_t)v);
    };
    push_u32(9); // chunk data length
    size_t type_data_start = chunk.size();
    const char type[4] = {'p', 'H', 'Y', 's'};
    chunk.insert(chunk.end(), type, type + 4);
    push_u32(ppm); // x pixels per unit
    push_u32(ppm); // y pixels per unit
    chunk.push_back(1); // unit specifier: 1 = meter
    uint32_t crc = png_crc32(chunk.data() + type_data_start, chunk.size() - type_data_start);
    push_u32(crc);
    png.insert(png.begin() + 33, chunk.begin(), chunk.end());
}

// Encodes to PNG in memory (so a pHYs/DPI chunk can be injected) then
// writes the result to disk. dpi <= 0 leaves the file without DPI metadata.
static bool write_png_file_dpi(const std::string& path, int w, int h,
                               const uint8_t* rgba, int stride, int dpi) {
    std::vector<uint8_t> data;
    if (!stbi_write_png_to_func(png_write_vec, &data, w, h, 4, rgba, stride)) return false;
    png_set_dpi(data, dpi);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return f.good();
}

static bool in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

// How far the physical side buttons stick out of a phone drawn `phone_w` wide.
static int side_btn_margin(int phone_w) { return std::max(3, phone_w / 90); }

// Anti-aliased discs, cached per size: white, alpha = pixel coverage
// (4x4 supersampled). `size` px wide, edge at `edge` px from the centre.
static SDL_Texture* aa_disc(SDL_Renderer* r, int size, float edge) {
    static std::unordered_map<long long, SDL_Texture*> cache;
    long long key = (long long)size * 100000 + (long long)(edge * 16);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 64) {    // window resized a lot: drop stale sizes
        for (auto& kv : cache) SDL_DestroyTexture(kv.second);
        cache.clear();
    }
    std::vector<uint8_t> px((size_t)size * size * 4);
    float c = size / 2.0f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int in = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float dx = x + (sx + 0.5f) / 4 - c, dy = y + (sy + 0.5f) / 4 - c;
                    if (dx * dx + dy * dy <= edge * edge) ++in;
                }
            uint8_t* p = &px[((size_t)y * size + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(in * 255 / 16);
        }
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, size, size);
    if (t) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(t, nullptr, px.data(), size * 4);
    }
    cache[key] = t;
    return t;
}

// Tints `t` with the current draw colour. With BLENDMODE_NONE the colour
// counts as opaque (only the AA edge blends), matching SDL_RenderFillRect.
static void tint_like_draw_color(SDL_Renderer* r, SDL_Texture* t) {
    uint8_t cr, cg, cb, ca;
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    SDL_BlendMode bm = SDL_BLENDMODE_BLEND;
    SDL_GetRenderDrawBlendMode(r, &bm);
    SDL_SetTextureColorMod(t, cr, cg, cb);
    SDL_SetTextureAlphaMod(t, bm == SDL_BLENDMODE_NONE ? 255 : ca);
}

static void fill_circle(SDL_Renderer* r, int cx, int cy, int radius) {
    if (radius <= 0) { SDL_RenderDrawPoint(r, cx, cy); return; }
    int size = radius * 2 + 1;
    SDL_Texture* t = aa_disc(r, size, radius + 0.5f);
    if (!t) return;
    tint_like_draw_color(r, t);
    SDL_Rect d = {cx - radius, cy - radius, size, size};
    SDL_RenderCopy(r, t, nullptr, &d);
}

// Rounded rectangle from non-overlapping pieces (safe with translucent
// colours) and anti-aliased corners.
static void fill_round_rect(SDL_Renderer* r, int x, int y, int w, int h, int rad) {
    if (w <= 0 || h <= 0) return;
    rad = std::max(0, std::min(rad, std::min(w, h) / 2));
    if (rad == 0) { SDL_Rect a = {x, y, w, h}; SDL_RenderFillRect(r, &a); return; }
    SDL_Rect top = {x + rad, y, w - rad * 2, rad};
    SDL_Rect mid = {x, y + rad, w, h - rad * 2};
    SDL_Rect bot = {x + rad, y + h - rad, w - rad * 2, rad};
    if (top.w > 0) { SDL_RenderFillRect(r, &top); SDL_RenderFillRect(r, &bot); }
    if (mid.h > 0) SDL_RenderFillRect(r, &mid);
    SDL_Texture* t = aa_disc(r, rad * 2, (float)rad);
    if (!t) return;
    tint_like_draw_color(r, t);
    SDL_Rect s0 = {0, 0, rad, rad}, s1 = {rad, 0, rad, rad}, s2 = {0, rad, rad, rad}, s3 = {rad, rad, rad, rad};
    SDL_Rect d0 = {x, y, rad, rad}, d1 = {x + w - rad, y, rad, rad};
    SDL_Rect d2 = {x, y + h - rad, rad, rad}, d3 = {x + w - rad, y + h - rad, rad, rad};
    SDL_RenderCopy(r, t, &s0, &d0);
    SDL_RenderCopy(r, t, &s1, &d1);
    SDL_RenderCopy(r, t, &s2, &d2);
    SDL_RenderCopy(r, t, &s3, &d3);
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Icon-font glyphs + cached text. Glyph/text textures are rendered white once
// per (code, size) and tinted with SDL colour/alpha mod, so drawing them every
// frame costs one SDL_RenderCopy.
// ---------------------------------------------------------------------------
namespace {
struct CachedTex { SDL_Texture* tex = nullptr; int w = 0, h = 0; };

std::unordered_map<std::string, CachedTex>& tex_cache() {
    static std::unordered_map<std::string, CachedTex> m;
    return m;
}

void tex_cache_trim() {
    auto& m = tex_cache();
    if (m.size() < 600) return;   // window being resized: drop stale sizes
    for (auto& kv : m) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    m.clear();
}

// Lucide is embedded; if it fails to register, draw_glyph maps the Lucide
// code points onto Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets.
const wchar_t* icon_font_name() {
    if (g_lucide_ok) return L"lucide";   // embedded lucide-static 0.469.0
    static const wchar_t* name = []() -> const wchar_t* {
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        std::wstring p = std::wstring(win) + L"\\Fonts\\SegoeIcons.ttf";
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES
                   ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    return name;
}
} // namespace

// Draws icon-font glyph `cp` centred on (cx, cy), `px` pixels tall.
static void draw_glyph(SDL_Renderer* r, wchar_t cp, int cx, int cy, int px,
                       uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
    if (!g_lucide_ok) {
        static const std::pair<wchar_t, wchar_t> to_segoe[] = {
            {0xE166, 0xE8EA}, {0xE157, 0xE713}, {0xE1B1, 0xE711}, {0xE0DC, 0xE8B7},
            {0xE068, 0xE722}, {0xE16A, 0xE71A}, {0xE07A, 0xEA3A}, {0xE071, 0xE70D},
            {0xE074, 0xE70E}, {0xE073, 0xE76C}, {0xE082, 0xE897}, {0xE1AD, 0xE701},
            {0xE04D, 0xE72A}, {0xE13F, 0xE768}, {0xE131, 0xE769}, {0xE070, 0xE73E},
            {0xE2A0, 0xE7A7}, {0xE197, 0xE8D2}, {0xE051, 0xE8AD}, {0xE0F7, 0xE7E6},
            {0xE503, 0xE80A}, {0xE1DE, 0xED14}, {0xE359, 0xE88E},
        };
        wchar_t mapped = 0;
        for (auto& m : to_segoe) if (m.first == cp) { mapped = m.second; break; }
        if (!mapped) return;
        cp = mapped;
    }
    tex_cache_trim();
    auto& c = tex_cache()["g:" + std::to_string((int)cp) + ":" + std::to_string(px)];
    if (!c.tex)
        c.tex = make_text_texture_w(r, std::wstring(1, cp), px, 255, 255, 255,
                                    &c.w, &c.h, icon_font_name());
    if (!c.tex) return;
    int ink_w = c.w - (px + 3) / 4;   // strip the right padding the helper adds
    SDL_SetTextureColorMod(c.tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(c.tex, ca);
    SDL_Rect d = {cx - ink_w / 2, cy - c.h / 2, c.w, c.h};
    SDL_RenderCopy(r, c.tex, nullptr, &d);
}

// Lucide code points (lucide-static 0.469.0, font/info.json).
namespace lucide {
constexpr wchar_t ArrowRight = 0xE04D, Camera = 0xE068, ChevronDown = 0xE071,
                  ChevronRight = 0xE073, ChevronUp = 0xE074, Circle = 0xE07A,
                  CircleHelp = 0xE082, Folder = 0xE0DC, LoaderCircle = 0xE10D,
                  Settings = 0xE157, Smartphone = 0xE166, Square = 0xE16A,
                  Wifi = 0xE1AD, X = 0xE1B1, QrCode = 0xE1DE, Pin = 0xE258,
                  CircleDot = 0xE348, Usb = 0xE359, ArrowUpRight = 0xE051,
                  ScrollText = 0xE463, ChartColumn = 0xE2A2, Webcam = 0xE204,
                  Power = 0xE143, Info = 0xE0FE, History = 0xE1F4, Plus = 0xE140,
                  Highlighter = 0xE0F7, Grid2x2 = 0xE503, Type = 0xE197,
                  Undo2 = 0xE2A0, Check = 0xE070, Play = 0xE13F, Pause = 0xE131;
}

// Accent colours (Settings > General > Accent colour). `dark_ink`: text on a
// filled accent surface is dark instead of white.
struct Accent { uint8_t r, g, b; uint8_t lr, lg, lb; bool dark_ink; };
static const Accent kAccents[6] = {
    {0x25, 0x63, 0xEB, 0x4F, 0x8E, 0xF7, false},  // Blue (default)
    {0x0E, 0xA5, 0xE9, 0x38, 0xBD, 0xF8, true},   // Sky
    {0x7C, 0x3A, 0xED, 0x9F, 0x67, 0xFF, false},  // Violet
    {0x22, 0xC5, 0x5E, 0x4A, 0xDE, 0x80, true},   // Green
    {0xDB, 0x27, 0x77, 0xF0, 0x52, 0x9C, false},  // Pink
    {0xF0, 0x7F, 0x2E, 0xFF, 0x9C, 0x55, true},   // Orange
};
static const Accent& accent_for(int index) {
    return kAccents[(index >= 0 && index < 6) ? index : 0];
}


// UI language (Settings > General > Language). English strings are the
// keys; tr() returns the Vietnamese text when it is selected.
static bool g_lang_vi = false;
static const char* tr(const char* en) {
    if (!g_lang_vi) return en;
    static const std::unordered_map<std::string, const char*> kVi = {
        {"Settings", "C\xC3\xA0i \xC4\x91\xE1\xBA\xB7t"},
        {"General", "Chung"},
        {"Phone", "\xC4\x90i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Capture", "Ch\xE1\xBB\xA5p & quay"},
        {"Help", "Tr\xE1\xBB\xA3 gi\xC3\xBAp"},
        {"Language", "Ng\xC3\xB4n ng\xE1\xBB\xAF"},
        {"Screenshot (Ctrl+S)", "Ch\xE1\xBB\xA5p m\xC3\xA0n h\xC3\xACnh (Ctrl+S)"},
        {"Record (Ctrl+R)", "Quay m\xC3\xA0n h\xC3\xACnh (Ctrl+R)"},
        {"Screenshots folder", "Th\xC6\xB0 m\xE1\xBB\xA5""c \xE1\xBA\xA3nh"},
        {"Log (L)", "Nh\xE1\xBA\xADt k\xC3\xBD (L)"},
        {"Statistics (T)", "Th\xE1\xBB\x91ng k\xC3\xAA (T)"},
        {"Quit PhoneMimi", "Tho\xC3\xA1t PhoneMimi"},
        {"Frame colour", "M\xC3\xA0u khung"},
        {"Accent colour", "M\xC3\xA0u nh\xE1\xBA\xA5n"},
        {"Computer name as receiver name", "D\xC3\xB9ng t\xC3\xAAn m\xC3\xA1y t\xC3\xADnh l\xC3\xA0m t\xC3\xAAn nh\xE1\xBA\xADn"},
        {"Save a log file", "L\xC6\xB0u file nh\xE1\xBA\xADt k\xC3\xBD"},
        {"No phone connected", "Ch\xC6\xB0""a c\xC3\xB3 \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Add", "Th\xC3\xAAm"},
        {"Disconnect", "Ng\xE1\xBA\xAFt k\xE1\xBA\xBFt n\xE1\xBB\x91i"},
        {"Auto-connect over USB", "T\xE1\xBB\xB1 k\xE1\xBA\xBFt n\xE1\xBB\x91i qua USB"},
        {"Type from this keyboard", "G\xC3\xB5 b\xE1\xBA\xB1ng b\xC3\xA0n ph\xC3\xADm n\xC3\xA0y"},
        {"Type by pasting", "G\xC3\xB5 b\xE1\xBA\xB1ng c\xC3\xA1""ch d\xC3\xA1n"},
        {"Hide the phone keyboard", "\xE1\xBA\xA8n b\xC3\xA0n ph\xC3\xADm \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Phone screen off", "T\xE1\xBA\xAFt m\xC3\xA0n h\xC3\xACnh \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Mute phone speaker", "T\xE1\xBA\xAFt loa \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Resolution", "\xC4\x90\xE1\xBB\x99 ph\xC3\xA2n gi\xE1\xBA\xA3i"},
        {"Frame rate", "T\xE1\xBB\x91""c \xC4\x91\xE1\xBB\x99 khung h\xC3\xACnh"},
        {"iPhone frame rate", "Khung h\xC3\xACnh iPhone"},
        {"Max", "T\xE1\xBB\x91i \xC4\x91""a"},
        {"Claude control (MCP)", "Claude \xC4\x91i\xE1\xBB\x81u khi\xE1\xBB\x83n (MCP)"},
        {"Copy command", "Sao ch\xC3\xA9p l\xE1\xBB\x87nh"},
        {"Copied", "\xC4\x90\xC3\xA3 ch\xC3\xA9p"},
        {"Applies the next time a phone connects.", "\xC3\x81p d\xE1\xBB\xA5ng t\xE1\xBB\xAB l\xE1\xBA\xA7n k\xE1\xBA\xBFt n\xE1\xBB\x91i sau."},
        {"Save to Pictures", "L\xC6\xB0u v\xC3\xA0o \xE1\xBA\xA2nh"},
        {"Copy to clipboard", "Ch\xC3\xA9p v\xC3\xA0o clipboard"},
        {"Open in Snagit", "M\xE1\xBB\x9F b\xE1\xBA\xB1ng Snagit"},
        {"Recording format", "\xC4\x90\xE1\xBB\x8Bnh d\xE1\xBA\xA1ng quay"},
        {"Resize screenshots", "\xC4\x90\xE1\xBB\x95i c\xE1\xBB\xA1 \xE1\xBA\xA3nh ch\xE1\xBB\xA5p"},
        {"Height", "Chi\xE1\xBB\x81u cao"},
        {"Keep the original too", "Gi\xE1\xBB\xAF c\xE1\xBA\xA3 \xE1\xBA\xA3nh g\xE1\xBB\x91""c"},
        {"Mirror the webcam", "L\xE1\xBA\xADt ngang webcam"},
        {"Webcam in recordings", "Webcam trong video quay"},
        {"Connect a phone with a cable", "K\xE1\xBA\xBFt n\xE1\xBB\x91i \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i b\xE1\xBA\xB1ng d\xC3\xA2y"},
        {"Drop files on the phone: APKs install, others go to Download", "Th\xE1\xBA\xA3 file v\xC3\xA0o \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i: APK \xC4\x91\xC6\xB0\xE1\xBB\xA3""c c\xC3\xA0i, file kh\xC3\xA1""c v\xC3\xA0o Download"},
        {"Copy on one side, paste on the other", "Sao ch\xC3\xA9p b\xC3\xAAn n\xC3\xA0y, d\xC3\xA1n b\xC3\xAAn kia"},
        {"Phone sound plays here too; U mutes the PC", "\xC3\x82m thanh \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i ph\xC3\xA1t c\xE1\xBA\xA3 \xE1\xBB\x9F \xC4\x91\xC3\xA2y; U t\xE1\xBA\xAFt ti\xE1\xBA\xBFng PC"},
        {"Ctrl+S screenshot, Ctrl+R record, F fullscreen", "Ctrl+S ch\xE1\xBB\xA5p, Ctrl+R quay, F to\xC3\xA0n m\xC3\xA0n h\xC3\xACnh"},
        {"Claude Code control: Phone > Claude control (MCP)", "Claude Code \xC4\x91i\xE1\xBB\x81u khi\xE1\xBB\x83n: \xC4\x90i\xE1\xBB\x87n tho\xE1\xBA\xA1i > Claude \xC4\x91i\xE1\xBB\x81u khi\xE1\xBB\x83n (MCP)"},
        {"Watch the tips", "Xem l\xE1\xBA\xA1i m\xE1\xBA\xB9o"},
        {"Version history", "L\xE1\xBB\x8B""ch s\xE1\xBB\xAD phi\xC3\xAAn b\xE1\xBA\xA3n"},
        {"About", "Gi\xE1\xBB\x9Bi thi\xE1\xBB\x87u"},
        {"Connect with a cable", "K\xE1\xBA\xBFt n\xE1\xBB\x91i b\xE1\xBA\xB1ng d\xC3\xA2y"},
        {"Phone found. Turn on USB debugging:", "\xC4\x90\xC3\xA3 th\xE1\xBA\xA5y \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i. B\xE1\xBA\xADt g\xE1\xBB\xA1 l\xE1\xBB\x97i USB:"},
        {"Unlock the phone and tap Allow (tick Always allow).", "M\xE1\xBB\x9F kh\xC3\xB3""a \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i v\xC3\xA0 b\xE1\xBA\xA5m Cho ph\xC3\xA9p (t\xC3\xAD""ch Lu\xC3\xB4n cho ph\xC3\xA9p)."},
        {"iPhone needs no cable: Control Center > Screen Mirroring > PhoneMimi.", "iPhone kh\xC3\xB4ng c\xE1\xBA\xA7n d\xC3\xA2y: Trung t\xC3\xA2m \xC4\x91i\xE1\xBB\x81u khi\xE1\xBB\x83n > Ph\xE1\xBA\xA3n chi\xE1\xBA\xBFu m\xC3\xA0n h\xC3\xACnh > PhoneMimi."},
        {"Turn on USB debugging, then plug in the cable.", "B\xE1\xBA\xADt g\xE1\xBB\xA1 l\xE1\xBB\x97i USB r\xE1\xBB\x93i c\xE1\xBA\xAFm d\xC3\xA2y."},
        {"Other", "Kh\xC3\xA1""c"},
        {"Then plug in the cable and tap Allow on the phone.", "Sau \xC4\x91\xC3\xB3 c\xE1\xBA\xAFm d\xC3\xA2y v\xC3\xA0 b\xE1\xBA\xA5m Cho ph\xC3\xA9p tr\xC3\xAAn \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i."},
        {"Tap Build number 7 times, then enter your PIN", "Ch\xE1\xBA\xA1m Build number 7 l\xE1\xBA\xA7n, r\xE1\xBB\x93i nh\xE1\xBA\xADp m\xC3\xA3 PIN"},
        {"Settings > Developer options > USB debugging: On", "Settings > Developer options > USB debugging: B\xE1\xBA\xADt"},
        {"Tap OS version 7 times", "Ch\xE1\xBA\xA1m OS version 7 l\xE1\xBA\xA7n"},
        {"Turn on USB debugging and USB debugging (Security settings), then restart the phone", "B\xE1\xBA\xADt USB debugging v\xC3\xA0 USB debugging (Security settings), r\xE1\xBB\x93i kh\xE1\xBB\x9Fi \xC4\x91\xE1\xBB\x99ng l\xE1\xBA\xA1i m\xC3\xA1y"},
        {"Settings > Additional settings > Developer options > USB debugging: On", "Settings > Additional settings > Developer options > USB debugging: B\xE1\xBA\xADt"},
        {"Settings > System > Developer options > USB debugging: On", "Settings > System > Developer options > USB debugging: B\xE1\xBA\xADt"},
        {"Tap Build number 7 times", "Ch\xE1\xBA\xA1m Build number 7 l\xE1\xBA\xA7n"},
        {"Waiting for a phone", "\xC4\x90""ang ch\xE1\xBB\x9D \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Screen Mirroring, then pick PhoneMimi", "Ph\xE1\xBA\xA3n chi\xE1\xBA\xBFu m\xC3\xA0n h\xC3\xACnh, r\xE1\xBB\x93i ch\xE1\xBB\x8Dn PhoneMimi"},
        {"Plug in a USB cable, or press A", "C\xE1\xBA\xAFm d\xC3\xA2y USB, ho\xE1\xBA\xB7""c nh\xE1\xBA\xA5n A"},
        {"Add Android phone", "Th\xC3\xAAm \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i Android"},
        {"NEARBY", "G\xE1\xBA\xA6N \xC4\x90\xC3\x82Y"},
        {"Scanning", "\xC4\x90""ang t\xC3\xACm"},
        {"No phones yet. Turn on Wireless debugging.", "Ch\xC6\xB0""a th\xE1\xBA\xA5y m\xC3\xA1y n\xC3\xA0o. B\xE1\xBA\xADt G\xE1\xBB\xA1 l\xE1\xBB\x97i kh\xC3\xB4ng d\xC3\xA2y."},
        {"IP address", "\xC4\x90\xE1\xBB\x8B""a ch\xE1\xBB\x89 IP"},
        {"Connect port", "C\xE1\xBB\x95ng k\xE1\xBA\xBFt n\xE1\xBB\x91i"},
        {"Pair port", "C\xE1\xBB\x95ng gh\xC3\xA9p"},
        {"Pairing code", "M\xC3\xA3 gh\xC3\xA9p"},
        {"Remote access", "Truy c\xE1\xBA\xADp t\xE1\xBB\xAB xa"},
        {"Enable", "B\xE1\xBA\xADt"},
        {"Working...", "\xC4\x90""ang x\xE1\xBB\xAD l\xC3\xBD..."},
        {"Connect the Android phone first (USB or Wi-Fi)", "K\xE1\xBA\xBFt n\xE1\xBB\x91i \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i Android tr\xC6\xB0\xE1\xBB\x9B""c (USB ho\xE1\xBA\xB7""c Wi-Fi)"},
        {"QR code", "M\xC3\xA3 QR"},
        {"USB cable", "D\xC3\xA2y USB"},
        {"Connect", "K\xE1\xBA\xBFt n\xE1\xBB\x91i"},
        {"Connecting...", "\xC4\x90""ang k\xE1\xBA\xBFt n\xE1\xBB\x91i..."},
        {"Settings (S)", "C\xC3\xA0i \xC4\x91\xE1\xBA\xB7t (S)"},
        {"Mute phone", "T\xE1\xBA\xAFt ti\xE1\xBA\xBFng \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Volume up", "T\xC4\x83ng \xC3\xA2m l\xC6\xB0\xE1\xBB\xA3ng"},
        {"Volume down", "Gi\xE1\xBA\xA3m \xC3\xA2m l\xC6\xB0\xE1\xBB\xA3ng"},
        {"Power", "Ngu\xE1\xBB\x93n"},
        {"Mute: use the phone", "T\xE1\xBA\xAFt ti\xE1\xBA\xBFng: d\xC3\xB9ng n\xC3\xBAt tr\xC3\xAAn \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Volume: use the phone", "\xC3\x82m l\xC6\xB0\xE1\xBB\xA3ng: d\xC3\xB9ng n\xC3\xBAt tr\xC3\xAAn \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Power: use the phone", "Ngu\xE1\xBB\x93n: d\xC3\xB9ng n\xC3\xBAt tr\xC3\xAAn \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i"},
        {"Tips", "M\xE1\xBA\xB9o"},
        {"Mouse and keyboard", "Chu\xE1\xBB\x99t v\xC3\xA0 b\xC3\xA0n ph\xC3\xADm"},
        {"Click and scroll like touch. Type on the PC keyboard.", "B\xE1\xBA\xA5m v\xC3\xA0 cu\xE1\xBB\x99n nh\xC6\xB0 ch\xE1\xBA\xA1m tay. G\xC3\xB5 b\xE1\xBA\xB1ng b\xC3\xA0n ph\xC3\xADm PC."},
        {"Clipboard sync", "\xC4\x90\xE1\xBB\x93ng b\xE1\xBB\x99 clipboard"},
        {"Copy content on the phone or the PC, paste it on the other.", "Sao ch\xC3\xA9p n\xE1\xBB\x99i dung tr\xC3\xAAn \xC4\x91i\xE1\xBB\x87n tho\xE1\xBA\xA1i ho\xE1\xBA\xB7""c PC, d\xC3\xA1n b\xC3\xAAn kia."},
        {"Drag and drop files", "K\xC3\xA9o th\xE1\xBA\xA3 file"},
        {"Drop any file: APKs install, others go to Download.", "Th\xE1\xBA\xA3 file b\xE1\xBA\xA5t k\xE1\xBB\xB3: APK \xC4\x91\xC6\xB0\xE1\xBB\xA3""c c\xC3\xA0i, file kh\xC3\xA1""c v\xC3\xA0o Download."},
        {"Got it", "\xC4\x90\xC3\xA3 hi\xE1\xBB\x83u"}
    };
    auto it = kVi.find(en);
    return it == kVi.end() ? en : it->second;
}

// Measures cached text (ink width, line height).
static void measure_text_c(SDL_Renderer* r, const std::string& s, int px, int* w, int* h,
                           int weight = 500) {
    tex_cache_trim();
    auto& c = tex_cache()["t:" + std::to_string(px) + ":" + std::to_string(weight) + ":" + s];
    if (!c.tex) c.tex = make_text_texture(r, s, px, 255, 255, 255, &c.w, &c.h, weight);
    *w = c.tex ? c.w - (px + 3) / 4 : 0;
    *h = c.tex ? c.h : 0;
}

// Draws cached text. align: 0 = x is the left edge, 1 = centred on x, 2 = right edge.
// Returns the line height.
static int draw_text_c(SDL_Renderer* r, const std::string& s, int px,
                       uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca,
                       int x, int y, int align, int weight = 500) {
    int w = 0, h = 0;
    measure_text_c(r, s, px, &w, &h, weight);
    auto& c = tex_cache()["t:" + std::to_string(px) + ":" + std::to_string(weight) + ":" + s];
    if (!c.tex) return 0;
    int dx = align == 1 ? x - w / 2 : (align == 2 ? x - w : x);
    SDL_SetTextureColorMod(c.tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(c.tex, ca);
    SDL_Rect d = {dx, y, c.w, c.h};
    SDL_RenderCopy(r, c.tex, nullptr, &d);
    return c.h;
}

// draw_text_c with `y_mid` as the vertical centre of the line.
static void draw_text_mid(SDL_Renderer* r, const std::string& s, int px,
                          uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca,
                          int x, int y_mid, int align, int weight = 500) {
    int w = 0, h = 0;
    measure_text_c(r, s, px, &w, &h, weight);
    draw_text_c(r, s, px, cr, cg, cb, ca, x, y_mid - h / 2, align, weight);
}

// Largest font size <= px at which `s` fits in max_w.
static int fit_text_px(SDL_Renderer* r, const std::string& s, int px, int max_w,
                       int weight = 500) {
    int w = 0, h = 0;
    while (px > 8) {
        measure_text_c(r, s, px, &w, &h, weight);
        if (w <= max_w) break;
        --px;
    }
    return px;
}
#endif

// ---------------------------------------------------------------------------
// Platform icon generators (128px monochrome, downscaled for crispness)
// ---------------------------------------------------------------------------
// Area-average downscale that weights colour by alpha, so transparent
// pixels (black RGB) do not leave a dark halo around the shape.
static std::vector<uint8_t> shrink_rgba(const uint8_t* src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> out(size_t(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        int y0 = int((long long)y * sh / dh), y1 = std::max(y0 + 1, int((long long)(y + 1) * sh / dh));
        for (int x = 0; x < dw; ++x) {
            int x0 = int((long long)x * sw / dw), x1 = std::max(x0 + 1, int((long long)(x + 1) * sw / dw));
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = src + (size_t(sy) * sw + sx) * 4;
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; ++n;
                }
            uint8_t* o = &out[(size_t(y) * dw + x) * 4];
            if (a) { o[0] = uint8_t(r / a); o[1] = uint8_t(g / a); o[2] = uint8_t(b / a); }
            o[3] = uint8_t(a / n);
        }
    }
    return out;
}
static SDL_Texture* create_apple_icon(SDL_Renderer* renderer, int sz,
                                       uint8_t cr, uint8_t cg, uint8_t cb) {
    std::vector<uint8_t> rgba(sz * sz * 4, 0);
    float sc = sz / 64.0f;
    for (int py = 0; py < sz; py++) {
        for (int px = 0; px < sz; px++) {
            float x = px / sc, y = py / sc;
            bool filled = false;
            // Left body lobe
            if ((x-26)*(x-26) + (y-38)*(y-38) <= 16*16) filled = true;
            // Right body lobe
            if ((x-38)*(x-38) + (y-38)*(y-38) <= 16*16) filled = true;
            // Bottom fill
            if ((x-32)*(x-32) + (y-42)*(y-42) <= 14*14) filled = true;
            // Bite cutout (right side)
            if ((x-52)*(x-52) + (y-30)*(y-30) <= 11*11) filled = false;
            // Top cutoff
            if (y < 23) filled = false;
            // Stem
            if (x >= 31 && x <= 33.5f && y >= 12 && y <= 23) filled = true;
            // Leaf (tilted ellipse)
            float lx = (x-37)*0.87f + (y-16)*0.5f;
            float ly = -(x-37)*0.5f + (y-16)*0.87f;
            if ((lx*lx)/(6*6) + (ly*ly)/(3*3) <= 1) filled = true;

            if (filled) {
                int idx = (py * sz + px) * 4;
                rgba[idx+0] = cr; rgba[idx+1] = cg; rgba[idx+2] = cb; rgba[idx+3] = 255;
            }
        }
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, sz, sz);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), sz * 4);
    }
    return tex;
}

static SDL_Texture* create_android_icon(SDL_Renderer* renderer, int sz,
                                          uint8_t cr, uint8_t cg, uint8_t cb) {
    std::vector<uint8_t> rgba(sz * sz * 4, 0);
    float sc = sz / 64.0f;
    for (int py = 0; py < sz; py++) {
        for (int px = 0; px < sz; px++) {
            float x = px / sc, y = py / sc;
            bool filled = false;
            // Head dome (top half of circle)
            if ((x-32)*(x-32) + (y-38)*(y-38) <= 18*18 && y <= 38) filled = true;
            // Head flat bottom
            if (x >= 14 && x <= 50 && y > 38 && y <= 46) filled = true;
            // Eye cutouts
            if ((x-24)*(x-24) + (y-33)*(y-33) <= 3*3) filled = false;
            if ((x-40)*(x-40) + (y-33)*(y-33) <= 3*3) filled = false;
            // Left antenna
            {
                float ax=21, ay=14, bx=25, by=22;
                float dx=bx-ax, dy=by-ay, len2=dx*dx+dy*dy;
                float t = std::clamp(((x-ax)*dx + (y-ay)*dy) / len2, 0.0f, 1.0f);
                float px2=ax+t*dx, py2=ay+t*dy;
                if ((x-px2)*(x-px2) + (y-py2)*(y-py2) <= 2.2f*2.2f) filled = true;
                if ((x-ax)*(x-ax) + (y-ay)*(y-ay) <= 2.5f*2.5f) filled = true;
            }
            // Right antenna
            {
                float ax=43, ay=14, bx=39, by=22;
                float dx=bx-ax, dy=by-ay, len2=dx*dx+dy*dy;
                float t = std::clamp(((x-ax)*dx + (y-ay)*dy) / len2, 0.0f, 1.0f);
                float px2=ax+t*dx, py2=ay+t*dy;
                if ((x-px2)*(x-px2) + (y-py2)*(y-py2) <= 2.2f*2.2f) filled = true;
                if ((x-ax)*(x-ax) + (y-ay)*(y-ay) <= 2.5f*2.5f) filled = true;
            }

            if (filled) {
                int idx = (py * sz + px) * 4;
                rgba[idx+0] = cr; rgba[idx+1] = cg; rgba[idx+2] = cb; rgba[idx+3] = 255;
            }
        }
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, sz, sz);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), sz * 4);
    }
    return tex;
}

// Alpha-blend the phone-bezel composite ON TOP of an already-painted
// destination canvas. Used by the webcam-strip capture/recording path
// to tuck the strip up behind the phone's rounded bottom bezel — the
// strip is painted first at `base_h - overlap`, then the phone bezel
// is composited over the first `phone_h` rows of the canvas. Where the
// bezel is opaque it hides the underlying strip; where the bezel's
// rounded-corner cutouts are transparent the drawer body bleeds through.
static inline void blit_phone_over_strip(uint8_t* dst, int dst_stride,
                                         const uint8_t* phone, int phone_stride,
                                         int phone_w, int phone_h) {
    for (int y = 0; y < phone_h; ++y) {
        const uint8_t* sp = phone + (size_t)y * phone_stride;
        uint8_t* dp = dst + (size_t)y * dst_stride;
        for (int x = 0; x < phone_w; ++x, sp += 4, dp += 4) {
            uint8_t a = sp[3];
            if (a == 0) continue;
            if (a == 255) {
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
            } else {
                int ia = 255 - a;
                dp[0] = (uint8_t)((sp[0] * a + dp[0] * ia) / 255);
                dp[1] = (uint8_t)((sp[1] * a + dp[1] * ia) / 255);
                dp[2] = (uint8_t)((sp[2] * a + dp[2] * ia) / 255);
                dp[3] = (uint8_t)std::max<int>(dp[3], a);
            }
        }
    }
}

namespace opm::media {

Renderer::Renderer() = default;

Renderer::~Renderer() { shutdown(); }

bool Renderer::init(const std::string& title, int /*width*/, int /*height*/) {
    // Deliver the click that activates an unfocused window instead of
    // forcing the user to click once to focus and again to interact.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
#ifdef _WIN32
    load_embedded_fonts();
#endif

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "[Renderer] SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }

    window_ = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        400, 800,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS
    );
    if (!window_) {
        std::cerr << "[Renderer] Failed to create window: " << SDL_GetError() << "\n";
        return false;
    }

#ifdef _WIN32
    install_renderer_window_subclass(window_);
#endif

    sdl_renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer_) {
        std::cerr << "[Renderer] Failed to create renderer: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window_); window_ = nullptr;
        return false;
    }
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    load_icon_texture();

    // Load persisted user settings (bezel colour, screenshot toggles) BEFORE
    // the first phone-frame generate so the initial texture uses the saved
    // colour. Otherwise set_bezel_color() would invalidate the freshly-built
    // frame texture and the waiting screen would never render.
    settings_ = opm::Settings::load();
    android_kbd_on_ = settings_.android_keyboard_on;
    android_screen_off_ = settings_.android_screen_off;
    phone_frame_.set_bezel_color(settings_.bezel_r, settings_.bezel_g, settings_.bezel_b);
    SDL_SetWindowAlwaysOnTop(window_, settings_.always_on_top ? SDL_TRUE : SDL_FALSE);

#ifdef ENABLE_WEBCAM
    // Restore previous drawer state without re-saving. Start the capture
    // worker in a detached thread so MF initialization never blocks the
    // first frame of UI.
    if (settings_.webcam_drawer_open) {
        // Crash guard: if the previous launch did not get past the first
        // webcam frame, the marker file from that run is still present.
        // Assume the persisted device is the culprit and disable the
        // auto-open path so the user can reopen the app and pick another
        // camera (or just keep the drawer closed).
        if (opm::Settings::webcam_pending_exists()) {
            std::cerr << "[Webcam] Previous launch did not finish the webcam start path; "
                         "disabling auto-open and clearing saved device.\n";
            settings_.webcam_drawer_open = false;
            settings_.webcam_device_id.clear();
            webcam_requires_manual_pick_ = true;
            settings_.save();
            opm::Settings::clear_webcam_pending();
        } else {
            webcam_drawer_visible_ = true;
            webcam_drawer_anim_    = 1.0f;
            std::string dev = settings_.webcam_device_id;
            opm::Settings::set_webcam_pending();
            webcam_pending_cleared_ = false;
            std::thread([this, dev]() {
                if (!webcam_.start(dev, 1280, 720)) {
                    std::cerr << "[Webcam] start failed: "
                              << webcam_.last_error() << "\n";
                    opm::Settings::clear_webcam_pending();
                    webcam_pending_cleared_ = true;
                }
            }).detach();
        }
    }
#endif

    phone_frame_.generate(sdl_renderer_, 390, 844);

    SDL_DisplayMode dm;
    SDL_GetCurrentDisplayMode(0, &dm);
    int fw = phone_frame_.frame_width();
    int fh = phone_frame_.frame_height();
    float s = std::min(dm.w * 0.32f / fw, dm.h * 0.65f / fh);
    SDL_SetWindowSize(window_, (int)(fw * s), (int)(fh * s));
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    // Set window icon for taskbar
#ifdef _WIN32
    {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window_, &info)) {
            HICON hIcon = LoadIconA(GetModuleHandle(nullptr), MAKEINTRESOURCE(101));
            if (hIcon) {
                SendMessage(info.info.win.window, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                SendMessage(info.info.win.window, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            }
        }
    }
#endif

    // Pre-render waiting text
#ifdef _WIN32
    waiting_tex_ = make_text_texture(sdl_renderer_, "Waiting for connection...",
                                      48, 180, 180, 180, &waiting_tex_w_, &waiting_tex_h_);
    ios_instr_tex_ = make_text_texture(sdl_renderer_,
        "Control Center > Screen Mirroring > PhoneMimi",
        32, 130, 130, 130, &ios_instr_w_, &ios_instr_h_);
    android_instr_tex_ = make_text_texture(sdl_renderer_,
        "Press A  -  Settings > Wireless debugging > Pair with code",
        32, 130, 130, 130, &android_instr_w_, &android_instr_h_);
    // Load platform icons from PNG files
    {
        char exe_path[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        std::string icon_dir(exe_path);
        auto sep = icon_dir.find_last_of("\\/");
        if (sep != std::string::npos) icon_dir = icon_dir.substr(0, sep);

        int iw = 0, ih = 0, ic = 0;
        unsigned char* apple_data = stbi_load((icon_dir + "\\apple_icon.png").c_str(), &iw, &ih, &ic, 4);
        if (apple_data) {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
            ios_icon_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC, iw, ih);
            if (ios_icon_tex_) {
                SDL_SetTextureBlendMode(ios_icon_tex_, SDL_BLENDMODE_BLEND);
                SDL_UpdateTexture(ios_icon_tex_, nullptr, apple_data, iw * 4);
                ios_icon_sz_ = iw;
            }
            stbi_image_free(apple_data);
        } else {
            ios_icon_tex_ = create_apple_icon(sdl_renderer_, 128, 130, 130, 130);
            ios_icon_sz_ = 128;
        }

        unsigned char* android_data = stbi_load((icon_dir + "\\android_icon.png").c_str(), &iw, &ih, &ic, 4);
        if (android_data) {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
            android_icon_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                                   SDL_TEXTUREACCESS_STATIC, iw, ih);
            if (android_icon_tex_) {
                SDL_SetTextureBlendMode(android_icon_tex_, SDL_BLENDMODE_BLEND);
                SDL_UpdateTexture(android_icon_tex_, nullptr, android_data, iw * 4);
                android_icon_sz_ = iw;
            }
            stbi_image_free(android_data);
        } else {
            android_icon_tex_ = create_android_icon(sdl_renderer_, 128, 130, 130, 130);
            android_icon_sz_ = 128;
        }
    }
    {
        const void* png = nullptr; DWORD png_size = 0;
        int lw = 0, lh = 0, lc = 0;
        unsigned char* logo = load_rcdata(210, &png, &png_size)
            ? stbi_load_from_memory(static_cast<const unsigned char*>(png), (int)png_size, &lw, &lh, &lc, 4)
            : nullptr;
        if (logo) {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
            logo_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, lw, lh);
            if (logo_tex_) {
                SDL_SetTextureBlendMode(logo_tex_, SDL_BLENDMODE_BLEND);
                SDL_UpdateTexture(logo_tex_, nullptr, logo, lw * 4);
                logo_w_ = lw; logo_h_ = lh;
                logo_rgba_.assign(logo, logo + size_t(lw) * lh * 4);
            }
            stbi_image_free(logo);
        }
    }
#endif

    // Footer segments for waiting screen
#ifdef _WIN32
    {
        auto seg = [&](const std::wstring& text, uint8_t r, uint8_t g, uint8_t b,
                        const std::string& url = "", const std::string& tip = "",
                        const wchar_t* font = nullptr) -> FooterSeg {
            FooterSeg fs;
            fs.url = url;
            fs.tooltip = tip;
            fs.tex = make_text_texture_w(sdl_renderer_, text, 40, r, g, b, &fs.w, &fs.h, font);
            return fs;
        };
        // Line 3: " · v<version>" — broken onto its own line so the second
        // line stays a comfortable width on narrow phone aspects. Version
        // string is built at runtime from OPM_VERSION_* (set by CMake) so
        // the displayed number always matches the built binary.
        std::wstring ver_w = L"v" + std::to_wstring(OPM_VERSION_MAJOR) +
                              L"." + std::to_wstring(OPM_VERSION_MINOR) +
                              L"." + std::to_wstring(OPM_VERSION_PATCH);
        footer_line3_.push_back(seg(ver_w.c_str(), 255, 255, 255,
                                     "", "Version history (V)"));

        // Mirror the same content for the Info panel, but baked at the
        // smaller font size used by the network-requirement section so
        // the visual weight matches the surrounding text.
        auto iseg = [&](const std::wstring& text, uint8_t r, uint8_t g, uint8_t b,
                         const std::string& url = "", const std::string& tip = "",
                         const wchar_t* font = nullptr) -> FooterSeg {
            FooterSeg fs;
            fs.url = url;
            fs.tooltip = tip;
            fs.tex = make_text_texture_w(sdl_renderer_, text, 30, r, g, b, &fs.w, &fs.h, font);
            return fs;
        };
        info_footer_line1_.push_back(iseg(ver_w.c_str(), 130, 130, 130,
                                           "", "Version history (V)"));
    }
#endif

    // Info panel content
#ifdef _WIN32
    {
        auto make_info = [&](const std::wstring& text, int font_sz,
                              uint8_t r, uint8_t g, uint8_t b) -> InfoLine {
            InfoLine line;
            line.tex = make_text_texture_w(sdl_renderer_, text, font_sz, r, g, b, &line.w, &line.h);
            return line;
        };
        std::wstring info_title = L"PhoneMimi " +
                                  (L"v" + std::to_wstring(OPM_VERSION_MAJOR) +
                                   L"." + std::to_wstring(OPM_VERSION_MINOR) +
                                   L"." + std::to_wstring(OPM_VERSION_PATCH));
        info_lines_.push_back(make_info(info_title.c_str(), 44, 255, 255, 255));
        info_lines_.push_back(make_info(L"AirPlay (iOS) \u00B7 scrcpy (Android)", 34, 160, 160, 160));
        info_lines_.push_back({nullptr, 0, 0}); // spacer
        info_lines_.push_back(make_info(L"(F) Fullscreen \u00B7 (M) Menu \u00B7 (L) Log \u00B7 (A) Add Android", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(I) Info \u00B7 (V) Version \u00B7 (S) Settings", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(W) Webcam \u00B7 (U) Mute audio \u00B7 (Esc) Quit", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(Ctrl+S) Screenshot \u00B7 (Ctrl+Shift+S) Annotate", 30, 130, 130, 130));
#ifdef _WIN32
        info_lines_.push_back(make_info(L"(Ctrl+Shift+T) OCR copy text from a region", 30, 130, 130, 130));
#endif
        info_lines_.push_back(make_info(L"(Ctrl+R) Record \u00B7 (Ctrl+0) Reset window size", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"(Ctrl+1\u20139) Switch to device 1\u20139 in the bottom-bezel picker", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"In log: (Ctrl+C) Copy \u00B7 (Ctrl+X) Clear", 30, 130, 130, 130));
        info_lines_.push_back({nullptr, 0, 0}); // spacer
        info_lines_.push_back(make_info(L"Network requirements", 34, 160, 160, 160));
        info_lines_.push_back(make_info(L"Same Wi-Fi / VLAN as the phone, mDNS (UDP 5353) allowed", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"AirPlay: TCP 7000/7001/7100, UDP 6000-6010", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"Android: TCP 27183 (loopback) + ADB pair port from phone", 30, 130, 130, 130));
        info_lines_.push_back(make_info(L"Installer adds Windows Firewall rules for the .exe", 30, 130, 130, 130));

        // "About" header that sits just above the MSEndpointMgr/Simon/version
        // footer block at the bottom of the Info panel. Sized to match the
        // "Network requirements" section header.
        info_about_header_ = make_info(L"About", 34, 160, 160, 160);
    }
#endif

    // Version panel content (newest first)
#ifdef _WIN32
    {
        auto make_ver = [&](const std::wstring& text, int font_sz,
                             uint8_t r, uint8_t g, uint8_t b) -> InfoLine {
            InfoLine line;
            line.tex = make_text_texture_w(sdl_renderer_, text, font_sz, r, g, b, &line.w, &line.h);
            return line;
        };
        // Wrap width for description rows. Picked so that at the panel's
        // default text scale (0.5) the displayed line width fits the
        // narrowest realistic panel without further auto-shrinking, but
        // is wide enough that short descriptions never wrap.
        constexpr int kVerWrapW = 720;
        auto make_desc = [&](const std::wstring& text) -> InfoLine {
            InfoLine line;
            line.tex = make_text_texture_w_wrapped(sdl_renderer_, text, 30,
                                                    160, 160, 160, kVerWrapW,
                                                    &line.w, &line.h);
            return line;
        };
        version_lines_.push_back(make_ver(L"Version History", 40, 255, 255, 255));
        version_lines_.push_back({nullptr, 0, 0}); // spacer
        version_lines_.push_back(make_ver(L"16.11.2026 \u2013 3.6.0", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"Android remote access over Tailscale; connect a paired phone without a code"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"16.11.2026 \u2013 3.5.2", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"New setup.exe installer: one window, no Windows Installer flash"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"16.11.2026 \u2013 3.5.1", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"Installer: step-by-step wizard, choose the folder, desktop and Uninstall shortcuts"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"16.11.2026 \u2013 3.5.0", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"Phone sound on the PC, clipboard both ways, drop files to install or send, cable setup guide"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"16.11.2026 \u2013 3.4.3", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"Sharper mirrored video: no more over-shrinking, 4 samples per pixel"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"15.11.2026 \u2013 3.4.2", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"Sharper phone frame edges and penguin logo in small windows"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"15.11.2026 \u2013 3.4.1", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"MCP: screenshots always show the latest screen, steadier reconnects over Wi-Fi"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"15.11.2026 \u2013 3.4.0", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"Claude Code can see and drive your phones (MCP server, Settings > Phone)"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"15.11.2026 \u2013 3.3.6", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"New look: Lucide icons, accent colours, new switches, Add Android panel, penguin waiting screen"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_ver(L"14.11.2026 \u2013 0.7.0", 34, 200, 200, 255));
        version_lines_.push_back(make_desc(L"PhoneMimi: Android over USB, typing and paste to the phone, side buttons, tabbed Settings"));
        version_lines_.push_back({nullptr, 0, 0});
        version_lines_.push_back(make_desc(L"Based on 1PhoneMirror by MSEndpointMgr (GPL-3.0)"));
    }
#endif

    // Screenshot folder
#ifdef _WIN32
    char pictures[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYPICTURES, nullptr, 0, pictures)))
        screenshot_dir_ = std::string(pictures) + "\\PhoneMimi";
    else
        screenshot_dir_ = "screenshots";
#else
    screenshot_dir_ = "screenshots";
#endif

    // (Settings already loaded earlier in init() before the first phone-frame
    // generate so the initial waiting screen uses the saved bezel colour.)

    std::cout << "[Renderer] Window initialized\n";
    return true;
}

void Renderer::shutdown() {
    running_.store(false);
    stop_android_discovery();
    if (recorder_.is_recording()) {
        std::cout << "[Recorder] Finalising on shutdown\n";
        recorder_.stop();
    }
    for (auto& s : footer_line1_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : footer_line2_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : footer_line3_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    footer_line1_.clear();
    footer_line2_.clear();
    footer_line3_.clear();
    for (auto& s : info_footer_line1_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : info_footer_line2_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    for (auto& s : info_footer_line3_) { if (s.tex) SDL_DestroyTexture(s.tex); }
    info_footer_line1_.clear();
    info_footer_line2_.clear();
    info_footer_line3_.clear();
    if (info_about_header_.tex) {
        SDL_DestroyTexture(info_about_header_.tex);
        info_about_header_.tex = nullptr;
    }
    if (footer_tooltip_tex_) { SDL_DestroyTexture(footer_tooltip_tex_); footer_tooltip_tex_ = nullptr; }
    if (toast_tex_) { SDL_DestroyTexture(toast_tex_); toast_tex_ = nullptr; }
    if (update_line1_tex_) { SDL_DestroyTexture(update_line1_tex_); update_line1_tex_ = nullptr; }
    if (update_link_tex_) { SDL_DestroyTexture(update_link_tex_); update_link_tex_ = nullptr; }
    if (tooltip_tex_) { SDL_DestroyTexture(tooltip_tex_); tooltip_tex_ = nullptr; }
    if (bezel_tip_tex_) { SDL_DestroyTexture(bezel_tip_tex_); bezel_tip_tex_ = nullptr; }
    if (bezel_tip_tex2_) { SDL_DestroyTexture(bezel_tip_tex2_); bezel_tip_tex2_ = nullptr; }
    if (waiting_tex_) { SDL_DestroyTexture(waiting_tex_); waiting_tex_ = nullptr; }
    if (pin_label_tex_) { SDL_DestroyTexture(pin_label_tex_); pin_label_tex_ = nullptr; }
    if (pin_digits_tex_) { SDL_DestroyTexture(pin_digits_tex_); pin_digits_tex_ = nullptr; }
    if (icon_texture_) { SDL_DestroyTexture(icon_texture_); icon_texture_ = nullptr; }
    if (ios_instr_tex_) { SDL_DestroyTexture(ios_instr_tex_); ios_instr_tex_ = nullptr; }
    if (android_instr_tex_) { SDL_DestroyTexture(android_instr_tex_); android_instr_tex_ = nullptr; }
    if (ios_icon_tex_) { SDL_DestroyTexture(ios_icon_tex_); ios_icon_tex_ = nullptr; }
    if (android_icon_tex_) { SDL_DestroyTexture(android_icon_tex_); android_icon_tex_ = nullptr; }
    if (logo_tex_) { SDL_DestroyTexture(logo_tex_); logo_tex_ = nullptr; }
    if (logo_small_) { SDL_DestroyTexture(logo_small_); logo_small_ = nullptr; }
    if (tips_tex_) { SDL_DestroyTexture(tips_tex_); tips_tex_ = nullptr; }
    for (MipChain* c : {&video_mip_, &frame_mip_, &tips_mip_}) {
        for (auto& t : c->tex) if (t) { SDL_DestroyTexture(t); t = nullptr; }
        if (c->out) { SDL_DestroyTexture(c->out); c->out = nullptr; }
    }
    for (auto& l : info_lines_) { if (l.tex) SDL_DestroyTexture(l.tex); }
    info_lines_.clear();
    for (auto& l : version_lines_) { if (l.tex) SDL_DestroyTexture(l.tex); }
    version_lines_.clear();
    if (pending_overlay_tex_) { SDL_DestroyTexture(pending_overlay_tex_); pending_overlay_tex_ = nullptr; }
    if (protected_overlay_tex_) { SDL_DestroyTexture(protected_overlay_tex_); protected_overlay_tex_ = nullptr; }
    if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
    if (sdl_renderer_) { SDL_DestroyRenderer(sdl_renderer_); sdl_renderer_ = nullptr; }
    if (window_) {
#ifdef _WIN32
        remove_renderer_window_subclass(window_);
#endif
        SDL_DestroyWindow(window_); window_ = nullptr;
    }
    SDL_Quit();
}

void Renderer::submit_frame(VideoFrame frame) {
    std::lock_guard lock(frame_mutex_);
    pending_frame_ = std::move(frame);
    has_new_frame_ = true;
}

bool Renderer::copy_shown_frame(std::vector<uint8_t>* rgba, int* w, int* h) {
    std::lock_guard lock(frame_mutex_);
    if (last_frame_data_.empty() || last_frame_w_ <= 0 || last_frame_h_ <= 0 ||
        last_frame_stride_ < last_frame_w_ * 4)
        return false;
    rgba->resize(size_t(last_frame_w_) * last_frame_h_ * 4);
    for (int y = 0; y < last_frame_h_; ++y)
        memcpy(rgba->data() + size_t(y) * last_frame_w_ * 4,
               last_frame_data_.data() + size_t(y) * last_frame_stride_, size_t(last_frame_w_) * 4);
    *w = last_frame_w_;
    *h = last_frame_h_;
    return true;
}

void Renderer::load_icon_texture() {
#ifdef _WIN32
    HICON hIcon = (HICON)LoadImageA(GetModuleHandle(nullptr),
                                     MAKEINTRESOURCE(101), IMAGE_ICON,
                                     64, 64, LR_DEFAULTCOLOR);
    if (!hIcon) return;

    ICONINFO ii;
    if (!GetIconInfo(hIcon, &ii)) { DestroyIcon(hIcon); return; }

    BITMAP bm;
    GetObject(ii.hbmColor, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    std::vector<uint8_t> pixels(w * h * 4);
    HDC hdc = CreateCompatibleDC(nullptr);
    GetDIBits(hdc, ii.hbmColor, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS);
    DeleteDC(hdc);

    for (int i = 0; i < w * h; i++)
        std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);

    icon_texture_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, w, h);
    if (icon_texture_) {
        SDL_SetTextureBlendMode(icon_texture_, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(icon_texture_, nullptr, pixels.data(), w * 4);
        icon_tex_w_ = w;
        icon_tex_h_ = h;
    }

    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    DestroyIcon(hIcon);
#endif
}

void Renderer::run() {
    running_.store(true);

    while (running_.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Screenshot annotator is modal: while it's up, every event
            // gets routed through it first and most are swallowed.
            if (annotator_active_) {
                if (event.type == SDL_QUIT) { running_.store(false); return; }
                if (handle_annotator_event(event)) continue;
            }
#ifdef _WIN32
            // OCR region picker is modal too.
            if (ocr_active_) {
                if (event.type == SDL_QUIT) { running_.store(false); return; }
                if (handle_ocr_event(event)) continue;
            }
#endif
            switch (event.type) {
            case SDL_QUIT:
                running_.store(false);
                return;

            case SDL_KEYUP:
                android_forward_key_(event);
                break;

            case SDL_DROPFILE:
                if (event.drop.file) {
                    drop_paths_.emplace_back(event.drop.file);
                    SDL_free(event.drop.file);
                }
                break;

            case SDL_DROPCOMPLETE:
                if (!drop_paths_.empty() && android_drop_fn_) android_drop_fn_(std::move(drop_paths_));
                drop_paths_.clear();
                break;

            case SDL_CLIPBOARDUPDATE:
                // Text copied on the PC goes to the phone clipboard (no paste).
                // Skips our own write of the phone's text and non-text data.
                if (android_input_active_() && SDL_HasClipboardText()) {
                    char* t = SDL_GetClipboardText();
                    std::string text = t ? t : "";
                    SDL_free(t);
                    if (!text.empty() && text != last_synced_clip_) {
                        last_synced_clip_ = text;
                        AndroidInput in;
                        in.kind = AndroidInput::Kind::SetClipboard;
                        in.text = text;
                        android_input_fn_(in);
                    }
                }
                break;

            case SDL_KEYDOWN:
                if (android_forward_key_(event)) break;
                // When the Android panel is open, capture keys for editing.
                if (android_panel_visible_) {
                    auto& fld = (android_focus_ == 0 ? android_ip_
                              :  android_focus_ == 1 ? android_connect_port_
                              :  android_focus_ == 2 ? android_port_
                              :                        android_pin_);
                    if (android_qr_active_.load()) {
                        if (event.key.keysym.sym == SDLK_ESCAPE) android_qr_stop();
                        break;
                    }
                    if (event.key.keysym.sym == SDLK_BACKSPACE) {
                        if (!fld.empty()) fld.pop_back();
                        break;
                    }
                    if (event.key.keysym.sym == SDLK_TAB) {
                        android_focus_ = (android_focus_ + 1) % 4;
                        break;
                    }
                    if (event.key.keysym.sym == SDLK_RETURN ||
                        event.key.keysym.sym == SDLK_KP_ENTER) {
                        android_submit();
                        break;
                    }
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        android_panel_visible_ = false;
                        android_panel_animating_ = true;
                        android_panel_anim_start_ = std::chrono::steady_clock::now();
                        SDL_StopTextInput();
                        break;
                    }
                    // Swallow everything else; SDL_TEXTINPUT delivers chars.
                    break;
                }

                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running_.store(false);
                    return;
                }
                if (event.key.keysym.sym == SDLK_f) {
                    uint32_t flags = SDL_GetWindowFlags(window_);
                    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_SetWindowFullscreen(window_, 0);
                        window_shape_set_ = false; // recalculate on next render
                    } else {
#ifdef _WIN32
                        SDL_SysWMinfo info; SDL_VERSION(&info.version);
                        if (SDL_GetWindowWMInfo(window_, &info))
                            SetWindowRgn(info.info.win.window, nullptr, TRUE);
#endif
                        SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
                        window_shape_set_ = false; // re-apply shape after layout
                    }
                }
                if (event.key.keysym.sym == SDLK_l) {
                    toggle_drawer(DrawerMode::Log);
                }
                if (event.key.keysym.sym == SDLK_t &&
                    !(event.key.keysym.mod & (KMOD_CTRL | KMOD_SHIFT))) {
                    toggle_drawer(DrawerMode::Stats);
                }
                if (event.key.keysym.sym == SDLK_w) {
                    toggle_webcam_drawer();
                }
                if (event.key.keysym.sym == SDLK_u &&
                    !(event.key.keysym.mod & (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT))) {
                    audio_muted_ = !audio_muted_;
                    if (audio_mute_fn_) audio_mute_fn_(audio_muted_);
                }
                if (event.key.keysym.sym == SDLK_i) {
                    info_panel_visible_ = !info_panel_visible_;
                    info_panel_animating_ = true;
                    info_panel_anim_start_ = std::chrono::steady_clock::now();
                    if (info_panel_visible_ && version_panel_visible_) {
                        version_panel_visible_ = false;
                        version_panel_animating_ = true;
                        version_panel_anim_start_ = std::chrono::steady_clock::now();
                    }
                    if (info_panel_visible_ && settings_panel_visible_) {
                        settings_panel_visible_ = false;
                        settings_panel_animating_ = true;
                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
                    }
                }
                if (event.key.keysym.sym == SDLK_v) {
                    version_panel_visible_ = !version_panel_visible_;
                    version_panel_animating_ = true;
                    version_panel_anim_start_ = std::chrono::steady_clock::now();
                    version_scroll_offset_ = 0;
                    if (version_panel_visible_ && info_panel_visible_) {
                        info_panel_visible_ = false;
                        info_panel_animating_ = true;
                        info_panel_anim_start_ = std::chrono::steady_clock::now();
                    }
                    if (version_panel_visible_ && settings_panel_visible_) {
                        settings_panel_visible_ = false;
                        settings_panel_animating_ = true;
                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
                    }
                }
                if (event.key.keysym.sym == SDLK_s &&
                    (event.key.keysym.mod & KMOD_CTRL) &&
                    (event.key.keysym.mod & KMOD_SHIFT)) {
                    // Ctrl+Shift+S: open the screenshot annotator on the
                    // current composited frame. Handled before plain Ctrl+S
                    // so the shifted shortcut wins.
                    begin_annotation();
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                } else if (event.key.keysym.sym == SDLK_s &&
                           (event.key.keysym.mod & KMOD_CTRL) &&
                           !(event.key.keysym.mod & KMOD_SHIFT)) {
                    screenshot_requested_ = true;
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
                // Ctrl+R toggles screen recording.
                if (event.key.keysym.sym == SDLK_r && (event.key.keysym.mod & KMOD_CTRL)) {
                    record_toggle_requested_ = true;
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
                // Ctrl+0 resets the window to its default size — matches
                // the "reset zoom" idiom (Ctrl+0 in browsers / editors) and
                // mirrors the right-click action on the resize grip.
                // Explicitly exclude Alt so AltGr+0 (which types '}' on
                // Norwegian/German/etc. layouts and arrives as Ctrl+Alt on
                // Windows) does not get misread as Ctrl+0.
                if ((event.key.keysym.sym == SDLK_0 || event.key.keysym.sym == SDLK_KP_0) &&
                    (event.key.keysym.mod & KMOD_CTRL) &&
                    !(event.key.keysym.mod & KMOD_ALT) &&
                    !(event.key.keysym.mod & KMOD_SHIFT)) {
                    reset_window_to_default_size();
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
                // Ctrl+1..Ctrl+9 — switch active source by picker order.
                // Same Alt/Shift guard as Ctrl+0 to avoid AltGr collisions.
                if ((event.key.keysym.mod & KMOD_CTRL) &&
                    !(event.key.keysym.mod & KMOD_ALT) &&
                    !(event.key.keysym.mod & KMOD_SHIFT)) {
                    int picked = -1;
                    SDL_Keycode k = event.key.keysym.sym;
                    if      (k == SDLK_1 || k == SDLK_KP_1) picked = 0;
                    else if (k == SDLK_2 || k == SDLK_KP_2) picked = 1;
                    else if (k == SDLK_3 || k == SDLK_KP_3) picked = 2;
                    else if (k == SDLK_4 || k == SDLK_KP_4) picked = 3;
                    else if (k == SDLK_5 || k == SDLK_KP_5) picked = 4;
                    else if (k == SDLK_6 || k == SDLK_KP_6) picked = 5;
                    else if (k == SDLK_7 || k == SDLK_KP_7) picked = 6;
                    else if (k == SDLK_8 || k == SDLK_KP_8) picked = 7;
                    else if (k == SDLK_9 || k == SDLK_KP_9) picked = 8;
                    if (picked >= 0 && get_sources_fn_ && set_active_source_fn_) {
                        auto sources = get_sources_fn_();
                        if (picked < (int)sources.size()) {
                            const auto& sel = sources[picked];
                            if (!sel.active) {
                                pending_source_name_ = sel.name;
                                source_just_switched_ = true;
                            }
                            set_active_source_fn_(sel.id);
                            btn_flash_ = true;
                            btn_flash_start_ = std::chrono::steady_clock::now();
                        }
                    }
                }
#ifdef _WIN32
                // Ctrl+Shift+T: OCR copy. Open the region picker over the
                // current composite; on mouse-up the cropped pixels go
                // through Windows.Media.Ocr and the recognised text lands
                // on the clipboard.
                if (event.key.keysym.sym == SDLK_t &&
                    (event.key.keysym.mod & KMOD_CTRL) &&
                    (event.key.keysym.mod & KMOD_SHIFT)) {
                    begin_ocr();
                    btn_flash_ = true;
                    btn_flash_start_ = std::chrono::steady_clock::now();
                }
#endif
                // Plain (S) without Ctrl toggles the Settings panel.
                if (event.key.keysym.sym == SDLK_s && !(event.key.keysym.mod & KMOD_CTRL)) {
                    settings_panel_visible_ = !settings_panel_visible_;
                    settings_panel_animating_ = true;
                    settings_panel_anim_start_ = std::chrono::steady_clock::now();
                    if (settings_panel_visible_) {
                        if (info_panel_visible_) {
                            info_panel_visible_ = false;
                            info_panel_animating_ = true;
                            info_panel_anim_start_ = std::chrono::steady_clock::now();
                        }
                        if (version_panel_visible_) {
                            version_panel_visible_ = false;
                            version_panel_animating_ = true;
                            version_panel_anim_start_ = std::chrono::steady_clock::now();
                        }
                    }
                }
                // Log shortcuts: only when log panel is visible and no
                // text input panel is open.
                if (log_panel_visible_ && !android_panel_visible_ &&
                    (event.key.keysym.mod & KMOD_CTRL)) {
                    if (event.key.keysym.sym == SDLK_c) {
                        std::string all;
                        for (auto& ln : opm::LogBuffer::instance().get_lines()) {
                            all += ln; all += '\n';
                        }
                        SDL_SetClipboardText(all.c_str());
                        std::cout << "[Renderer] Log copied to clipboard ("
                                  << all.size() << " bytes)\n";
                    } else if (event.key.keysym.sym == SDLK_x) {
                        opm::LogBuffer::instance().clear();
                        std::cout << "[Renderer] Log cleared\n";
                    }
                }
                if (event.key.keysym.sym == SDLK_a && add_android_fn_) {
                    show_android_panel();
                }
                break;

            case SDL_TEXTEDITING:
                // An IME (e.g. Vietnamese Telex) holds the word it is
                // composing until Space; mirror it live so every keystroke
                // shows on the phone straight away.
                if (android_kbd_on_ && !android_panel_visible_ && android_input_active_())
                    android_type_diff_(event.edit.text);
                break;

            case SDL_TEXTINPUT:
                if (android_kbd_on_ && !android_panel_visible_ && android_input_active_()) {
                    // Committed text: the phone usually shows it already
                    // from the composition above, so this sends nothing.
                    android_type_diff_(event.text.text);
                    android_ime_shown_.clear();
                    break;
                }
                if (android_panel_visible_) {
                    auto& fld = (android_focus_ == 0 ? android_ip_
                              :  android_focus_ == 1 ? android_connect_port_
                              :  android_focus_ == 2 ? android_port_
                              :                        android_pin_);
                    for (const char* p = event.text.text; *p; ++p) {
                        char c = *p;
                        if (android_focus_ == 0) {
                            // IP: digits and dots
                            if ((c >= '0' && c <= '9') || c == '.') fld.push_back(c);
                        } else {
                            // Ports + PIN: digits only
                            if (c >= '0' && c <= '9') fld.push_back(c);
                        }
                    }
                    if (android_focus_ == 1 && android_connect_port_.size() > 5)
                        android_connect_port_.resize(5);
                    if (android_focus_ == 2 && android_port_.size() > 5)
                        android_port_.resize(5);
                    if (android_focus_ == 3 && android_pin_.size() > 6)
                        android_pin_.resize(6);
                }
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    running_.store(false);
                    return;
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                // Tips card: X or "Got it" closes it; other clicks pass.
                if (tips_visible_ && event.button.button == SDL_BUTTON_LEFT) {
                    int mx = event.button.x, my = event.button.y;
                    auto in = [&](const BtnRect& r) {
                        return r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h);
                    };
                    if (in(tips_close_btn_)) {
                        tips_visible_ = false;
                        tips_player_.close();
                        tips_open_index_ = -1;
                        break;
                    }
                    if (in(tips_prev_btn_)) { --tips_index_; break; }
                    if (in(tips_next_btn_)) { ++tips_index_; break; }
                    for (int i = 0; i < 3; ++i)
                        if (in(tips_dot_btns_[i])) tips_index_ = i;
                    if (in(tips_rect_)) break;
                }
                // The setup guide takes every left click while open: tabs
                // inside it, and close on its button or anywhere outside.
                if (setup_guide_visible_ && event.button.button == SDL_BUTTON_LEFT) {
                    int mx = event.button.x, my = event.button.y;
                    auto in = [&](const BtnRect& r) {
                        return r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h);
                    };
                    if (in(setup_guide_close_btn_) || !in(setup_guide_rect_)) {
                        setup_guide_visible_ = false;
                        setup_guide_auto_ = false;
                        break;
                    }
                    for (int i = 0; i < 4; ++i)
                        if (in(setup_guide_tab_btns_[i])) setup_guide_tab_ = i;
                    break;
                }
                // Android panel intercepts left-clicks while open.
                if (android_panel_visible_ && event.button.button == SDL_BUTTON_LEFT) {
                    int mx = event.button.x, my = event.button.y;
                    auto in = [&](const BtnRect& r) {
                        return r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h);
                    };
                    if (android_qr_active_.load()) {
                        if (in(android_qr_cancel_btn_)) { android_qr_stop(); break; }
                        // Clicks inside the QR panel are swallowed; a click
                        // outside cancels and falls through to close the panel.
                        if (in_rect(mx, my, android_panel_rect_.x, android_panel_rect_.y,
                                    android_panel_rect_.w, android_panel_rect_.h)) break;
                        android_qr_stop();
                    }
                    if (in(android_qr_btn_) && !android_busy_) {
                        android_qr_start();
                        break;
                    }
                    if (in(android_usb_btn_) && !android_busy_) {
                        android_usb_start();
                        break;
                    }
                    if (in(android_close_btn_)) {
                        android_panel_visible_ = false;
                        android_panel_animating_ = true;
                        android_panel_anim_start_ = std::chrono::steady_clock::now();
                        SDL_StopTextInput();
                        break;
                    }
                    if (in(android_help_btn_)) {
                        setup_guide_visible_ = true;
                        setup_guide_auto_ = false;
                        break;
                    }
                    if (in(android_connect_btn_) && !android_busy_) {
                        android_submit();
                        break;
                    }
                    if (in(android_disconnect_btn_)) {
                        if (android_disconnect_fn_) android_disconnect_fn_();
                        std::lock_guard lk(android_status_mutex_);
                        android_status_ = "Disconnected.";
                        android_status_is_error_ = false;
                        break;
                    }
                    for (int i = 0; i < 4; ++i) {
                        if (in(android_field_rects_[i])) { android_focus_ = i; break; }
                    }
                    // Discovered-device row clicked: copy ip/ports into the
                    // form so the user only has to type the pairing code.
                    {
                        bool consumed = false;
                        std::vector<DiscoveredAndroidDevice> snap;
                        {
                            std::lock_guard lk(android_discovered_mutex_);
                            snap = android_discovered_;
                        }
                        for (size_t i = 0; i < android_discover_btns_.size() &&
                                            i < snap.size(); ++i) {
                            if (in(android_discover_btns_[i])) {
                                const auto& dev = snap[i];
                                android_ip_ = dev.ip;
                                if (!dev.connect_port.empty())
                                    android_connect_port_ = dev.connect_port;
                                if (!dev.pair_port.empty()) {
                                    android_port_ = dev.pair_port;
                                    // Pairing required: focus the PIN field.
                                    android_focus_ = 3;
                                } else {
                                    android_focus_ = 2;
                                }
                                consumed = true;
                                break;
                            }
                        }
                        if (consumed) break;
                    }
                    // Click outside panel? close it.
                    if (!in_rect(mx, my, android_panel_rect_.x, android_panel_rect_.y,
                                 android_panel_rect_.w, android_panel_rect_.h)) {
                        android_panel_visible_ = false;
                        android_panel_animating_ = true;
                        android_panel_anim_start_ = std::chrono::steady_clock::now();
                        SDL_StopTextInput();
                    }
                    break;
                }

                if (event.button.button == SDL_BUTTON_RIGHT) {
                    int mx = event.button.x, my = event.button.y;
                    // Right-click on any bezel button opens its popup menu.
                    std::string target;
                    if (log_btn_.w > 0 && in_rect(mx, my, log_btn_.x, log_btn_.y,
                                                          log_btn_.w, log_btn_.h)) {
                        target = "log";
                    } else if (record_btn_.w > 0 && in_rect(mx, my, record_btn_.x, record_btn_.y,
                                                              record_btn_.w, record_btn_.h)) {
                        target = "record";
                    } else if (bezel_record_btn_.w > 0 &&
                               in_rect(mx, my, bezel_record_btn_.x, bezel_record_btn_.y,
                                       bezel_record_btn_.w, bezel_record_btn_.h)) {
                        target = "record";
                    } else if (screenshot_btn_.w > 0 &&
                               in_rect(mx, my, screenshot_btn_.x, screenshot_btn_.y,
                                       screenshot_btn_.w, screenshot_btn_.h)) {
                        target = "screenshot";
                    } else if (bezel_screenshot_btn_.w > 0 &&
                               in_rect(mx, my, bezel_screenshot_btn_.x, bezel_screenshot_btn_.y,
                                       bezel_screenshot_btn_.w, bezel_screenshot_btn_.h)) {
                        target = "screenshot";
                    } else if (phone_frame_enabled_ &&
                               ((resize_grip_.w > 0 &&
                                 in_rect(mx, my, resize_grip_.x, resize_grip_.y,
                                         resize_grip_.w, resize_grip_.h)) ||
                                (resize_grip_tl_.w > 0 &&
                                 in_rect(mx, my, resize_grip_tl_.x, resize_grip_tl_.y,
                                         resize_grip_tl_.w, resize_grip_tl_.h)))) {
                        target = "resize";
                    } else if (webcam_btn_.w > 0 &&
                               in_rect(mx, my, webcam_btn_.x, webcam_btn_.y,
                                       webcam_btn_.w, webcam_btn_.h)) {
                        target = "webcam";
                    } else {
                        for (auto& [id, r] : source_btns_) {
                            if (r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h)) {
                                target = "src:" + id;
                                break;
                            }
                        }
                    }
                    if (!target.empty()) {
                        bezel_menu_visible_ = true;
                        bezel_menu_target_ = target;
                        bezel_menu_anchor_x_ = mx;
                        bezel_menu_anchor_y_ = my;
                        bezel_menu_items_.clear();
                        bezel_menu_animating_ = true;
                        bezel_menu_anim_start_ = std::chrono::steady_clock::now();
                        bezel_menu_anim_ = 0.0f;
                        // Hide any pending bezel hover tooltip while menu is up.
                        hover_key_.clear();
                    } else {
                        // Right-click elsewhere dismisses any open menu.
                        bezel_menu_visible_ = false;
                        bezel_menu_anim_ = 0.0f;
                    }
                    break;
                }
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int mx = event.button.x, my = event.button.y;

                    // Update banner — handle clicks before anything else
                    // so the link and close glyph win over background
                    // surfaces beneath them. Banner draws above any frame.
                    if (update_banner_active_) {
                        if (update_close_rect_.w > 0 &&
                            in_rect(mx, my, update_close_rect_.x, update_close_rect_.y,
                                    update_close_rect_.w, update_close_rect_.h)) {
                            update_banner_active_ = false;
                            break;
                        }
                        if (update_link_rect_.w > 0 &&
                            in_rect(mx, my, update_link_rect_.x, update_link_rect_.y,
                                    update_link_rect_.w, update_link_rect_.h)) {
#ifdef _WIN32
                            std::string url;
                            {
                                std::lock_guard<std::mutex> lk(update_check_mutex_);
                                url = update_release_url_.empty()
                                    ? std::string("https://github.com/CDinhh/PhoneMimi")
                                    : update_release_url_;
                            }
                            ShellExecuteA(nullptr, "open", url.c_str(),
                                          nullptr, nullptr, SW_SHOWNORMAL);
#endif
                            update_banner_active_ = false;
                            break;
                        }
                    }

                    // Bezel popup menu — handle first if visible.
                    if (bezel_menu_visible_) {
                        std::string clicked_action;
                        for (auto& [act, r] : bezel_menu_items_) {
                            if (r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h)) {
                                clicked_action = act;
                                break;
                            }
                        }
                        std::string tgt = bezel_menu_target_;
                        bezel_menu_visible_ = false;
                        bezel_menu_anim_ = 0.0f;
                        bezel_menu_animating_ = false;
                        if (!clicked_action.empty()) {
                            if (clicked_action == "disconnect" &&
                                tgt.rfind("src:", 0) == 0 && disconnect_source_fn_) {
                                disconnect_source_fn_(tgt.substr(4));
                            } else if (clicked_action == "exit") {
                                running_.store(false);
                                return;
                            } else if (clicked_action == "copy") {
                                std::string all;
                                for (auto& ln : opm::LogBuffer::instance().get_lines()) {
                                    all += ln;
                                    all += '\n';
                                }
                                SDL_SetClipboardText(all.c_str());
                                std::cout << "[Renderer] Log copied to clipboard ("
                                          << all.size() << " bytes)\n";
                            } else if (clicked_action == "clear") {
                                opm::LogBuffer::instance().clear();
                                std::cout << "[Renderer] Log cleared\n";
                            } else if (tgt == "screenshot") {
                                if (clicked_action == "shot") {
                                    screenshot_requested_ = true;
                                    btn_flash_ = true;
                                    btn_flash_start_ = std::chrono::steady_clock::now();
                                } else if (clicked_action == "annotate") {
                                    begin_annotation();
                                    btn_flash_ = true;
                                    btn_flash_start_ = std::chrono::steady_clock::now();
#ifdef _WIN32
                                } else if (clicked_action == "ocr") {
                                    begin_ocr();
                                    btn_flash_ = true;
                                    btn_flash_start_ = std::chrono::steady_clock::now();
#endif
                                } else if (clicked_action == "open_dir") {
                                    open_screenshot_folder();
                                }
                            } else if (tgt == "resize" && clicked_action == "reset_size") {
                                reset_window_to_default_size();
                            } else if (tgt == "webcam") {
#ifdef ENABLE_WEBCAM
                                if (clicked_action.rfind("wc:", 0) == 0) {
                                    std::string new_id = clicked_action.substr(3);
                                    if (new_id != settings_.webcam_device_id) {
                                        settings_.webcam_device_id = new_id;
                                        webcam_requires_manual_pick_ = false;
                                        settings_.save();
                                        bool was_running = webcam_.is_running();
                                        bool keep_open   = webcam_drawer_visible_;
                                        std::thread([this, new_id, was_running, keep_open]() {
                                            if (was_running) {
                                                webcam_.stop();
                                                opm::Settings::clear_webcam_pending();
                                            }
                                            if (keep_open) {
                                                opm::Settings::set_webcam_pending();
                                                webcam_pending_cleared_ = false;
                                                if (!webcam_.start(new_id, 1280, 720)) {
                                                    std::cerr << "[Webcam] start failed: "
                                                              << webcam_.last_error() << "\n";
                                                    opm::Settings::clear_webcam_pending();
                                                    webcam_pending_cleared_ = true;
                                                }
                                            }
                                        }).detach();
                                    }
                                } else if (clicked_action == "mirror_on" ||
                                           clicked_action == "mirror_off") {
                                    settings_.webcam_mirror_h =
                                        (clicked_action == "mirror_on");
                                    settings_.save();
                                } else if (clicked_action == "show" ||
                                           clicked_action == "hide") {
                                    bool want_visible = (clicked_action == "show");
                                    if (want_visible != webcam_drawer_visible_) {
                                        toggle_webcam_drawer();
                                    }
                                }
#endif
                            } else if (tgt == "record") {
                                if (clicked_action == "start") {
                                    pending_record_duration_sec_ = 0;
                                    record_countdown_ms_ = 0;
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "stop" ||
                                           clicked_action == "cancel") {
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "delay5") {
                                    pending_record_duration_sec_ = 0;
                                    record_countdown_ms_ = 5000;
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "timed5") {
                                    pending_record_duration_sec_ = 5;
                                    record_countdown_ms_ = 0;
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "timed10") {
                                    pending_record_duration_sec_ = 10;
                                    record_countdown_ms_ = 0;
                                    record_toggle_requested_ = true;
                                } else if (clicked_action == "timed15") {
                                    pending_record_duration_sec_ = 15;
                                    record_countdown_ms_ = 0;
                                    record_toggle_requested_ = true;
                                }
                            }
                            break;
                        }
                        // Click outside menu dismisses it; fall through.
                    }

                    // Physical side buttons (power / volume / mute).
                    if (phone_frame_enabled_) {
                        int pressed = -1;
                        for (int i = 0; i < 4; ++i) {
                            const BtnRect& b = side_btn_hit_[i];
                            if (b.w > 0 && in_rect(mx, my, b.x, b.y, b.w, b.h)) pressed = i;
                        }
                        if (pressed >= 0) {
                            press_side_button_(pressed);
                            break;
                        }
                        const BtnRect& pin = side_btn_hit_[7];
                        if (pin.w > 0 && in_rect(mx, my, pin.x, pin.y, pin.w, pin.h)) {
                            settings_.always_on_top = !settings_.always_on_top;
                            SDL_SetWindowAlwaysOnTop(window_, settings_.always_on_top ? SDL_TRUE : SDL_FALSE);
                            settings_.save();
                            break;
                        }
                    }

                    // Resize grips (check first, highest priority corners)
                    bool grip_br = resize_grip_.w > 0 &&
                        in_rect(mx, my, resize_grip_.x, resize_grip_.y,
                                resize_grip_.w, resize_grip_.h);
                    bool grip_tl = resize_grip_tl_.w > 0 &&
                        in_rect(mx, my, resize_grip_tl_.x, resize_grip_tl_.y,
                                resize_grip_tl_.w, resize_grip_tl_.h);
                    if (phone_frame_enabled_ && (grip_br || grip_tl)) {
                        resizing_ = true;
                        resize_from_tl_ = grip_tl;
                        SDL_GetWindowPosition(window_, &resize_win_x_, &resize_win_y_);
                        SDL_GetWindowSize(window_, &resize_win_w_, &resize_win_h_);
                        SDL_GetGlobalMouseState(&resize_start_gx_, &resize_start_gy_);
                        // Track the PHONE width as the starting size, not the
                        // whole window width — the window includes the log
                        // drawer when it is open and the auto-resize loop in
                        // render_frame() forces window_w = phone_w + drawer_w
                        // every frame, which would otherwise undo the resize.
                        resize_start_w_ = frame_dst_w_;
                        resize_start_h_ = frame_dst_h_;
                        break;
                    }

                    // Source picker dots (bottom bezel) — switch active source
                    {
                        bool handled = false;
                        for (auto& [id, r] : source_btns_) {
                            if (r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h)) {
                                // If the user is switching to a *different*
                                // source, show a "waiting for <name>" overlay
                                // until the next frame arrives so the app
                                // does not appear frozen during re-negotiation.
                                if (get_sources_fn_) {
                                    auto sources = get_sources_fn_();
                                    bool is_active = false;
                                    std::string nm;
                                    for (auto& s : sources) {
                                        if (s.id == id) { is_active = s.active; nm = s.name; break; }
                                    }
                                    if (!is_active && !nm.empty()) {
                                        pending_source_name_ = nm;
                                    }
                                }
                                if (set_active_source_fn_) {
                                    // Flag so the next texture-resize event
                                    // triggers a phone-frame regenerate +
                                    // window reshape (otherwise the
                                    // canvas-change branch would keep the
                                    // outgoing source's frame and letterbox
                                    // the new device's video).
                                    source_just_switched_ = true;
                                    set_active_source_fn_(id);
                                }
                                handled = true;
                                break;
                            }
                        }
                        if (handled) break;
                    }

                    // Log scrollbar drag start
                    if (log_panel_visible_ && log_sb_track_h_ > 0 && log_sb_max_scroll_ > 0) {
                        int sb_w = std::max(6, log_sb_track_h_ / 80);
                        int sb_hit_w = sb_w + 12; // generous hit area
                        int lp_w = (int)(log_panel_full_w_ * log_panel_anim_);
                        int lp_margin = std::max(4, frame_dst_h_ / 40);
                        int drawer_inset = frame_dst_h_ / 16;
                        int panel_x = frame_dst_x_ + frame_dst_w_;
                        int panel_w = lp_w - lp_margin;
                        int pad = std::max(6, std::max(8, panel_w / 30));
                        int sb_x = panel_x + panel_w - pad;
                        if (mx >= sb_x - sb_hit_w / 2 && mx <= sb_x + sb_w + sb_hit_w / 2 &&
                            my >= log_sb_track_y_ && my <= log_sb_track_y_ + log_sb_track_h_) {
                            log_scrollbar_dragging_ = true;
                            // Jump scroll to click position
                            float frac = (float)(my - log_sb_track_y_ - log_sb_thumb_h_ / 2) /
                                         (float)(log_sb_track_h_ - log_sb_thumb_h_);
                            frac = std::max(0.0f, std::min(1.0f, frac));
                            log_scroll_offset_ = (int)(frac * log_sb_max_scroll_);
                            break;
                        }
                    }

                    // Settings gear in the top bezel.
                    if (menu_btn_.w > 0 &&
                        in_rect(mx, my, menu_btn_.x, menu_btn_.y,
                                menu_btn_.w, menu_btn_.h)) {
                        settings_panel_visible_ = !settings_panel_visible_;
                        settings_panel_animating_ = true;
                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
                        break;
                    }

                    // Bezel screenshot button (only present when the menu
                    // is hidden — same shortcut as Ctrl+S / island camera).
                    if (bezel_screenshot_btn_.w > 0 &&
                        in_rect(mx, my, bezel_screenshot_btn_.x, bezel_screenshot_btn_.y,
                                bezel_screenshot_btn_.w, bezel_screenshot_btn_.h)) {
                        screenshot_requested_ = true;
                        btn_flash_ = true;
                        btn_flash_start_ = std::chrono::steady_clock::now();
                        break;
                    }

                    // Bezel record button (only present when the menu is
                    // hidden — same effect as Ctrl+R / island record).
                    if (bezel_record_btn_.w > 0 &&
                        in_rect(mx, my, bezel_record_btn_.x, bezel_record_btn_.y,
                                bezel_record_btn_.w, bezel_record_btn_.h)) {
                        record_toggle_requested_ = true;
                        btn_flash_ = true;
                        btn_flash_start_ = std::chrono::steady_clock::now();
                        break;
                    }

                    // Bezel close (X) button — only present when the menu
                    // is hidden. Same effect as the island close button.
                    if (bezel_close_btn_.w > 0 &&
                        in_rect(mx, my, bezel_close_btn_.x, bezel_close_btn_.y,
                                bezel_close_btn_.w, bezel_close_btn_.h)) {
                        running_.store(false);
                        return;
                    }

                    // Log star toggle (log panel visibility)
                    if (log_btn_.w > 0 &&
                        in_rect(mx, my, log_btn_.x, log_btn_.y,
                                log_btn_.w, log_btn_.h)) {
                        toggle_drawer(DrawerMode::Log);
                        break;
                    }

                    // Statistics star toggle (same drawer, Stats content)
                    if (stats_btn_.w > 0 &&
                        in_rect(mx, my, stats_btn_.x, stats_btn_.y,
                                stats_btn_.w, stats_btn_.h)) {
                        toggle_drawer(DrawerMode::Stats);
                        break;
                    }

                    // "Open usage_log.csv" button inside the Statistics drawer.
                    if (log_panel_visible_ && drawer_mode_ == DrawerMode::Stats &&
                        stats_open_csv_btn_.w > 0 &&
                        in_rect(mx, my, stats_open_csv_btn_.x, stats_open_csv_btn_.y,
                                stats_open_csv_btn_.w, stats_open_csv_btn_.h)) {
#ifdef _WIN32
                        ShellExecuteA(nullptr, "open", opm::UsageLog::file_path().c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
#endif
                        break;
                    }

                    // Webcam drawer toggle (bottom bezel chevron)
                    if (webcam_btn_.w > 0 &&
                        in_rect(mx, my, webcam_btn_.x, webcam_btn_.y,
                                webcam_btn_.w, webcam_btn_.h)) {
                        toggle_webcam_drawer();
                        break;
                    }

#ifdef ENABLE_WEBCAM
                    // In-drawer screenshot button (left rail) — saves
                    // just the camera frame to "<date> <time> Webcam.png".
                    if (webcam_drawer_visible_ && webcam_shot_btn_.w > 0 &&
                        in_rect(mx, my, webcam_shot_btn_.x, webcam_shot_btn_.y,
                                webcam_shot_btn_.w, webcam_shot_btn_.h)) {
                        save_webcam_screenshot();
                        btn_flash_ = true;
                        btn_flash_start_ = std::chrono::steady_clock::now();
                        break;
                    }
                    // In-drawer camera switch dots (right rail) — click
                    // a dot to make that camera active.
                    if (webcam_drawer_visible_) {
                        bool handled = false;
                        for (auto& [dev_id, r] : webcam_cam_btns_) {
                            if (r.w > 0 && in_rect(mx, my, r.x, r.y, r.w, r.h)) {
                                if (dev_id != settings_.webcam_device_id) {
                                    settings_.webcam_device_id = dev_id;
                                    webcam_requires_manual_pick_ = false;
                                    settings_.save();
                                    bool was_running = webcam_.is_running();
                                    bool keep_open   = webcam_drawer_visible_;
                                    std::string new_id = dev_id;
                                    std::thread([this, new_id, was_running, keep_open]() {
                                        if (was_running) {
                                            webcam_.stop();
                                            opm::Settings::clear_webcam_pending();
                                        }
                                        if (keep_open) {
                                            opm::Settings::set_webcam_pending();
                                            webcam_pending_cleared_ = false;
                                            if (!webcam_.start(new_id, 1280, 720)) {
                                                std::cerr << "[Webcam] start failed: "
                                                          << webcam_.last_error() << "\n";
                                                opm::Settings::clear_webcam_pending();
                                                webcam_pending_cleared_ = true;
                                            }
                                        }
                                    }).detach();
                                }
                                handled = true;
                                break;
                            }
                        }
                        if (handled) break;
                    }
#endif

                    // Icon button toggle (info panel)
                    if (icon_btn_.w > 0 &&
                        in_rect(mx, my, icon_btn_.x, icon_btn_.y,
                                icon_btn_.w, icon_btn_.h)) {
                        info_panel_visible_ = !info_panel_visible_;
                        info_panel_animating_ = true;
                        info_panel_anim_start_ = std::chrono::steady_clock::now();
                        if (info_panel_visible_ && version_panel_visible_) {
                            version_panel_visible_ = false;
                            version_panel_animating_ = true;
                            version_panel_anim_start_ = std::chrono::steady_clock::now();
                        }
                        if (info_panel_visible_ && settings_panel_visible_) {
                            settings_panel_visible_ = false;
                            settings_panel_animating_ = true;
                            settings_panel_anim_start_ = std::chrono::steady_clock::now();
                        }
                        break;
                    }
                    // Footer link clicks (waiting screen + info panel).
                    // The info panel re-publishes the same footer hits at
                    // its bottom, so the same dispatcher handles both.
                    if (!ever_received_frame_ || info_panel_visible_) {
                        bool handled = false;
                        for (auto& hit : footer_hits_) {
                            if (in_rect(mx, my, hit.x, hit.y, hit.w, hit.h)) {
                                if (hit.tooltip.rfind("Version history", 0) == 0) {
                                    version_panel_visible_ = !version_panel_visible_;
                                    version_panel_animating_ = true;
                                    version_panel_anim_start_ = std::chrono::steady_clock::now();
                                    version_scroll_offset_ = 0;
                                    if (version_panel_visible_ && info_panel_visible_) {
                                        info_panel_visible_ = false;
                                        info_panel_animating_ = true;
                                        info_panel_anim_start_ = std::chrono::steady_clock::now();
                                    }
                                    if (version_panel_visible_ && settings_panel_visible_) {
                                        settings_panel_visible_ = false;
                                        settings_panel_animating_ = true;
                                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
                                    }
                                    handled = true;
                                    break;
                                }
                                if (hit.tooltip == "info_check_updates") {
                                    check_for_update_async(true);
                                    handled = true;
                                    break;
                                }
                                if (!hit.url.empty()) {
#ifdef _WIN32
                                    ShellExecuteA(nullptr, "open", hit.url.c_str(),
                                                  nullptr, nullptr, SW_SHOWNORMAL);
#endif
                                    handled = true;
                                    break;
                                }
                            }
                        }
                        if (handled) break;
                    }

                    // Close button
                    if (in_rect(mx, my, close_btn_.x, close_btn_.y,
                                close_btn_.w, close_btn_.h)) {
                        running_.store(false);
                        return;
                    }
                    // Settings (gear) button — toggle settings panel
                    if (settings_btn_.w > 0 &&
                        in_rect(mx, my, settings_btn_.x, settings_btn_.y,
                                settings_btn_.w, settings_btn_.h)) {
                        settings_panel_visible_ = !settings_panel_visible_;
                        settings_panel_animating_ = true;
                        settings_panel_anim_start_ = std::chrono::steady_clock::now();
                        // Close other panels for clarity
                        if (settings_panel_visible_) {
                            if (info_panel_visible_) {
                                info_panel_visible_ = false;
                                info_panel_animating_ = true;
                                info_panel_anim_start_ = std::chrono::steady_clock::now();
                            }
                            if (version_panel_visible_) {
                                version_panel_visible_ = false;
                                version_panel_animating_ = true;
                                version_panel_anim_start_ = std::chrono::steady_clock::now();
                            }
                        }
                        break;
                    }
                    // Settings panel inner clicks (swatches + toggles).
                    // Use a low threshold so clicks register as soon as the
                    // panel is mostly open, rather than waiting for the full
                    // 200ms slide-in animation to finish.
                    if (settings_panel_visible_ && settings_panel_anim_ > 0.3f) {
                        bool inside = in_rect(mx, my, settings_panel_rect_.x, settings_panel_rect_.y,
                                              settings_panel_rect_.w, settings_panel_rect_.h);
                        if (inside) {
                            auto hit = [&](const BtnRect& b) {
                                return b.w > 0 && in_rect(mx, my, b.x, b.y, b.w, b.h);
                            };
                            for (int i = 0; i < 4; ++i)
                                if (hit(settings_tab_btns_[i])) settings_tab_ = i;
                            if (hit(settings_close_btn_)) {
                                settings_panel_visible_ = false;
                                settings_panel_animating_ = true;
                                settings_panel_anim_start_ = std::chrono::steady_clock::now();
                            }
#ifdef ENABLE_MCP
                            if (hit(settings_toggle_mcp_btn_)) {
                                settings_.mcp_enabled = !settings_.mcp_enabled;
                                if (settings_.mcp_enabled && settings_.mcp_token.empty())
                                    settings_.mcp_token = opm::mcp::make_token();
                                settings_.save();
                                set_mcp_status(settings_.mcp_enabled ? "Starting..." : "");
                                if (mcp_fn_) mcp_fn_(settings_.mcp_enabled, settings_.mcp_port, settings_.mcp_token);
                            }
                            if (hit(settings_mcp_copy_btn_)) {
                                SDL_SetClipboardText(opm::mcp::setup_command(settings_.mcp_port, settings_.mcp_token).c_str());
                                mcp_copied_at_ = std::chrono::steady_clock::now();
                            }
#endif
                            for (int i = 0; i < 7; ++i) {
                                if (!hit(settings_action_btns_[i])) continue;
                                if (i == 6) { running_.store(false); return; }
                                settings_panel_visible_ = false;
                                settings_panel_animating_ = true;
                                settings_panel_anim_start_ = std::chrono::steady_clock::now();
                                if (i == 0) {
                                    screenshot_requested_ = true;
                                    btn_flash_ = true;
                                    btn_flash_start_ = std::chrono::steady_clock::now();
                                } else if (i == 1) {
                                    record_toggle_requested_ = true;
                                } else if (i == 2) {
                                    open_screenshot_folder();
                                } else if (i == 3) {
                                    toggle_drawer(DrawerMode::Log);
                                } else if (i == 4) {
                                    toggle_drawer(DrawerMode::Stats);
                                } else {
                                    toggle_webcam_drawer();
                                }
                            }
                            if (hit(settings_dev_add_btn_)) {
                                settings_panel_visible_ = false;
                                settings_panel_animating_ = true;
                                settings_panel_anim_start_ = std::chrono::steady_clock::now();
                                show_android_panel();
                            }
                            if (hit(settings_dev_disc_btn_) && disconnect_source_fn_ && !settings_dev_id_.empty())
                                disconnect_source_fn_(settings_dev_id_);
                            if (hit(settings_remote_btn_) && android_remote_fn_ && !android_remote_busy_.load()) {
                                if (settings_dev_id_.rfind("android:", 0) != 0) {
                                    show_toast(tr("Connect the Android phone first (USB or Wi-Fi)"), 3000);
                                } else {
                                    android_remote_busy_ = true;
                                    std::thread([this, fn = android_remote_fn_, serial = settings_dev_id_.substr(8)]() {
                                        std::string r = fn(serial);
                                        bool ok = r.rfind("Remote ready ", 0) == 0;
                                        (ok ? std::cout : std::cerr) << "[Android] " << r << "\n";
                                        std::lock_guard lk(async_mutex_);
                                        async_toast_ = r;
                                        if (ok) async_last_wifi_ = r.substr(13);
                                        android_remote_busy_ = false;
                                    }).detach();
                                }
                            }
                            if (hit(settings_help_guide_btn_)) {
                                settings_panel_visible_ = false;
                                settings_panel_animating_ = true;
                                settings_panel_anim_start_ = std::chrono::steady_clock::now();
                                setup_guide_visible_ = true;
                                setup_guide_auto_ = false;
                            }
                            if (hit(settings_help_tips_btn_)) {
                                settings_panel_visible_ = false;
                                settings_panel_animating_ = true;
                                settings_panel_anim_start_ = std::chrono::steady_clock::now();
                                tips_index_ = 0;
                                tips_open_index_ = -1;
                                tips_visible_ = true;
                            }
                            if (hit(settings_help_ver_btn_) || hit(settings_help_about_btn_)) {
                                bool ver = hit(settings_help_ver_btn_);
                                settings_panel_visible_ = false;
                                settings_panel_animating_ = true;
                                settings_panel_anim_start_ = std::chrono::steady_clock::now();
                                if (ver) {
                                    version_panel_visible_ = true;
                                    version_panel_animating_ = true;
                                    version_panel_anim_start_ = std::chrono::steady_clock::now();
                                    version_scroll_offset_ = 0;
                                } else {
                                    info_panel_visible_ = true;
                                    info_panel_animating_ = true;
                                    info_panel_anim_start_ = std::chrono::steady_clock::now();
                                }
                            }
                            for (int i = 0; i < 2; ++i)
                                if (hit(settings_seg_lang_btns_[i]) && settings_.language != i) {
                                    settings_.language = i;
                                    g_lang_vi = i == 1;
                                    settings_.save();
                                }
                            if (hit(settings_toggle_mutephone_btn_)) {
                                settings_.android_mute_phone = !settings_.android_mute_phone;
                                settings_.save();
                            }
                            if (hit(settings_toggle_usb_btn_)) {
                                settings_.android_auto_usb = !settings_.android_auto_usb;
                                settings_.save();
                            }
                            if (hit(settings_toggle_paste_btn_)) {
                                settings_.android_type_via_paste = !settings_.android_type_via_paste;
                                settings_.save();
                            }
                            if (hit(settings_toggle_softkbd_btn_)) {
                                settings_.android_hide_soft_keyboard = !settings_.android_hide_soft_keyboard;
                                settings_.save();
                            }
                            if (hit(settings_toggle_kbd_btn_)) {
                                settings_.android_keyboard_on = !settings_.android_keyboard_on;
                                android_kbd_on_ = settings_.android_keyboard_on;
                                android_ime_shown_.clear();
                                settings_.save();
                            }
                            if (hit(settings_toggle_scroff_btn_)) {
                                settings_.android_screen_off = !settings_.android_screen_off;
                                settings_.save();
                                // Takes effect right away on the phone being mirrored.
                                if (android_input_active_()) {
                                    android_screen_off_ = settings_.android_screen_off;
                                    AndroidInput in;
                                    in.kind = AndroidInput::Kind::ScreenPower;
                                    in.action = android_screen_off_ ? 0 : 1;
                                    android_input_fn_(in);
                                }
                            }
                            {
                                static const int res_vals[3] = {0, 1920, 1280};
                                static const int fps_vals[3] = {0, 60, 30};
                                static const int ios_vals[2] = {60, 30};
                                for (int i = 0; i < 3; ++i) {
                                    if (hit(settings_seg_res_btns_[i])) {
                                        settings_.android_max_size = res_vals[i];
                                        settings_.save();
                                    }
                                    if (hit(settings_seg_fps_btns_[i])) {
                                        settings_.android_max_fps = fps_vals[i];
                                        settings_.save();
                                    }
                                }
                                for (int i = 0; i < 2; ++i) {
                                    if (hit(settings_seg_ios_fps_btns_[i])) {
                                        settings_.airplay_max_fps = ios_vals[i];
                                        settings_.save();
                                    }
                                }
                            }
                            for (auto& sw : settings_swatch_btns_) {
                                if (in_rect(mx, my, sw.second.x, sw.second.y, sw.second.w, sw.second.h)) {
                                    static const uint8_t presets[][3] = {
                                        { 28,  28,  30},  // Dark titanium (default)
                                        { 90,  90,  95},  // Graphite
                                        { 36,  46,  72},  // Midnight blue
                                        { 36,  60,  44},  // Forest green
                                        { 80,  34,  34},  // Deep red
                                        {110,  84,  46},  // Bronze
                                    };
                                    int idx = sw.first;
                                    if (idx >= 100) {
                                        settings_.accent_color = std::clamp(idx - 100, 0, 5);
                                        settings_.save();
                                    } else if (idx >= 0 && idx < (int)(sizeof(presets) / sizeof(presets[0]))) {
                                        apply_bezel_color(presets[idx][0], presets[idx][1], presets[idx][2]);
                                    }
                                    break;
                                }
                            }
                            if (in_rect(mx, my, settings_toggle_save_btn_.x, settings_toggle_save_btn_.y,
                                        settings_toggle_save_btn_.w, settings_toggle_save_btn_.h)) {
                                settings_.screenshot_save_to_folder = !settings_.screenshot_save_to_folder;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_clip_btn_.x, settings_toggle_clip_btn_.y,
                                        settings_toggle_clip_btn_.w, settings_toggle_clip_btn_.h)) {
                                settings_.screenshot_copy_to_clipboard = !settings_.screenshot_copy_to_clipboard;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_snagit_btn_.x, settings_toggle_snagit_btn_.y,
                                        settings_toggle_snagit_btn_.w, settings_toggle_snagit_btn_.h)) {
                                settings_.screenshot_open_in_snagit = !settings_.screenshot_open_in_snagit;
                                // If user just enabled it, warn (in toast) when
                                // Snagit isn't installed so they're not
                                // surprised by silent no-ops later.
                                if (settings_.screenshot_open_in_snagit && find_snagit_editor().empty()) {
                                    toast_text_ = "Snagit Editor not found on this PC";
                                    toast_active_ = true;
                                    toast_start_ = std::chrono::steady_clock::now();
                                }
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_compname_btn_.x, settings_toggle_compname_btn_.y,
                                        settings_toggle_compname_btn_.w, settings_toggle_compname_btn_.h)) {
                                settings_.use_computer_name = !settings_.use_computer_name;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_aot_btn_.x, settings_toggle_aot_btn_.y,
                                        settings_toggle_aot_btn_.w, settings_toggle_aot_btn_.h)) {
                                settings_.always_on_top = !settings_.always_on_top;
                                SDL_SetWindowAlwaysOnTop(window_, settings_.always_on_top ? SDL_TRUE : SDL_FALSE);
                                settings_.save();
                            }
                            if (settings_toggle_webcam_mirror_btn_.w > 0 &&
                                in_rect(mx, my, settings_toggle_webcam_mirror_btn_.x, settings_toggle_webcam_mirror_btn_.y,
                                        settings_toggle_webcam_mirror_btn_.w, settings_toggle_webcam_mirror_btn_.h)) {
                                settings_.webcam_mirror_h = !settings_.webcam_mirror_h;
                                settings_.save();
                            }
                            if (settings_toggle_webcam_record_btn_.w > 0 &&
                                in_rect(mx, my, settings_toggle_webcam_record_btn_.x, settings_toggle_webcam_record_btn_.y,
                                        settings_toggle_webcam_record_btn_.w, settings_toggle_webcam_record_btn_.h)) {
                                settings_.webcam_include_in_recording = !settings_.webcam_include_in_recording;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_log_btn_.x, settings_toggle_log_btn_.y,
                                        settings_toggle_log_btn_.w, settings_toggle_log_btn_.h)) {
                                log_to_file_session_ = !log_to_file_session_;
                                if (log_to_file_session_) {
                                    std::error_code ec;
                                    std::filesystem::create_directories(screenshot_dir_, ec);
                                    std::string path = screenshot_dir_ + "/PhoneMimi.log";
                                    if (opm::LogBuffer::instance().open_file(path)) {
                                        std::cout << "[Log] File logging enabled: " << path << "\n";
                                    } else {
                                        std::cout << "[Log] Failed to open log file: " << path << "\n";
                                        log_to_file_session_ = false;
                                    }
                                } else {
                                    std::cout << "[Log] File logging disabled\n";
                                    opm::LogBuffer::instance().close_file();
                                }
                            }
                            if (settings_fmt_mp4_btn_.w > 0 &&
                                in_rect(mx, my, settings_fmt_mp4_btn_.x, settings_fmt_mp4_btn_.y,
                                        settings_fmt_mp4_btn_.w, settings_fmt_mp4_btn_.h)) {
                                settings_.record_format = 0;
                                settings_.save();
                            }
                            if (settings_fmt_gif_btn_.w > 0 &&
                                in_rect(mx, my, settings_fmt_gif_btn_.x, settings_fmt_gif_btn_.y,
                                        settings_fmt_gif_btn_.w, settings_fmt_gif_btn_.h)) {
                                settings_.record_format = 1;
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_resize_btn_.x, settings_toggle_resize_btn_.y,
                                        settings_toggle_resize_btn_.w, settings_toggle_resize_btn_.h)) {
                                settings_.screenshot_resize_enabled = !settings_.screenshot_resize_enabled;
                                settings_.save();
                            }
                            if (settings_resize_minus_btn_.w > 0 &&
                                in_rect(mx, my, settings_resize_minus_btn_.x, settings_resize_minus_btn_.y,
                                        settings_resize_minus_btn_.w, settings_resize_minus_btn_.h)) {
                                settings_.screenshot_resize_height_cm =
                                    std::clamp(settings_.screenshot_resize_height_cm - 0.5f, 1.0f, 30.0f);
                                settings_.save();
                            }
                            if (settings_resize_plus_btn_.w > 0 &&
                                in_rect(mx, my, settings_resize_plus_btn_.x, settings_resize_plus_btn_.y,
                                        settings_resize_plus_btn_.w, settings_resize_plus_btn_.h)) {
                                settings_.screenshot_resize_height_cm =
                                    std::clamp(settings_.screenshot_resize_height_cm + 0.5f, 1.0f, 30.0f);
                                settings_.save();
                            }
                            if (in_rect(mx, my, settings_toggle_save_original_btn_.x, settings_toggle_save_original_btn_.y,
                                        settings_toggle_save_original_btn_.w, settings_toggle_save_original_btn_.h)) {
                                settings_.screenshot_save_original_too = !settings_.screenshot_save_original_too;
                                settings_.save();
                            }
                            break;
                        } else {
                            // Click outside the open settings panel closes it.
                            settings_panel_visible_ = false;
                            settings_panel_animating_ = true;
                            settings_panel_anim_start_ = std::chrono::steady_clock::now();
                            break;
                        }
                    }
                    // Screenshot button
                    if (in_rect(mx, my, screenshot_btn_.x, screenshot_btn_.y,
                                screenshot_btn_.w, screenshot_btn_.h)) {
                        screenshot_requested_ = true;
                        btn_flash_ = true;
                        btn_flash_start_ = std::chrono::steady_clock::now();
                        break;
                    }
                    // Record button (toggle / cancel countdown)
                    if (record_btn_.w > 0 &&
                        in_rect(mx, my, record_btn_.x, record_btn_.y,
                                record_btn_.w, record_btn_.h)) {
                        record_toggle_requested_ = true;
                        btn_flash_ = true;
                        btn_flash_start_ = std::chrono::steady_clock::now();
                        break;
                    }
                    // Folder button
                    if (in_rect(mx, my, folder_btn_.x, folder_btn_.y,
                                folder_btn_.w, folder_btn_.h)) {
                        open_screenshot_folder();
                        break;
                    }
                    // Dismiss info panel on click outside its rect.
                    // Placed AFTER all bezel-button checks so that clicking
                    // a bezel button (e.g. the gear) is not eaten by this
                    // dismiss handler.
                    if (info_panel_visible_ && info_panel_anim_ >= 1.0f) {
                        // "Copy network test script" — copies a PowerShell
                        // troubleshooting block to the clipboard.
                        if (info_copy_ps_btn_.w > 0 &&
                            in_rect(mx, my, info_copy_ps_btn_.x, info_copy_ps_btn_.y,
                                    info_copy_ps_btn_.w, info_copy_ps_btn_.h)) {
                            const char* ps_script =
                                "# PhoneMimi — network troubleshooting\r\n"
                                "Write-Host '== Firewall profile (LocalRulesAllowed = False means MDM/Intune blocks installer rules) ==' -ForegroundColor Cyan\r\n"
                                "Get-NetFirewallProfile |\r\n"
                                "    Format-Table Name, Enabled, AllowLocalFirewallRules, AllowLocalIPsecRules, DefaultInboundAction -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== MDM/Intune Firewall CSP policy (AllowLocalPolicyMerge = 0 means locked) ==' -ForegroundColor Cyan\r\n"
                                "Get-ChildItem 'HKLM:\\SOFTWARE\\Microsoft\\PolicyManager\\current\\device\\Firewall' -Recurse -ErrorAction SilentlyContinue |\r\n"
                                "    Get-ItemProperty |\r\n"
                                "    Select-Object PSChildName, AllowLocalPolicyMerge, AllowLocalIpsecPolicyMerge, EnableFirewall, DefaultInboundAction |\r\n"
                                "    Format-Table -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== Firewall rules for PhoneMimi ==' -ForegroundColor Cyan\r\n"
                                "Get-NetFirewallApplicationFilter |\r\n"
                                "    Where-Object Program -like '*PhoneMimi*' |\r\n"
                                "    ForEach-Object {\r\n"
                                "        $r = Get-NetFirewallRule -AssociatedNetFirewallApplicationFilter $_\r\n"
                                "        [pscustomobject]@{\r\n"
                                "            Name      = $r.DisplayName\r\n"
                                "            Enabled   = $r.Enabled\r\n"
                                "            Direction = $r.Direction\r\n"
                                "            Action    = $r.Action\r\n"
                                "            Profile   = $r.Profile\r\n"
                                "            Program   = $_.Program\r\n"
                                "        }\r\n"
                                "    } | Format-Table -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== Listening TCP ports (AirPlay 7000/7001/7100, scrcpy 27183) ==' -ForegroundColor Cyan\r\n"
                                "Get-NetTCPConnection -State Listen |\r\n"
                                "    Where-Object LocalPort -in 7000,7001,7100,27183 |\r\n"
                                "    Select-Object LocalAddress, LocalPort,\r\n"
                                "        @{n='Process';e={(Get-Process -Id $_.OwningProcess).ProcessName}} |\r\n"
                                "    Format-Table -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== Listening UDP ports (mDNS 5353, AirPlay 6000-6010) ==' -ForegroundColor Cyan\r\n"
                                "Get-NetUDPEndpoint |\r\n"
                                "    Where-Object LocalPort -in 5353,6000,6001,6002,6003,6004,6005,6006,6007,6008,6009,6010 |\r\n"
                                "    Select-Object LocalAddress, LocalPort,\r\n"
                                "        @{n='Process';e={(Get-Process -Id $_.OwningProcess).ProcessName}} |\r\n"
                                "    Format-Table -AutoSize\r\n"
                                "\r\n"
                                "Write-Host '== Local IPv4 addresses (share with the phone) ==' -ForegroundColor Cyan\r\n"
                                "Get-NetIPAddress -AddressFamily IPv4 |\r\n"
                                "    Where-Object { $_.PrefixOrigin -ne 'WellKnown' } |\r\n"
                                "    Select-Object IPAddress, InterfaceAlias, PrefixOrigin |\r\n"
                                "    Format-Table -AutoSize\r\n";
                            SDL_SetClipboardText(ps_script);
                            toast_text_ = "PowerShell test script copied to clipboard";
                            toast_active_ = true;
                            toast_start_ = std::chrono::steady_clock::now();
                            std::cout << "[Renderer] Copied network troubleshooting script\n";
                            break;
                        }
                        // "Check for updates" button — manual GitHub poll.
                        if (info_check_btn_.w > 0 &&
                            !update_check_in_progress_.load() &&
                            in_rect(mx, my, info_check_btn_.x, info_check_btn_.y,
                                    info_check_btn_.w, info_check_btn_.h)) {
                            check_for_update_async(true);
                            break;
                        }
                        if (!in_rect(mx, my, info_panel_rect_.x, info_panel_rect_.y,
                                     info_panel_rect_.w, info_panel_rect_.h)) {
                            info_panel_visible_ = false;
                            info_panel_animating_ = true;
                            info_panel_anim_start_ = std::chrono::steady_clock::now();
                            break;
                        }
                    }
                    // Dismiss version panel on click outside its rect.
                    if (version_panel_visible_ && version_panel_anim_ >= 1.0f) {
                        // First chance: handle a click on the scrollbar so the
                        // user can grab the thumb (or click on the track to
                        // jump) instead of being limited to the mouse wheel.
                        if (version_sb_track_h_ > 0 && version_sb_max_scroll_ > 0) {
                            int sb_hit_w = version_sb_w_ + 12; // generous hit area
                            if (mx >= version_sb_x_ - sb_hit_w / 2 &&
                                mx <= version_sb_x_ + version_sb_w_ + sb_hit_w / 2 &&
                                my >= version_sb_track_y_ &&
                                my <= version_sb_track_y_ + version_sb_track_h_) {
                                version_scrollbar_dragging_ = true;
                                float frac = (float)(my - version_sb_track_y_ - version_sb_thumb_h_ / 2) /
                                             (float)std::max(1, version_sb_track_h_ - version_sb_thumb_h_);
                                frac = std::max(0.0f, std::min(1.0f, frac));
                                version_scroll_offset_ = (int)(frac * version_sb_max_scroll_);
                                break;
                            }
                        }
                        if (!in_rect(mx, my, version_panel_rect_.x, version_panel_rect_.y,
                                     version_panel_rect_.w, version_panel_rect_.h)) {
                            version_panel_visible_ = false;
                            version_panel_animating_ = true;
                            version_panel_anim_start_ = std::chrono::steady_clock::now();
                            break;
                        }
                    }
                    // Touch the phone: left-press inside the mirrored screen.
                    if (!android_panel_visible_ && !info_panel_visible_ &&
                        !version_panel_visible_ && !settings_panel_visible_ &&
                        android_input_active_()) {
                        int rx, ry, rw, rh;
                        if (android_video_rect_(&rx, &ry, &rw, &rh) &&
                            in_rect(mx, my, rx, ry, rw, rh)) {
                            android_touch_down_ = true;
                            android_send_touch_(0, mx, my);
                            break;
                        }
                    }
                    // Bezel drag
                    if (phone_frame_enabled_ && frame_dst_w_ > 0) {
                        if (in_rect(mx, my, frame_dst_x_, frame_dst_y_,
                                    frame_dst_w_, frame_dst_h_)) {
                            float sc = (float)frame_dst_w_ / phone_frame_.frame_width();
                            int vx = frame_dst_x_ + (int)(phone_frame_.screen_x() * sc);
                            int vy = frame_dst_y_ + (int)(phone_frame_.screen_y() * sc);
                            int vw = (int)(phone_frame_.screen_width() * sc);
                            int vh = (int)(phone_frame_.screen_height() * sc);
                            if (!in_rect(mx, my, vx, vy, vw, vh))
                                begin_window_drag();
                        }
                    } else {
                        begin_window_drag();
                    }
                }
                break;

            case SDL_MOUSEMOTION:
                if (android_touch_down_) {
                    android_send_touch_(2, event.motion.x, event.motion.y);
                    break;
                }
                if (log_scrollbar_dragging_ && log_sb_track_h_ > 0 && log_sb_max_scroll_ > 0) {
                    int my = event.motion.y;
                    float frac = (float)(my - log_sb_track_y_ - log_sb_thumb_h_ / 2) /
                                 (float)(log_sb_track_h_ - log_sb_thumb_h_);
                    frac = std::max(0.0f, std::min(1.0f, frac));
                    log_scroll_offset_ = (int)(frac * log_sb_max_scroll_);
                } else if (version_scrollbar_dragging_ &&
                           version_sb_track_h_ > 0 && version_sb_max_scroll_ > 0) {
                    int my = event.motion.y;
                    float frac = (float)(my - version_sb_track_y_ - version_sb_thumb_h_ / 2) /
                                 (float)std::max(1, version_sb_track_h_ - version_sb_thumb_h_);
                    frac = std::max(0.0f, std::min(1.0f, frac));
                    version_scroll_offset_ = (int)(frac * version_sb_max_scroll_);
                } else if (resizing_ && phone_frame_.frame_width() > 0) {
                    int gmx, gmy;
                    SDL_GetGlobalMouseState(&gmx, &gmy);
                    float aspect = (float)phone_frame_.frame_height() /
                                   (float)phone_frame_.frame_width();
                    // resize_start_w_/h_ are the PHONE dimensions at drag start.
                    // Dragging the top-left grip towards the top-left grows the phone.
                    int drag_dx = resize_from_tl_ ? (resize_start_gx_ - gmx)
                                                  : (gmx - resize_start_gx_);
                    int new_phone_w = std::max(200, resize_start_w_ + drag_dx);
                    SDL_DisplayMode dm;
                    SDL_GetCurrentDisplayMode(0, &dm);
                    int new_phone_h = (int)(new_phone_w * aspect);
                    // Add the current drawer width (scales with phone height)
                    // so the resulting window fits both phone and drawer and
                    // the auto-resize in render_frame() will not fight it.
                    int drawer_full_w = (int)(new_phone_h * 0.55f);
                    int drawer_w = (int)(drawer_full_w * log_panel_anim_);
                    int margin = std::max(4, new_phone_h / 40);
                    int needed_w = new_phone_w + std::max(0, drawer_w - margin);
                    if (needed_w > (int)(dm.w * 0.95f)) {
                        needed_w = (int)(dm.w * 0.95f);
                        new_phone_w = needed_w - std::max(0, drawer_w - margin);
                        new_phone_h = (int)(new_phone_w * aspect);
                    }
                    if (new_phone_h > (int)(dm.h * 0.95f)) {
                        new_phone_h = (int)(dm.h * 0.95f);
                        new_phone_w = (int)(new_phone_h / aspect);
                        drawer_full_w = (int)(new_phone_h * 0.55f);
                        drawer_w = (int)(drawer_full_w * log_panel_anim_);
                        margin = std::max(4, new_phone_h / 40);
                        needed_w = new_phone_w + std::max(0, drawer_w - margin);
                    }
                    needed_w += side_btn_margin(std::min(new_phone_w, new_phone_h)) * 2;
                    SDL_SetWindowSize(window_, needed_w, new_phone_h);
                    if (resize_from_tl_) {
                        // Keep the bottom-right corner where it was.
                        int cw, ch;
                        SDL_GetWindowSize(window_, &cw, &ch);
                        SDL_SetWindowPosition(window_,
                                              resize_win_x_ + resize_win_w_ - cw,
                                              resize_win_y_ + resize_win_h_ - ch);
                    }
                    window_shape_set_ = false;
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    if (android_touch_down_) {
                        android_touch_down_ = false;
                        android_send_touch_(1, event.button.x, event.button.y);
                    }
                    if (resizing_) resizing_ = false;
                    if (log_scrollbar_dragging_) log_scrollbar_dragging_ = false;
                    if (version_scrollbar_dragging_) version_scrollbar_dragging_ = false;
                }
                break;

            case SDL_MOUSEWHEEL:
                if (!android_panel_visible_ && !info_panel_visible_ &&
                    !version_panel_visible_ && !settings_panel_visible_ &&
                    android_input_active_()) {
                    int wmx, wmy, rx, ry, rw, rh;
                    SDL_GetMouseState(&wmx, &wmy);
                    if (android_video_rect_(&rx, &ry, &rw, &rh) &&
                        in_rect(wmx, wmy, rx, ry, rw, rh)) {
                        AndroidInput in;
                        in.kind = AndroidInput::Kind::Scroll;
                        in.x = std::max(0, std::min(tex_width_ - 1, (wmx - rx) * tex_width_ / rw));
                        in.y = std::max(0, std::min(tex_height_ - 1, (wmy - ry) * tex_height_ / rh));
                        in.frame_w = tex_width_;
                        in.frame_h = tex_height_;
                        in.hscroll = (float)std::max(-1, std::min(1, event.wheel.x));
                        in.vscroll = (float)std::max(-1, std::min(1, event.wheel.y));
                        android_input_fn_(in);
                        break;
                    }
                }
                if (version_panel_visible_) {
                    int scroll_amount = std::max(8, frame_dst_w_ / 30);
                    version_scroll_offset_ -= event.wheel.y * scroll_amount;
                } else if (log_panel_visible_) {
                    int scroll_amount = std::max(12, frame_dst_w_ / 20);
                    log_scroll_offset_ -= event.wheel.y * scroll_amount;
                }
                break;
            }
        }

        // Handle disconnect reset (from TEARDOWN on another thread)
        if (reset_requested_.load()) {
            reset_requested_.store(false);
            if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
            tex_width_ = tex_height_ = 0;
            ever_received_frame_ = false;
            {
                std::lock_guard lk(frame_mutex_);   // copy_shown_frame reads these
                last_frame_data_.clear();
                last_frame_w_ = last_frame_h_ = last_frame_stride_ = 0;
            }
            pending_source_name_.clear();
            // Drop any in-flight frame that the source's worker thread
            // may have submitted just before its disconnect callback
            // fired. Without this, render_frame() would recreate the
            // texture from that stale frame and the user would see the
            // last image of the just-disconnected device hanging on the
            // waiting screen.
            {
                std::lock_guard lk(frame_mutex_);
                pending_frame_ = {};
                has_new_frame_ = false;
            }
            std::cout << "[Renderer] Reset to waiting screen\n";
        }

        // Periodic status heartbeat (every 30s) so log panel shows activity
        {
            static auto last_heartbeat = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count();
            if (elapsed >= 30) {
                last_heartbeat = now;
                if (ever_received_frame_)
                    std::cout << "[Status] Streaming active\n";
                else
                    std::cout << "[Status] Waiting for connection...\n";
            }
        }

        render_frame();

        if (first_render_) {
            SDL_ShowWindow(window_);
            first_render_ = false;
        }

        SDL_Delay(1);
    }
}

void Renderer::render_frame() {
    // Work handed over from other threads.
    {
        std::string toast, clip;
        bool has_clip = false;
        {
            std::lock_guard lk(async_mutex_);
            toast.swap(async_toast_);
            if (!async_last_wifi_.empty()) {
                settings_.android_last_wifi.swap(async_last_wifi_);
                async_last_wifi_.clear();
                settings_.save();
                android_ip_.clear();   // re-filled from android_last_wifi on next panel open
                android_connect_port_.clear();
            }
            if (async_clip_set_) { clip.swap(async_clip_); has_clip = true; async_clip_set_ = false; }
        }
        if (!toast.empty()) {
            toast_text_ = toast;
            toast_active_ = true;
            toast_start_ = std::chrono::steady_clock::now();
        }
        if (has_clip && !clip.empty() && clip != last_synced_clip_) {
            last_synced_clip_ = clip;
            SDL_SetClipboardText(clip.c_str());
        }
        g_lang_vi = settings_.language == 1;
        if (!settings_.tip_drop_shown && ever_received_frame_ && android_input_active_()) {
            tips_visible_ = true;   // first Android phone: show the tips card
            settings_.tip_drop_shown = true;
            settings_.save();
        }
        // A phone plugged in that needs a step opens the guide on the right
        // brand; it closes again once the phone is usable.
        int hint = setup_hint_.load();
        if (hint != setup_hint_seen_) {
            if (hint != 0) {
                setup_guide_tab_ = setup_brand_.load() == 0 ? 3 : setup_brand_.load() - 1;
                setup_guide_visible_ = true;
                setup_guide_auto_ = true;
            } else if (setup_guide_auto_) {
                setup_guide_visible_ = false;
                setup_guide_auto_ = false;
            }
            setup_hint_seen_ = hint;
        }
    }
    // 1. Update texture from pending frame
    {
        std::lock_guard lock(frame_mutex_);
        if (has_new_frame_ && pending_frame_.data) {
            if (pending_frame_.width != tex_width_ || pending_frame_.height != tex_height_) {
                // Decide whether this texture change is a real device
                // rotation (portrait <-> landscape) or just a same-orientation
                // resolution switch / weird in-app canvas (e.g. TripIt
                // reallocates a 16:9 1920x1080 AirPlay surface even when the
                // phone is still held portrait — that is NOT a rotation).
                //
                // A genuine rotation swaps the previous width and height, so
                // the new aspect ratio should be ~ 1 / prev_aspect. If the
                // new aspect differs from that by more than ~15%, treat it
                // as a same-device canvas change and keep the existing
                // phone frame.
                bool first_real_texture = (tex_width_ == 0 || tex_height_ == 0);
                bool real_rotation = false;
                if (!first_real_texture) {
                    bool prev_portrait = (tex_height_ >= tex_width_);
                    bool new_portrait  = (pending_frame_.height >= pending_frame_.width);
                    if (prev_portrait != new_portrait) {
                        float prev_ar = (float)tex_width_ / (float)tex_height_;
                        float new_ar  = (float)pending_frame_.width / (float)pending_frame_.height;
                        float expected_ar = 1.0f / prev_ar; // rotated previous
                        if (expected_ar > 0.0f) {
                            float rel = std::abs(new_ar - expected_ar) / expected_ar;
                            real_rotation = (rel < 0.15f);
                        }
                    }
                }
                // User just clicked a different source dot \u2014 force the
                // canvas-change branch to be treated as a fresh device so
                // the phone frame is regenerated and the window is reshaped
                // for the new aspect ratio (Mac landscape \u2194 iPhone portrait).
                bool source_switched = source_just_switched_;
                source_just_switched_ = false;

                if (texture_) SDL_DestroyTexture(texture_);
                texture_ = SDL_CreateTexture(sdl_renderer_,
                    SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                    pending_frame_.width, pending_frame_.height);
                if (texture_) SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);
                tex_width_ = pending_frame_.width;
                tex_height_ = pending_frame_.height;
                std::cout << "[Renderer] Texture resized: " << tex_width_ << "x" << tex_height_
                          << (real_rotation       ? " (rotation)" :
                              first_real_texture  ? " (initial)"  :
                              source_switched     ? " (source switch)" :
                                                    " (canvas change - keeping frame)")
                          << "\n";

                if (real_rotation || first_real_texture || source_switched) {
                    window_shape_set_ = false;
                    if (phone_frame_enabled_) {
                        phone_frame_.generate(sdl_renderer_, tex_width_, tex_height_);
                        int fw = phone_frame_.frame_width();
                        int fh = phone_frame_.frame_height();
                        int cur_w, cur_h;
                        SDL_GetWindowSize(window_, &cur_w, &cur_h);
                        int cur_log_w = (int)(log_panel_full_w_ * log_panel_anim_);

                        if (first_real_texture || source_switched) {
                            // Fresh device (first connect or user picked a
                            // different source) \u2014 use the same display-
                            // relative sizing as Ctrl+0 so the window fits
                            // the new source's aspect (e.g. iPhone portrait
                            // vs Mac landscape) without being squashed.
                            SDL_DisplayMode dm;
                            if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && fw > 0 && fh > 0) {
                                float s = std::min(dm.w * 0.32f / fw,
                                                   dm.h * 0.65f / fh);
                                int new_w = std::max(1, (int)std::round(fw * s));
                                int new_h = std::max(1, (int)std::round(fh * s));
                                SDL_SetWindowSize(window_, new_w + cur_log_w, new_h);
                            }
                        } else {
                            // Same device rotated \u2014 preserve the user's
                            // window placement AND the perceived size by
                            // keeping the SHORT side the same length, then
                            // derive the long side from the new aspect.
                            // Without this, rotating a 400x800 portrait
                            // would jump to 1600x800 landscape.
                            int phone_w   = std::max(1, cur_w - cur_log_w);
                            int prev_short = std::min(phone_w, cur_h);
                            int new_short  = std::min(fw, fh);
                            if (new_short <= 0) new_short = 1;
                            float s = (float)prev_short / (float)new_short;
                            int new_w = std::max(1, (int)std::round(fw * s));
                            int new_h = std::max(1, (int)std::round(fh * s));
                            SDL_SetWindowSize(window_, new_w + cur_log_w, new_h);
                        }
                    }
                }
                // Same-orientation resolution change: keep phone_frame_ as-is.
                // The existing window stays the same shape; the new texture
                // (possibly a different aspect ratio than the phone frame's
                // inner screen rect) will be rendered with letterboxing
                // inside that rect by the existing draw code.

                // A new stream started (first frame, or a different device
                // joined) — auto-collapse the island menu so the freshly
                // mirrored screen is unobstructed. The bezel chevron, X
                // and screenshot icons remain visible for quick access.
                if (island_visible_) {
                    island_visible_ = false;
                    island_animating_ = true;
                    island_anim_start_ = std::chrono::steady_clock::now();
                }
            }

            int data_size = pending_frame_.stride * pending_frame_.height;
            if ((int)last_frame_data_.size() != data_size) last_frame_data_.resize(data_size);
            memcpy(last_frame_data_.data(), pending_frame_.data, data_size);
            last_frame_w_ = pending_frame_.width;
            last_frame_h_ = pending_frame_.height;
            last_frame_stride_ = pending_frame_.stride;
            SDL_UpdateTexture(texture_, nullptr, pending_frame_.data, pending_frame_.stride);
            ++video_frame_serial_;
            has_new_frame_ = false;
            ever_received_frame_ = true;
            // First frame after a device switch — clear the "waiting for
            // <name>" overlay.
            pending_source_name_.clear();
        }
    }

    // 2. Compute layout
    int win_w, win_h;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    if (!phone_frame_enabled_ || !phone_frame_.is_generated()) {
        if (!texture_) return;
        // If we still have a phone-shaped window region, clear it so the
        // un-framed view isn't clipped to the old phone outline.
        if (window_shape_set_) {
            frame_dst_w_ = 0;
            update_window_shape();
            window_shape_set_ = false;
        }
        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_);
        float sx = (float)win_w / tex_width_, sy = (float)win_h / tex_height_;
        float sc = std::min(sx, sy);
        SDL_Rect dst = {(int)((win_w - tex_width_ * sc) / 2), (int)((win_h - tex_height_ * sc) / 2),
                        (int)(tex_width_ * sc), (int)(tex_height_ * sc)};
        SDL_RenderCopy(sdl_renderer_, video_tex_for_(dst.w, dst.h), nullptr, &dst);
        close_btn_ = screenshot_btn_ = folder_btn_ = icon_btn_ = {};
        record_btn_ = {};
        resize_grip_ = {};
        resize_grip_tl_ = {};
        for (int i = 0; i < kSideBtnCount; ++i) side_btn_[i] = side_btn_hit_[i] = {};
        frame_dst_w_ = 0;
        SDL_RenderPresent(sdl_renderer_);
        return;
    }

    // Phone frame mode
    int fw = phone_frame_.frame_width(), fh = phone_frame_.frame_height();

    // Animate log panel
    if (log_panel_animating_) {
        float elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - log_panel_anim_start_).count() / 1000.0f;
        float duration = 260.0f; // ms
        float t = std::min(1.0f, elapsed / duration);
        // Ease-in-out cubic: smooth start AND smooth end (no harsh deceleration
        // that makes the drawer look like it "stops" near full extension).
        float eased = (t < 0.5f)
                          ? 4.0f * t * t * t
                          : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        log_panel_anim_ = log_panel_visible_ ? eased : (1.0f - eased);
        if (t >= 1.0f) {
            log_panel_animating_ = false;
            if (!log_panel_visible_) log_panel_anim_ = 0.0f;
            else log_panel_anim_ = 1.0f;
        }
    }

    // Phone frame size is always determined by window HEIGHT (height never changes)
    float scale = (float)win_h / fh;
    frame_dst_w_ = (int)(fw * scale);
    frame_dst_h_ = (int)(fh * scale);

    // Animate webcam drawer (matches log-panel easing for consistency).
    if (webcam_drawer_animating_) {
        float elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - webcam_drawer_anim_start_).count() / 1000.0f;
        float duration = 260.0f; // ms
        float t = std::min(1.0f, elapsed / duration);
        float eased = (t < 0.5f)
                          ? 4.0f * t * t * t
                          : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        webcam_drawer_anim_ = webcam_drawer_visible_ ? eased : (1.0f - eased);
        if (t >= 1.0f) {
            webcam_drawer_animating_ = false;
            webcam_drawer_anim_ = webcam_drawer_visible_ ? 1.0f : 0.0f;
        }
    }

    // Webcam drawer geometry. The drawer body is inset horizontally so it
    // starts INSIDE the bezel corner curves (matches the design directive
    // "start within the corner curve"). Height is derived from the inset
    // width as a 16:9 letterbox so a typical 1280x720 webcam fills the
    // drawer without distortion.
    //
    // Layout strategy (mirrors how the log panel grows the window WIDTH):
    //   * The phone keeps its size — it never shrinks just because the
    //     drawer is open.
    //   * The window GROWS vertically by exactly the drawer height.
    //   * We track `webcam_drawer_h_applied_` (the drawer area we've
    //     already grown the window to hold) and compensate by the delta
    //     each frame so animation ticks don't fight with user-initiated
    //     window resizes.
    //
    // The phone always fits the window — we never floor at the un-scaled
    // frame height (`fh`), because tall sources (Samsung 864x1920 → frame
    // height 1992) would otherwise be drawn at 1:1 inside a ~700 px window
    // and the bottom bezel would be clipped.
    int phone_region_h = std::max(1, win_h - webcam_drawer_h_applied_);
    // Buttons stick out of every edge (volume / power, drawer handles):
    // shrink the phone a little so they fit inside the window.
    phone_region_h = std::max(1, phone_region_h -
        side_btn_margin(std::min(phone_region_h, (int)(phone_region_h * (float)fw / fh))) * 2);
    scale          = (float)phone_region_h / fh;
    frame_dst_w_   = (int)(fw * scale);
    frame_dst_h_   = (int)(fh * scale);

    // Match the drawer's flat top edge to the straight section of the
    // phone's bottom bezel (inset by the scaled corner radius).
    int webcam_corner_r_px = (int)(phone_frame_.corner_radius() *
                                   ((float)frame_dst_w_ /
                                    std::max(1, phone_frame_.frame_width())));
    int webcam_inset_x = std::max(frame_dst_w_ / 16, webcam_corner_r_px);
    int webcam_body_w  = std::max(0, frame_dst_w_ - webcam_inset_x * 2);
    webcam_drawer_full_h_ = (int)(webcam_body_w * 9.0f / 16.0f);
    int webcam_drawer_h   = (int)(webcam_drawer_full_h_ * webcam_drawer_anim_);

    // Log panel full-open width: fixed at 55% of phone height (legacy look).
    log_panel_full_w_ = (int)(frame_dst_h_ * 0.55f);

    // Log panel width based on frame height
    int log_panel_w = (int)(log_panel_full_w_ * log_panel_anim_);

    // Window width = phone frame + log panel (expands rightward, phone stays put).
    // Window height = phone frame + webcam drawer (expands downward, phone
    // stays the same size — we only grow/shrink by the drawer delta).
    // In fullscreen we don't resize the window; instead we centre the phone
    // (and its drawers) inside the fullscreen rect.
    bool is_fullscreen =
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    // Strip on each side of the phone for the buttons that stick out.
    int side_m = side_btn_margin(std::min(frame_dst_w_, frame_dst_h_));
    int needed_w = frame_dst_w_ + log_panel_w + side_m * 2;
    int webcam_h_delta = webcam_drawer_h - webcam_drawer_h_applied_;
    int needed_h = win_h + webcam_h_delta;
    if (!is_fullscreen && (win_w != needed_w || webcam_h_delta != 0)) {
        SDL_SetWindowSize(window_, needed_w, needed_h);
        SDL_GetWindowSize(window_, &win_w, &win_h);
        webcam_drawer_h_applied_ = webcam_drawer_h;
        window_shape_set_ = false;
    } else if (is_fullscreen) {
        webcam_drawer_h_applied_ = webcam_drawer_h;
    }

    if (is_fullscreen) {
        // Centre the phone+drawer block horizontally in the fullscreen rect.
        frame_dst_x_ = std::max(0, (win_w - needed_w) / 2) + side_m;
    } else {
        frame_dst_x_ = side_m; // Phone at left edge; log panel expands right
    }
    // Phone keeps its centred position; the webcam drawer hangs in the
    // newly-grown bottom region of the window.
    frame_dst_y_ = std::max(0, (win_h - webcam_drawer_h - frame_dst_h_) / 2);
    layout_side_buttons_();

    int svx = frame_dst_x_ + (int)std::round(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)std::round(phone_frame_.screen_y() * scale);
    int svw = (int)std::round(phone_frame_.screen_width() * scale);
    int svh = (int)std::round(phone_frame_.screen_height() * scale);

    if (texture_ && ever_received_frame_) {
        // --- Video mode ---
        SDL_SetRenderDrawColor(sdl_renderer_, 28, 28, 30, 255);
        SDL_RenderClear(sdl_renderer_);
        SDL_Rect vdst = {svx - 1, svy - 1, svw + 2, svh + 2};
        // If the texture aspect ratio differs noticeably from the phone
        // screen rect (e.g. an app like TripIt sends a landscape 1920x1080
        // canvas while the phone frame is portrait), aspect-fit the texture
        // inside vdst with black bars so the content doesn't get squished.
        if (tex_width_ > 0 && tex_height_ > 0 && vdst.w > 0 && vdst.h > 0) {
            float tex_ar  = (float)tex_width_  / (float)tex_height_;
            float rect_ar = (float)vdst.w      / (float)vdst.h;
            if (std::abs(tex_ar - rect_ar) > 0.05f * rect_ar) {
                // Fill the phone screen rect with black first.
                SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 255);
                SDL_RenderFillRect(sdl_renderer_, &vdst);
                int fit_w, fit_h;
                if (tex_ar > rect_ar) {
                    // Texture wider than rect — fit by width, letterbox.
                    fit_w = vdst.w;
                    fit_h = (int)std::round(vdst.w / tex_ar);
                } else {
                    fit_h = vdst.h;
                    fit_w = (int)std::round(vdst.h * tex_ar);
                }
                SDL_Rect fit{vdst.x + (vdst.w - fit_w) / 2,
                             vdst.y + (vdst.h - fit_h) / 2,
                             fit_w, fit_h};
                SDL_RenderCopy(sdl_renderer_, video_tex_for_(fit.w, fit.h), nullptr, &fit);
            } else {
                SDL_RenderCopy(sdl_renderer_, video_tex_for_(vdst.w, vdst.h), nullptr, &vdst);
            }
        } else {
            SDL_RenderCopy(sdl_renderer_, video_tex_for_(vdst.w, vdst.h), nullptr, &vdst);
        }

        // Optional "Waiting for screen updates from <name>" overlay shown
        // briefly after a source switch and cleared on the next frame.
        if (!pending_source_name_.empty()) {
            std::string want = "Waiting for screen updates from " + pending_source_name_;
            if (want != pending_overlay_text_) {
                if (pending_overlay_tex_) SDL_DestroyTexture(pending_overlay_tex_);
                pending_overlay_tex_ = make_text_texture(sdl_renderer_, want.c_str(),
                    32, 235, 235, 235, &pending_overlay_w_, &pending_overlay_h_);
                pending_overlay_text_ = want;
            }
            // Frosty translucent belt spanning the screen width, centered
            // vertically behind the text. Simulated frosted-glass look:
            // a darker translucent fill with thin highlight lines on the
            // top and bottom edges.
            if (pending_overlay_tex_) {
                float ts = std::min(1.0f, svw * 0.78f / (float)pending_overlay_w_);
                int tw = (int)(pending_overlay_w_ * ts);
                int th = (int)(pending_overlay_h_ * ts);
                int pad = std::max(12, th / 2);
                int belt_h = th + pad * 2;
                int belt_y = svy + (svh - belt_h) / 2;
                SDL_Rect belt = {svx, belt_y, svw, belt_h};

                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                // Main translucent fill (cool dark tint).
                SDL_SetRenderDrawColor(sdl_renderer_, 18, 22, 28, 165);
                SDL_RenderFillRect(sdl_renderer_, &belt);
                // Subtle inner lift for a frosted feel.
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 16);
                SDL_RenderFillRect(sdl_renderer_, &belt);
                // Highlight edges (top & bottom) for crispness.
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 70);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y,
                                   belt.x + belt.w - 1, belt.y);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + belt.h - 1,
                                   belt.x + belt.w - 1, belt.y + belt.h - 1);
                SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 90);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + 1,
                                   belt.x + belt.w - 1, belt.y + 1);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + belt.h - 2,
                                   belt.x + belt.w - 1, belt.y + belt.h - 2);

                SDL_Rect tr = {svx + (svw - tw) / 2, belt_y + pad, tw, th};
                SDL_RenderCopy(sdl_renderer_, pending_overlay_tex_, nullptr, &tr);
            }
        }

        // Protected/blank-content detector. Sustained near-black frames
        // most commonly mean Android FLAG_SECURE (lock screen, banking
        // app, MDM-restricted document, DRM video). The framework gives
        // us no signal, so we wait ~1.2 s of unbroken black before
        // surfacing the overlay to avoid flickering on legitimate dark
        // transitions (app switcher fade-to-black, video letterbox, etc).
        if (is_frame_near_black()) {
            auto now = std::chrono::steady_clock::now();
            if (!black_frame_active_) {
                black_frame_active_ = true;
                black_frame_since_ = now;
            }
            auto held_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - black_frame_since_).count();
            protected_overlay_visible_ = held_ms >= 1200;
        } else {
            black_frame_active_ = false;
            protected_overlay_visible_ = false;
        }

        if (protected_overlay_visible_ && pending_source_name_.empty()) {
            // Two lines, baked at a font size chosen to fit the current
            // screen width. Re-baked only when that target size changes,
            // so glyphs are always rendered 1:1 (no scaling = crisp) but
            // still fit even on small windows / narrow phone aspects.
            const char* line1 = "Screen content is hidden by the device.";
            const char* line2 = "Lock screen or protected content.";
            // Pick a font size that, given the longer line, leaves comfortable
            // side margins. Clamp to a sane absolute range.
            int target_px = std::clamp(svw / 22, 11, 48);
            static SDL_Texture* tex1 = nullptr;
            static SDL_Texture* tex2 = nullptr;
            static int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
            static int baked_px = 0;
            if (target_px != baked_px) {
                if (tex1) { SDL_DestroyTexture(tex1); tex1 = nullptr; }
                if (tex2) { SDL_DestroyTexture(tex2); tex2 = nullptr; }
                tex1 = make_text_texture(sdl_renderer_, line1, target_px,
                                         235, 235, 235, &w1, &h1);
                tex2 = make_text_texture(sdl_renderer_, line2, target_px,
                                         235, 235, 235, &w2, &h2);
                baked_px = target_px;
            }
            if (tex1 && tex2) {
                int gap = std::max(2, h1 / 6);
                int text_h = h1 + gap + h2;
                int pad = std::max(8, h1 / 2);
                int belt_h = text_h + pad * 2;
                int belt_y = svy + (svh - belt_h) / 2;
                SDL_Rect belt = {svx, belt_y, svw, belt_h};

                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(sdl_renderer_, 18, 22, 28, 200);
                SDL_RenderFillRect(sdl_renderer_, &belt);
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 16);
                SDL_RenderFillRect(sdl_renderer_, &belt);
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 70);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y,
                                   belt.x + belt.w - 1, belt.y);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + belt.h - 1,
                                   belt.x + belt.w - 1, belt.y + belt.h - 1);
                SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 90);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + 1,
                                   belt.x + belt.w - 1, belt.y + 1);
                SDL_RenderDrawLine(sdl_renderer_, belt.x, belt.y + belt.h - 2,
                                   belt.x + belt.w - 1, belt.y + belt.h - 2);

                int ty = belt_y + pad;
                SDL_Rect tr1 = {svx + (svw - w1) / 2, ty, w1, h1};
                SDL_RenderCopy(sdl_renderer_, tex1, nullptr, &tr1);
                SDL_Rect tr2 = {svx + (svw - w2) / 2, ty + h1 + gap, w2, h2};
                SDL_RenderCopy(sdl_renderer_, tex2, nullptr, &tr2);
            }
        }
    } else {
        // --- Waiting screen: "polar night" around the penguin ---
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl_renderer_, 9, 11, 16, 255);
        SDL_RenderClear(sdl_renderer_);
#ifdef _WIN32
        {
            const int u = std::min(svw, (int)(svh * 0.62f));   // layout unit
            const int mcx = svx + svw / 2;
            const int mcy = svy + (int)(svh * 0.30f);
            const uint32_t ms = SDL_GetTicks();
            const Accent& ac = accent_for(settings_.accent_color);
            SDL_Rect screen_clip = {svx, svy, svw, svh};
            SDL_RenderSetClipRect(sdl_renderer_, &screen_clip);

            // Radial glow behind the penguin (triangle fan).
            {
                const int seg = 48;
                float rad = svh * 0.75f;
                SDL_Color mid = {28, 38, 54, 255}, edge = {9, 11, 16, 255};
                std::vector<SDL_Vertex> v;
                std::vector<int> idx;
                v.push_back({{(float)mcx, (float)mcy}, mid, {0, 0}});
                for (int i = 0; i <= seg; ++i) {
                    float a = i * 6.2831853f / seg;
                    v.push_back({{mcx + std::cos(a) * rad, mcy + std::sin(a) * rad * 0.8f}, edge, {0, 0}});
                }
                for (int i = 1; i <= seg; ++i) { idx.push_back(0); idx.push_back(i); idx.push_back(i + 1); }
                SDL_RenderGeometry(sdl_renderer_, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
            }

            // A few snow dots drifting down.
            static const float flakes[8][3] = {   // x (0..1), phase (0..1), size
                {0.12f, 0.10f, 1.4f}, {0.78f, 0.55f, 1.0f}, {0.64f, 0.30f, 1.2f}, {0.22f, 0.80f, 1.0f},
                {0.88f, 0.20f, 1.4f}, {0.40f, 0.65f, 1.0f}, {0.55f, 0.92f, 1.2f}, {0.05f, 0.45f, 1.0f},
            };
            for (auto& f : flakes) {
                float y = std::fmod(f[1] + ms / 26000.0f, 1.0f);
                int fx = svx + (int)(f[0] * svw + std::sin(ms / 2400.0f + f[1] * 6.0f) * 6.0f);
                int fy = svy + (int)(y * svh);
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, (uint8_t)(110 * (1.0f - y * 0.6f)));
                fill_circle(sdl_renderer_, fx, fy, std::max(1, (int)std::lround(f[2] * u / 390.0f)));
            }

            // Two ice-blue rings expanding behind the badge ("listening").
            const int R = std::max(40, (int)(u * 0.20f));
            for (int k = 0; k < 2; ++k) {
                float p = std::fmod(ms / 2800.0f + k * 0.5f, 1.0f);
                float rr = R * (0.95f + p * 0.45f);
                SDL_SetRenderDrawColor(sdl_renderer_, 156, 201, 232, (uint8_t)((1.0f - p) * 120));
                for (int i = 0; i < 360; ++i) {
                    float a = i * 0.0174533f;
                    SDL_RenderDrawPointF(sdl_renderer_, mcx + std::cos(a) * rr, mcy + std::sin(a) * rr);
                    SDL_RenderDrawPointF(sdl_renderer_, mcx + std::cos(a) * (rr + 0.8f),
                                         mcy + std::sin(a) * (rr + 0.8f));
                }
            }

            // Badge: glass shell, white-ice core, penguin bobbing inside.
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 22);
            fill_circle(sdl_renderer_, mcx, mcy, R + 1);
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 14);
            fill_circle(sdl_renderer_, mcx, mcy, R);
            const int core = R - std::max(5, R / 13);
            SDL_SetRenderDrawColor(sdl_renderer_, 214, 226, 238, 255);
            fill_circle(sdl_renderer_, mcx, mcy, core);
            SDL_SetRenderDrawColor(sdl_renderer_, 236, 242, 248, 255);
            fill_circle(sdl_renderer_, mcx, mcy - core / 14, core - core / 10);
            SDL_SetRenderDrawColor(sdl_renderer_, 252, 253, 255, 255);
            fill_circle(sdl_renderer_, mcx, mcy - core / 6, core * 6 / 10);
            if (logo_tex_ && logo_w_ > 0) {
                float bob = std::sin(ms / 3200.0f * 6.2831853f);
                int lw = core * 137 / 100, lh = lw * logo_h_ / logo_w_;
                SDL_Texture* lt = logo_tex_;
                if (lw > 0 && lh > 0 && lw < logo_w_ && !logo_rgba_.empty()) {
                    if (!logo_small_ || logo_small_w_ != lw || logo_small_h_ != lh) {
                        if (logo_small_) SDL_DestroyTexture(logo_small_);
                        auto px = shrink_rgba(logo_rgba_.data(), logo_w_, logo_h_, lw, lh);
                        logo_small_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                                        SDL_TEXTUREACCESS_STATIC, lw, lh);
                        if (logo_small_) {
                            SDL_SetTextureBlendMode(logo_small_, SDL_BLENDMODE_BLEND);
                            SDL_SetTextureScaleMode(logo_small_, SDL_ScaleModeLinear);
                            SDL_UpdateTexture(logo_small_, nullptr, px.data(), lw * 4);
                        }
                        logo_small_w_ = lw; logo_small_h_ = lh;
                    }
                    if (logo_small_) lt = logo_small_;
                }
                SDL_Rect d = {mcx - lw / 2, mcy - lh / 2 - (int)(bob * u / 48.0f), lw, lh};
                SDL_RenderCopyEx(sdl_renderer_, lt, nullptr, &d, bob * 2.0, nullptr, SDL_FLIP_NONE);
            }

            // Wordmark + "Waiting for a phone" with three accent dots.
            int ty = mcy + R + (int)(u * 0.06f);
            int th = draw_text_c(sdl_renderer_, "PhoneMimi", (int)(u * 0.087f),
                                 244, 246, 248, 255, mcx, ty, 1, 800);
            int sub_px = (int)(u * 0.040f);
            int sw = 0, sh = 0;
            measure_text_c(sdl_renderer_, tr("Waiting for a phone"), sub_px, &sw, &sh);
            int dot_r = std::max(2, sub_px / 7), dots_w = dot_r * 2 * 3 + dot_r * 2 * 2;
            int sx = mcx - (sw + dot_r * 3 + dots_w) / 2;
            int sy = ty + th + (int)(u * 0.012f);
            draw_text_c(sdl_renderer_, tr("Waiting for a phone"), sub_px, 154, 167, 186, 255, sx, sy, 0);
            for (int i = 0; i < 3; ++i) {
                float ph = std::fmod(ms / 1400.0f - i * 0.14f, 1.0f);
                if (ph < 0) ph += 1.0f;
                uint8_t a = (uint8_t)(64 + 191 * std::max(0.0f, 1.0f - std::abs(ph - 0.4f) * 3.0f));
                SDL_SetRenderDrawColor(sdl_renderer_, ac.lr, ac.lg, ac.lb, a);
                fill_circle(sdl_renderer_, sx + sw + dot_r * 3 + dot_r + i * dot_r * 4, sy + sh / 2 + 1, dot_r);
            }

            // Two double-bezel cards: how to connect.
            const int card_w = (int)(svw * 0.88f);
            const int card_x = svx + (svw - card_w) / 2;
            const int card_h = (int)(u * 0.20f);
            const int rad = card_h / 3;
            int card_y = svy + (int)(svh * 0.58f);
            auto draw_card = [&](wchar_t icon, bool accent_tile, const char* title, const char* line) {
                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(sdl_renderer_, 32, 38, 50, 255);
                fill_round_rect(sdl_renderer_, card_x, card_y, card_w, card_h, rad);
                SDL_SetRenderDrawColor(sdl_renderer_, 17, 21, 29, 255);
                fill_round_rect(sdl_renderer_, card_x + 1, card_y + 1, card_w - 2, card_h - 2, rad - 1);
                int in = std::max(4, card_h / 16);
                SDL_SetRenderDrawColor(sdl_renderer_, 23, 28, 38, 255);
                fill_round_rect(sdl_renderer_, card_x + in, card_y + in, card_w - in * 2, card_h - in * 2, rad - in);
                SDL_SetRenderDrawColor(sdl_renderer_, 40, 47, 60, 255);
                SDL_RenderDrawLine(sdl_renderer_, card_x + rad, card_y + in, card_x + card_w - rad, card_y + in);
                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

                int tile = card_h * 52 / 100, tmid = card_y + card_h / 2;
                int tx = card_x + in + card_h / 4;
                if (accent_tile) SDL_SetRenderDrawColor(sdl_renderer_, ac.r, ac.g, ac.b, 40);
                else             SDL_SetRenderDrawColor(sdl_renderer_, 156, 201, 232, 30);
                fill_round_rect(sdl_renderer_, tx, tmid - tile / 2, tile, tile, tile / 3);
                if (accent_tile) draw_glyph(sdl_renderer_, icon, tx + tile / 2, tmid, tile / 2, ac.lr, ac.lg, ac.lb, 255);
                else             draw_glyph(sdl_renderer_, icon, tx + tile / 2, tmid, tile / 2, 207, 228, 244, 255);

                int text_x = tx + tile + card_h / 5;
                int avail = card_x + card_w - in - card_h / 5 - text_x;
                int tpx = fit_text_px(sdl_renderer_, title, (int)(u * 0.040f), avail, 700);
                int lpx = fit_text_px(sdl_renderer_, line, (int)(u * 0.034f), avail);
                draw_text_mid(sdl_renderer_, title, tpx, 244, 246, 248, 255, text_x, tmid - tpx * 6 / 10, 0, 700);
                draw_text_mid(sdl_renderer_, line, lpx, 154, 167, 186, 255, text_x, tmid + lpx * 7 / 10, 0);
                card_y += card_h + (int)(u * 0.035f);
            };
            draw_card(lucide::Smartphone, false, "iPhone", tr("Screen Mirroring, then pick PhoneMimi"));
            draw_card(lucide::Usb, true, "Android", tr("Plug in a USB cable, or press A"));
            SDL_RenderSetClipRect(sdl_renderer_, nullptr);
        }
#endif

        // Footer with interactive segments
        draw_footer(svx, svy, svw, svh);
    }

    // Animate island visibility
    if (island_animating_) {
        float elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - island_anim_start_).count() / 1000.0f;
        float duration = 200.0f; // ms
        float t = std::min(1.0f, elapsed / duration);
        island_anim_ = island_visible_ ? t : (1.0f - t);
        if (t >= 1.0f) island_animating_ = false;
    }

    // Info/Version panels (furthest back — emerge from behind island)
    if (info_panel_visible_ || info_panel_animating_) draw_info_panel();
    if (version_panel_visible_ || version_panel_animating_) draw_version_panel();
    if (settings_panel_visible_ || settings_panel_animating_) draw_settings_panel();
    if (android_panel_visible_ || android_panel_animating_) draw_android_panel();
    if (setup_guide_visible_) draw_setup_guide_();
    if (tips_visible_) draw_tips_();


    std::string bezel_hover_key;
    std::string bezel_hover_text;
    int bezel_hover_ax = 0, bezel_hover_ay = 0;

    // Phone frame overlay (on top — covers island and panel edges)
    if (SDL_Texture* ft = phone_frame_.texture()) {
        SDL_Rect fd = {frame_dst_x_, frame_dst_y_, frame_dst_w_, frame_dst_h_};
        SDL_RenderCopy(sdl_renderer_,
                       mip_for_(frame_mip_, ft, phone_frame_.frame_width(), phone_frame_.frame_height(),
                                ++frame_draw_serial_, fd.w, fd.h),
                       nullptr, &fd);
    }
    draw_side_buttons_(bezel_hover_key, bezel_hover_text, bezel_hover_ax, bezel_hover_ay);

    // Recording HUD — countdown digit while waiting, "REC + elapsed" chip
    // while recording. Drawn on the screen content (under phone bezel) but
    // above the video itself.
    if (record_countdown_ms_ > 0 && !recorder_.is_recording() && tex_width_ > 0) {
        auto cd_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_countdown_start_).count();
        int remaining = std::max(0, (record_countdown_ms_ - (int)cd_elapsed + 999) / 1000);
        char buf[8]; std::snprintf(buf, sizeof(buf), "%d", remaining);
        int big = std::max(64, frame_dst_w_ / 4);
        int tw = 0, th = 0;
        SDL_Texture* t = make_text_texture(sdl_renderer_, buf, big,
                                           255, 255, 255, &tw, &th);
        if (t) {
            // Pulse pop on each second tick.
            int phase_ms = (int)(cd_elapsed % 1000);
            float pop = 1.0f + 0.15f * std::max(0.0f, 1.0f - phase_ms / 250.0f);
            int dw = (int)(tw * pop), dh = (int)(th * pop);
            int cx = frame_dst_x_ + frame_dst_w_ / 2;
            int cy = frame_dst_y_ + frame_dst_h_ / 2;
            // Frosty circular backdrop — partly transparent so the
            // mirrored content remains visible behind the digit. Soft
            // outer halo + main translucent disc + faint inner highlight.
            int radius = (int)(std::max(dw, dh) * 0.85f);
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
            for (int rr = radius + radius / 4; rr > radius; --rr) {
                uint8_t a = (uint8_t)(20 * (radius + radius / 4 - rr) / std::max(1, radius / 4));
                SDL_SetRenderDrawColor(sdl_renderer_, 20, 20, 30, a);
                fill_circle(sdl_renderer_, cx, cy, rr);
            }
            // Main frosted disc — alpha ~95/255 keeps the underlying
            // mirror legible while still floating the digit visually.
            SDL_SetRenderDrawColor(sdl_renderer_, 28, 30, 40, 95);
            fill_circle(sdl_renderer_, cx, cy, radius);
            // Subtle inner highlight ring for a glassy edge.
            int ring_r = radius - std::max(2, radius / 18);
            SDL_SetRenderDrawColor(sdl_renderer_, 200, 210, 230, 45);
            for (int rr = ring_r; rr >= ring_r - 1 && rr > 0; --rr) {
                for (int dy = -rr; dy <= rr; ++dy) {
                    for (int dx = -rr; dx <= rr; ++dx) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 <= rr * rr && d2 >= (rr - 1) * (rr - 1))
                            SDL_RenderDrawPoint(sdl_renderer_, cx + dx, cy + dy);
                    }
                }
            }
            SDL_Rect dst{cx - dw / 2, cy - dh / 2, dw, dh};
            SDL_RenderCopy(sdl_renderer_, t, nullptr, &dst);
            SDL_DestroyTexture(t);
        }
    } else if (recorder_.is_recording() && tex_width_ > 0) {
        int s = (int)recorder_.elapsed_seconds();
        char buf[64]; std::snprintf(buf, sizeof(buf), "REC  %d:%02d", s / 60, s % 60);
        int fh = std::max(14, frame_dst_w_ / 28);
        int tw = 0, th = 0;
        SDL_Texture* t = make_text_texture(sdl_renderer_, buf, fh,
                                           255, 240, 240, &tw, &th);
        if (t) {
            int pad_ = std::max(6, fh / 2);
            int chip_w = tw + pad_ * 2 + fh; // extra room for the dot
            int chip_h = th + pad_;
            int chip_x = frame_dst_x_ + std::max(8, frame_dst_w_ / 24);
            int chip_y = frame_dst_y_ + std::max(8, frame_dst_h_ / 28);
            // Pulsing red background.
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            float pulse = 0.55f + 0.45f * (float)std::abs(std::sin(ms * 0.005));
            SDL_SetRenderDrawColor(sdl_renderer_,
                                   (uint8_t)(180 * pulse + 50), 30, 30, 220);
            SDL_Rect chip{chip_x, chip_y, chip_w, chip_h};
            SDL_RenderFillRect(sdl_renderer_, &chip);
            // Filled dot to the left of the text.
            int dot_r = th / 3;
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 230);
            fill_circle(sdl_renderer_, chip_x + pad_ + dot_r,
                        chip_y + chip_h / 2, dot_r);
            SDL_Rect td{chip_x + pad_ + dot_r * 2 + pad_ / 2,
                        chip_y + (chip_h - th) / 2, tw, th};
            SDL_RenderCopy(sdl_renderer_, t, nullptr, &td);
            SDL_DestroyTexture(t);
        }
    }

    // Log panel (slides out to the left)
    if (log_panel_anim_ > 0.01f) draw_log_panel();

    // Webcam drawer (slides down out of the bottom bezel)
    if (webcam_drawer_anim_ > 0.01f) draw_webcam_drawer();

    // Menu star — center top bezel
    {
        float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
        int bezel_top = (int)(phone_frame_.screen_y() * scale);
        // Glyph size derived from the phone-equivalent reference width
        // (capped) so the menu chevron stays compact regardless of source
        // dimensions — see ui_ref_width() for the rationale.
        int dot_r = std::max(2, ui_ref_width() / 175);
        int star_cx = frame_dst_x_ + frame_dst_w_ / 2;
        int star_cy = frame_dst_y_ + bezel_top / 2;
        // A horizontal bar inside the top bezel; the whole bezel strip
        // around it is the click target.
        int pill_w = std::max(28, frame_dst_w_ * 22 / 100);
        int pill_h = std::max(5, bezel_top * 36 / 100);
        menu_btn_ = {star_cx - pill_w / 2 - 8, frame_dst_y_, pill_w + 16, std::max(14, bezel_top)};

        int smx, smy;
        SDL_GetMouseState(&smx, &smy);
        bool star_hover = in_rect(smx, smy, menu_btn_.x, menu_btn_.y, menu_btn_.w, menu_btn_.h);
        uint8_t sa = star_hover ? 240 : 160;
        SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, sa);

        // Settings: the one button on the frame (everything else lives
        // inside the Settings panel).
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
        if (star_hover) {
            const Accent& sac = accent_for(settings_.accent_color);
            SDL_SetRenderDrawColor(sdl_renderer_, sac.lr, sac.lg, sac.lb, 255);
        } else {
            SDL_SetRenderDrawColor(sdl_renderer_, 205, 210, 218, 150);
        }
        fill_round_rect(sdl_renderer_, star_cx - pill_w / 2, star_cy - pill_h / 2, pill_w, pill_h,
                        pill_h / 2);
        (void)sa;
        if (star_hover) {
            bezel_hover_key = "menu";
            bezel_hover_text = tr("Settings (S)");
            bezel_hover_ax = star_cx;
            bezel_hover_ay = star_cy + dot_r + 4;
        }
    }

    // Mini screenshot button in the top bezel — shown only when the island
    // menu is hidden so users still have a clickable shortcut for Ctrl+S.
    // Horizontal position matches the screenshot button on the island so
    // the affordance feels continuous when the menu is toggled.
    bezel_screenshot_btn_ = {};
    bezel_close_btn_ = {};
    bezel_record_btn_ = {};
    if (island_anim_ < 0.5f) {
        float scale_b = (float)frame_dst_w_ / phone_frame_.frame_width();
        int bezel_top = (int)(phone_frame_.screen_y() * scale_b);
        int svx_b = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale_b);
        int svw_b = (int)(phone_frame_.screen_width() * scale_b);
        // Mirror the maths inside draw_island() so the X position is identical.
        int phone_eq_w = ui_ref_width();
        int btn_sz_full = std::max(20, phone_eq_w / 14);
        int pad_full = btn_sz_full / 3;
        int gap_full = pad_full / 2 + 2;
        int island_w = std::max(160, (int)(phone_eq_w * 0.80f));
        int island_x = svx_b + (svw_b - island_w) / 2;
        // Order on the island (right→left): close, folder, record, screenshot.
        int rec_full_x = island_x + island_w - pad_full - btn_sz_full
                        - 2 * (btn_sz_full + gap_full);
        int rec_full_cx = rec_full_x + btn_sz_full / 2;

        // Mini button — sized to fit inside the top bezel with breathing room.
        int mini_sz = std::max(12, std::min(btn_sz_full * 2 / 3, bezel_top - 4));
        if (mini_sz < 12) mini_sz = 12;
        int mini_y = frame_dst_y_ + (bezel_top - mini_sz) / 2;
        int bmx, bmy;
        SDL_GetMouseState(&bmx, &bmy);
        auto now_b = std::chrono::steady_clock::now();

        // Screenshot and record live in the menu. The record dot only
        // shows up here while recording (or counting down) so it can be
        // stopped without opening the menu.
        if (recorder_.is_recording() || record_countdown_ms_ > 0) {
        // Bezel record button — same horizontal position as the island
        // record button, same red filled-dot / square language.
        int rec_mini_x = rec_full_cx - mini_sz / 2;
        int rec_mini_y = mini_y;
        bezel_record_btn_ = {rec_mini_x, rec_mini_y, mini_sz, mini_sz};
        bool rhov = in_rect(bmx, bmy, rec_mini_x, rec_mini_y, mini_sz, mini_sz);
        bool rec_active_b = recorder_.is_recording();
        bool rec_count_b  = (record_countdown_ms_ > 0 && !rec_active_b);
        float rec_pulse_b = 1.0f;
        if (rec_active_b) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now_b.time_since_epoch()).count();
            rec_pulse_b = 0.55f + 0.45f * (float)std::abs(std::sin(ms * 0.005));
        }
        uint8_t rbg_r = rec_active_b ? (uint8_t)(110 * rec_pulse_b + 60)
                                     : (rhov ? 80 : 40);
        uint8_t rbg_g = rhov ? 35 : 35;
        uint8_t rbg_b = rhov ? 35 : 40;
        SDL_SetRenderDrawColor(sdl_renderer_, rbg_r, rbg_g, rbg_b,
                               rhov || rec_active_b ? 200 : 130);
        fill_circle(sdl_renderer_, rec_mini_x + mini_sz / 2,
                    rec_mini_y + mini_sz / 2, mini_sz / 2);
        int rrr = std::max(2, mini_sz / 4);
        int rrcx = rec_mini_x + mini_sz / 2;
        int rrcy = rec_mini_y + mini_sz / 2;
        uint8_t ra = rec_active_b ? 255 : (rhov ? 240 : 210);
        SDL_SetRenderDrawColor(sdl_renderer_, 235, 80, 80, ra);
        if (rec_active_b) {
            SDL_Rect sqr{rrcx - rrr, rrcy - rrr, rrr * 2, rrr * 2};
            SDL_RenderFillRect(sdl_renderer_, &sqr);
        } else {
            fill_circle(sdl_renderer_, rrcx, rrcy, rrr);
            if (rec_count_b) {
                int ring = rrr + 2;
                SDL_SetRenderDrawColor(sdl_renderer_, 240, 240, 240, 220);
                for (int dy = -ring; dy <= ring; dy++) {
                    for (int dx = -ring; dx <= ring; dx++) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 <= ring * ring && d2 >= (ring - 1) * (ring - 1))
                            SDL_RenderDrawPoint(sdl_renderer_, rrcx + dx, rrcy + dy);
                    }
                }
            }
        }
        if (rhov) {
            bezel_hover_key = "bezel_rec";
            if (rec_active_b) {
                int s = (int)recorder_.elapsed_seconds();
                static char rb[64];
                std::snprintf(rb, sizeof(rb), "Stop recording - %d:%02d (Ctrl+R)", s / 60, s % 60);
                bezel_hover_text = rb;
            } else if (rec_count_b) {
                bezel_hover_text = "Cancel countdown";
            } else {
                bezel_hover_text = "Record (Ctrl+R)\nRight-click for delay/timed";
            }
            bezel_hover_ax = rrcx;
            bezel_hover_ay = rec_mini_y + mini_sz + 4;
        }
        }

        // Bezel close button — same vertical/horizontal placement strategy
        // as the bezel screenshot button, anchored to the island's close
        // button X. Drawn as two thin diagonal strokes (X) that match the
        // chevron aesthetic used by the menu/log toggle indicators rather
        // than a filled circle with a glyph.
        int close_full_x  = island_x + island_w - pad_full - btn_sz_full;
        int close_full_cx = close_full_x + btn_sz_full / 2;
        int close_mini_x  = close_full_cx - mini_sz / 2;
        int close_mini_y  = mini_y;
        bezel_close_btn_ = {close_mini_x, close_mini_y, mini_sz, mini_sz};

        bool xhov = in_rect(bmx, bmy, close_mini_x, close_mini_y, mini_sz, mini_sz);
        // Stroke geometry mirrors the menu/log toggle: same thinner chevron
        // stroke so the three indicators read as a set. Length is clamped a
        // bit tighter than the chevrons so the diagonals don't graze the
        // bezel edges (the X reaches the corners of its bounding box, where
        // a chevron only reaches one side).
        int dot_r_close = std::max(2, ui_ref_width() / 175);
        int xg_sz   = std::max(6, dot_r_close * 4);
        // Also keep the X comfortably inside the mini button — leave at
        // least ~25% padding on every side.
        if (xg_sz > mini_sz * 3 / 4) xg_sz = mini_sz * 3 / 4;
        int xcx_c   = close_mini_x + mini_sz / 2;
        int xcy_c   = close_mini_y + mini_sz / 2;
        // Hover: tinted red ("destructive"), like the menu toggle otherwise.
        uint8_t xa = xhov ? 240 : 160;
        draw_glyph(sdl_renderer_, lucide::X, xcx_c, xcy_c, xg_sz * 3 / 2,
                   xhov ? 235 : 220, xhov ? 110 : 224, xhov ? 110 : 230, xa);

        if (xhov) {
            bezel_hover_key = "bezel_close";
            bezel_hover_text = "Close (Esc)";
            bezel_hover_ax = xcx_c;
            bezel_hover_ay = close_mini_y + mini_sz + 4;
        }

        // Standalone screenshot toast — shown when the island is hidden so
        // the "Saved to Pictures" / "Copied to clipboard" confirmation is
        // never lost just because the menu happens to be collapsed.
        if (toast_active_) {
            auto elapsed_t = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - toast_start_).count();
            if (elapsed_t < toast_duration_ms_) {
#ifdef _WIN32
                if (toast_tex_str_ != toast_text_) {
                    if (toast_tex_) { SDL_DestroyTexture(toast_tex_); toast_tex_ = nullptr; }
                    int fh_toast = std::max(28, btn_sz_full * 2);
                    toast_tex_ = make_text_texture(sdl_renderer_, toast_text_, fh_toast,
                                                    230, 230, 230,
                                                    &toast_tex_w_, &toast_tex_h_);
                    toast_tex_str_ = toast_text_;
                }
#endif
                if (toast_tex_) {
                    uint8_t alpha = 255;
                    if (elapsed_t < 150) alpha = (uint8_t)(elapsed_t * 255 / 150);
                    else if (elapsed_t > toast_duration_ms_ - 300)
                        alpha = (uint8_t)((toast_duration_ms_ - elapsed_t) * 255 / 300);
                    float t_scale = (float)std::max(11, btn_sz_full / 2)
                                  / std::max(28, btn_sz_full * 2);
                    int disp_tw = (int)(toast_tex_w_ * t_scale);
                    int disp_th = (int)(toast_tex_h_ * t_scale);
                    int pillx_pad = std::max(8, btn_sz_full / 3);
                    int pill_w = disp_tw + pillx_pad * 2;
                    int pill_h = disp_th + std::max(6, pillx_pad / 2) * 2;
                    int pill_x = svx_b + (svw_b - pill_w) / 2;
                    int pill_y = frame_dst_y_ + bezel_top + std::max(6, btn_sz_full / 4);
                    int pr = pill_h / 2;
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(sdl_renderer_, 20, 20, 22,
                                           (uint8_t)(alpha * 200 / 255));
                    SDL_Rect body = {pill_x + pr, pill_y, pill_w - pr * 2, pill_h};
                    SDL_RenderFillRect(sdl_renderer_, &body);
                    fill_circle(sdl_renderer_, pill_x + pr, pill_y + pr, pr);
                    fill_circle(sdl_renderer_, pill_x + pill_w - pr, pill_y + pr, pr);
                    SDL_SetTextureAlphaMod(toast_tex_, alpha);
                    SDL_Rect tdst = {pill_x + pillx_pad,
                                     pill_y + (pill_h - disp_th) / 2,
                                     disp_tw, disp_th};
                    SDL_RenderCopy(sdl_renderer_, toast_tex_, nullptr, &tdst);
                    SDL_SetTextureAlphaMod(toast_tex_, 255);
                }
            } else {
                toast_active_ = false;
            }
        }
    }

    // Persistent "Update available" banner. Sits at the top of the phone
    // frame and is independent of the island/bezel mini-button visibility
    // (the previous placement was inside `if (island_anim_ < 0.5f)`, which
    // suppressed the banner whenever the menu was open — i.e. the default
    // first-run state). Draws below the toast pill when both are visible.
    draw_update_banner();

    // Source picker: only with two or more phones. One chip per phone with
    // its (shortened) name in the bottom bezel; the phone on screen is
    // filled with the accent colour. Click switches, right-click opens the
    // per-phone menu. With one phone nothing is shown (Settings > Phone).
    source_btns_.clear();
    if (get_sources_fn_) {
        auto sources = get_sources_fn_();
        if (sources.size() >= 2) {
            float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
            int bezel_bottom = frame_dst_h_ -
                (int)((phone_frame_.screen_y() + phone_frame_.screen_height()) * scale);
            int cr = (int)(phone_frame_.corner_radius() * scale);
            int chip_h = std::max(12, std::min(bezel_bottom - 4, ui_ref_width() / 16));
            int font = std::max(9, chip_h * 62 / 100);
            int cy = frame_dst_y_ + frame_dst_h_ - bezel_bottom / 2;
            int avail = frame_dst_w_ - cr * 2;
            int n = (int)sources.size();
            int gapc = std::max(4, chip_h / 3);
            int chip_w = std::min(chip_h * 6, (avail - (n - 1) * gapc) / n);
            int x = frame_dst_x_ + (frame_dst_w_ - (n * chip_w + (n - 1) * gapc)) / 2;
            const Accent& ac = accent_for(settings_.accent_color);
            int pmx, pmy;
            SDL_GetMouseState(&pmx, &pmy);
            for (int i = 0; i < n; ++i, x += chip_w + gapc) {
                BtnRect r{x, cy - chip_h / 2, chip_w, chip_h};
                bool hover = in_rect(pmx, pmy, r.x, r.y, r.w, r.h);
                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
                if (sources[i].active)  SDL_SetRenderDrawColor(sdl_renderer_, ac.r, ac.g, ac.b, 255);
                else if (hover)         SDL_SetRenderDrawColor(sdl_renderer_, 64, 70, 82, 255);
                else                    SDL_SetRenderDrawColor(sdl_renderer_, 44, 48, 56, 255);
                fill_round_rect(sdl_renderer_, r.x, r.y, r.w, r.h, chip_h / 2);
                std::string name = sources[i].name;
                if (sources[i].paused) name += " (paused)";
                int maxw = chip_w - chip_h;
                uint8_t ink = sources[i].active ? (ac.dark_ink ? 14 : 255) : 210;
                draw_text_mid(sdl_renderer_, name, fit_text_px(sdl_renderer_, name, font, maxw, 600),
                              ink, ink, ink, 255, r.x + r.w / 2, cy, 1, 600);
                source_btns_.emplace_back(sources[i].id, r);
                if (hover) {
                    bezel_hover_key = "src:" + sources[i].id;
                    std::string shortcut = i < 9 ? "  (Ctrl+" + std::to_string(i + 1) + ")" : "";
                    bezel_hover_text = sources[i].name + shortcut +
                                       "\nClick: show this phone  ·  Right-click: menu";
                    bezel_hover_ax = r.x + r.w / 2;
                    bezel_hover_ay = r.y - 4;
                }
            }
        }
    }

    // Resize grips — a short arc hugging the top-left and bottom-right
    // corners of the phone.
    {
        float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
        int cr = std::max(4, (int)(phone_frame_.corner_radius() * scale));
        // Stroke derived from ui_ref_width() so the grip keeps the same
        // visual weight whether mirroring a phone or a tablet/desktop. The
        // arc itself follows the phone frame's actual corner radius.
        int stroke_r = std::max(1, ui_ref_width() / 260);
        float arc_r = cr - stroke_r * 4.0f;
        float step = std::max(1.0f, 40.0f / std::max(1.0f, arc_r));

        int rmx, rmy;
        SDL_GetMouseState(&rmx, &rmy);
        const float pi = 3.14159265f;
        // (cx, cy) is the centre of the corner arc; start_deg is where the
        // corner's quadrant begins (0 = bottom-right, 180 = top-left).
        auto draw_grip = [&](BtnRect& hit, int cx, int cy, float start_deg) {
            bool hover = in_rect(rmx, rmy, hit.x, hit.y, hit.w, hit.h);
            // Opaque greys: the overlapping dots that make up the arc
            // would add up unevenly with alpha.
            uint8_t g = hover ? 235 : 135;
            SDL_SetRenderDrawColor(sdl_renderer_, g, g, (uint8_t)(g + 5), 255);
            for (float deg = start_deg + 20.0f; deg <= start_deg + 70.0f; deg += step) {
                float rad = deg * pi / 180.0f;
                fill_circle(sdl_renderer_, cx + (int)std::lround(arc_r * cosf(rad)),
                            cy + (int)std::lround(arc_r * sinf(rad)), stroke_r);
            }
            if (hover) {
                bezel_hover_key = "resize";
                bezel_hover_text = "Drag to resize\nRight-click for menu";
                bezel_hover_ax = hit.x + hit.w / 2;
                bezel_hover_ay = hit.y + hit.h / 2;
            }
        };
        resize_grip_ = {frame_dst_x_ + frame_dst_w_ - cr, frame_dst_y_ + frame_dst_h_ - cr, cr, cr};
        draw_grip(resize_grip_, resize_grip_.x, resize_grip_.y, 0.0f);
        resize_grip_tl_ = {frame_dst_x_, frame_dst_y_, cr, cr};
        draw_grip(resize_grip_tl_, frame_dst_x_ + cr, frame_dst_y_ + cr, 180.0f);
    }

    // Delayed (1 s) tooltip for any bezel button currently hovered.
    // Hidden while a popup menu is open to avoid visual conflict.
    if (!bezel_menu_visible_ && !bezel_hover_key.empty() && tooltip_ready(bezel_hover_key)) {
        draw_bezel_tooltip(bezel_hover_text, bezel_hover_ax, bezel_hover_ay);
    } else if (bezel_hover_key.empty() || bezel_menu_visible_) {
        // Reset hover tracker when the cursor leaves all bezel buttons so
        // re-entering restarts the 1 s delay.
        if (hover_key_.rfind("menu", 0) == 0 ||
            hover_key_.rfind("log", 0) == 0 ||
            hover_key_.rfind("stats", 0) == 0 ||
            hover_key_.rfind("src:", 0) == 0 ||
            hover_key_.rfind("resize", 0) == 0 ||
            hover_key_.rfind("webcam", 0) == 0 ||
            hover_key_.rfind("bezel_", 0) == 0) {
            hover_key_.clear();
        }
    }

    // Generic right-click popup menu for any bezel button.
    if (bezel_menu_visible_) {
        // Build the item list for this target.
        struct Item { std::string action, label; };
        std::vector<Item> items;
        if (bezel_menu_target_ == "menu") {
            items.push_back({"exit", "Exit application (Esc)"});
        } else if (bezel_menu_target_ == "log") {
            items.push_back({"copy", "Copy log to clipboard (Ctrl+C)"});
            items.push_back({"clear", "Clear log (Ctrl+X)"});
        } else if (bezel_menu_target_.rfind("src:", 0) == 0) {
            std::string src_id = bezel_menu_target_.substr(4);
            std::string name = "device";
            if (get_sources_fn_) {
                for (auto& s : get_sources_fn_()) {
                    if (s.id == src_id) { name = s.name; break; }
                }
            }
            items.push_back({"disconnect", "Disconnect " + name});
        } else if (bezel_menu_target_ == "record") {
            const bool active = recorder_.is_recording();
            const bool counting = (record_countdown_ms_ > 0 && !active);
            if (active) {
                items.push_back({"stop", "Stop recording (Ctrl+R)"});
            } else if (counting) {
                items.push_back({"cancel", "Cancel countdown"});
            } else {
                items.push_back({"start",   "Start now (Ctrl+R)"});
                items.push_back({"delay5",  "Start in 5 s"});
                items.push_back({"timed5",  "Record 5 s"});
                items.push_back({"timed10", "Record 10 s"});
                items.push_back({"timed15", "Record 15 s"});
            }
        } else if (bezel_menu_target_ == "resize") {
            items.push_back({"reset_size", "Reset to default size (Ctrl+0)"});
        } else if (bezel_menu_target_ == "screenshot") {
            items.push_back({"shot",     "Take screenshot (Ctrl+S)"});
            items.push_back({"annotate", "Annotate screenshot (Ctrl+Shift+S)"});
#ifdef _WIN32
            items.push_back({"ocr",      "OCR copy text (Ctrl+Shift+T)"});
#endif
            items.push_back({"open_dir", "Open screenshot folder"});
        } else if (bezel_menu_target_ == "webcam") {
#ifdef ENABLE_WEBCAM
            // List cameras visible to Media Foundation. The currently
            // selected device (settings_.webcam_device_id, empty = first
            // available) is prefixed with a checkmark.
            auto cams = WebcamCapture::enumerate();
            // SDL_ttf renders the menu with an ASCII-only bitmap font;
            // any byte >= 0x80 in the MF friendly name shows up as
            // garbled glyphs. Strip those down to plain ASCII for the
            // picker so the labels stay readable (the underlying
            // device id is unchanged and still matches on click).
            auto sanitize_ascii = [](const std::string& s) {
                std::string out;
                out.reserve(s.size());
                for (unsigned char c : s) {
                    if (c >= 0x20 && c < 0x7F) out.push_back((char)c);
                    else if (c == ' ' || c == '\t') out.push_back(' ');
                    // skip multi-byte / control bytes entirely
                }
                // collapse runs of spaces
                std::string collapsed;
                collapsed.reserve(out.size());
                bool prev_space = false;
                for (char c : out) {
                    if (c == ' ') {
                        if (!prev_space) collapsed.push_back(' ');
                        prev_space = true;
                    } else {
                        collapsed.push_back(c);
                        prev_space = false;
                    }
                }
                while (!collapsed.empty() && collapsed.back() == ' ')
                    collapsed.pop_back();
                size_t start = collapsed.find_first_not_of(' ');
                if (start == std::string::npos) return std::string("(camera)");
                return collapsed.substr(start);
            };
            if (cams.empty()) {
                items.push_back({"_none", "(no webcam detected)"});
            } else {
                bool any_selected = false;
                for (auto& c : cams) {
                    bool sel = (c.id == settings_.webcam_device_id);
                    if (sel) any_selected = true;
                    // SDL_ttf's bitmap font is ASCII-only, so use a
                    // plain '>' as the selection marker instead of a
                    // UTF-8 check glyph (which renders as garbage).
                    std::string prefix = sel ? "> " : "  ";
                    items.push_back({"wc:" + c.id, prefix + sanitize_ascii(c.name)});
                }
                if (!any_selected && !items.empty()) {
                    items.front().label = "> " +
                                          items.front().label.substr(2);
                }
            }
            items.push_back({
                settings_.webcam_mirror_h ? "mirror_off" : "mirror_on",
                settings_.webcam_mirror_h
                    ? "Stop mirroring (selfie view)"
                    : "Mirror horizontally (selfie view)"});
            items.push_back({
                webcam_drawer_visible_ ? "hide" : "show",
                webcam_drawer_visible_
                    ? "Hide Webcam (W)"
                    : "Show Webcam (W)"});
#else
            items.push_back({"_none", "(webcam support disabled at build time)"});
#endif
        }
        if (items.empty()) {
            bezel_menu_visible_ = false;
        } else {
            // Slide-in animation, matching info/version panels.
            const float anim_duration = 200.0f;
            if (bezel_menu_animating_) {
                float elapsed = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - bezel_menu_anim_start_).count();
                float t = std::min(1.0f, elapsed / anim_duration);
                float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
                bezel_menu_anim_ = eased;
                if (t >= 1.0f) bezel_menu_animating_ = false;
            } else {
                bezel_menu_anim_ = 1.0f;
            }

            // Use the shared phone-equivalent UI reference width so the
            // right-click popup stays the same neat size regardless of
            // whether the source is a phone, tablet or desktop.
            int eq_w = ui_ref_width();
            int font_h = std::max(12, eq_w / 36);
            int line_gap = std::max(3, font_h / 4);
            int pad = std::max(8, font_h);

            // Render each item to a texture and find the widest.
            struct R { SDL_Texture* tex; int w, h; std::string action; };
            std::vector<R> rendered;
            int max_w = 0, item_h = font_h;
            for (auto& it : items) {
                int tw = 0, th = 0;
                SDL_Texture* tex = make_text_texture(
                    sdl_renderer_, it.label, font_h, 240, 240, 240, &tw, &th);
                rendered.push_back({tex, tw, th, it.action});
                if (tw > max_w) max_w = tw;
                if (th > item_h) item_h = th;
            }
            int row_h = item_h + line_gap * 2;
            int panel_w = max_w + pad * 2;
            int panel_h = pad + (int)rendered.size() * row_h + pad - line_gap;

            // Choose slide direction based on which bezel button was clicked:
            //   menu (top)    → slide down
            //   log  (right)  → slide left
            //   src  (bottom) → slide up
            //   resize/other  → slide up by default
            int dx = 0, dy = 0;          // initial offset before easing
            int panel_x = 0, panel_y = 0; // final position
            int win_w = 0, win_h = 0;
            SDL_GetWindowSize(window_, &win_w, &win_h);

            if (bezel_menu_target_ == "menu") {
                panel_x = bezel_menu_anchor_x_ - panel_w / 2;
                panel_y = bezel_menu_anchor_y_ + 12;
                dy = -(int)(panel_h * 0.3f);
            } else if (bezel_menu_target_ == "log") {
                panel_x = bezel_menu_anchor_x_ - panel_w - 12;
                panel_y = bezel_menu_anchor_y_ - panel_h / 2;
                dx = (int)(panel_w * 0.3f);
            } else if (bezel_menu_target_ == "record") {
                // Record button lives in the top island — slide DOWN from
                // the cursor so the menu doesn't crash into the window top.
                panel_x = bezel_menu_anchor_x_ - panel_w / 2;
                panel_y = bezel_menu_anchor_y_ + 12;
                dy = -(int)(panel_h * 0.3f);
            } else if (bezel_menu_target_ == "screenshot") {
                // Screenshot button is also in the top island — match the
                // record menu and drop the popup below the cursor.
                panel_x = bezel_menu_anchor_x_ - panel_w / 2;
                panel_y = bezel_menu_anchor_y_ + 12;
                dy = -(int)(panel_h * 0.3f);
            } else if (bezel_menu_target_ == "resize") {
                // Resize grip lives in the bottom-right corner — open the
                // menu to the LEFT of the grip so it stays inside the
                // phone-frame area (and doesn't collide with the log panel
                // when that is open on the right).
                panel_x = bezel_menu_anchor_x_ - panel_w - 12;
                panel_y = bezel_menu_anchor_y_ - panel_h / 2;
                dx = (int)(panel_w * 0.3f);
            } else if (bezel_menu_target_ == "webcam") {
                // Webcam toggle sits in the bottom-LEFT bezel — open the
                // menu to the RIGHT of the cursor so it doesn't clip off
                // the left edge of the window.
                panel_x = bezel_menu_anchor_x_ + 12;
                panel_y = bezel_menu_anchor_y_ - panel_h - 12;
                dy = (int)(panel_h * 0.3f);
            } else {
                // Sources (bottom) and fallback: slide up from the button.
                panel_x = bezel_menu_anchor_x_ - panel_w / 2;
                panel_y = bezel_menu_anchor_y_ - panel_h - 12;
                dy = (int)(panel_h * 0.3f);
            }
            // Clamp inside window.
            if (panel_x + panel_w > win_w - 4) panel_x = win_w - panel_w - 4;
            if (panel_x < 4) panel_x = 4;
            if (panel_y + panel_h > win_h - 4) panel_y = win_h - panel_h - 4;
            if (panel_y < 4) panel_y = 4;

            // Apply slide-in offset (decays with anim progress).
            float inv = 1.0f - bezel_menu_anim_;
            int draw_x = panel_x + (int)(dx * inv);
            int draw_y = panel_y + (int)(dy * inv);
            uint8_t alpha = (uint8_t)(240 * bezel_menu_anim_);
            uint8_t text_alpha = (uint8_t)(255 * bezel_menu_anim_);

            int mmx, mmy;
            SDL_GetMouseState(&mmx, &mmy);

            // Rounded-rect background, same recipe as draw_info_panel.
            // Switch off blending so the body rect + side strips + corner
            // discs do not alpha-compound into visible darker arcs at their
            // overlap.
            int pr = std::max(6, pad / 2);
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
            SDL_Rect body{draw_x + pr, draw_y, panel_w - pr * 2, panel_h};
            SDL_RenderFillRect(sdl_renderer_, &body);
            SDL_Rect ls{draw_x, draw_y + pr, pr, panel_h - pr * 2};
            SDL_RenderFillRect(sdl_renderer_, &ls);
            SDL_Rect rs{draw_x + panel_w - pr, draw_y + pr, pr, panel_h - pr * 2};
            SDL_RenderFillRect(sdl_renderer_, &rs);
            fill_circle(sdl_renderer_, draw_x + pr, draw_y + pr, pr);
            fill_circle(sdl_renderer_, draw_x + panel_w - pr, draw_y + pr, pr);
            fill_circle(sdl_renderer_, draw_x + pr, draw_y + panel_h - pr, pr);
            fill_circle(sdl_renderer_, draw_x + panel_w - pr, draw_y + panel_h - pr, pr);
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

            bezel_menu_items_.clear();
            int row_y = draw_y + pad - line_gap;
            for (size_t i = 0; i < rendered.size(); ++i) {
                SDL_Rect row{draw_x + pr, row_y, panel_w - pr * 2, row_h};
                bool hov = in_rect(mmx, mmy, row.x, row.y, row.w, row.h);
                if (hov && bezel_menu_anim_ >= 0.5f) {
                    SDL_SetRenderDrawColor(sdl_renderer_, 70, 70, 80, alpha);
                    SDL_RenderFillRect(sdl_renderer_, &row);
                }
                if (rendered[i].tex) {
                    int dw = rendered[i].w;
                    int dh = rendered[i].h;
                    SDL_Rect td{draw_x + (panel_w - dw) / 2,
                                row.y + (row_h - dh) / 2, dw, dh};
                    SDL_SetTextureAlphaMod(rendered[i].tex, text_alpha);
                    SDL_RenderCopy(sdl_renderer_, rendered[i].tex, nullptr, &td);
                    SDL_DestroyTexture(rendered[i].tex);
                }
                // Hit-rect uses the FINAL panel position so clicks line up
                // even mid-animation.
                bezel_menu_items_.emplace_back(
                    rendered[i].action,
                    BtnRect{panel_x + pr, panel_y + (pad - line_gap) + (int)i * row_h,
                            panel_w - pr * 2, row_h});
                row_y += row_h;
            }
        }
    }

    // Window shape
    if (!window_shape_set_ || log_panel_animating_ || webcam_drawer_animating_) {
        update_window_shape();
        window_shape_set_ = true;
    }

    // PIN overlay (drawn last, on top of everything)
    draw_pin_overlay();

    // Screenshot annotator (drawn above PIN so the modal editor wins).
    if (annotator_active_) {
        draw_annotator();
    }
#ifdef _WIN32
    if (ocr_active_) {
        draw_ocr_overlay();
    }
    process_ocr_result();
#endif

    // Deferred tooltip from early-frame panels (e.g. webcam drawer).
    // Painted here so it sits above the bottom bezel / dynamic island
    // / resize grip that drew over the drawer earlier in the frame.
    if (!pending_tooltip_text_.empty()) {
        draw_bezel_tooltip(pending_tooltip_text_,
                           pending_tooltip_x_, pending_tooltip_y_,
                           pending_tooltip_below_,
                           pending_tooltip_align_,
                           pending_tooltip_valign_);
        pending_tooltip_text_.clear();
        pending_tooltip_align_  = 0;
        pending_tooltip_valign_ = 0;
    }

    SDL_RenderPresent(sdl_renderer_);

    if (screenshot_requested_) {
        screenshot_requested_ = false;
        take_screenshot();
    }

    // ---- Recording lifecycle (after the present so we feed encoded frames
    // immediately and any UI changes happen on the next loop) ----
    if (record_toggle_requested_) {
        record_toggle_requested_ = false;
        if (recorder_.is_recording()) {
            stop_recording();
        } else if (record_countdown_ms_ > 0) {
            record_countdown_start_ = std::chrono::steady_clock::now();
        } else {
            start_recording();
        }
    }

    // Countdown -> start
    if (record_countdown_ms_ > 0 && !recorder_.is_recording()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_countdown_start_).count();
        if (elapsed >= record_countdown_ms_) {
            record_countdown_ms_ = 0;
            start_recording();
        }
    }

    // Push the latest composited frame to the recorder.
    if (recorder_.is_recording() && !last_frame_data_.empty()) {
#ifdef ENABLE_WEBCAM
        // Keep webcam_last_rgba_ fresh even when the drawer is collapsed
        // so the recording composite never freezes on a stale frame.
        if (recording_with_webcam_ && webcam_.is_running()) {
            poll_webcam_frame();
        }
#endif

        // When the phone bezel is on, encode the same composite the user sees
        // (rounded corners, dynamic island, bottom bar). MP4/GIF cannot carry
        // alpha, so the area outside the rounded bezel ends up black — which
        // reads as an intentional matte around the phone.
        const uint8_t* base_rgba = nullptr;
        int base_w = 0, base_h = 0, base_stride = 0;
        std::unique_ptr<uint8_t[]> phone_owned;
        if (phone_frame_enabled_ && phone_frame_.is_generated()) {
            int cw = 0, ch = 0;
            uint8_t* composite = phone_frame_.composite_screenshot(
                sdl_renderer_,
                last_frame_data_.data(), last_frame_w_, last_frame_h_, last_frame_stride_,
                &cw, &ch);
            if (composite) {
                phone_owned.reset(composite);
                base_rgba   = composite;
                base_w      = cw;
                base_h      = ch;
                base_stride = cw * 4;
            }
        }
        if (!base_rgba) {
            base_rgba   = last_frame_data_.data();
            base_w      = last_frame_w_;
            base_h      = last_frame_h_;
            base_stride = last_frame_stride_;
        }

#ifdef ENABLE_WEBCAM
        if (recording_with_webcam_ && recording_webcam_panel_h_ > 0 &&
            base_w == recording_canvas_w_ && base_h == recording_canvas_h_base_) {
            // Allocate the extended canvas (phone composite on top with
            // the panel tucked behind it by recording_webcam_overlap_).
            // Paint the strip FIRST so its top rows sit inside the
            // phone-bezel area, then alpha-composite the phone bezel on
            // top — same flow as the screenshot/annotation paths.
            int out_w    = recording_canvas_w_;
            int overlap  = recording_webcam_overlap_;
            int out_h    = recording_canvas_h_base_ +
                           recording_webcam_panel_h_ - overlap;
            std::vector<uint8_t> out(static_cast<size_t>(out_w) * out_h * 4, 0);
            paint_webcam_strip(out.data(), out_w,
                               base_h - overlap, recording_webcam_panel_h_,
                               overlap);
            blit_phone_over_strip(out.data(), out_w * 4,
                                  base_rgba, base_stride, base_w, base_h);
            recorder_.push_frame(out.data(), out_w, out_h, out_w * 4);
        } else
#endif
        {
            recorder_.push_frame(base_rgba, base_w, base_h, base_stride);
        }
    }

    // Recorder may have hit max-duration on the worker — finalise from the
    // renderer thread so we own the toast/UI updates.
    if (recorder_.should_finalize()) {
        stop_recording();
    }
}

// Returns a "phone-equivalent" reference width for sizing overlay UI.
// Panels, popup menus, tooltips, swatches and the connect/help dialogs
// previously sized everything from `svw` (the on-screen phone screen
// width). When the user mirrors a tablet or desktop, `svw` becomes huge
// and every UI element bloats out of proportion. This helper folds the
// source down to a phone-equivalent width (taking the narrower of the
// real viewport width and `svh / 2`, which is the natural width of a
// 2:1 phone aspect) and clamps to an absolute ceiling so overlays stay
// compact even on very large windows. For phone-shaped sources this
// returns ~svw and behaviour is unchanged.
int Renderer::ui_ref_width() const {
    if (frame_dst_w_ == 0) return 0;
    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svw = (int)(phone_frame_.screen_width()  * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);
    int phone_eq = (std::min)(svw, svh / 2);
    constexpr int kAbsCap = 480;
    return (std::min)(phone_eq, kAbsCap);
}

bool Renderer::tooltip_ready(const std::string& key) {
    if (key.empty()) {
        hover_key_.clear();
        return false;
    }
    if (hover_key_ != key) {
        hover_key_ = key;
        hover_start_ = std::chrono::steady_clock::now();
        return false;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hover_start_).count();
    return elapsed >= 300;
}

#ifdef _WIN32
// Persistent two-line "Update available" banner. Pill background mirrors
// the standalone screenshot toast, but stacks two lines and exposes a
// clickable GitHub link on line 2 (with the same hover-underline + 1 s
// tooltip styling as the footer links). A small "x" in the upper-right
// dismisses it.
void Renderer::draw_update_banner() {
    if (!update_banner_active_) return;
    if (frame_dst_w_ == 0) return;

    // Resolve the version string under the lock and rebuild line-1 if it
    // changed (or on first show).
    std::string ver;
    {
        std::lock_guard<std::mutex> lk(update_check_mutex_);
        ver = update_latest_version_;
    }
    std::string line1 = "Update available: v" + ver;
    static const std::string kLinkLabel = "github.com/CDinhh/PhoneMimi";

    if (line1 != update_line1_cached_) {
        if (update_line1_tex_) { SDL_DestroyTexture(update_line1_tex_); update_line1_tex_ = nullptr; }
        if (update_link_tex_)  { SDL_DestroyTexture(update_link_tex_);  update_link_tex_  = nullptr; }
        update_line1_cached_ = line1;
    }

    // Render textures lazily at a font size matched to the standalone
    // toast pill so visual weight stays consistent with sibling overlays.
    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int btn_sz_full = std::max(20, ui_ref_width() / 14);
    int fh_text = std::max(28, btn_sz_full * 2);

    if (!update_line1_tex_) {
        update_line1_tex_ = make_text_texture(sdl_renderer_, line1, fh_text,
                                              230, 230, 230,
                                              &update_line1_w_, &update_line1_h_);
    }
    if (!update_link_tex_) {
        // Slight blue tint so the link reads as interactive even at rest,
        // matching the convention used by the footer links.
        update_link_tex_ = make_text_texture(sdl_renderer_, kLinkLabel, fh_text,
                                             150, 190, 240,
                                             &update_link_w_, &update_link_h_);
    }
    if (!update_line1_tex_ || !update_link_tex_) return;

    // Display scale: same recipe as the standalone toast, so font size
    // matches what the user sees on the screenshot/clipboard pill.
    float t_scale = (float)std::max(11, btn_sz_full / 2)
                  / std::max(28, btn_sz_full * 2);
    int l1_w = (int)(update_line1_w_ * t_scale);
    int l1_h = (int)(update_line1_h_ * t_scale);
    int l2_w = (int)(update_link_w_  * t_scale);
    int l2_h = (int)(update_link_h_  * t_scale);

    int line_gap = std::max(2, l1_h / 6);
    int pillx_pad = std::max(8, btn_sz_full / 3);
    int pilly_pad = std::max(6, pillx_pad / 2);
    // Reserve room for a small close glyph on the right.
    int close_sz  = std::max(10, l1_h);
    int close_gap = std::max(6, pillx_pad / 2);

    int content_w = std::max(l1_w, l2_w);
    int pill_w = content_w + pillx_pad * 2 + close_sz + close_gap;
    int pill_h = l1_h + line_gap + l2_h + pilly_pad * 2;

    // Center horizontally over the screen viewport, anchor near the top
    // bezel just like the standalone toast. If the toast is also up,
    // nudge below it so they don't overlap.
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int bezel_top = svy - frame_dst_y_;
    int pill_x = svx + (svw - pill_w) / 2;
    int pill_y = frame_dst_y_ + bezel_top + std::max(6, btn_sz_full / 4);
    if (toast_active_) {
        // Drop below a typical single-line toast so they stack cleanly.
        pill_y += l1_h + pilly_pad * 2 + std::max(4, line_gap);
    }

    // Soft slide-in over the first 200 ms.
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - update_banner_start_).count();
    float in_t = std::min(1.0f, (float)elapsed_ms / 200.0f);
    float eased = 1.0f - (1.0f - in_t) * (1.0f - in_t) * (1.0f - in_t);
    int slide = (int)((1.0f - eased) * pill_h * 0.4f);
    pill_y -= slide;
    uint8_t alpha = (uint8_t)(255 * eased);

    int pr = pill_h / 2;
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer_, 20, 20, 22, (uint8_t)(alpha * 220 / 255));
    SDL_Rect body = {pill_x + pr, pill_y, pill_w - pr * 2, pill_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    fill_circle(sdl_renderer_, pill_x + pr,         pill_y + pr, pr);
    fill_circle(sdl_renderer_, pill_x + pill_w - pr, pill_y + pr, pr);

    // Line 1 — plain text, centered against the content (close glyph
    // sits to the right of the content area).
    int content_x = pill_x + pillx_pad;
    int l1_x = content_x + (content_w - l1_w) / 2;
    int l1_y = pill_y + pilly_pad;
    SDL_SetTextureAlphaMod(update_line1_tex_, alpha);
    SDL_Rect l1_dst = {l1_x, l1_y, l1_w, l1_h};
    SDL_RenderCopy(sdl_renderer_, update_line1_tex_, nullptr, &l1_dst);

    // Line 2 — the GitHub link. Hit-rect a bit larger than the glyph
    // bounds so it's easy to click.
    int l2_x = content_x + (content_w - l2_w) / 2;
    int l2_y = l1_y + l1_h + line_gap;
    update_link_rect_ = {l2_x - 4, l2_y - 2, l2_w + 8, l2_h + 4};

    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    bool link_hover = in_rect(mx, my, update_link_rect_.x, update_link_rect_.y,
                              update_link_rect_.w, update_link_rect_.h);
    // Brighten on hover; baseline tint already reads as a link.
    if (link_hover) {
        SDL_SetTextureColorMod(update_link_tex_, 200, 220, 255);
    } else {
        SDL_SetTextureColorMod(update_link_tex_, 255, 255, 255);
    }
    SDL_SetTextureAlphaMod(update_link_tex_, alpha);
    SDL_Rect l2_dst = {l2_x, l2_y, l2_w, l2_h};
    SDL_RenderCopy(sdl_renderer_, update_link_tex_, nullptr, &l2_dst);

    // Hover underline — same affordance the footer links use.
    if (link_hover) {
        SDL_SetRenderDrawColor(sdl_renderer_, 200, 220, 255, alpha);
        int uy = l2_y + l2_h - 1;
        SDL_RenderDrawLine(sdl_renderer_, l2_x, uy, l2_x + l2_w, uy);
        // Hand cursor.
        static SDL_Cursor* hand = nullptr;
        if (!hand) hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
        if (hand) SDL_SetCursor(hand);
    }

    // Close glyph (small "x") in the upper-right of the pill.
    int close_x = pill_x + pill_w - pillx_pad - close_sz;
    int close_y = pill_y + pilly_pad - 1;
    update_close_rect_ = {close_x - 4, close_y - 4, close_sz + 8, close_sz + 8};
    bool close_hover = in_rect(mx, my, update_close_rect_.x, update_close_rect_.y,
                               update_close_rect_.w, update_close_rect_.h);
    uint8_t cr = close_hover ? 235 : 180;
    uint8_t cg = close_hover ? 110 : 180;
    uint8_t cb = close_hover ? 110 : 185;
    SDL_SetRenderDrawColor(sdl_renderer_, cr, cg, cb, alpha);
    int t = std::max(1, close_sz / 8);
    auto draw_x = [&](int cx0, int cy0, int half) {
        for (int dx = -t / 2; dx <= t / 2; ++dx)
            for (int dy = -t / 2; dy <= t / 2; ++dy) {
                SDL_RenderDrawLine(sdl_renderer_,
                                   cx0 - half + dx, cy0 - half + dy,
                                   cx0 + half + dx, cy0 + half + dy);
                SDL_RenderDrawLine(sdl_renderer_,
                                   cx0 - half + dx, cy0 + half + dy,
                                   cx0 + half + dx, cy0 - half + dy);
            }
    };
    draw_x(close_x + close_sz / 2, close_y + close_sz / 2, close_sz / 2 - 1);

    // Hover tooltips — drive the shared 1 s delay directly via
    // tooltip_ready() and draw_bezel_tooltip(), since this helper runs
    // outside the bezel render block where bezel_hover_* locals live.
    if (link_hover) {
        if (tooltip_ready("bezel_update_link")) {
            draw_bezel_tooltip("Open release on GitHub",
                               update_link_rect_.x + update_link_rect_.w / 2,
                               update_link_rect_.y + update_link_rect_.h + 4,
                               /*prefer_below=*/true);
        }
    } else if (close_hover) {
        if (tooltip_ready("bezel_update_close")) {
            draw_bezel_tooltip("Dismiss",
                               update_close_rect_.x + update_close_rect_.w / 2,
                               update_close_rect_.y + update_close_rect_.h + 4,
                               /*prefer_below=*/true);
        }
    }

    SDL_SetTextureAlphaMod(update_line1_tex_, 255);
    SDL_SetTextureColorMod(update_link_tex_, 255, 255, 255);
    SDL_SetTextureAlphaMod(update_link_tex_, 255);
}
#else
void Renderer::draw_update_banner() {}
#endif

void Renderer::draw_bezel_tooltip(const std::string& text, int anchor_x, int anchor_y,
                                  bool prefer_below, int h_align, int v_align) {
    if (text.empty()) return;

    // Split on the first '\n' into a primary line and an optional secondary
    // line (rendered smaller/dimmer below).
    std::string line1 = text;
    std::string line2;
    auto nl = text.find('\n');
    if (nl != std::string::npos) {
        line1 = text.substr(0, nl);
        line2 = text.substr(nl + 1);
    }

    // Use the shared phone-equivalent UI reference width so the tooltip
    // stays the same neat size regardless of source dimensions.
    int eq_w = ui_ref_width();
    int target_h = std::max(12, eq_w / 38);
    int target_h2 = std::max(10, (target_h * 4) / 5);

#ifdef _WIN32
    // Render at the actual target font height for crisp text (no scaling).
    if (text != bezel_tip_str_ || target_h != bezel_tip_font_h_) {
        if (bezel_tip_tex_)  { SDL_DestroyTexture(bezel_tip_tex_);  bezel_tip_tex_  = nullptr; }
        if (bezel_tip_tex2_) { SDL_DestroyTexture(bezel_tip_tex2_); bezel_tip_tex2_ = nullptr; }
        bezel_tip_tex_ = make_text_texture(sdl_renderer_, line1, target_h,
                                           255, 255, 255, &bezel_tip_w_, &bezel_tip_h_);
        if (!line2.empty()) {
            bezel_tip_tex2_ = make_text_texture(sdl_renderer_, line2, target_h2,
                                                170, 170, 170, &bezel_tip_w2_, &bezel_tip_h2_);
        } else {
            bezel_tip_w2_ = bezel_tip_h2_ = 0;
        }
        bezel_tip_str_ = text;
        bezel_tip_font_h_ = target_h;
    }
#endif
    if (!bezel_tip_tex_) return;

    int line_gap = bezel_tip_tex2_ ? std::max(2, target_h2 / 4) : 0;
    int disp_w = std::max(bezel_tip_w_, bezel_tip_w2_);
    int disp_h = bezel_tip_h_ + (bezel_tip_tex2_ ? line_gap + bezel_tip_h2_ : 0);
    int tp = std::max(3, bezel_tip_h_ / 3);
    int tw = disp_w + tp * 2;
    int th = disp_h + tp * 2;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    int tx;
    if (h_align == 1)      tx = anchor_x - tw; // tooltip ends at anchor
    else if (h_align == 2) tx = anchor_x;      // tooltip starts at anchor
    else                   tx = anchor_x - tw / 2;
    int ty;
    if (v_align == 1) {
        ty = anchor_y - th / 2; // vertically centered on anchor_y
    } else if (prefer_below) {
        ty = anchor_y + 12;
        if (ty + th > win_h - 4) ty = anchor_y - th - 6; // flip up if no room
    } else {
        ty = anchor_y - th - 6;
        if (ty < 4) ty = anchor_y + 12;
    }
    if (tx + tw > win_w - 4) tx = win_w - tw - 4;
    if (tx < 4) tx = 4;

    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 32, 230);
    SDL_Rect bg{tx, ty, tw, th};
    SDL_RenderFillRect(sdl_renderer_, &bg);
    SDL_SetRenderDrawColor(sdl_renderer_, 100, 100, 100, 220);
    SDL_RenderDrawRect(sdl_renderer_, &bg);
    SDL_Rect td{tx + (tw - bezel_tip_w_) / 2, ty + tp, bezel_tip_w_, bezel_tip_h_};
    SDL_RenderCopy(sdl_renderer_, bezel_tip_tex_, nullptr, &td);
    if (bezel_tip_tex2_) {
        SDL_Rect td2{tx + (tw - bezel_tip_w2_) / 2,
                     td.y + bezel_tip_h_ + line_gap,
                     bezel_tip_w2_, bezel_tip_h2_};
        SDL_RenderCopy(sdl_renderer_, bezel_tip_tex2_, nullptr, &td2);
    }
}

void Renderer::draw_footer(int svx, int svy, int svw, int svh) {
    footer_hits_.clear();
    if (footer_line3_.empty()) return;

    // Just the version, centred at the bottom of the screen (click opens the
    // version history). Textures are baked at a 40px font and scaled down.
    float sf = std::max(0.3f, svw / 300.0f) * (13.0f / 40.0f);
    sf = std::min(sf, 0.6f) * 0.9f;
    int total_w = 0;
    for (auto& s : footer_line3_) total_w += (int)(s.w * sf);
    int line_h = (int)(footer_line3_[0].h * sf);
    int fy = svy + svh - line_h - std::max(10, svh / 28);
    int fx = svx + (svw - total_w) / 2;
    for (auto& s : footer_line3_) {
        int sw = (int)(s.w * sf), sh = (int)(s.h * sf);
        SDL_Rect dst = {fx, fy, sw, sh};
        if (s.tex) {
            SDL_SetTextureColorMod(s.tex, 100, 112, 140);
            SDL_RenderCopy(sdl_renderer_, s.tex, nullptr, &dst);
        }
        if (!s.url.empty() || !s.tooltip.empty())
            footer_hits_.push_back({fx, fy, sw, sh, s.url, s.tooltip});
        fx += sw;
    }

    // Hover tooltip for footer segments
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    std::string hovered;
    int hx = 0, hy = 0, hw = 0;
    for (auto& h : footer_hits_) {
        if (!h.tooltip.empty() && in_rect(mx, my, h.x, h.y, h.w, h.h)) {
            hovered = h.tooltip;
            hx = h.x; hy = h.y; hw = h.w;
            break;
        }
    }

    if (!hovered.empty() && tooltip_ready("footer:" + hovered)) {
        // Use the unified bezel tooltip styling so footer hovers match
        // the rest of the UI. Anchor at the segment's top-center.
        draw_bezel_tooltip(hovered, hx + hw / 2, hy);
    } else if (hovered.empty() &&
               hover_key_.rfind("footer:", 0) == 0) {
        hover_key_.clear();
    }
}

void Renderer::draw_info_panel() {
    if (frame_dst_w_ == 0 || info_lines_.empty()) return;

    // Update animation
    const float anim_duration = 200.0f; // ms
    if (info_panel_animating_) {
        float elapsed = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - info_panel_anim_start_).count();
        float t = std::min(1.0f, elapsed / anim_duration);
        // Ease out cubic
        float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        if (info_panel_visible_)
            info_panel_anim_ = eased;
        else
            info_panel_anim_ = 1.0f - eased;
        if (t >= 1.0f) info_panel_animating_ = false;
    } else {
        info_panel_anim_ = info_panel_visible_ ? 1.0f : 0.0f;
    }

    if (info_panel_anim_ <= 0.0f) return;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);

    // UI dimensions are derived from a phone-equivalent reference width so
    // the Info panel stays the same neat size whether the source is a
    // phone, an iPad, or a Mac desktop. Centering still uses svw.
    int uw = ui_ref_width();
    int pad = std::max(8, uw / 20);
    int line_gap = std::max(3, pad / 4);
    int spacer_h = std::max(4, pad / 2);

    // Scale text to fit panel, never below a readable minimum.
    float max_text_w = uw * 0.7f;
    const float min_text_scale = 0.40f;
    float text_scale = 0.5f;
    for (auto& line : info_lines_) {
        if (line.tex && line.w * text_scale > max_text_w)
            text_scale = max_text_w / line.w;
    }
    if (text_scale < min_text_scale) text_scale = min_text_scale;

    // Compute total height
    int total_h = pad;
    for (auto& line : info_lines_) {
        if (line.tex)
            total_h += (int)(line.h * text_scale) + line_gap;
        else
            total_h += spacer_h;
    }
    // Reserve space for the "Copy network test script" button beneath the
    // text lines: spacer + button + bottom pad. Sized to match the
    // network-requirement lines so it doesn't dominate the panel.
    int btn_label_h = std::max(14, (int)(30 * text_scale + 0.5f));
    int btn_h = btn_label_h + std::max(4, pad / 5);
    // Hover caption is a smaller line that appears under the button on hover
    // — reserve its slot up-front so the panel doesn't reflow when shown.
    int hint_label_h = std::max(11, (int)(22 * text_scale + 0.5f));
    total_h += spacer_h + btn_h + line_gap + hint_label_h;

    // Reserve space for the footer block (MSEndpointMgr / Simon / coffee
    // links + version + Check-for-updates button) at the bottom of the
    // Info panel. Uses dedicated info_footer_* textures baked at the
    // same font size as the network-requirement lines, scaled by
    // text_scale so they share the visual weight of the surrounding
    // body text.
    auto info_footer_line_w = [&](const std::vector<FooterSeg>& line) {
        int total = 0;
        for (auto& s : line) total += (int)(s.w * text_scale);
        return total;
    };
    int foot_h1 = info_footer_line1_.empty() ? 0 : (int)(info_footer_line1_[0].h * text_scale);
    int foot_h2 = info_footer_line2_.empty() ? 0 : (int)(info_footer_line2_[0].h * text_scale);
    int foot_h3 = info_footer_line3_.empty() ? 0 : (int)(info_footer_line3_[0].h * text_scale);
    int foot_gap = std::max(2, foot_h1 / 4);
    int about_h = (info_about_header_.tex)
                      ? (int)(info_about_header_.h * text_scale) : 0;
    int footer_block_h = (about_h > 0 ? about_h + foot_gap : 0)
                       + (foot_h1 > 0 ? foot_h1 : 0)
                       + (foot_h2 > 0 ? (foot_gap + foot_h2) : 0)
                       + (foot_h3 > 0 ? (foot_gap + foot_h3) : 0);
    if (footer_block_h > 0) total_h += spacer_h + footer_block_h;

    // Reserve space for the "Check for updates" button — same recipe as
    // the "Copy network test script" button above (button + hover hint
    // slot) so the panel doesn't reflow on hover.
    int chk_btn_label_h = btn_label_h;
    int chk_btn_h = btn_h;
    total_h += spacer_h + chk_btn_h + line_gap + hint_label_h;

    total_h += pad - line_gap;

    int panel_w = (std::min)((int)(svw * 0.80f), (int)(uw * 1.6f));
    int panel_x = svx + (svw - panel_w) / 2;

    // Position below island bar
    int btn_sz = std::max(20, uw / 14);
    int ipad = btn_sz / 3;
    int island_bottom = svy + ipad + btn_sz + ipad * 2;
    int panel_y_target = island_bottom + pad / 2;

    // Animate: slide down from island_bottom, with fade
    int slide_offset = (int)((1.0f - info_panel_anim_) * total_h * 0.3f);
    int panel_y = panel_y_target - slide_offset;
    uint8_t alpha = (uint8_t)(240 * info_panel_anim_);
    uint8_t text_alpha = (uint8_t)(255 * info_panel_anim_);

    info_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    // Rounded rect background. BLENDMODE_NONE so the body + strips +
    // corner discs cannot alpha-compound at their overlap.
    int pr = std::max(6, pad / 2);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
    SDL_Rect body = {panel_x + pr, panel_y, panel_w - pr * 2, total_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    SDL_Rect ls = {panel_x, panel_y + pr, pr, total_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &ls);
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, total_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + total_h - pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + total_h - pr, pr);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    // Draw text lines centered
    int cy = panel_y + pad;
    for (auto& line : info_lines_) {
        if (line.tex) {
            int dw = (int)(line.w * text_scale);
            int dh = (int)(line.h * text_scale);
            int lx = panel_x + (panel_w - dw) / 2;
            SDL_Rect dst = {lx, cy, dw, dh};
            SDL_SetTextureAlphaMod(line.tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, line.tex, nullptr, &dst);
            cy += dh + line_gap;
        } else {
            cy += spacer_h;
        }
    }

    // "Copy network test script" button — pasted into a PowerShell window
    // to verify firewall rules and listening ports for AirPlay / scrcpy.
    // Sized to roughly match the network-requirement text rather than a
    // big call-to-action; a hover caption explains usage.
    cy += spacer_h;

    int mx, my;
    SDL_GetMouseState(&mx, &my);

#ifdef _WIN32
    // Render the label first so the button can wrap tightly around it.
    int lw = 0, lh = 0;
    SDL_Texture* lab = make_text_texture(sdl_renderer_,
        "Copy network test script", btn_label_h,
        220, 220, 230, &lw, &lh);
    int btn_w = lw + std::max(12, pad);
    if (btn_w > panel_w - pad * 2) btn_w = panel_w - pad * 2;
    int btn_x = panel_x + (panel_w - btn_w) / 2;
    int btn_y = cy;
    info_copy_ps_btn_ = {btn_x, btn_y, btn_w, btn_h};

    bool hov = info_panel_anim_ >= 1.0f &&
               in_rect(mx, my, btn_x, btn_y, btn_w, btn_h);
    uint8_t bg = hov ? 70 : 50;
    SDL_SetRenderDrawColor(sdl_renderer_, bg, bg, bg + 8, alpha);
    SDL_Rect br = {btn_x, btn_y, btn_w, btn_h};
    SDL_RenderFillRect(sdl_renderer_, &br);
    SDL_SetRenderDrawColor(sdl_renderer_, 110, 110, 130,
                           (uint8_t)(180 * info_panel_anim_));
    SDL_RenderDrawRect(sdl_renderer_, &br);

    if (lab) {
        SDL_SetTextureColorMod(lab, hov ? 255 : 220, hov ? 255 : 220, hov ? 255 : 230);
        SDL_SetTextureAlphaMod(lab, text_alpha);
        SDL_Rect ldst = {btn_x + (btn_w - lw) / 2,
                         btn_y + (btn_h - lh) / 2, lw, lh};
        SDL_RenderCopy(sdl_renderer_, lab, nullptr, &ldst);
        SDL_DestroyTexture(lab);
    }
    cy += btn_h + line_gap;

    // Hover caption — only rendered when the button is hovered. Reuses
    // the slot reserved in the height calculation so the panel doesn't
    // reflow when it appears.
    if (hov) {
        int hw = 0, hh = 0;
        SDL_Texture* hint = make_text_texture(sdl_renderer_,
            "Paste into a PowerShell window to verify firewall rules and listening ports.",
            hint_label_h, 150, 150, 160, &hw, &hh);
        if (hint) {
            // Shrink-to-fit if the line is wider than the panel.
            int max_w = panel_w - pad * 2;
            int draw_w = hw, draw_h = hh;
            if (hw > max_w) {
                float s = (float)max_w / hw;
                draw_w = max_w;
                draw_h = (int)(hh * s);
            }
            SDL_SetTextureAlphaMod(hint, text_alpha);
            SDL_Rect hdst = {panel_x + (panel_w - draw_w) / 2, cy, draw_w, draw_h};
            SDL_RenderCopy(sdl_renderer_, hint, nullptr, &hdst);
            SDL_DestroyTexture(hint);
        }
    }
#else
    info_copy_ps_btn_ = {0, 0, 0, 0};
#endif

    // Footer block at bottom of info panel — clones the waiting-screen
    // footer (MSEndpointMgr link / Simon link / Buy-me-a-coffee link +
    // version) using info_footer_* textures baked at the same font size
    // as the network-requirement lines so the visual weight matches the
    // rest of the panel. Append to footer_hits_ so the shared click
    // dispatcher (extended to fire while info_panel_visible_) handles
    // link clicks.
    if (footer_block_h > 0) {
        // Re-clear stale hits when we're streaming, since draw_footer
        // didn't run this frame and footer_hits_ would otherwise contain
        // last-frame waiting-screen rects. On the waiting screen we
        // intentionally keep the bottom-of-screen hits and APPEND.
        if (ever_received_frame_) footer_hits_.clear();

        int foot_y = panel_y + total_h - pad - chk_btn_h - line_gap
                     - hint_label_h - spacer_h - footer_block_h;
        // "About" header (small, same size as "Network requirements")
        if (about_h > 0) {
            int aw = (int)(info_about_header_.w * text_scale);
            int ax = panel_x + (panel_w - aw) / 2;
            SDL_Rect adst = {ax, foot_y, aw, about_h};
            SDL_SetTextureAlphaMod(info_about_header_.tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, info_about_header_.tex, nullptr, &adst);
            foot_y += about_h + foot_gap;
        }
        auto draw_info_footer_line = [&](const std::vector<FooterSeg>& segs,
                                          int line_y, int line_h_px) {
            if (segs.empty()) return;
            int total_w = info_footer_line_w(segs);
            int fx = panel_x + (panel_w - total_w) / 2;
            for (auto& s : segs) {
                int sw = (int)(s.w * text_scale), sh = (int)(s.h * text_scale);
                SDL_Rect dst = {fx, line_y, sw, sh};
                if (s.tex) {
                    SDL_SetTextureAlphaMod(s.tex, text_alpha);
                    SDL_RenderCopy(sdl_renderer_, s.tex, nullptr, &dst);
                }
                if (!s.url.empty() || !s.tooltip.empty())
                    footer_hits_.push_back({fx, line_y, sw, sh, s.url, s.tooltip});
                fx += sw;
            }
            (void)line_h_px;
        };
        draw_info_footer_line(info_footer_line1_, foot_y, foot_h1);
        int y2 = foot_y + foot_h1 + foot_gap;
        draw_info_footer_line(info_footer_line2_, y2, foot_h2);
        int y3 = y2 + foot_h2 + foot_gap;
        draw_info_footer_line(info_footer_line3_, y3, foot_h3);

        // Hover tooltip for footer segments — same UX as draw_footer.
        if (info_panel_anim_ >= 1.0f) {
            int tmx, tmy;
            SDL_GetMouseState(&tmx, &tmy);
            std::string hovered;
            int hx = 0, hy = 0, hw = 0;
            for (auto& h : footer_hits_) {
                if (!h.tooltip.empty() && in_rect(tmx, tmy, h.x, h.y, h.w, h.h)) {
                    hovered = h.tooltip;
                    hx = h.x; hy = h.y; hw = h.w;
                    break;
                }
            }
            if (!hovered.empty() && tooltip_ready("infofooter:" + hovered)) {
                draw_bezel_tooltip(hovered, hx + hw / 2, hy);
            } else if (hovered.empty() &&
                       hover_key_.rfind("infofooter:", 0) == 0) {
                hover_key_.clear();
            }
        }
    }

    // "Check for updates" button — same visual recipe as the
    // "Copy network test script" button above. Sits at the very bottom
    // of the panel, with a hover hint slot reserved beneath it.
#ifdef _WIN32
    {
        bool busy = update_check_in_progress_.load();
        const char* chk_label = busy ? "Checking for updates\xE2\x80\xA6"
                                     : "Check for updates";
        int chk_lw = 0, chk_lh = 0;
        SDL_Texture* chk_lab = make_text_texture(sdl_renderer_,
            chk_label, chk_btn_label_h,
            220, 220, 230, &chk_lw, &chk_lh);
        int chk_btn_w = chk_lw + std::max(12, pad);
        if (chk_btn_w > panel_w - pad * 2) chk_btn_w = panel_w - pad * 2;
        int chk_btn_x = panel_x + (panel_w - chk_btn_w) / 2;
        // Anchor to bottom of the panel so the button is always pinned.
        int chk_btn_y = panel_y + total_h - pad - hint_label_h - line_gap - chk_btn_h;
        info_check_btn_ = {chk_btn_x, chk_btn_y, chk_btn_w, chk_btn_h};

        bool chk_hov = info_panel_anim_ >= 1.0f && !busy &&
                       in_rect(mx, my, chk_btn_x, chk_btn_y, chk_btn_w, chk_btn_h);
        uint8_t chk_bg = busy ? 40 : (chk_hov ? 70 : 50);
        SDL_SetRenderDrawColor(sdl_renderer_, chk_bg, chk_bg, chk_bg + 8, alpha);
        SDL_Rect chk_br = {chk_btn_x, chk_btn_y, chk_btn_w, chk_btn_h};
        SDL_RenderFillRect(sdl_renderer_, &chk_br);
        SDL_SetRenderDrawColor(sdl_renderer_, 110, 110, 130,
                               (uint8_t)(180 * info_panel_anim_));
        SDL_RenderDrawRect(sdl_renderer_, &chk_br);

        if (chk_lab) {
            uint8_t lr = busy ? 160 : (chk_hov ? 255 : 220);
            uint8_t lg = busy ? 160 : (chk_hov ? 255 : 220);
            uint8_t lb = busy ? 170 : (chk_hov ? 255 : 230);
            SDL_SetTextureColorMod(chk_lab, lr, lg, lb);
            SDL_SetTextureAlphaMod(chk_lab, text_alpha);
            SDL_Rect ldst = {chk_btn_x + (chk_btn_w - chk_lw) / 2,
                             chk_btn_y + (chk_btn_h - chk_lh) / 2,
                             chk_lw, chk_lh};
            SDL_RenderCopy(sdl_renderer_, chk_lab, nullptr, &ldst);
            SDL_DestroyTexture(chk_lab);
        }

        // Hover hint, mirrors the copy-ps button hint slot.
        if (chk_hov) {
            int hw = 0, hh = 0;
            SDL_Texture* hint = make_text_texture(sdl_renderer_,
                "Check GitHub for a newer release.",
                hint_label_h, 150, 150, 160, &hw, &hh);
            if (hint) {
                int max_w = panel_w - pad * 2;
                int draw_w = hw, draw_h = hh;
                if (hw > max_w) {
                    float s = (float)max_w / hw;
                    draw_w = max_w;
                    draw_h = (int)(hh * s);
                }
                SDL_SetTextureAlphaMod(hint, text_alpha);
                SDL_Rect hdst = {panel_x + (panel_w - draw_w) / 2,
                                 chk_btn_y + chk_btn_h + line_gap, draw_w, draw_h};
                SDL_RenderCopy(sdl_renderer_, hint, nullptr, &hdst);
                SDL_DestroyTexture(hint);
            }
        }
    }
#else
    info_check_btn_ = {0, 0, 0, 0};
#endif
}

void Renderer::draw_version_panel() {
    if (frame_dst_w_ == 0 || version_lines_.empty()) return;

    // Update animation
    const float anim_duration = 200.0f; // ms
    if (version_panel_animating_) {
        float elapsed = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - version_panel_anim_start_).count();
        float t = std::min(1.0f, elapsed / anim_duration);
        float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        if (version_panel_visible_)
            version_panel_anim_ = eased;
        else
            version_panel_anim_ = 1.0f - eased;
        if (t >= 1.0f) version_panel_animating_ = false;
    } else {
        version_panel_anim_ = version_panel_visible_ ? 1.0f : 0.0f;
    }

    if (version_panel_anim_ <= 0.0f) return;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);

    // UI dimensions are derived from a phone-equivalent reference width so
    // the Version panel stays compact on tablet/desktop sources.
    int uw = ui_ref_width();
    int pad = std::max(8, uw / 20);
    int line_gap = std::max(3, pad / 4);
    int spacer_h = std::max(4, pad / 2);

    // Panel width is computed up-front so the text-fit scaling below can
    // target the actual usable content width (panel_w - pad*2) rather than
    // a fixed fraction of ui_ref_width(). Without this the panel reserves
    // a lot of empty horizontal space and is forced to shrink the long
    // pre-wrapped description textures down even when there is plenty of
    // room to draw them at the default scale.
    int panel_w = (std::min)((int)(svw * 0.85f), (int)(uw * 1.6f));
    int panel_x = svx + (svw - panel_w) / 2;

    // Scale text to fit panel, but never shrink below a readable minimum.
    // If a line is still too wide at the minimum scale it will overflow the
    // clip rect (acceptable) — the scrollbar handles vertical overflow.
    float max_text_w = (float)std::max(40, panel_w - pad * 2);
    const float min_text_scale = 0.40f;
    float text_scale = 0.5f;
    for (auto& line : version_lines_) {
        if (line.tex && line.w * text_scale > max_text_w)
            text_scale = max_text_w / line.w;
    }
    if (text_scale < min_text_scale) text_scale = min_text_scale;

    // Compute total content height
    int content_h = pad;
    for (auto& line : version_lines_) {
        if (line.tex)
            content_h += (int)(line.h * text_scale) + line_gap;
        else
            content_h += spacer_h;
    }
    content_h += pad - line_gap;

    // Position below island bar
    int btn_sz = std::max(20, uw / 14);
    int ipad = btn_sz / 3;
    int island_bottom = svy + ipad + btn_sz + ipad * 2;
    int panel_y_target = island_bottom + pad / 2;

    // Animate: slide down from island, with fade
    int slide_offset = (int)((1.0f - version_panel_anim_) * content_h * 0.3f);
    int panel_y = panel_y_target - slide_offset;
    uint8_t alpha = (uint8_t)(240 * version_panel_anim_);
    uint8_t text_alpha = (uint8_t)(255 * version_panel_anim_);

    // Limit panel height to screen area
    int max_panel_h = svy + svh - panel_y - pad * 2;
    int panel_h = std::min(content_h, max_panel_h);
    bool needs_scroll = content_h > panel_h;

    // Clamp scroll offset
    if (needs_scroll) {
        int max_scroll = content_h - panel_h;
        if (version_scroll_offset_ < 0) version_scroll_offset_ = 0;
        if (version_scroll_offset_ > max_scroll) version_scroll_offset_ = max_scroll;
    } else {
        version_scroll_offset_ = 0;
    }

    version_panel_rect_ = {panel_x, panel_y, panel_w, panel_h};

    // Rounded rect background. BLENDMODE_NONE so the body + strips +
    // corner discs cannot alpha-compound at their overlap.
    int pr = std::max(6, pad / 2);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
    SDL_Rect body = {panel_x + pr, panel_y, panel_w - pr * 2, panel_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    SDL_Rect ls = {panel_x, panel_y + pr, pr, panel_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &ls);
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, panel_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + panel_h - pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + panel_h - pr, pr);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    // Set clip rect for scrolling content
    SDL_Rect clip = {panel_x, panel_y, panel_w, panel_h};
    SDL_RenderSetClipRect(sdl_renderer_, &clip);

    // Draw text lines centered, offset by scroll
    int cy = panel_y + pad - version_scroll_offset_;
    for (auto& line : version_lines_) {
        if (line.tex) {
            int dw = (int)(line.w * text_scale);
            int dh = (int)(line.h * text_scale);
            int lx = panel_x + (panel_w - dw) / 2;
            SDL_Rect dst = {lx, cy, dw, dh};
            SDL_SetTextureAlphaMod(line.tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, line.tex, nullptr, &dst);
            cy += dh + line_gap;
        } else {
            cy += spacer_h;
        }
    }

    SDL_RenderSetClipRect(sdl_renderer_, nullptr);

    // Draw scrollbar if needed
    if (needs_scroll) {
        int sb_w = std::max(3, pad / 4);
        int sb_x = panel_x + panel_w - sb_w - pr;
        int track_y = panel_y + pr;
        int track_h = panel_h - pr * 2;
        float visible_frac = (float)panel_h / content_h;
        int thumb_h = std::max(pr * 2, (int)(track_h * visible_frac));
        int max_scroll = content_h - panel_h;
        float scroll_frac = (max_scroll > 0) ? (float)version_scroll_offset_ / max_scroll : 0.0f;
        int thumb_y = track_y + (int)((track_h - thumb_h) * scroll_frac);

        // Stash geometry for the click/drag handlers in the event loop.
        version_sb_x_          = sb_x;
        version_sb_w_          = sb_w;
        version_sb_track_y_    = track_y;
        version_sb_track_h_    = track_h;
        version_sb_thumb_h_    = thumb_h;
        version_sb_max_scroll_ = max_scroll;

        SDL_SetRenderDrawColor(sdl_renderer_, 80, 80, 90, (uint8_t)(180 * version_panel_anim_));
        SDL_Rect thumb = {sb_x, thumb_y, sb_w, thumb_h};
        SDL_RenderFillRect(sdl_renderer_, &thumb);
    } else {
        version_sb_track_h_    = 0;
        version_sb_max_scroll_ = 0;
    }
}

// ----------------------------------------------------------------------------
// Settings panel — bezel colour swatches + screenshot toggles
// ----------------------------------------------------------------------------

void Renderer::drawer_color(uint8_t& r, uint8_t& g, uint8_t& b) const {
    // Derive the drawer / sub-panel colour from the bezel: a darker shade
    // of the same hue, clamped so log text remains legible.
    int br = phone_frame_.bezel_r();
    int bg = phone_frame_.bezel_g();
    int bb = phone_frame_.bezel_b();
    int dr = br - 8, dg = bg - 8, db = bb - 8;
    if (dr < 18) dr = 18; if (dr > 40) dr = 40;
    if (dg < 18) dg = 18; if (dg > 40) dg = 40;
    if (db < 22) db = 22; if (db > 45) db = 45;
    r = (uint8_t)dr; g = (uint8_t)dg; b = (uint8_t)db;
}

void Renderer::apply_bezel_color(uint8_t r, uint8_t g, uint8_t b) {
    settings_.bezel_r = r;
    settings_.bezel_g = g;
    settings_.bezel_b = b;
    settings_.save();
    // PhoneFrame::set_bezel_color invalidates its own texture/pixel caches.
    // We must immediately re-generate it for the current screen size, otherwise
    // is_generated() stays false until the next video frame arrives — which
    // would leave the waiting screen blank for a user who is not yet connected.
    phone_frame_.set_bezel_color(r, g, b);
    int gw = (tex_width_  > 0) ? tex_width_  : 390;
    int gh = (tex_height_ > 0) ? tex_height_ : 844;
    phone_frame_.generate(sdl_renderer_, gw, gh);
}

// ----------------------------------------------------------------------------
// Panel kit shared by Settings and Add Android (approved mockup, 15.11.2026):
// double-bezel shell, switches, segmented choices, fields, pill buttons.
// ----------------------------------------------------------------------------

float Renderer::ui_spring_(const std::string& key, float target) {
    UiSpring& sp = ui_springs_[key];
    uint32_t now = SDL_GetTicks();
    if (!sp.init) { sp.init = true; sp.x = target; sp.v = 0.0f; sp.t = now; return target; }
    float dt = std::min(0.033f, (now - sp.t) / 1000.0f);
    sp.t = now;
    // Slightly under-damped: settles in ~0.4 s with a small overshoot.
    const float k = 220.0f, c = 22.0f;
    sp.v += (k * (target - sp.x) - c * sp.v) * dt;
    sp.x += sp.v * dt;
    if (std::abs(target - sp.x) < 0.002f && std::abs(sp.v) < 0.02f) { sp.x = target; sp.v = 0.0f; }
    return sp.x;
}

void Renderer::ui_shell_(int x, int y, int w, int h, int rad) {
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_renderer_, 44, 48, 56, 255);       // hairline
    fill_round_rect(sdl_renderer_, x - 7, y - 7, w + 14, h + 14, rad + 7);
    SDL_SetRenderDrawColor(sdl_renderer_, 27, 30, 37, 255);       // outer shell
    fill_round_rect(sdl_renderer_, x - 6, y - 6, w + 12, h + 12, rad + 6);
    SDL_SetRenderDrawColor(sdl_renderer_, 22, 25, 31, 255);       // inner core
    fill_round_rect(sdl_renderer_, x, y, w, h, rad);
    SDL_SetRenderDrawColor(sdl_renderer_, 46, 50, 60, 255);       // top highlight
    SDL_RenderDrawLine(sdl_renderer_, x + rad, y, x + w - 1 - rad, y);
}

void Renderer::ui_round_btn_(const BtnRect& b, wchar_t icon, bool hover) {
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, hover ? 30 : 16);
    fill_circle(sdl_renderer_, b.x + b.w / 2, b.y + b.h / 2, b.w / 2);
    draw_glyph(sdl_renderer_, icon, b.x + b.w / 2, b.y + b.h / 2, b.w / 2,
               201, 209, 220, hover ? 255 : 220);
}

void Renderer::ui_toggle_(int x, int y, int h, bool on, const std::string& key) {
    const Accent& ac = accent_for(settings_.accent_color);
    int w = h * 12 / 7;
    float t = ui_spring_(key, on ? 1.0f : 0.0f);
    float tc = std::clamp(t, 0.0f, 1.0f);
    auto mix = [&](int a, int b) { return (uint8_t)(a + (b - a) * tc); };
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_renderer_, mix(40, ac.lr), mix(44, ac.lg), mix(52, ac.lb), 255);
    fill_round_rect(sdl_renderer_, x, y, w, h, h / 2);
    SDL_SetRenderDrawColor(sdl_renderer_, mix(11, ac.r), mix(13, ac.g), mix(18, ac.b), 255);
    fill_round_rect(sdl_renderer_, x + 1, y + 1, w - 2, h - 2, (h - 2) / 2);
    int d = h - 6;
    int kx = x + 3 + (int)std::lround(tc * (w - 6 - d)) + d / 2, ky = y + h / 2;
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 90);
    fill_circle(sdl_renderer_, kx, ky + 1, d / 2 + 1);
    SDL_SetRenderDrawColor(sdl_renderer_, 242, 244, 247, 255);
    fill_circle(sdl_renderer_, kx, ky, d / 2);
}

void Renderer::ui_segment_(int x, int y, int w, int h, const char* const* opts, int n,
                           int selected, const std::string& key, BtnRect* out,
                           int font_px, uint8_t alpha) {
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_renderer_, 34, 38, 46, 255);
    fill_round_rect(sdl_renderer_, x, y, w, h, h / 2);
    SDL_SetRenderDrawColor(sdl_renderer_, 11, 13, 18, 255);
    fill_round_rect(sdl_renderer_, x + 1, y + 1, w - 2, h - 2, (h - 2) / 2);
    int seg_w = (w - 6) / n, ph = h - 6;
    for (int i = 0; i < n; ++i) {
        out[i] = {x + 3 + i * seg_w, y, seg_w, h};
        if (i != selected && in_rect(mx, my, out[i].x, y, seg_w, h)) {
            SDL_SetRenderDrawColor(sdl_renderer_, 22, 25, 31, 255);
            fill_round_rect(sdl_renderer_, out[i].x, y + 3, seg_w, ph, ph / 2);
        }
    }
    if (selected >= 0) {
        float pos = std::clamp(ui_spring_(key, (float)selected), 0.0f, (float)(n - 1));
        int px = x + 3 + (int)std::lround(pos * seg_w);
        SDL_SetRenderDrawColor(sdl_renderer_, 52, 58, 71, 255);
        fill_round_rect(sdl_renderer_, px, y + 3, seg_w, ph, ph / 2);
        SDL_SetRenderDrawColor(sdl_renderer_, 76, 82, 96, 255);
        SDL_RenderDrawLine(sdl_renderer_, px + ph / 2, y + 3, px + seg_w - 1 - ph / 2, y + 3);
    }
    for (int i = 0; i < n; ++i) {
        bool sel = (i == selected);
        draw_text_mid(sdl_renderer_, opts[i], font_px, sel ? 255 : 138, sel ? 255 : 148,
                      sel ? 255 : 166, alpha, out[i].x + seg_w / 2, y + h / 2, 1, 600);
    }
}

void Renderer::ui_field_(const BtnRect& r, const std::string& val, const char* placeholder,
                         bool focused, uint8_t alpha) {
    const Accent& ac = accent_for(settings_.accent_color);
    int rad = std::min(14, r.h / 3);
    if (focused) {
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl_renderer_, ac.r, ac.g, ac.b, 46);
        fill_round_rect(sdl_renderer_, r.x - 4, r.y - 4, r.w + 8, r.h + 8, rad + 4);
    }
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    if (focused) SDL_SetRenderDrawColor(sdl_renderer_, ac.lr, ac.lg, ac.lb, 255);
    else         SDL_SetRenderDrawColor(sdl_renderer_, 34, 38, 46, 255);
    fill_round_rect(sdl_renderer_, r.x - 1, r.y - 1, r.w + 2, r.h + 2, rad + 1);
    SDL_SetRenderDrawColor(sdl_renderer_, 11, 13, 18, 255);
    fill_round_rect(sdl_renderer_, r.x, r.y, r.w, r.h, rad);

    int font = std::max(11, r.h * 42 / 100);
    bool ph = val.empty();
    const std::string& shown = ph ? std::string(placeholder) : val;
    int tx = r.x + std::max(10, r.h / 3);
    if (ph) draw_text_mid(sdl_renderer_, shown, font, 85, 96, 116, alpha, tx, r.y + r.h / 2, 0);
    else    draw_text_mid(sdl_renderer_, shown, font, 244, 246, 248, alpha, tx, r.y + r.h / 2, 0);
    if (focused && (SDL_GetTicks() / 500) % 2 == 0) {
        int tw = 0, th = 0;
        if (!ph) measure_text_c(sdl_renderer_, val, font, &tw, &th);
        SDL_SetRenderDrawColor(sdl_renderer_, ac.lr, ac.lg, ac.lb, 255);
        SDL_Rect cur = {tx + tw + 1, r.y + r.h / 4, 2, r.h / 2};
        SDL_RenderFillRect(sdl_renderer_, &cur);
    }
}

void Renderer::ui_pill_(const BtnRect& r, const char* label, wchar_t icon, bool primary,
                        bool hover, uint8_t alpha) {
    const Accent& ac = accent_for(settings_.accent_color);
    int rad = r.h / 2;
    int font = std::max(11, r.h * 36 / 100);
    if (primary) {
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
        if (hover) SDL_SetRenderDrawColor(sdl_renderer_, ac.lr, ac.lg, ac.lb, 255);
        else       SDL_SetRenderDrawColor(sdl_renderer_, ac.r, ac.g, ac.b, 255);
        fill_round_rect(sdl_renderer_, r.x, r.y, r.w, r.h, rad);
        SDL_SetRenderDrawColor(sdl_renderer_, ac.lr, ac.lg, ac.lb, 255);
        SDL_RenderDrawLine(sdl_renderer_, r.x + rad, r.y, r.x + r.w - 1 - rad, r.y);
        uint8_t ink = ac.dark_ink ? 14 : 255;
        draw_text_mid(sdl_renderer_, label, font, ink, ink, ink, alpha, r.x + r.h * 6 / 10,
                      r.y + r.h / 2, 0, 700);
        // Trailing arrow in its own circle; nudges right on hover.
        int d = r.h - 12, ccx = r.x + r.w - 6 - d / 2, ccy = r.y + r.h / 2;
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl_renderer_, ac.dark_ink ? 0 : 255, ac.dark_ink ? 0 : 255,
                               ac.dark_ink ? 0 : 255, ac.dark_ink ? 36 : 46);
        fill_circle(sdl_renderer_, ccx, ccy, d / 2);
        draw_glyph(sdl_renderer_, lucide::ArrowRight, ccx + (hover ? 2 : 0), ccy, d * 11 / 20,
                   ink, ink, ink, alpha);
        return;
    }
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_renderer_, 44, 48, 56, 255);
    fill_round_rect(sdl_renderer_, r.x, r.y, r.w, r.h, rad);
    if (hover) SDL_SetRenderDrawColor(sdl_renderer_, 40, 44, 53, 255);
    else       SDL_SetRenderDrawColor(sdl_renderer_, 30, 33, 40, 255);
    fill_round_rect(sdl_renderer_, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad - 1);
    int tw = 0, th = 0;
    measure_text_c(sdl_renderer_, label, font, &tw, &th, 600);
    int ipx = icon ? font * 5 / 4 : 0, igap = icon ? font / 2 : 0;
    int x0 = r.x + (r.w - (ipx + igap + tw)) / 2;
    if (icon) draw_glyph(sdl_renderer_, icon, x0 + ipx / 2, r.y + r.h / 2, ipx, 230, 234, 240, alpha);
    draw_text_mid(sdl_renderer_, label, font, 230, 234, 240, alpha, x0 + ipx + igap,
                  r.y + r.h / 2, 0, 600);
}

// Cable setup guide: what to switch on so a plugged-in phone can be
// mirrored. Opens by itself when the USB watch sees a phone adb cannot use
// yet, and from the "?" in the Add Android panel. Steps come from the
// vendors' / Android's own help pages (see the 3.5.0 design doc).
void Renderer::draw_setup_guide_() {
    if (frame_dst_w_ == 0) return;
    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);

    int uw = ui_ref_width();
    int pad     = std::max(12, uw / 16);
    int gap     = std::max(8,  pad * 2 / 3);
    int title_h = std::max(15, uw / 20);
    int label_h = std::max(11, uw / 30);
    int note_h  = std::max(10, label_h * 4 / 5);
    int row_pad = std::max(8,  label_h * 3 / 4);
    int tabs_h  = label_h + row_pad * 2 - 2;
    int line_h  = label_h * 135 / 100;
    int panel_w = (std::min)((int)(svw * 0.9f), (int)(uw * 1.7f));
    int panel_x = svx + (svw - panel_w) / 2;
    int btn_sz  = std::max(20, uw / 14);
    int ipad    = btn_sz / 3;
    int panel_y = svy + ipad + btn_sz + ipad * 2 + pad / 2;
    int inner   = std::max(10, pad * 3 / 4);
    int pr      = std::max(14, pad);
    const uint8_t alpha = 255;

    const char* const kTabs[4] = {"Samsung", "Xiaomi", "OPPO", tr("Other")};
    const std::vector<std::vector<std::string>> kSteps = {
        {"Settings > About phone > Software information",
         tr("Tap Build number 7 times, then enter your PIN"),
         tr("Settings > Developer options > USB debugging: On")},
        {"Settings > About phone",
         tr("Tap OS version 7 times"),
         "Settings > Additional settings > Developer options",
         tr("Turn on USB debugging and USB debugging (Security settings), then restart the phone")},
        {"Settings > About device > Version",
         tr("Tap Build number 7 times"),
         tr("Settings > Additional settings > Developer options > USB debugging: On")},
        {"Settings > About phone",
         tr("Tap Build number 7 times"),
         tr("Settings > System > Developer options > USB debugging: On")},
    };
    const int hint = setup_hint_.load();
    const char* status =
        hint == 1 ? tr("Phone found. Turn on USB debugging:") :
        hint == 2 ? tr("Unlock the phone and tap Allow (tick Always allow).") :
        hint == 3 ? tr("iPhone needs no cable: Control Center > Screen Mirroring > PhoneMimi.") :
                    tr("Turn on USB debugging, then plug in the cable.");
    const bool show_steps = hint == 0 || hint == 1;
    setup_guide_tab_ = std::clamp(setup_guide_tab_, 0, 3);

    // Greedy word wrap with the real font metrics.
    auto wrap = [&](const std::string& text, int px, int maxw, int weight) {
        std::vector<std::string> lines;
        std::string cur, word;
        auto fits = [&](const std::string& t) {
            int w = 0, h = 0;
            measure_text_c(sdl_renderer_, t, px, &w, &h, weight);
            return w <= maxw;
        };
        size_t i = 0;
        while (i <= text.size()) {
            size_t sp = text.find(' ', i);
            if (sp == std::string::npos) sp = text.size();
            word = text.substr(i, sp - i);
            std::string cand = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && !fits(cand)) { lines.push_back(cur); cur = word; }
            else cur = cand;
            i = sp + 1;
        }
        if (!cur.empty()) lines.push_back(cur);
        return lines;
    };

    const int content_w = panel_w - pad * 2;
    const int num_d = label_h + 8;
    const int step_text_w = content_w - inner * 2 - num_d - gap;
    auto status_lines = wrap(status, label_h, content_w, hint == 2 ? 700 : 500);
    std::vector<std::vector<std::string>> step_lines;
    int card_h = 0;
    if (show_steps) {
        for (auto& st : kSteps[setup_guide_tab_]) {
            step_lines.push_back(wrap(st, label_h, step_text_w, 500));
            card_h += std::max(num_d, (int)step_lines.back().size() * line_h) + row_pad * 2;
        }
    }
    int total_h = pad + title_h + gap + (int)status_lines.size() * line_h + gap +
                  (show_steps ? tabs_h + gap + card_h + gap + note_h : 0) + pad;
    setup_guide_rect_ = {panel_x, panel_y, panel_w, total_h};

    int mx, my;
    SDL_GetMouseState(&mx, &my);
    const Accent& ac = accent_for(settings_.accent_color);
    auto surface = [&](int x, int y, int w, int h, int rad, uint8_t r, uint8_t g, uint8_t b) {
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
        fill_round_rect(sdl_renderer_, x, y, w, h, rad);
    };

    ui_shell_(panel_x, panel_y, panel_w, total_h, pr);
    int cy = panel_y + pad;

    // Header: accent tile, title, close.
    {
        int tile = title_h + 10, mid = cy + title_h / 2;
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl_renderer_, ac.r, ac.g, ac.b, 42);
        fill_round_rect(sdl_renderer_, panel_x + pad, mid - tile / 2, tile, tile, tile / 3);
        draw_glyph(sdl_renderer_, lucide::Usb, panel_x + pad + tile / 2, mid, tile / 2,
                   ac.lr, ac.lg, ac.lb, alpha);
        int sz = title_h + 8;
        setup_guide_close_btn_ = {panel_x + panel_w - pad - sz, mid - sz / 2, sz, sz};
        int title_x = panel_x + pad + tile + tile / 3;
        const char* title = tr("Connect with a cable");
        draw_text_mid(sdl_renderer_, title,
                      fit_text_px(sdl_renderer_, title, title_h, setup_guide_close_btn_.x - 6 - title_x, 700),
                      244, 246, 248, alpha, title_x, mid, 0, 700);
        ui_round_btn_(setup_guide_close_btn_, lucide::X,
                      in_rect(mx, my, setup_guide_close_btn_.x, setup_guide_close_btn_.y, sz, sz));
    }
    cy += title_h + gap;

    for (auto& l : status_lines) {
        if (hint == 2)
            draw_text_mid(sdl_renderer_, l, label_h, ac.lr, ac.lg, ac.lb, alpha, panel_x + pad, cy + line_h / 2, 0, 700);
        else
            draw_text_mid(sdl_renderer_, l, label_h, 230, 234, 240, alpha, panel_x + pad, cy + line_h / 2, 0, 500);
        cy += line_h;
    }
    cy += gap;

    for (auto& b : setup_guide_tab_btns_) b = {};
    if (!show_steps) return;

    ui_segment_(panel_x + pad, cy, content_w, tabs_h, kTabs, 4, setup_guide_tab_,
                "guide.tabs", setup_guide_tab_btns_, label_h, alpha);
    cy += tabs_h + gap;

    // Recessed card with numbered steps.
    const int card_r = std::max(10, pr * 3 / 4);
    surface(panel_x + pad - 1, cy - 1, content_w + 2, card_h + 2, card_r + 1, 30, 33, 40);
    surface(panel_x + pad, cy, content_w, card_h, card_r, 17, 19, 24);
    for (size_t k = 0; k < step_lines.size(); ++k) {
        int rh = std::max(num_d, (int)step_lines[k].size() * line_h) + row_pad * 2;
        int ncx = panel_x + pad + inner + num_d / 2, ncy = cy + row_pad + num_d / 2;
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl_renderer_, ac.r, ac.g, ac.b, 255);
        fill_circle(sdl_renderer_, ncx, ncy, num_d / 2);
        uint8_t ink = ac.dark_ink ? 14 : 255;
        draw_text_mid(sdl_renderer_, std::to_string(k + 1), label_h * 9 / 10, ink, ink, ink, alpha,
                      ncx, ncy, 1, 700);
        int tx = panel_x + pad + inner + num_d + gap;
        int ty = cy + row_pad + (step_lines[k].size() == 1 ? (num_d - line_h) / 2 : 0);
        for (auto& l : step_lines[k]) {
            draw_text_mid(sdl_renderer_, l, label_h, 230, 234, 240, alpha, tx, ty + line_h / 2, 0, 500);
            ty += line_h;
        }
        cy += rh;
        if (k + 1 < step_lines.size()) {
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(sdl_renderer_, 30, 33, 40, 255);
            SDL_RenderDrawLine(sdl_renderer_, tx, cy, panel_x + pad + content_w - inner, cy);
        }
    }
    cy += gap;
    draw_text_mid(sdl_renderer_, tr("Then plug in the cable and tap Allow on the phone."),
                  fit_text_px(sdl_renderer_, tr("Then plug in the cable and tap Allow on the phone."), note_h, content_w, 500),
                  150, 154, 162, alpha, panel_x + pad, cy + note_h / 2, 0, 500);
}

// First-connect tips: three short looping demo clips in the lower half of
// the phone screen; arrows and dots switch, X closes. Shown once.
void Renderer::draw_tips_() {
    if (frame_dst_w_ == 0) return;
    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);
    int uw = ui_ref_width();
    int pad = std::max(12, uw / 16), gap = std::max(8, pad * 2 / 3);
    int label_h = std::max(11, uw / 30);
    int pr = std::max(14, pad);
    const Accent& ac = accent_for(settings_.accent_color);
    const char* const titles[3] = {tr("Mouse and keyboard"), tr("Clipboard sync"), tr("Drag and drop files")};
    tips_index_ = (tips_index_ % 3 + 3) % 3;

    // Edge to edge over the lower half of the screen; the bezel drawn
    // afterwards rounds the bottom corners like the screen itself.
    int panel_x = svx, panel_w = svw;
    int panel_h = svh / 2;
    int panel_y = svy + svh - panel_h;
    tips_rect_ = {panel_x, panel_y, panel_w, panel_h};
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    ui_shell_(panel_x, panel_y, panel_w, panel_h, pr);

    // Header: title of the clip on screen + close.
    int hh = label_h + 14;
    int cy = panel_y + pad * 2 / 3;
    tips_close_btn_ = {panel_x + panel_w - pad * 2 / 3 - hh, cy, hh, hh};
    draw_text_mid(sdl_renderer_, titles[tips_index_],
                  fit_text_px(sdl_renderer_, titles[tips_index_], label_h + 2,
                              tips_close_btn_.x - panel_x - pad * 2, 700),
                  244, 246, 248, 255, panel_x + pad, cy + hh / 2, 0, 700);
    ui_round_btn_(tips_close_btn_, lucide::X,
                  in_rect(mx, my, tips_close_btn_.x, tips_close_btn_.y, hh, hh));
    cy += hh + gap / 2;

    // What the clip shows, under the title (at most two lines).
    {
        const char* const descs[3] = {
            tr("Click and scroll like touch. Type on the PC keyboard."),
            tr("Copy content on the phone or the PC, paste it on the other."),
            tr("Drop any file: APKs install, others go to Download.")};
        const int note_h = std::max(10, label_h * 9 / 10);
        const int line_h = note_h * 135 / 100;
        const int maxw = panel_w - pad * 2;
        std::vector<std::string> lines;
        std::string cur, text = descs[tips_index_];
        size_t i = 0;
        while (i <= text.size()) {
            size_t sp = text.find(' ', i);
            if (sp == std::string::npos) sp = text.size();
            std::string word = text.substr(i, sp - i);
            std::string cand = cur.empty() ? word : cur + " " + word;
            int w = 0, h = 0;
            measure_text_c(sdl_renderer_, cand, note_h, &w, &h, 500);
            if (!cur.empty() && w > maxw) { lines.push_back(cur); cur = word; }
            else cur = cand;
            i = sp + 1;
        }
        if (!cur.empty()) lines.push_back(cur);
        if (lines.size() > 2) lines.resize(2);
        for (auto& l : lines) {
            draw_text_mid(sdl_renderer_, l, note_h, 172, 178, 190, 255, panel_x + pad, cy + line_h / 2, 0, 500);
            cy += line_h;
        }
        cy += gap / 2;
    }

    // Video, arrows either side, dots under it.
    const int dots_h = std::max(10, label_h);
    const int as = label_h + 14;
    int video_h = panel_y + panel_h - pad * 2 / 3 - dots_h - gap / 2 - cy;
    int video_w = video_h * 720 / 800;
    int max_w = panel_w - 2 * (as + pad / 2 + 6);
    if (video_w > max_w) { video_w = max_w; video_h = video_w * 800 / 720; }
    int vx = panel_x + (panel_w - video_w) / 2;

    if (tips_open_index_ != tips_index_) {
        char* base = SDL_GetBasePath();
        std::string path = std::string(base ? base : "") + "tips\\tip" + std::to_string(tips_index_ + 1) + ".mp4";
        if (base) SDL_free(base);
        tips_player_.open(path);
        tips_open_index_ = tips_index_;
        tips_start_ = std::chrono::steady_clock::now();
    }
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - tips_start_).count();
    if (tips_player_.is_open() && tips_player_.frame_at(t, &tips_rgba_)) {
        int w = tips_player_.width(), h = tips_player_.height();
        int tw = 0, th = 0;
        if (tips_tex_) SDL_QueryTexture(tips_tex_, nullptr, nullptr, &tw, &th);
        if (!tips_tex_ || tw != w || th != h) {
            if (tips_tex_) SDL_DestroyTexture(tips_tex_);
            tips_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
            if (tips_tex_) SDL_SetTextureScaleMode(tips_tex_, SDL_ScaleModeLinear);
        }
        if (tips_tex_) SDL_UpdateTexture(tips_tex_, nullptr, tips_rgba_.data(), w * 4);
        ++tips_serial_;
    }
    if (tips_tex_ && tips_player_.is_open()) {
        SDL_Rect d = {vx, cy, video_w, video_h};
        SDL_RenderCopy(sdl_renderer_,
                       mip_for_(tips_mip_, tips_tex_, tips_player_.width(), tips_player_.height(),
                                tips_serial_, d.w, d.h, /*supersample=*/true),
                       nullptr, &d);
    }
    int ay = cy + video_h / 2 - as / 2;
    tips_prev_btn_ = {panel_x + pad / 2, ay, as, as};
    tips_next_btn_ = {panel_x + panel_w - pad / 2 - as, ay, as, as};
    ui_round_btn_(tips_prev_btn_, 0xE072, in_rect(mx, my, tips_prev_btn_.x, tips_prev_btn_.y, as, as));
    ui_round_btn_(tips_next_btn_, 0xE073, in_rect(mx, my, tips_next_btn_.x, tips_next_btn_.y, as, as));

    int dot_r = std::max(3, dots_h / 3), dot_gap = dot_r * 4;
    int dcy = cy + video_h + gap / 2 + dots_h / 2;
    int dx0 = panel_x + panel_w / 2 - dot_gap;
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 3; ++i) {
        int dcx = dx0 + i * dot_gap;
        if (i == tips_index_) SDL_SetRenderDrawColor(sdl_renderer_, ac.lr, ac.lg, ac.lb, 255);
        else SDL_SetRenderDrawColor(sdl_renderer_, 120, 128, 142, 255);
        fill_circle(sdl_renderer_, dcx, dcy, i == tips_index_ ? dot_r + 1 : dot_r);
        tips_dot_btns_[i] = {dcx - dot_gap / 2, dcy - dots_h, dot_gap, dots_h * 2};
    }
}

void Renderer::draw_settings_panel() {
    if (frame_dst_w_ == 0) return;

    // Update animation
    const float anim_duration = 200.0f;
    if (settings_panel_animating_) {
        float elapsed = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - settings_panel_anim_start_).count();
        float t = std::min(1.0f, elapsed / anim_duration);
        float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        settings_panel_anim_ = settings_panel_visible_ ? eased : (1.0f - eased);
        if (t >= 1.0f) settings_panel_animating_ = false;
    } else {
        settings_panel_anim_ = settings_panel_visible_ ? 1.0f : 0.0f;
    }
    if (settings_panel_anim_ <= 0.0f) return;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);

    // UI dimensions are derived from a phone-equivalent reference width so
    // the panel stays compact on tablet/desktop sources.
    int uw = ui_ref_width();
    int pad     = std::max(12, uw / 16);
    int gap     = std::max(8,  pad * 2 / 3);
    int title_h = std::max(15, uw / 20);
    int label_h = std::max(11, uw / 30);
    int note_h  = std::max(10, label_h * 4 / 5);
    int row_pad = std::max(8,  label_h * 3 / 4);
    int row_h   = label_h + row_pad * 2;
    int tabs_h  = row_h - 2;

    int panel_w = (std::min)((int)(svw * 0.9f), (int)(uw * 1.7f));
    int panel_x = svx + (svw - panel_w) / 2;

    // Bottom of island row
    int btn_sz = std::max(20, uw / 14);
    int ipad = btn_sz / 3;
    int island_bottom = svy + ipad + btn_sz + ipad * 2;
    int panel_y_target = island_bottom + pad / 2;

    // Tabs: 0 General, 1 Phone, 2 Capture, 3 Webcam.
    settings_tab_ = std::clamp(settings_tab_, 0, 3);
    const int tab = settings_tab_;
    const bool resize_on = settings_.screenshot_resize_enabled;
#ifdef ENABLE_MCP
    const int phone_rows = settings_.mcp_enabled ? 12 : 11;
#else
    const int phone_rows = 10;
#endif
    // Phone: + the connected-phone row. Capture: + the two webcam rows.
    // Help: guide row, five tips, version/about row.
    const int rows = tab == 0 ? 5 : tab == 1 ? phone_rows + 1 : tab == 2 ? (resize_on ? 9 : 7) : 8;
    const int act_h = row_h;   // quick-action buttons above the tabs
    int total_h = pad + title_h + gap + act_h + gap + tabs_h + gap + rows * row_h
                + (tab == 1 ? gap + note_h : 0) + pad;

    // Slide animation
    int slide_offset = (int)((1.0f - settings_panel_anim_) * total_h * 0.3f);
    int panel_y = panel_y_target - slide_offset;
    uint8_t text_alpha = (uint8_t)(255 * settings_panel_anim_);

    settings_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    // Only the controls of the visible tab can be clicked.
    settings_swatch_btns_.clear();
    settings_toggle_save_btn_ = settings_toggle_clip_btn_ = settings_toggle_snagit_btn_ = {};
    settings_toggle_compname_btn_ = settings_toggle_aot_btn_ = settings_toggle_log_btn_ = {};
    settings_toggle_webcam_mirror_btn_ = settings_toggle_webcam_record_btn_ = {};
    settings_toggle_resize_btn_ = settings_toggle_save_original_btn_ = {};
    settings_resize_minus_btn_ = settings_resize_plus_btn_ = {};
    settings_fmt_mp4_btn_ = settings_fmt_gif_btn_ = {};
    settings_toggle_usb_btn_ = settings_toggle_softkbd_btn_ = {};
    settings_toggle_kbd_btn_ = settings_toggle_scroff_btn_ = settings_toggle_paste_btn_ = {};
    settings_toggle_mcp_btn_ = settings_mcp_copy_btn_ = {};
    settings_toggle_mutephone_btn_ = {};
    for (auto& b : settings_seg_lang_btns_) b = {};
    for (auto& b : settings_action_btns_) b = {};
    settings_dev_add_btn_ = settings_dev_disc_btn_ = settings_remote_btn_ = {};
    settings_help_guide_btn_ = settings_help_tips_btn_ = settings_help_ver_btn_ = settings_help_about_btn_ = {};
    for (auto& b : settings_seg_res_btns_) b = {};
    for (auto& b : settings_seg_fps_btns_) b = {};
    for (auto& b : settings_seg_ios_fps_btns_) b = {};

    int mx, my;
    SDL_GetMouseState(&mx, &my);

    const Accent& ac = accent_for(settings_.accent_color);

    // Surfaces are opaque (BLENDMODE_NONE) so the pieces a rounded rect is
    // built from cannot alpha-compound where they overlap.
    auto surface = [&](int x, int y, int w, int h, int rad, uint8_t r, uint8_t g, uint8_t b) {
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
        fill_round_rect(sdl_renderer_, x, y, w, h, rad);
    };
    // align: 0 left, 1 centred, 2 right (x is the anchor); y_mid is the
    // vertical centre of the text.
    auto text = [&](const std::string& s, int font_h, int x, int y_mid,
                    uint8_t r, uint8_t g, uint8_t b, int align, int weight = 500) {
        draw_text_mid(sdl_renderer_, s, font_h, r, g, b, text_alpha, x, y_mid, align, weight);
    };

    int pr = std::max(14, pad);
    ui_shell_(panel_x, panel_y, panel_w, total_h, pr);

    int cy = panel_y + pad;

    // Header: title + round close button.
    text(tr("Settings"), title_h, panel_x + pad, cy + title_h / 2, 244, 246, 248, 0, 700);
    {
        int sz = title_h + 8;
        settings_close_btn_ = {panel_x + panel_w - pad - sz, cy + title_h / 2 - sz / 2, sz, sz};
        ui_round_btn_(settings_close_btn_, lucide::X, in_rect(mx, my, settings_close_btn_.x,
                                                              settings_close_btn_.y, sz, sz));
    }
    cy += title_h + gap;

    // Quick actions: what used to be on the frame.
    {
        static const wchar_t kIcons[7] = {lucide::Camera, lucide::Circle, lucide::Folder,
                                          lucide::ScrollText, lucide::ChartColumn,
                                          lucide::Webcam, lucide::Power};
        const char* const kTips[7] = {tr("Screenshot (Ctrl+S)"), tr("Record (Ctrl+R)"),
                                             tr("Screenshots folder"), tr("Log (L)"), tr("Statistics (T)"),
                                             "Webcam (W)", tr("Quit PhoneMimi")};
        const int n = 7, content_w = panel_w - pad * 2;
        int sz = std::min(act_h, (content_w - (n - 1) * 6) / n);
        int sgap = n > 1 ? (content_w - n * sz) / (n - 1) : 0;
        for (int i = 0; i < n; ++i) {
            BtnRect b{panel_x + pad + i * (sz + sgap), cy + (act_h - sz) / 2, sz, sz};
            bool hov = in_rect(mx, my, b.x, b.y, b.w, b.h);
            wchar_t icon = kIcons[i];
            if (i == 1 && recorder_.is_recording()) icon = lucide::Square;
            ui_round_btn_(b, icon, hov);
            settings_action_btns_[i] = b;
            if (hov) {
                pending_tooltip_text_ = kTips[i];
                pending_tooltip_x_ = b.x + b.w / 2;
                pending_tooltip_y_ = b.y + b.h + 4;
                pending_tooltip_below_ = true;
            }
        }
    }
    cy += act_h + gap;

    // Tab bar: segmented track with a sliding pill.
    {
        const char* const names[4] = {tr("General"), tr("Phone"), tr("Capture"), tr("Help")};
        ui_segment_(panel_x + pad, cy, panel_w - pad * 2, tabs_h, names, 4, tab,
                    "settings.tabs", settings_tab_btns_, label_h, text_alpha);
    }
    cy += tabs_h + gap;

    // Recessed card holding the rows of the current tab.
    const int card_x = panel_x + pad, card_w = panel_w - pad * 2;
    const int inner = std::max(10, pad * 3 / 4);
    const int card_r = std::max(10, pr * 3 / 4);
    surface(card_x - 1, cy - 1, card_w + 2, rows * row_h + 2, card_r + 1, 30, 33, 40);
    surface(card_x, cy, card_w, rows * row_h, card_r, 17, 19, 24);

    int row_i = 0;
    // Starts a row (hover highlight). Returns the row's top y.
    auto begin_row = [&](bool hoverable) {
        int y = cy + row_i * row_h;
        if (hoverable && in_rect(mx, my, card_x, y, card_w, row_h))
            surface(card_x + 3, y + 3, card_w - 6, row_h - 6, std::max(4, card_r - 3), 24, 27, 33);
        ++row_i;
        return y;
    };
    // Label on the left, switch on the right; the whole row is clickable.
    // Row label, shrunk if needed so it never runs under the control.
    auto row_label = [&](const char* label, int y, int control_w) {
        int avail = card_w - inner * 3 - control_w;
        text(label, fit_text_px(sdl_renderer_, label, label_h, avail), card_x + inner,
             y + row_h / 2, 230, 234, 240, 0);
    };
    auto toggle = [&](const char* label, bool on) -> BtnRect {
        int y = begin_row(true);
        int sh = label_h + 8, sw = sh * 12 / 7;
        row_label(label, y, sw);
        ui_toggle_(card_x + card_w - inner - sw, y + (row_h - sh) / 2, sh, on,
                   std::string("settings.") + label);
        return BtnRect{card_x, y, card_w, row_h};
    };
    // Label on the left, a segmented choice on the right.
    auto segment = [&](const char* label, const char* const* opts, int n, int selected, BtnRect* out) {
        int y = begin_row(false);
        int ph = label_h + 10, pw = n * (label_h * 3 + 8) + 6;
        pw = std::max(n * label_h * 2, std::min(pw, card_w - inner * 3 - label_h * 5));
        row_label(label, y, pw);
        ui_segment_(card_x + card_w - inner - pw, y + (row_h - ph) / 2, pw, ph, opts, n, selected,
                    std::string("settings.") + label, out, label_h * 9 / 10, text_alpha);
    };
    // A row of round colour swatches; the selected one gets a `ring`.
    // Click ids: id_base + index (see the Settings click handler).
    auto swatches = [&](const char* label, const uint8_t (*cols)[3], int n, int selected,
                        int id_base, const uint8_t* ring) {
        int y = begin_row(false);
        // Swatch size from the room left after a short label (small windows).
        int room = card_w - inner * 3 - label_h * 5;
        int d = std::clamp((int)(room / (n + (n - 1) * 0.5f)), 12, label_h + 8);
        int sg = std::max(4, d / 2);
        row_label(label, y, n * d + (n - 1) * sg + 8);
        int x0 = card_x + card_w - inner - (n * d + (n - 1) * sg);
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
        for (int i = 0; i < n; ++i) {
            int scx = x0 + i * (d + sg) + d / 2, scy = y + row_h / 2;
            bool hov = in_rect(mx, my, scx - d / 2, scy - d / 2, d, d);
            int rr = d / 2 + (hov ? 1 : 0);
            if (i == selected) {
                SDL_SetRenderDrawColor(sdl_renderer_, ring[0], ring[1], ring[2], 255);
                fill_circle(sdl_renderer_, scx, scy, rr + 4);
                SDL_SetRenderDrawColor(sdl_renderer_, 17, 19, 24, 255);
                fill_circle(sdl_renderer_, scx, scy, rr + 2);
            } else {
                SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 30);
                fill_circle(sdl_renderer_, scx, scy, rr + 1);
            }
            SDL_SetRenderDrawColor(sdl_renderer_, cols[i][0], cols[i][1], cols[i][2], 255);
            fill_circle(sdl_renderer_, scx, scy, rr);
            settings_swatch_btns_.push_back({id_base + i, BtnRect{scx - d / 2, scy - d / 2, d, d}});
        }
    };
    auto index_of = [](const int* vals, int n, int v) {
        for (int i = 0; i < n; ++i) if (vals[i] == v) return i;
        return -1;
    };

    if (tab == 0) {
        // Frame colour and accent colour swatches.
        static const uint8_t presets[6][3] = {
            { 28,  28,  30},  // Dark titanium (default)
            { 90,  90,  95},  // Graphite
            { 36,  46,  72},  // Midnight blue
            { 36,  60,  44},  // Forest green
            { 80,  34,  34},  // Deep red
            {110,  84,  46},  // Bronze
        };
        int frame_sel = -1;
        for (int i = 0; i < 6; ++i)
            if (presets[i][0] == settings_.bezel_r && presets[i][1] == settings_.bezel_g &&
                presets[i][2] == settings_.bezel_b) frame_sel = i;
        const uint8_t ring[3] = {ac.lr, ac.lg, ac.lb};
        swatches(tr("Frame colour"), presets, 6, frame_sel, 0, ring);
        uint8_t acc_cols[6][3];
        for (int i = 0; i < 6; ++i) {
            acc_cols[i][0] = kAccents[i].r; acc_cols[i][1] = kAccents[i].g; acc_cols[i][2] = kAccents[i].b;
        }
        swatches(tr("Accent colour"), acc_cols, 6, settings_.accent_color, 100, ring);
        {
            const char* const langs[2] = {"English", "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t"};
            segment(tr("Language"), langs, 2, settings_.language, settings_seg_lang_btns_);
        }
        settings_toggle_compname_btn_ = toggle(tr("Computer name as receiver name"), settings_.use_computer_name);
        settings_toggle_log_btn_      = toggle(tr("Save a log file"), log_to_file_session_);
    } else if (tab == 1) {
        {
            // The phone on screen, with Add / Disconnect.
            int y = begin_row(false);
            std::string dev_name;
            settings_dev_id_.clear();
            if (get_sources_fn_)
                for (auto& s : get_sources_fn_())
                    if (s.active) { dev_name = s.name; settings_dev_id_ = s.id; }
            int ph = label_h + 10, pw = label_h * 5;
            BtnRect add{card_x + card_w - inner - pw, y + (row_h - ph) / 2, pw, ph};
            ui_pill_(add, tr("Add"), lucide::Plus, false, in_rect(mx, my, add.x, add.y, add.w, add.h), text_alpha);
            settings_dev_add_btn_ = add;
            int right = add.x - 6;
            if (!settings_dev_id_.empty() && disconnect_source_fn_) {
                int dw = label_h * 7;
                BtnRect dc{add.x - 6 - dw, add.y, dw, ph};
                ui_pill_(dc, tr("Disconnect"), 0, false, in_rect(mx, my, dc.x, dc.y, dc.w, dc.h), text_alpha);
                settings_dev_disc_btn_ = dc;
                right = dc.x - 6;
            }
            std::string label = dev_name.empty() ? tr("No phone connected") : dev_name;
            text(label, fit_text_px(sdl_renderer_, label, label_h, right - card_x - inner * 2), card_x + inner,
                 y + row_h / 2, dev_name.empty() ? 150 : 230, dev_name.empty() ? 154 : 234,
                 dev_name.empty() ? 162 : 240, 0);
        }
        {
            // Remote access: fixed adb port + Tailscale address, connect from anywhere later.
            int y = begin_row(false);
            bool busy = android_remote_busy_.load();
            int ph = label_h + 10, pw = label_h * 6;
            BtnRect pill{card_x + card_w - inner - pw, y + (row_h - ph) / 2, pw, ph};
            ui_pill_(pill, busy ? tr("Working...") : tr("Enable"), lucide::Wifi, false,
                     !busy && in_rect(mx, my, pill.x, pill.y, pill.w, pill.h), text_alpha);
            settings_remote_btn_ = pill;
            std::string label = tr("Remote access");
            if (!settings_.android_last_wifi.empty()) label += "  " + settings_.android_last_wifi;
            text(label, fit_text_px(sdl_renderer_, label, label_h, pill.x - 6 - card_x - inner * 2),
                 card_x + inner, y + row_h / 2, 230, 234, 240, 0);
        }
        settings_toggle_usb_btn_     = toggle(tr("Auto-connect over USB"), settings_.android_auto_usb);
        settings_toggle_kbd_btn_     = toggle(tr("Type from this keyboard"), settings_.android_keyboard_on);
        settings_toggle_paste_btn_   = toggle(tr("Type by pasting"), settings_.android_type_via_paste);
        settings_toggle_softkbd_btn_ = toggle(tr("Hide the phone keyboard"), settings_.android_hide_soft_keyboard);
        settings_toggle_scroff_btn_  = toggle(tr("Phone screen off"), settings_.android_screen_off);
        settings_toggle_mutephone_btn_ = toggle(tr("Mute phone speaker"), settings_.android_mute_phone);
        const char* const res_opts[3] = {tr("Max"), "1920", "1280"};
        static const int res_vals[3] = {0, 1920, 1280};
        segment(tr("Resolution"), res_opts, 3,
                index_of(res_vals, 3, settings_.android_max_size), settings_seg_res_btns_);
        const char* const fps_opts[3] = {tr("Max"), "60", "30"};
        static const int fps_vals[3] = {0, 60, 30};
        segment(tr("Frame rate"), fps_opts, 3,
                index_of(fps_vals, 3, settings_.android_max_fps), settings_seg_fps_btns_);
        static const char* const ios_opts[2] = {"60", "30"};
        static const int ios_vals[2] = {60, 30};
        segment(tr("iPhone frame rate"), ios_opts, 2,
                index_of(ios_vals, 2, settings_.airplay_max_fps), settings_seg_ios_fps_btns_);
#ifdef ENABLE_MCP
        settings_toggle_mcp_btn_ = toggle(tr("Claude control (MCP)"), settings_.mcp_enabled);
        if (settings_.mcp_enabled) {
            // Server status on the left, "Copy command" on the right.
            int y = begin_row(false);
            std::string st;
            {
                std::lock_guard lk(mcp_status_mutex_);
                st = mcp_status_;
            }
            bool copied = std::chrono::steady_clock::now() - mcp_copied_at_ < std::chrono::milliseconds(1500);
            int ph = label_h + 10, pw = std::min(label_h * 8, card_w / 2);
            BtnRect pill{card_x + card_w - inner - pw, y + (row_h - ph) / 2, pw, ph};
            ui_pill_(pill, copied ? tr("Copied") : tr("Copy command"), copied ? lucide::Check : 0, false,
                     in_rect(mx, my, pill.x, pill.y, pill.w, pill.h), text_alpha);
            settings_mcp_copy_btn_ = pill;
            int avail = card_w - inner * 3 - pw;
            text(st, fit_text_px(sdl_renderer_, st.c_str(), note_h, avail), card_x + inner,
                 y + row_h / 2, 150, 154, 162, 0);
        }
#endif
        text(tr("Applies the next time a phone connects."), note_h, card_x + inner,
             cy + rows * row_h + gap + note_h / 2, 132, 132, 142, 0);
    } else if (tab == 2) {
        settings_toggle_save_btn_   = toggle(tr("Save to Pictures"), settings_.screenshot_save_to_folder);
        settings_toggle_clip_btn_   = toggle(tr("Copy to clipboard"), settings_.screenshot_copy_to_clipboard);
        settings_toggle_snagit_btn_ = toggle(tr("Open in Snagit"), settings_.screenshot_open_in_snagit);
        static const char* const fmt_opts[2] = {"MP4", "GIF"};
        BtnRect fmt[2];
        segment(tr("Recording format"), fmt_opts, 2, settings_.record_format, fmt);
        settings_fmt_mp4_btn_ = fmt[0];
        settings_fmt_gif_btn_ = fmt[1];
        settings_toggle_resize_btn_ = toggle(tr("Resize screenshots"), resize_on);
        if (resize_on) {
            // Height stepper: "-" / "X.X cm" / "+".
            int y = begin_row(false);
            text(tr("Height"), label_h, card_x + inner, y + row_h / 2, 228, 228, 233, 0);
            int bs = label_h + 8, val_w = label_h * 5;
            int plus_x = card_x + card_w - inner - bs, by = y + (row_h - bs) / 2;
            int minus_x = plus_x - val_w - bs;
            auto step = [&](const char* sym, int bx) -> BtnRect {
                bool hov = in_rect(mx, my, bx, by, bs, bs);
                if (hov) surface(bx, by, bs, bs, bs / 2, 52, 58, 71);
                else     surface(bx, by, bs, bs, bs / 2, 34, 38, 46);
                text(sym, label_h, bx + bs / 2, by + bs / 2, 240, 242, 246, 1, 600);
                return BtnRect{bx, by, bs, bs};
            };
            settings_resize_minus_btn_ = step("-", minus_x);
            settings_resize_plus_btn_  = step("+", plus_x);
            char cmbuf[16];
            float cmv = settings_.screenshot_resize_height_cm;
            if (std::abs(cmv - std::round(cmv)) < 0.01f) snprintf(cmbuf, sizeof(cmbuf), "%d cm", (int)std::lround(cmv));
            else snprintf(cmbuf, sizeof(cmbuf), "%.1f cm", cmv);
            text(cmbuf, label_h, minus_x + bs + val_w / 2, y + row_h / 2, 236, 236, 241, 1);
            settings_toggle_save_original_btn_ = toggle(tr("Keep the original too"), settings_.screenshot_save_original_too);
        }
        settings_toggle_webcam_mirror_btn_ = toggle(tr("Mirror the webcam"), settings_.webcam_mirror_h);
        settings_toggle_webcam_record_btn_ = toggle(tr("Webcam in recordings"), settings_.webcam_include_in_recording);
    } else {
        // Help: the cable guide, what the app can do, version and about.
        {
            int y = begin_row(true);
            row_label(tr("Connect a phone with a cable"), y, label_h * 2);
            draw_glyph(sdl_renderer_, lucide::ChevronRight, card_x + card_w - inner - label_h / 2,
                       y + row_h / 2, label_h, 150, 154, 162, text_alpha);
            settings_help_guide_btn_ = {card_x, y, card_w, row_h};
        }
        {
            int y = begin_row(true);
            row_label(tr("Watch the tips"), y, label_h * 2);
            draw_glyph(sdl_renderer_, lucide::Play, card_x + card_w - inner - label_h / 2,
                       y + row_h / 2, label_h, 150, 154, 162, text_alpha);
            settings_help_tips_btn_ = {card_x, y, card_w, row_h};
        }
        const char* const kTipsHelp[5] = {
            tr("Drop files on the phone: APKs install, others go to Download"),
            tr("Copy on one side, paste on the other"),
            tr("Phone sound plays here too; U mutes the PC"),
            tr("Ctrl+S screenshot, Ctrl+R record, F fullscreen"),
            tr("Claude Code control: Phone > Claude control (MCP)")};
        for (const char* t : kTipsHelp) {
            int y = begin_row(false);
            text(t, fit_text_px(sdl_renderer_, t, label_h, card_w - inner * 2), card_x + inner,
                 y + row_h / 2, 200, 204, 212, 0);
        }
        {
            int y = begin_row(false);
            int ph = label_h + 10, pw = (card_w - inner * 3) / 2;
            BtnRect ver{card_x + inner, y + (row_h - ph) / 2, pw, ph};
            BtnRect about{ver.x + pw + inner, ver.y, pw, ph};
            ui_pill_(ver, tr("Version history"), lucide::History, false,
                     in_rect(mx, my, ver.x, ver.y, ver.w, ver.h), text_alpha);
            ui_pill_(about, tr("About"), lucide::Info, false,
                     in_rect(mx, my, about.x, about.y, about.w, about.h), text_alpha);
            settings_help_ver_btn_ = ver;
            settings_help_about_btn_ = about;
        }
    }

    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
}

void Renderer::clear_log_row_cache() {
    for (auto& r : log_row_cache_) {
        if (r.tex) SDL_DestroyTexture(r.tex);
    }
    log_row_cache_.clear();
}

void Renderer::toggle_drawer(DrawerMode mode) {
    if (log_panel_visible_ && drawer_mode_ == mode) {
        log_panel_visible_ = false;
    } else {
        if (log_panel_visible_ && drawer_mode_ != mode) log_scroll_offset_ = 0;
        if (mode == DrawerMode::Stats) stats_cache_dirty_ = true;
        drawer_mode_ = mode;
        log_panel_visible_ = true;
    }
    log_panel_animating_ = true;
    log_panel_anim_start_ = std::chrono::steady_clock::now();
}

void Renderer::draw_log_panel() {
#ifdef _WIN32
    if (frame_dst_w_ == 0 || log_panel_anim_ < 0.01f) return;
    int lp_w = (int)(log_panel_full_w_ * log_panel_anim_);
    int lp_margin = std::max(4, frame_dst_h_ / 40);
    int drawer_inset = frame_dst_h_ / 16; // slightly shorter than phone
    int panel_x = frame_dst_x_ + frame_dst_w_; // flush against phone frame
    int panel_y = frame_dst_y_ + drawer_inset;
    int panel_w = lp_w - lp_margin;
    int panel_h = frame_dst_h_ - drawer_inset * 2;

    if (panel_w < 40) return;

    // Background — flat left edge (flush with phone), rounded right edge (drawer)
    int pr = std::max(8, panel_w / 30);
    // Right corner centers — placed so that the arc tangent points sit
    // EXACTLY on the panel edges (cx + pr = panel_x + panel_w - 1, etc.).
    int cx_r  = panel_x + panel_w - 1 - pr;
    int cy_t  = panel_y + pr;
    int cy_b  = panel_y + panel_h - 1 - pr;

    uint8_t dr, dg, db; drawer_color(dr, dg, db);
    // Compute the bezel-edge ("highlight") colour up front so we can both
    // stroke the border and use it as the disc fill OUTSIDE the body so
    // there is no alpha-blend mismatch creating a visible circle ghost.
    int bw = std::max(2, std::min(3, panel_h / 200 + 2));
    auto lite = [](uint8_t c, int d) -> uint8_t {
        int v = (int)c + d; if (v < 0) v = 0; if (v > 255) v = 255; return (uint8_t)v;
    };
    uint8_t br_ = phone_frame_.bezel_r();
    uint8_t bg_ = phone_frame_.bezel_g();
    uint8_t bb_ = phone_frame_.bezel_b();
    uint8_t er = lite(br_, 30), eg = lite(bg_, 30), eb = lite(bb_, 32);

    // ---- Drawer interior + corner border --------------------------------
    // Disable alpha blending for the silhouette so overlapping fills do not
    // alpha-compound into a visible darker arc.
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);

    // 1. Paint the bulk of the body (everything left of the right-side
    //    rounded corners) in DRAWER colour, then add the top/bottom/right
    //    border strips on top.
    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
    SDL_Rect drawer_body = {panel_x, panel_y + bw,
                            cx_r - panel_x, panel_h - bw * 2};
    SDL_RenderFillRect(sdl_renderer_, &drawer_body);
    // Right inner strip between the two corner centers.
    SDL_Rect drawer_strip = {cx_r, cy_t,
                             pr - bw, cy_b - cy_t + 1};
    SDL_RenderFillRect(sdl_renderer_, &drawer_strip);

    // 2. Border strips (top / bottom / right). Edges stop at the corner
    //    center column so the corner rasterizer below owns those pixels
    //    exclusively.
    SDL_SetRenderDrawColor(sdl_renderer_, er, eg, eb, 255);
    SDL_Rect e_top = {panel_x, panel_y, cx_r - panel_x, bw};
    SDL_RenderFillRect(sdl_renderer_, &e_top);
    SDL_Rect e_bot = {panel_x, panel_y + panel_h - bw, cx_r - panel_x, bw};
    SDL_RenderFillRect(sdl_renderer_, &e_bot);
    SDL_Rect e_rgt = {panel_x + panel_w - bw, cy_t, bw, cy_b - cy_t + 1};
    SDL_RenderFillRect(sdl_renderer_, &e_rgt);

    // 3. Two right-side corners drawn as a single per-pixel pass per corner.
    //    Uses coverage-based anti-aliasing via a signed distance from the
    //    corner centre. The interior is painted opaquely; only the ~1px
    //    outer rim and the body/border seam use partial alpha so the curve
    //    looks smooth without any stair-step ghosts.
    //       d + 0.5 < r_in            -> drawer (opaque)
    //       d in (r_in-0.5, r_in+0.5) -> blend drawer/border
    //       d in [r_in+0.5, r_out-0.5)-> border (opaque)
    //       d in [r_out-0.5, r_out+0.5]-> blend border with destination
    //       d > r_out + 0.5           -> skip (transparent)
    auto paint_corner = [&](int cx, int cy, int quad) {
        float r_out = (float)pr;
        float r_in  = (float)(pr - bw);
        for (int dy = -pr - 1; dy <= pr + 1; dy++) {
            for (int dx = -pr - 1; dx <= pr + 1; dx++) {
                if (quad == 0 && !(dx >= 0 && dy <= 0)) continue;
                if (quad == 1 && !(dx >= 0 && dy >= 0)) continue;
                float d = std::sqrt((float)(dx * dx + dy * dy));
                // Outer coverage: 1 well inside r_out, 0 well outside.
                float cov_out = std::clamp(r_out + 0.5f - d, 0.0f, 1.0f);
                if (cov_out <= 0.0f) continue;
                // Inner coverage: 1 well inside r_in, 0 well outside.
                float cov_in  = std::clamp(r_in  + 0.5f - d, 0.0f, 1.0f);
                float ring = cov_out - cov_in; // 1 only in the strict band
                int px = cx + dx;
                int py = cy + dy;
                if (cov_in >= 1.0f) {
                    // Fully inside -> opaque drawer.
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
                    SDL_RenderDrawPoint(sdl_renderer_, px, py);
                } else if (cov_out >= 1.0f && ring <= 0.0f) {
                    // Fully drawer, fully inside outer (cov_in must be <1
                    // here so we're in the body/border seam transition).
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
                    SDL_RenderDrawPoint(sdl_renderer_, px, py);
                } else {
                    // Blend drawer + border according to (cov_in) and the
                    // remaining outer coverage. Premix the RGB once, then
                    // emit with cov_out as the alpha so the rim feathers
                    // smoothly against whatever is behind the window.
                    float bd = cov_in;          // drawer share inside ring
                    float bb = ring;            // border share
                    float total = bd + bb;
                    uint8_t mr, mg, mb;
                    if (total <= 0.0001f) {
                        mr = er; mg = eg; mb = eb;
                    } else {
                        mr = (uint8_t)((dr * bd + er * bb) / total);
                        mg = (uint8_t)((dg * bd + eg * bb) / total);
                        mb = (uint8_t)((db * bd + eb * bb) / total);
                    }
                    uint8_t a = (uint8_t)(cov_out * 255.0f);
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(sdl_renderer_, mr, mg, mb, a);
                    SDL_RenderDrawPoint(sdl_renderer_, px, py);
                }
            }
        }
    };
    paint_corner(cx_r, cy_t, 0);
    paint_corner(cx_r, cy_b, 1);

    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    if (drawer_mode_ == DrawerMode::Stats) {
        draw_stats_drawer_content(panel_x, panel_y, panel_w, panel_h, pr, lp_margin);
        return;
    }

    // Get log lines
    auto lines = opm::LogBuffer::instance().get_lines();
    if (lines.empty()) return;

    // Layout is computed against the FULL-OPEN panel width, not the current
    // animated width. Otherwise word-wrap + text rasterization run on every
    // frame of the slide animation and produce visible hitches.
    int full_panel_w = log_panel_full_w_ - lp_margin;
    if (full_panel_w < 40) full_panel_w = panel_w;
    int font_sz = std::max(9, panel_h / 85);
    int line_h = font_sz + 2;
    int pad = std::max(6, pr);
    int max_text_w = full_panel_w - pad * 2;
    int visible_h = panel_h - pad * 2;

    // Rebuild the wrapped+rasterized row cache only when something the
    // layout depends on actually changes.
    uint64_t cur_ver = opm::LogBuffer::instance().version();
    bool need_rebuild = (cur_ver != log_cache_version_) ||
                        (font_sz != log_cache_font_sz_) ||
                        (full_panel_w != log_cache_full_w_);
    if (need_rebuild) {
        clear_log_row_cache();
        log_cache_version_ = cur_ver;
        log_cache_font_sz_ = font_sz;
        log_cache_full_w_ = full_panel_w;

        auto color_for = [](const std::string& s, uint8_t& r, uint8_t& g, uint8_t& b) {
            r = 160; g = 160; b = 160;
            if      (s.find("[Cast]")     != std::string::npos) { r = 120; g = 200; b = 120; }
            else if (s.find("[AirPlay]")  != std::string::npos) { r = 120; g = 160; b = 255; }
            else if (s.find("[Renderer]") != std::string::npos) { r = 200; g = 180; b = 120; }
            if (s.find("Warning") != std::string::npos ||
                s.find("Failed")  != std::string::npos ||
                s.find("Error")   != std::string::npos) { r = 255; g = 120; b = 120; }
        };

        const std::string cont_indent = "  ";
        int wrap_budget = std::max(8, max_text_w - font_sz);
        log_row_cache_.reserve(lines.size() + 16);
        for (auto& src : lines) {
            uint8_t lr, lg, lb; color_for(src, lr, lg, lb);
            auto push_row = [&](std::string s) {
                LogRowCache row;
                row.text = std::move(s);
                row.r = lr; row.g = lg; row.b = lb;
                row.tex = make_text_texture(sdl_renderer_, row.text, font_sz,
                                             row.r, row.g, row.b, &row.w, &row.h);
                log_row_cache_.push_back(std::move(row));
            };
            if (measure_text_width_a(src, font_sz) <= wrap_budget) {
                push_row(src);
                continue;
            }
            std::string remaining = src;
            bool first_row = true;
            while (!remaining.empty()) {
                std::string prefix = first_row ? std::string{} : cont_indent;
                int lo = 1, hi = (int)remaining.size(), best = 1;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    int w = measure_text_width_a(prefix + remaining.substr(0, mid), font_sz);
                    if (w <= wrap_budget) { best = mid; lo = mid + 1; }
                    else hi = mid - 1;
                }
                int brk = best;
                if (best < (int)remaining.size()) {
                    int sp = (int)remaining.rfind(' ', best);
                    if (sp > 0 && sp >= best / 2) brk = sp;
                }
                push_row(prefix + remaining.substr(0, brk));
                remaining.erase(0, brk);
                if (!remaining.empty() && remaining.front() == ' ') remaining.erase(0, 1);
                first_row = false;
            }
        }
        log_cache_total_h_ = (int)log_row_cache_.size() * line_h;
    }

    int content_h = log_cache_total_h_;

    // Clamp scroll
    int max_scroll = std::max(0, content_h - visible_h);
    if (log_scroll_offset_ < 0) log_scroll_offset_ = 0;
    if (log_scroll_offset_ > max_scroll) log_scroll_offset_ = max_scroll;

    // Auto-scroll to bottom on new lines (unless user is dragging the thumb).
    if (cur_ver != log_last_version_) {
        log_last_version_ = cur_ver;
        if (!log_scrollbar_dragging_) {
            log_scroll_offset_ = max_scroll;
        }
    }

    // Clip to the currently-visible portion of the drawer (animated width).
    SDL_Rect clip = {panel_x + pad, panel_y + pad, panel_w - pad * 2, visible_h};
    SDL_RenderSetClipRect(sdl_renderer_, &clip);

    int first_visible = log_scroll_offset_ / line_h;
    int last_visible = (log_scroll_offset_ + visible_h) / line_h + 1;
    first_visible = std::max(0, first_visible);
    last_visible = std::min((int)log_row_cache_.size(), last_visible);

    int text_x = panel_x + pad;

    for (int i = first_visible; i < last_visible; i++) {
        int y = panel_y + pad + i * line_h - log_scroll_offset_;
        const auto& row = log_row_cache_[i];
        if (row.tex) {
            SDL_Rect dst = {text_x, y, row.w, row.h};
            SDL_RenderCopy(sdl_renderer_, row.tex, nullptr, &dst);
        }
    }

    SDL_RenderSetClipRect(sdl_renderer_, nullptr);

    // Scrollbar
    if (content_h > visible_h) {
        int sb_w = std::max(3, pad / 3);
        int sb_x = panel_x + panel_w - pad;
        int track_y = panel_y + pad;
        int track_h = visible_h;
        float visible_frac = (float)visible_h / content_h;
        int thumb_h = std::max(pr * 2, (int)(track_h * visible_frac));
        float scroll_frac = (max_scroll > 0) ? (float)log_scroll_offset_ / max_scroll : 0.0f;
        int thumb_y = track_y + (int)((track_h - thumb_h) * scroll_frac);

        // Store geometry for drag handling
        log_sb_track_y_ = track_y;
        log_sb_track_h_ = track_h;
        log_sb_thumb_h_ = thumb_h;
        log_sb_max_scroll_ = max_scroll;

        uint8_t sb_alpha = log_scrollbar_dragging_ ? 220 : 150;
        SDL_SetRenderDrawColor(sdl_renderer_, 80, 80, 90, sb_alpha);
        SDL_Rect thumb_rect = {sb_x, thumb_y, sb_w, thumb_h};
        SDL_RenderFillRect(sdl_renderer_, &thumb_rect);
    } else {
        log_sb_track_h_ = 0;
        log_sb_max_scroll_ = 0;
    }
#endif
}

void Renderer::rebuild_stats_cache(int font_sz) {
    for (auto& r : stats_kpi_cache_) if (r.tex) SDL_DestroyTexture(r.tex);
    stats_kpi_cache_.clear();
    for (auto& r : stats_row_cache_) if (r.tex) SDL_DestroyTexture(r.tex);
    stats_row_cache_.clear();

    auto rows = opm::UsageLog::load_rows();
    auto stats = opm::UsageLog::compute_stats(rows);

    auto push_kpi = [&](const std::string& s) {
        LogRowCache row;
        row.text = s; row.r = 210; row.g = 210; row.b = 215;
        row.tex = make_text_texture(sdl_renderer_, s, font_sz, row.r, row.g, row.b, &row.w, &row.h);
        stats_kpi_cache_.push_back(std::move(row));
    };
    char buf[128];
    snprintf(buf, sizeof(buf), "App starts: %d", stats.app_starts);
    push_kpi(buf);
    snprintf(buf, sizeof(buf), "iOS sessions: %d (%.0f min)", stats.ios_sessions, stats.ios_minutes);
    push_kpi(buf);
    snprintf(buf, sizeof(buf), "Android sessions: %d (%.0f min)", stats.android_sessions, stats.android_minutes);
    push_kpi(buf);
    snprintf(buf, sizeof(buf), "Screenshots: iOS %d / Android %d",
             stats.screenshots.ios, stats.screenshots.android);
    push_kpi(buf);
    snprintf(buf, sizeof(buf), "Annotated: iOS %d / Android %d",
             stats.annotations.ios, stats.annotations.android);
    push_kpi(buf);
    snprintf(buf, sizeof(buf), "OCR copies: iOS %d / Android %d",
             stats.ocr_copies.ios, stats.ocr_copies.android);
    push_kpi(buf);
    snprintf(buf, sizeof(buf), "Recordings: iOS %d / Android %d",
             stats.recordings.ios, stats.recordings.android);
    push_kpi(buf);

    // Table rows, most-recent first.
    stats_row_cache_.reserve(rows.size());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        std::string line = it->timestamp + "  " + it->event;
        if (!it->platform.empty()) line += "  " + it->platform;
        if (!it->device.empty()) line += "  " + it->device;
        if (it->event == "session_end" && it->duration_sec > 0) {
            char d[32];
            snprintf(d, sizeof(d), "  %lldm%02llds", it->duration_sec / 60, it->duration_sec % 60);
            line += d;
        }
        LogRowCache row;
        row.text = line; row.r = 170; row.g = 170; row.b = 175;
        row.tex = make_text_texture(sdl_renderer_, line, font_sz, row.r, row.g, row.b, &row.w, &row.h);
        stats_row_cache_.push_back(std::move(row));
    }
}

// Renders KPIs + an "Open CSV" button + a scrollable event table into the
// same drawer chrome draw_log_panel() already painted. Shares that
// function's scroll/scrollbar state (log_scroll_offset_, log_sb_*) since
// only one of Log/Stats content is ever visible at a time.
void Renderer::draw_stats_drawer_content(int panel_x, int panel_y, int panel_w,
                                         int panel_h, int pr, int lp_margin) {
#ifdef _WIN32
    int full_panel_w = log_panel_full_w_ - lp_margin;
    if (full_panel_w < 40) full_panel_w = panel_w;
    int font_sz = std::max(9, panel_h / 85);
    int line_h = font_sz + 4;
    int pad = std::max(6, pr);
    int max_text_w = full_panel_w - pad * 2;

    if (stats_cache_dirty_) {
        rebuild_stats_cache(font_sz);
        stats_cache_dirty_ = false;
    }

    int cy = panel_y + pad;
    int title_h = std::max(13, font_sz + 6);
    {
        int tw = 0, th = 0;
        SDL_Texture* tt = make_text_texture(sdl_renderer_, "Statistics", title_h, 230, 230, 235, &tw, &th);
        if (tt) {
            SDL_Rect dst{panel_x + pad, cy, tw, th};
            SDL_RenderCopy(sdl_renderer_, tt, nullptr, &dst);
            SDL_DestroyTexture(tt);
        }
    }
    cy += title_h + pad / 2;

    for (auto& kpi : stats_kpi_cache_) {
        if (kpi.tex) {
            SDL_Rect dst{panel_x + pad, cy, kpi.w, kpi.h};
            SDL_RenderCopy(sdl_renderer_, kpi.tex, nullptr, &dst);
        }
        cy += line_h;
    }
    cy += pad / 2;

    // "Open CSV" button.
    int btn_h = std::max(16, font_sz + 10);
    stats_open_csv_btn_ = {panel_x + pad, cy, max_text_w, btn_h};
    SDL_SetRenderDrawColor(sdl_renderer_, 60, 90, 130, 255);
    SDL_Rect bb{stats_open_csv_btn_.x, stats_open_csv_btn_.y, stats_open_csv_btn_.w, stats_open_csv_btn_.h};
    SDL_RenderFillRect(sdl_renderer_, &bb);
    SDL_SetRenderDrawColor(sdl_renderer_, 110, 150, 200, 255);
    SDL_RenderDrawRect(sdl_renderer_, &bb);
    {
        int btw = 0, bth = 0;
        SDL_Texture* btex = make_text_texture(sdl_renderer_, "Open usage_log.csv", font_sz,
                                              230, 235, 255, &btw, &bth);
        if (btex) {
            SDL_Rect td{bb.x + (bb.w - btw) / 2, bb.y + (bb.h - bth) / 2, btw, bth};
            SDL_RenderCopy(sdl_renderer_, btex, nullptr, &td);
            SDL_DestroyTexture(btex);
        }
    }
    cy += btn_h + pad;

    SDL_SetRenderDrawColor(sdl_renderer_, 90, 90, 95, 255);
    SDL_RenderDrawLine(sdl_renderer_, panel_x + pad, cy, panel_x + panel_w - pad, cy);
    cy += pad / 2;

    int table_top = cy;
    int visible_h = std::max(line_h, panel_y + panel_h - pad - table_top);
    int content_h = (int)stats_row_cache_.size() * line_h;
    int max_scroll = std::max(0, content_h - visible_h);
    if (log_scroll_offset_ < 0) log_scroll_offset_ = 0;
    if (log_scroll_offset_ > max_scroll) log_scroll_offset_ = max_scroll;

    SDL_Rect clip = {panel_x + pad, table_top, panel_w - pad * 2, visible_h};
    SDL_RenderSetClipRect(sdl_renderer_, &clip);
    int first_visible = std::max(0, log_scroll_offset_ / line_h);
    int last_visible = std::min((int)stats_row_cache_.size(), (log_scroll_offset_ + visible_h) / line_h + 1);
    for (int i = first_visible; i < last_visible; i++) {
        int y = table_top + i * line_h - log_scroll_offset_;
        auto& row = stats_row_cache_[i];
        if (row.tex) {
            SDL_Rect dst{panel_x + pad, y, row.w, row.h};
            SDL_RenderCopy(sdl_renderer_, row.tex, nullptr, &dst);
        }
    }
    SDL_RenderSetClipRect(sdl_renderer_, nullptr);

    if (content_h > visible_h) {
        int sb_w = std::max(3, pad / 3);
        int sb_x = panel_x + panel_w - pad;
        int track_y = table_top;
        int track_h = visible_h;
        float visible_frac = (float)visible_h / content_h;
        int thumb_h = std::max(pr * 2, (int)(track_h * visible_frac));
        float scroll_frac = (max_scroll > 0) ? (float)log_scroll_offset_ / max_scroll : 0.0f;
        int thumb_y = track_y + (int)((track_h - thumb_h) * scroll_frac);

        log_sb_track_y_ = track_y;
        log_sb_track_h_ = track_h;
        log_sb_thumb_h_ = thumb_h;
        log_sb_max_scroll_ = max_scroll;

        uint8_t sb_alpha = log_scrollbar_dragging_ ? 220 : 150;
        SDL_SetRenderDrawColor(sdl_renderer_, 80, 80, 90, sb_alpha);
        SDL_Rect thumb_rect = {sb_x, thumb_y, sb_w, thumb_h};
        SDL_RenderFillRect(sdl_renderer_, &thumb_rect);
    } else {
        log_sb_track_h_ = 0;
        log_sb_max_scroll_ = 0;
    }
#endif
}

// ----------------------------------------------------------------------------
// Android connect panel — themed to match info_panel
// ----------------------------------------------------------------------------

#ifdef _WIN32
// Discover this machine's primary IPv4 by "connecting" a UDP socket to a
// public address — the OS picks the outbound interface without sending
// any packets. Returns "192.168.0." (first 3 octets + dot) or "".
static std::string local_subnet_prefix() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = inet_addr("8.8.8.8");
    std::string out;
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in name{};
        int len = sizeof(name);
        if (getsockname(s, (sockaddr*)&name, &len) == 0) {
            unsigned long a = ntohl(name.sin_addr.s_addr);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu.",
                          (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF);
            out = buf;
        }
    }
    closesocket(s);
    return out;
}
#endif

void Renderer::show_android_panel() {
    android_panel_visible_ = true;
    android_panel_animating_ = true;
    android_panel_anim_start_ = std::chrono::steady_clock::now();
    // Pair port and PIN are single-use; the phone shows fresh values every
    // time the Wireless debugging → Pair dialog is opened. Always clear
    // them so the user can't paste in stale values from the last session.
    // IP and Connect port survive sleep/wake on the phone, so we keep
    // them between panel opens within a single app session.
    android_port_.clear();
    android_pin_.clear();
    if (android_qr_cancel_) android_qr_cancel_->store(true);
    android_qr_active_.store(false);
    if (android_ip_.empty()) {
        const std::string& last = settings_.android_last_wifi;
        auto colon = last.rfind(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < last.size()) {
            android_ip_ = last.substr(0, colon);
            android_connect_port_ = last.substr(colon + 1);
        }
    }
#ifdef _WIN32
    if (android_ip_.empty()) {
        std::string pfx = local_subnet_prefix();
        if (!pfx.empty()) android_ip_ = pfx;
    }
#endif
    // Smart focus: jump to the field that needs typing next.
    //   - IP not set yet              -> focus IP (0)
    //   - IP set, no connect port yet -> focus Connect port (1)
    //   - both already filled         -> focus Pair port (2), the value the
    //                                    user must always re-type each session
    auto looks_complete_ip = [](const std::string& s) {
        if (s.empty() || s.back() == '.') return false;
        int dots = 0;
        for (char c : s) if (c == '.') ++dots;
        return dots == 3;
    };
    if (!looks_complete_ip(android_ip_))      android_focus_ = 0;
    else if (android_connect_port_.empty())   android_focus_ = 1;
    else                                       android_focus_ = 2;
    {
        std::lock_guard lk(android_status_mutex_);
    }
    SDL_StartTextInput();
    start_android_discovery();
}

void Renderer::start_android_discovery() {
    if (!android_discover_fn_) return;
    // Always restart cleanly so re-opening the panel (e.g. while another
    // device is already connected) gets a fresh poll and a fresh list,
    // rather than silently re-attaching to a stale worker.
    stop_android_discovery();
    android_discover_running_.store(true);
    {
        std::lock_guard lk(android_discovered_mutex_);
        android_discovered_.clear();
    }
    android_discover_thread_ = std::thread([this]() {
        while (android_discover_running_.load()) {
            android_discover_in_progress_.store(true);
            std::vector<DiscoveredAndroidDevice> fresh;
            try {
                fresh = android_discover_fn_();
            } catch (...) {
                // Ignore — keep polling.
            }
            android_discover_in_progress_.store(false);
            {
                std::lock_guard lk(android_discovered_mutex_);
                android_discovered_ = std::move(fresh);
            }
            // Poll roughly every 1 s so a phone that powers on Wireless
            // Debugging while the panel is already open shows up quickly.
            // Sleep in 100 ms chunks so a panel close is responsive.
            for (int i = 0; i < 10 && android_discover_running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void Renderer::stop_android_discovery() {
    if (!android_discover_running_.exchange(false)) return;
    if (!android_discover_thread_.joinable()) return;

    // The discovery worker spends most of its time inside
    // `adb mdns services` / `adb devices -l` subprocess calls. adb is
    // known to occasionally hang for many seconds (especially after a
    // phone disconnects), and `spawn_capture` blocks with
    // WaitForSingleObject(..., INFINITE). Give the worker a brief window
    // to notice the running flag and exit cleanly; if it doesn't, detach
    // so we never freeze the UI / app shutdown waiting on a stuck adb.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (android_discover_in_progress_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (android_discover_in_progress_.load()) {
        // Still inside an adb call — detaching is the safe choice. The
        // thread captures `this` (Renderer) and uses an App-owned adb
        // controller via android_discover_fn_, so the leaked thread will
        // be torn down only at process exit. That's acceptable: it's a
        // diagnostic shutdown path and the OS reclaims everything.
        android_discover_thread_.detach();
        return;
    }
    android_discover_thread_.join();
}

namespace {
bool valid_ipv4(const std::string& s) {
    int parts = 0, n = 0, digits = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : '.';
        if (c == '.') {
            if (digits == 0 || n > 255) return false;
            ++parts; n = 0; digits = 0;
            if (parts > 4) return false;
        } else if (c >= '0' && c <= '9') {
            n = n * 10 + (c - '0'); ++digits;
            if (digits > 3) return false;
        } else {
            return false;
        }
    }
    return parts == 4;
}
}

void Renderer::android_qr_stop() {
    if (android_qr_cancel_) android_qr_cancel_->store(true);
    if (android_qr_active_.exchange(false)) {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = "QR pairing cancelled.";
        android_status_is_error_ = false;
    }
}

void Renderer::android_qr_start() {
    auto fail = [&](const std::string& m) {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = m;
        android_status_is_error_ = true;
    };
    if (!android_qr_fn_) return fail("QR pairing handler not wired.");

    // Same shape Android Studio uses: S:studio-<10 chars>, P:<12 chars>.
    static const char kAlnum[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::uniform_int_distribution<int> pick(0, (int)sizeof(kAlnum) - 2);
    auto rnd = [&](int n) {
        std::string s;
        for (int i = 0; i < n; ++i) s.push_back(kAlnum[pick(rd)]);
        return s;
    };
    std::string name = "studio-" + rnd(10);
    std::string password = rnd(12);
    std::string payload = "WIFI:T:ADB;S:" + name + ";P:" + password + ";;";

    try {
        auto qr = qrcodegen::QrCode::encodeText(payload.c_str(),
                                                qrcodegen::QrCode::Ecc::MEDIUM);
        android_qr_size_ = qr.getSize();
        android_qr_modules_.assign((size_t)android_qr_size_ * android_qr_size_, 0);
        for (int y = 0; y < android_qr_size_; ++y)
            for (int x = 0; x < android_qr_size_; ++x)
                android_qr_modules_[(size_t)y * android_qr_size_ + x] =
                    qr.getModule(x, y) ? 1 : 0;
    } catch (const std::exception& e) {
        return fail(std::string("QR generation failed: ") + e.what());
    }

    if (android_qr_cancel_) android_qr_cancel_->store(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    android_qr_cancel_ = cancel;
    {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = "Waiting for the phone to scan the code...";
        android_status_is_error_ = false;
    }
    android_qr_active_.store(true);

    auto fn = android_qr_fn_;
    std::thread([this, fn, name, password, cancel]() {
        std::string r = fn(name, password, [cancel]() { return cancel->load(); });
        if (cancel->load()) return;   // cancelled / superseded: stay silent
        bool err = (r.rfind("Mirroring ", 0) != 0);
        if (err) std::cerr << "[Android] " << r << "\n";
        else     std::cout << "[Android] " << r << "\n";
        std::lock_guard lk(android_status_mutex_);
        android_status_ = r;
        android_status_is_error_ = err;
        android_qr_active_.store(false);
        if (!err) {
            android_panel_visible_ = false;
            android_panel_animating_ = true;
            android_panel_anim_start_ = std::chrono::steady_clock::now();
        }
    }).detach();
}

void Renderer::draw_android_qr(int svx, int svy, int svw, int svh) {
    int uw = ui_ref_width();
    int panel_w = std::min((int)(svw * 0.85f), (int)(uw * 1.6f));
    int pad     = std::max(10, panel_w / 22);
    int label_h = std::max(14, panel_w / 26);
    int btn_h   = std::max(28, panel_w / 11);
    int gap     = std::max(6, pad / 2);
    int title_h = std::max(20, panel_w / 16);

    // Other panel controls are not on screen in this mode: clear their hit
    // rects so stale ones can't be clicked.
    android_connect_btn_ = {};
    android_disconnect_btn_ = {};
    android_help_btn_ = {};
    android_close_btn_ = {};
    android_qr_btn_ = {};
    android_usb_btn_ = {};
    for (auto& r : android_field_rects_) r = {};
    android_discover_btns_.clear();

    const int quiet = 2;   // quiet-zone modules around the code
    int qn = android_qr_size_;
    int max_box = std::min(panel_w - pad * 2, (int)(svh * 0.46f));
    int ms  = qn > 0 ? std::max(1, max_box / (qn + quiet * 2)) : 1;
    int box = qn > 0 ? ms * (qn + quiet * 2) : 0;

    auto wrap = [&](const std::string& s) {
        std::vector<std::string> lines;
        int max_w = panel_w - pad * 2;
        std::vector<std::string> words;
        std::string cur;
        for (char ch : s) {
            if (ch == ' ' || ch == '\n') {
                if (!cur.empty()) { words.push_back(cur); cur.clear(); }
            } else cur.push_back(ch);
        }
        if (!cur.empty()) words.push_back(cur);
        std::string line;
        for (auto& w : words) {
            std::string trial = line.empty() ? w : line + " " + w;
            int tw = 0, th = 0;
            SDL_Texture* probe = make_text_texture(sdl_renderer_, trial,
                                                   label_h, 200, 200, 200, &tw, &th);
            if (probe) SDL_DestroyTexture(probe);
            if (tw <= max_w || line.empty()) line = trial;
            else { lines.push_back(line); line = w; }
        }
        if (!line.empty()) lines.push_back(line);
        return lines;
    };

    std::string status;
    bool status_err = false;
    {
        std::lock_guard lk(android_status_mutex_);
        status = android_status_;
        status_err = android_status_is_error_;
    }
    auto instr_lines = wrap("On the phone: Developer options > Wireless debugging > "
                            "Pair device with QR code, then scan this code.");
    auto status_lines = wrap(status);
    int status_rows = std::max<int>(1, (int)status_lines.size());

    int btn_bottom_pad = std::max(10, pad);
    int total_h = pad + title_h + gap * 2
                + box + gap
                + label_h * (int)instr_lines.size() + gap
                + label_h * status_rows + gap
                + btn_h + pad + btn_bottom_pad;

    int panel_x = svx + (svw - panel_w) / 2;
    int bottom_margin = std::max(8, svh / 28);
    int panel_y = svy + (svh - total_h) / 2 + svh / 18;
    if (panel_y + total_h > svy + svh - bottom_margin)
        panel_y = svy + svh - total_h - bottom_margin;
    if (panel_y < svy + 4) panel_y = svy + 4;
    panel_y -= (int)((1.0f - android_panel_anim_) * 30);
    uint8_t alpha      = (uint8_t)(255 * android_panel_anim_);
    uint8_t text_alpha = (uint8_t)(255 * android_panel_anim_);
    uint8_t qr_alpha   = (uint8_t)(255 * android_panel_anim_);

    android_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    int pr = std::max(8, pad / 2);
    SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 34, alpha);
    SDL_Rect body = {panel_x + pr, panel_y, panel_w - pr * 2, total_h};
    SDL_RenderFillRect(sdl_renderer_, &body);
    SDL_Rect ls = {panel_x, panel_y + pr, pr, total_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &ls);
    SDL_Rect rs = {panel_x + panel_w - pr, panel_y + pr, pr, total_h - pr * 2};
    SDL_RenderFillRect(sdl_renderer_, &rs);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + pr, pr);
    fill_circle(sdl_renderer_, panel_x + pr, panel_y + total_h - pr, pr);
    fill_circle(sdl_renderer_, panel_x + panel_w - pr, panel_y + total_h - pr, pr);

    auto draw_text = [&](const std::string& s, int size, uint8_t r, uint8_t g,
                         uint8_t b, int x, int y, int max_w, bool center) {
        int tw = 0, th = 0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, s, size, r, g, b, &tw, &th);
        if (!tex) return th > 0 ? th : size;
        int dw = std::min(tw, max_w);
        int dx = center ? x + (max_w - dw) / 2 : x;
        SDL_Rect d = {dx, y, dw, th};
        SDL_SetTextureAlphaMod(tex, text_alpha);
        SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
        SDL_DestroyTexture(tex);
        return th;
    };

    int cy = panel_y + pad;
    draw_text("Pair with QR code", title_h, 235, 235, 240,
              panel_x + pad, cy, panel_w - pad * 2, true);
    cy += title_h + gap * 2;

    // QR code: white plate (with quiet zone) + dark modules.
    if (qn > 0) {
        int bx = panel_x + (panel_w - box) / 2;
        SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, qr_alpha);
        SDL_Rect plate = {bx, cy, box, box};
        SDL_RenderFillRect(sdl_renderer_, &plate);
        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, qr_alpha);
        for (int y = 0; y < qn; ++y)
            for (int x = 0; x < qn; ++x)
                if (android_qr_modules_[(size_t)y * qn + x]) {
                    SDL_Rect m = {bx + (x + quiet) * ms, cy + (y + quiet) * ms, ms, ms};
                    SDL_RenderFillRect(sdl_renderer_, &m);
                }
    }
    cy += box + gap;

    for (auto& ln : instr_lines) {
        draw_text(ln, label_h, 170, 170, 180, panel_x + pad, cy, panel_w - pad * 2, false);
        cy += label_h;
    }
    cy += gap;

    {
        uint8_t cr = status_err ? 230 : 140;
        uint8_t cg = status_err ? 110 : 200;
        uint8_t cb = status_err ? 110 : 140;
        for (auto& ln : status_lines) {
            draw_text(ln, label_h, cr, cg, cb, panel_x + pad, cy, panel_w - pad * 2, false);
            cy += label_h;
        }
        if (status_lines.empty()) cy += label_h;
        cy += gap;
    }

    android_qr_cancel_btn_ = {panel_x + pad, cy, panel_w - pad * 2, btn_h};
    {
        const BtnRect& r = android_qr_cancel_btn_;
        SDL_SetRenderDrawColor(sdl_renderer_, 50, 50, 55, alpha);
        SDL_Rect br = {r.x, r.y, r.w, r.h};
        SDL_RenderFillRect(sdl_renderer_, &br);
        SDL_SetRenderDrawColor(sdl_renderer_, 90, 90, 100, alpha);
        SDL_RenderDrawRect(sdl_renderer_, &br);
        int tw = 0, th = 0;
        SDL_Texture* tex = make_text_texture(sdl_renderer_, "Cancel", btn_h - 12,
                                             240, 240, 245, &tw, &th);
        if (tex) {
            SDL_Rect d = {r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, tw, th};
            SDL_SetTextureAlphaMod(tex, text_alpha);
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
            SDL_DestroyTexture(tex);
        }
    }
}

// Turns what the phone shows for the word being typed (android_ime_shown_)
// into `target`: backspace over the part that differs, then type the rest.
void Renderer::android_type_diff_(const std::string& target) {
    const std::string& shown = android_ime_shown_;
    size_t common = 0;
    while (common < shown.size() && common < target.size() && shown[common] == target[common])
        ++common;
    // Never split a UTF-8 character: back up to the start of one.
    auto mid_char = [](const std::string& s, size_t i) {
        return i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80;
    };
    while (common > 0 && (mid_char(shown, common) || mid_char(target, common))) --common;

    AndroidInput key;
    key.kind = AndroidInput::Kind::Key;
    key.keycode = 67;   // KEYCODE_DEL (backspace), once per character
    for (size_t i = common; i < shown.size(); ++i) {
        if (mid_char(shown, i)) continue;
        key.action = 0; android_input_fn_(key);
        key.action = 1; android_input_fn_(key);
    }

    if (common < target.size()) {
        AndroidInput in;
        in.text = target.substr(common);
        // Injected key events pass through the phone's own keyboard app,
        // and Vietnamese ones (Laban Key, ...) hold the letters back until
        // the word is finished. Pasting bypasses it, and is the only way
        // to send accented characters anyway.
        // shortcut: overwrites the phone clipboard on every keystroke.
        bool ascii = true;
        for (unsigned char c : in.text) if (c >= 0x80) { ascii = false; break; }
        in.kind = (ascii && !settings_.android_type_via_paste) ? AndroidInput::Kind::Text
                                                              : AndroidInput::Kind::Clipboard;
        android_input_fn_(in);
    }
    android_ime_shown_ = target;
}

// A texture drawn far smaller than it is (the phone sends 1080 px into a
// ~290 px window; the frame is built at the video's size) loses pixels in
// one bilinear step: thin text and curved edges turn jagged. So it is first
// halved on the GPU (each halving averages 2x2 pixels, like a mipmap) while
// the half is still at least the draw size; it is never halved below it,
// which would have to be stretched back up and blur. With `supersample`
// the last step (under 2x) is drawn into a target of the exact draw size
// from 2x2 sub-pixel samples, close to a true area average. Levels are
// rebuilt when `serial` changes (a new video frame; every draw for the
// frame, which is cheap and survives a lost GPU device).
SDL_Texture* Renderer::mip_for_(MipChain& c, SDL_Texture* src, int sw, int sh, uint64_t serial,
                                int dst_w, int dst_h, bool supersample) {
    if (!src || dst_w <= 0 || dst_h <= 0) return src;
    int levels = 0;
    // Without supersampling (the frame, which has transparent edges) stop
    // within 1.6x and let one bilinear step finish: soft but never jagged.
    for (int w = sw, h = sh; levels < MipChain::kLevels &&
         (supersample ? (w / 2 >= dst_w && h / 2 >= dst_h) : (w >= dst_w * 1.6f && h >= dst_h * 1.6f));
         w /= 2, h /= 2)
        ++levels;
    if (levels == 0 && !(supersample && sw > dst_w && sh > dst_h)) return src;
    if (c.serial != serial) c.built = 0;
    SDL_BlendMode draw_bm = SDL_BLENDMODE_NONE;
    SDL_GetTextureBlendMode(src, &draw_bm);   // how the result is drawn
    for (int i = 0; i < levels; ++i) {
        int lw = std::max(1, sw / 2), lh = std::max(1, sh / 2);
        if (!c.tex[i] || c.w[i] != lw || c.h[i] != lh) {
            if (c.tex[i]) SDL_DestroyTexture(c.tex[i]);
            c.tex[i] = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_TARGET, lw, lh);
            c.w[i] = lw; c.h[i] = lh;
            c.built = std::min(c.built, i);
            if (!c.tex[i]) return src;   // no render targets: plain bilinear
            SDL_SetTextureScaleMode(c.tex[i], SDL_ScaleModeLinear);
        }
        if (i >= c.built) {
            SDL_Texture* prev = SDL_GetRenderTarget(sdl_renderer_);
            SDL_BlendMode bm = SDL_BLENDMODE_NONE;
            SDL_GetTextureBlendMode(src, &bm);
            SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
            SDL_SetRenderTarget(sdl_renderer_, c.tex[i]);
            SDL_RenderCopy(sdl_renderer_, src, nullptr, nullptr);
            SDL_SetRenderTarget(sdl_renderer_, prev);
            SDL_SetTextureBlendMode(src, bm);
            c.built = i + 1;
        }
        SDL_SetTextureBlendMode(c.tex[i], draw_bm);
        src = c.tex[i];
        sw = lw; sh = lh;
    }
    c.serial = serial;
    if (!supersample || (sw == dst_w && sh == dst_h)) return src;

    if (!c.out || c.out_w != dst_w || c.out_h != dst_h) {
        if (c.out) SDL_DestroyTexture(c.out);
        c.out = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                  dst_w, dst_h);
        c.out_w = dst_w; c.out_h = dst_h;
        c.out_serial = ~0ull;
        if (!c.out) return src;
        SDL_SetTextureScaleMode(c.out, SDL_ScaleModeNearest);   // drawn 1:1
    }
    if (c.out_serial != serial) {
        // Running average of four copies shifted by a quarter pixel: the
        // first replaces, the next ones blend in at 1/2, 1/3, 1/4.
        static const float kOff[4][2] = {{-0.25f, -0.25f}, {0.25f, -0.25f}, {-0.25f, 0.25f}, {0.25f, 0.25f}};
        static const Uint8 kAlpha[4] = {255, 128, 85, 64};
        SDL_Texture* prev = SDL_GetRenderTarget(sdl_renderer_);
        SDL_BlendMode bm = SDL_BLENDMODE_NONE;
        SDL_GetTextureBlendMode(src, &bm);
        Uint8 am = 255;
        SDL_GetTextureAlphaMod(src, &am);
        SDL_SetRenderTarget(sdl_renderer_, c.out);
        for (int i = 0; i < 4; ++i) {
            SDL_SetTextureBlendMode(src, i == 0 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(src, kAlpha[i]);
            SDL_FRect d = {kOff[i][0], kOff[i][1], (float)dst_w, (float)dst_h};
            SDL_RenderCopyF(sdl_renderer_, src, nullptr, &d);
        }
        SDL_SetRenderTarget(sdl_renderer_, prev);
        SDL_SetTextureBlendMode(src, bm);
        SDL_SetTextureAlphaMod(src, am);
        c.out_serial = serial;
    }
    SDL_SetTextureBlendMode(c.out, draw_bm);
    return c.out;
}


void Renderer::layout_side_buttons_() {
    for (int i = 0; i < kSideBtnCount; ++i) {
        side_btn_[i] = side_btn_hit_[i] = {};
        side_btn_edge_[i] = 0;
    }
    log_btn_ = stats_btn_ = webcam_btn_ = {};
    if (frame_dst_w_ <= 0 || phone_frame_.frame_width() <= 0) return;

    const float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    const int fx = frame_dst_x_, fy = frame_dst_y_, fw = frame_dst_w_, fh = frame_dst_h_;
    // m: how far a button sticks out. tuck: how far it reaches under the
    // chassis, so only its outer corners look rounded.
    const int m = side_btn_margin(std::min(fw, fh));
    const int tuck = std::max(2, m / 2);

    // A button sticking out of `edge` (0 left, 1 right, 2 top, 3 bottom),
    // `len` pixels long starting at `start` along that edge. `grab` is how
    // far its click target reaches into the bezel.
    auto place = [&](int i, int edge, int start, int len, int grab) {
        BtnRect r, h;
        switch (edge) {
            case 0:  r = {fx - m, start, m + tuck, len};
                     h = {r.x, start, m + tuck + grab, len}; break;
            case 1:  r = {fx + fw - tuck, start, m + tuck, len};
                     h = {r.x - grab, start, m + tuck + grab, len}; break;
            case 2:  r = {start, fy - m, len, m + tuck};
                     h = {start, r.y, len, m + tuck + grab}; break;
            default: r = {start, fy + fh - tuck, len, m + tuck};
                     h = {start, r.y - grab, len, m + tuck + grab}; break;
        }
        side_btn_[i] = r;
        side_btn_hit_[i] = h;
        side_btn_edge_[i] = edge;
    };
    // The same button sitting inside the bezel, centred on (cx, cy): used
    // while a drawer covers the edge it normally sticks out of.
    auto place_inside = [&](int i, int edge, int cx, int cy, int len, int thick) {
        BtnRect r = (edge <= 1) ? BtnRect{cx - thick / 2, cy - len / 2, thick, len}
                                : BtnRect{cx - len / 2, cy - thick / 2, len, thick};
        int grow = std::max(4, thick);
        side_btn_[i] = r;
        side_btn_hit_[i] = {r.x - grow, r.y - grow, r.w + grow * 2, r.h + grow * 2};
        side_btn_edge_[i] = edge;
    };

    const bool log_edge_free    = log_panel_anim_ <= 0.01f;     // right edge
    const bool webcam_edge_free = webcam_drawer_anim_ <= 0.01f; // bottom edge
    const int bezel_r = std::max(4, fw - (int)((phone_frame_.screen_x() + phone_frame_.screen_width()) * scale));
    const int bezel_t = std::max(4, (int)(phone_frame_.screen_y() * scale));

    // The phone's own buttons.
    if (phone_frame_.has_side_buttons()) {
        const bool horizontal = phone_frame_.side_buttons_horizontal();
        // In portrait the click target reaches into the (empty) side bezel,
        // because the visible part is only a few pixels thick. Not in
        // landscape: the top and bottom bezels hold other controls.
        const int grab = horizontal ? 0 : std::max(4, (int)(phone_frame_.bezel_left() * scale));
        // Portrait: volume on the left, power on the right. Landscape (top
        // of the phone to the left): volume along the bottom, power on top.
        auto phys = [&](int i, PhoneFrame::SideButton sb, bool volume_side) {
            int along = (int)std::round(sb.y * scale);
            int len   = std::max(6, (int)std::round(sb.h * scale));
            if (horizontal) place(i, volume_side ? 3 : 2, fx + along, len, grab);
            else            place(i, volume_side ? 0 : 1, fy + along, len, grab);
        };
        if (!horizontal || webcam_edge_free) {
            phys(0, phone_frame_.side_action(), true);
            phys(1, phone_frame_.side_volume_up(), true);
            phys(2, phone_frame_.side_volume_down(), true);
        }
        if (horizontal || log_edge_free) phys(3, phone_frame_.side_power(), false);
    }

    // Log, statistics and webcam (4-6) open from Settings now: no handles.
    (void)place_inside;
    (void)bezel_r;

    // Always-on-top pin: top edge, just past the top-left corner curve.
    {
        const int corner = (int)(phone_frame_.corner_radius() * scale);
        const int len = std::max(18, fw / 10);
        place(7, 2, fx + std::max(corner + 2, fw * 14 / 100), len, bezel_t / 2);
    }

    log_btn_    = side_btn_hit_[4];
    stats_btn_  = side_btn_hit_[5];
    webcam_btn_ = side_btn_hit_[6];
}

void Renderer::draw_side_buttons_(std::string& hover_key, std::string& hover_text,
                                  int& hover_ax, int& hover_ay) {
    uint8_t br, bg, bb;
    phone_frame_.button_color(&br, &bg, &bb);
    const bool android = android_input_active_();
    const char* const android_tips[4] = {tr("Mute phone"), tr("Volume up"), tr("Volume down"), tr("Power")};
    const char* const other_tips[4] = {
        tr("Mute: use the phone"), tr("Volume: use the phone"), tr("Volume: use the phone"), tr("Power: use the phone")};
    const bool drawer_open = log_panel_visible_ && log_panel_anim_ > 0.5f;
    const bool active[kSideBtnCount] = {
        false, false, false, false,
        drawer_open && drawer_mode_ == DrawerMode::Log,
        drawer_open && drawer_mode_ == DrawerMode::Stats,
        webcam_drawer_visible_,
        settings_.always_on_top,
    };

    int mx, my;
    SDL_GetMouseState(&mx, &my);
    for (int i = 0; i < kSideBtnCount; ++i) {
        const BtnRect& b = side_btn_[i];
        if (b.w <= 0) continue;
        const BtnRect& hit = side_btn_hit_[i];
        const int edge = side_btn_edge_[i];
        const bool along_x = edge >= 2;              // lies along the top / bottom edge
        const bool far_edge = (edge == 1 || edge == 3);
        const bool hover = in_rect(mx, my, hit.x, hit.y, hit.w, hit.h);
        auto shade = [&](uint8_t c, int d) { return (uint8_t)std::min(255, std::max(0, c + d)); };
        const int lift = hover ? 45 : (active[i] ? 30 : 0);
        const int rad = std::max(1, (along_x ? b.h : b.w) / 3);

        // Body. The pin turns green while the window is kept on top.
        uint8_t r = br, g = bg, bl = bb;
        if (i == 7 && active[i]) { r = 52; g = 199; bl = 112; }
        SDL_SetRenderDrawColor(sdl_renderer_, shade(r, lift), shade(g, lift), shade(bl, lift), 255);
        fill_round_rect(sdl_renderer_, b.x, b.y, b.w, b.h, rad);
        // Thin highlight on the outer face for a machined-metal look.
        SDL_SetRenderDrawColor(sdl_renderer_, shade(r, lift + 30), shade(g, lift + 30),
                               shade(bl, lift + 32), 255);
        if (along_x) {
            int edge_y = far_edge ? b.y + b.h - 1 : b.y;
            SDL_RenderDrawLine(sdl_renderer_, b.x + rad, edge_y, b.x + b.w - 1 - rad, edge_y);
        } else {
            int edge_x = far_edge ? b.x + b.w - 1 : b.x;
            SDL_RenderDrawLine(sdl_renderer_, edge_x, b.y + rad, edge_x, b.y + b.h - 1 - rad);
        }

        if (hover) {
            switch (i) {
                case 4: hover_key = "log";   hover_text = "Show log (L)"; break;
                case 5: hover_key = "stats"; hover_text = "Show statistics (T)"; break;
                case 6:
                    hover_key = "webcam";
                    hover_text = std::string(webcam_drawer_visible_ ? "Hide webcam (W)" : "Show webcam (W)") +
                                 "\nRight-click: pick camera";
                    break;
                case 7:
                    hover_key = "pin";
                    hover_text = settings_.always_on_top ? "Always on top: on" : "Always on top: off";
                    break;
                default:
                    hover_key  = std::string("side") + (char)('0' + i);
                    hover_text = android ? android_tips[i] : other_tips[i];
                    break;
            }
            switch (edge) {
                case 0:  hover_ax = hit.x + hit.w;      hover_ay = b.y + b.h / 2; break;
                case 1:  hover_ax = hit.x;              hover_ay = b.y + b.h / 2; break;
                case 2:  hover_ax = b.x + b.w / 2;      hover_ay = hit.y + hit.h; break;
                default: hover_ax = b.x + b.w / 2;      hover_ay = hit.y; break;
            }
        }
    }
}

void Renderer::press_side_button_(int index) {
    if (index < 0 || index > 3) return;
    if (android_input_active_()) {
        // KEYCODE_VOLUME_MUTE, VOLUME_UP, VOLUME_DOWN, POWER
        static const int codes[4] = {164, 24, 25, 26};
        AndroidInput in;
        in.kind = AndroidInput::Kind::Key;
        in.keycode = codes[index];
        in.action = 0; android_input_fn_(in);
        in.action = 1; android_input_fn_(in);
        return;
    }
    // AirPlay is one-way: nothing to send to an iPhone.
    if (active_platform_fn_ && !active_platform_fn_().empty()) {
        toast_text_ = "AirPlay is view-only - use the buttons on the phone";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
    }
}

bool Renderer::android_input_active_() {
    if (!android_input_fn_ || !active_platform_fn_ || !texture_ || !ever_received_frame_)
        return false;
    return active_platform_fn_() == "Android";
}

// Where the mirrored video is drawn (same geometry as render_frame()).
bool Renderer::android_video_rect_(int* x, int* y, int* w, int* h) {
    if (frame_dst_w_ <= 0 || tex_width_ <= 0 || tex_height_ <= 0 ||
        phone_frame_.frame_width() <= 0)
        return false;
    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)std::round(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)std::round(phone_frame_.screen_y() * scale);
    int svw = (int)std::round(phone_frame_.screen_width() * scale);
    int svh = (int)std::round(phone_frame_.screen_height() * scale);
    int vx = svx - 1, vy = svy - 1, vw = svw + 2, vh = svh + 2;
    if (vw <= 0 || vh <= 0) return false;
    float tex_ar = (float)tex_width_ / (float)tex_height_;
    float rect_ar = (float)vw / (float)vh;
    if (std::abs(tex_ar - rect_ar) > 0.05f * rect_ar) {
        int fw, fh;
        if (tex_ar > rect_ar) { fw = vw; fh = (int)std::round(vw / tex_ar); }
        else                  { fh = vh; fw = (int)std::round(vh * tex_ar); }
        vx += (vw - fw) / 2; vy += (vh - fh) / 2; vw = fw; vh = fh;
    }
    if (vw <= 0 || vh <= 0) return false;
    *x = vx; *y = vy; *w = vw; *h = vh;
    return true;
}

void Renderer::android_send_touch_(int action, int mx, int my) {
    int rx, ry, rw, rh;
    if (!android_input_fn_ || !android_video_rect_(&rx, &ry, &rw, &rh)) return;
    AndroidInput in;
    in.kind = AndroidInput::Kind::Touch;
    in.action = action;
    in.x = std::max(0, std::min(tex_width_ - 1, (mx - rx) * tex_width_ / rw));
    in.y = std::max(0, std::min(tex_height_ - 1, (my - ry) * tex_height_ / rh));
    in.frame_w = tex_width_;
    in.frame_h = tex_height_;
    android_input_fn_(in);
}

// Returns true when the key event was consumed (sent to the phone).
bool Renderer::android_forward_key_(const SDL_Event& ev) {
    if (android_panel_visible_ || !android_input_active_()) return false;
    const bool down = (ev.type == SDL_KEYDOWN);
    const SDL_Keycode k = ev.key.keysym.sym;
    const uint16_t mod = ev.key.keysym.mod;
    const bool ctrl = (mod & KMOD_CTRL) != 0;
    const bool alt  = (mod & KMOD_ALT) != 0;

    auto key = [&](int action, int code) {
        AndroidInput in;
        in.kind = AndroidInput::Kind::Key;
        in.action = action;
        in.keycode = code;
        android_input_fn_(in);
    };
    auto tap = [&](int code) { key(0, code); key(1, code); };
    auto toast = [&](const char* s) {
        toast_text_ = s;
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
    };

    // Shortcuts that work whenever an Android phone is mirrored.
    if (ctrl && !alt) {
        switch (k) {
            case SDLK_k:
                if (down && !ev.key.repeat) {
                    android_kbd_on_ = !android_kbd_on_;
                    android_ime_shown_.clear();
                    if (android_kbd_on_) SDL_StartTextInput(); else SDL_StopTextInput();
                    toast(android_kbd_on_ ? "Keyboard -> phone: ON (Ctrl+K to turn off)"
                                          : "Keyboard -> phone: OFF");
                }
                return true;
            case SDLK_b: if (down && !ev.key.repeat) tap(4);   return true;  // BACK
            case SDLK_h: if (down && !ev.key.repeat) tap(3);   return true;  // HOME
            case SDLK_e: if (down && !ev.key.repeat) tap(187); return true;  // APP_SWITCH
            case SDLK_p: if (down && !ev.key.repeat) tap(26);  return true;  // POWER
            case SDLK_o:
                // Phone display off while the mirror keeps running.
                if (down && !ev.key.repeat) {
                    android_screen_off_ = !android_screen_off_;
                    AndroidInput in;
                    in.kind = AndroidInput::Kind::ScreenPower;
                    in.action = android_screen_off_ ? 0 : 1;
                    android_input_fn_(in);
                    toast(android_screen_off_ ? "Phone screen: OFF, still mirroring (Ctrl+O to turn on)"
                                              : "Phone screen: ON");
                }
                return true;
            case SDLK_v:
                if (down && !ev.key.repeat) {
                    android_ime_shown_.clear();
                    char* clip = SDL_GetClipboardText();
                    if (clip && *clip) {
                        AndroidInput in;
                        in.kind = AndroidInput::Kind::Clipboard;
                        in.text = clip;
                        android_input_fn_(in);
                    }
                    if (clip) SDL_free(clip);
                }
                return true;
            default: return false;   // Ctrl+S / Ctrl+R / ... stay with the app
        }
    }

    if (!android_kbd_on_) return false;   // plain keys keep their app shortcuts
    SDL_StartTextInput();                 // the panel may have switched it off
    if (ctrl || alt) return false;

    int code = 0;
    switch (k) {
        case SDLK_RETURN: case SDLK_KP_ENTER: code = 66; break;
        case SDLK_BACKSPACE: code = 67; break;
        case SDLK_TAB:       code = 61; break;
        case SDLK_ESCAPE:    code = 4;  break;
        case SDLK_DELETE:    code = 112; break;
        case SDLK_UP:        code = 19; break;
        case SDLK_DOWN:      code = 20; break;
        case SDLK_LEFT:      code = 21; break;
        case SDLK_RIGHT:     code = 22; break;
        case SDLK_HOME:      code = 122; break;
        case SDLK_END:       code = 123; break;
        case SDLK_PAGEUP:    code = 92; break;
        case SDLK_PAGEDOWN:  code = 93; break;
        default: break;
    }
    if (code) key(down ? 0 : 1, code);
    return true;   // printable keys arrive as SDL_TEXTINPUT
}

void Renderer::android_usb_start() {
    if (!android_usb_fn_) {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = "USB handler not wired.";
        android_status_is_error_ = true;
        return;
    }
    {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = "Looking for a phone on USB...";
        android_status_is_error_ = false;
        android_busy_ = true;
    }
    auto fn = android_usb_fn_;
    std::thread([this, fn]() {
        std::string r = fn();
        bool err = (r.rfind("Mirroring ", 0) != 0);
        if (err) std::cerr << "[Android] " << r << "\n";
        else     std::cout << "[Android] " << r << "\n";
        std::lock_guard lk(android_status_mutex_);
        android_status_ = r;
        android_status_is_error_ = err;
        android_busy_ = false;
        if (!err) {
            android_panel_visible_ = false;
            android_panel_animating_ = true;
            android_panel_anim_start_ = std::chrono::steady_clock::now();
        }
    }).detach();
}

void Renderer::android_submit() {
    auto fail = [&](const std::string& m) {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = m;
        android_status_is_error_ = true;
    };
    if (!valid_ipv4(android_ip_))      return fail("Invalid IP address.");
    // Both pair fields empty = connect an already-paired phone directly.
    bool connect_only = android_port_.empty() && android_pin_.empty();
    if (connect_only) {
        if (android_connect_port_.empty()) return fail("Connect port required.");
    } else {
        if (android_port_.empty())      return fail("Pair port required.");
        if (android_pin_.size() != 6)   return fail("PIN must be 6 digits.");
    }
    if (!android_connect_fn_)           return fail("Connect handler not wired.");
    if (!android_connect_port_.empty()) {
        settings_.android_last_wifi = android_ip_ + ":" + android_connect_port_;
        settings_.save();
    }

    {
        std::lock_guard lk(android_status_mutex_);
        android_status_ = "Working...";
        android_status_is_error_ = false;
        android_busy_ = true;
    }
    std::string ip   = android_ip_;
    std::string port = android_port_;
    std::string pin  = android_pin_;
    std::string cport = android_connect_port_;
    auto fn = android_connect_fn_;
    std::thread([this, ip, port, pin, cport, fn]() {
        std::string r = fn(ip, port, pin, cport);
        // Treat anything that doesn't start with the success prefix
        // ("Mirroring <serial>") as an error. Keyword matching missed
        // cases like "Paired but device did not appear via mDNS..." and
        // auto-closed the panel before the user could read the message.
        bool err = (r.rfind("Mirroring ", 0) != 0);
        // Mirror the result into the log so the user can review long
        // failure messages there too (the panel wraps but is dismissed on
        // success, and the toast disappears quickly).
        if (err) std::cerr << "[Android] " << r << "\n";
        else     std::cout << "[Android] " << r << "\n";
        std::lock_guard lk(android_status_mutex_);
        android_status_ = r;
        android_status_is_error_ = err;
        android_busy_ = false;
        // On success, auto-close the pair panel so the projected phone
        // becomes visible immediately.
        if (!err) {
            android_panel_visible_ = false;
            android_panel_animating_ = true;
            android_panel_anim_start_ = std::chrono::steady_clock::now();
        }
    }).detach();
}

void Renderer::draw_android_panel() {
    if (frame_dst_w_ == 0) return;

    // Animation
    const float dur = 200.0f;
    if (android_panel_animating_) {
        float el = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - android_panel_anim_start_).count();
        float t = std::min(1.0f, el / dur);
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        android_panel_anim_ = android_panel_visible_ ? e : 1.0f - e;
        if (t >= 1.0f) android_panel_animating_ = false;
    } else {
        android_panel_anim_ = android_panel_visible_ ? 1.0f : 0.0f;
    }
    if (android_panel_anim_ <= 0.0f) {
        // Panel fully hidden — stop the discovery worker if it's still
        // running from the previous open. Guard on `visible_` so that
        // the very first frame after show_android_panel() (anim still
        // at 0) doesn't immediately tear down the worker that
        // show_android_panel() just spawned.
        if (!android_panel_visible_) stop_android_discovery();
        if (!android_panel_visible_) return;
    }

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int svx = frame_dst_x_ + (int)(phone_frame_.screen_x() * scale);
    int svy = frame_dst_y_ + (int)(phone_frame_.screen_y() * scale);
    int svw = (int)(phone_frame_.screen_width() * scale);
    int svh = (int)(phone_frame_.screen_height() * scale);

    if (android_qr_active_.load()) {
        draw_android_qr(svx, svy, svw, svh);
        return;
    }

    // Sizes follow the phone-equivalent reference (ui_ref_width) so the
    // dialog stays compact on tablet/desktop sources.
    int uw = ui_ref_width();
    int panel_w = std::min((int)(svw * 0.88f), (int)(uw * 1.7f));
    int pad     = std::max(14, panel_w / 18);
    int label_h = std::max(12, panel_w / 30);
    int field_h = std::max(32, panel_w / 10);
    int btn_h   = std::max(36, panel_w / 9);
    int gap     = std::max(8, pad * 2 / 3);
    int title_h = std::max(16, panel_w / 20);
    int tile    = title_h * 9 / 5;            // header icon tile
    int round_b = title_h + 10;               // help / close buttons

    // Status text, word-wrapped to the panel width (errors can be long).
    std::vector<std::string> status_lines;
    bool status_err = false;
    {
        std::string st;
        {
            std::lock_guard lk(android_status_mutex_);
            st = android_status_;
            status_err = android_status_is_error_;
        }
        int max_w = panel_w - pad * 2;
        std::string line, word;
        auto flush_word = [&]() {
            if (word.empty()) return;
            std::string trial = line.empty() ? word : line + " " + word;
            int tw = 0, th = 0;
            measure_text_c(sdl_renderer_, trial, label_h, &tw, &th);
            if (tw <= max_w || line.empty()) line = trial;
            else { status_lines.push_back(line); line = word; }
            word.clear();
        };
        for (char c : st) {
            if (c == ' ' || c == '\n') flush_word();
            else word.push_back(c);
        }
        flush_word();
        if (!line.empty()) status_lines.push_back(line);
    }

    // Discovered devices, snapshotted once so layout and drawing agree.
    std::vector<DiscoveredAndroidDevice> discovered_snapshot;
    {
        std::lock_guard lk(android_discovered_mutex_);
        discovered_snapshot = android_discovered_;
    }
    const int max_discover_rows = 3;
    int discover_visible = std::min<int>((int)discovered_snapshot.size(), max_discover_rows);
    int dev_row_h = std::max(field_h + 6, label_h * 3);
    int near_head_h = label_h + 14;
    int near_body_h = discover_visible > 0 ? discover_visible * dev_row_h : label_h + 16;
    int near_h = 6 + near_head_h + near_body_h + 6;

    int field_block = 0, status_h = 0, disc_h = 0, total_h = 0;
    auto layout = [&]() {
        near_body_h = discover_visible > 0 ? discover_visible * dev_row_h : label_h + 16;
        near_h = 6 + near_head_h + near_body_h + 6;
        field_block = label_h + 7 + field_h;
        status_h = status_lines.empty() ? 0 : (int)status_lines.size() * (label_h + 4) + gap;
        disc_h = label_h + 14;
        total_h = pad + std::max(tile, round_b) + gap + near_h + gap
                + field_block * 2 + gap + gap + status_h
                + btn_h + gap + btn_h + 4 + disc_h + pad / 2;
    };
    layout();
    // Small windows: list fewer nearby phones, then shrink the controls, so
    // the panel stays inside the phone screen.
    const int fit_h = svh - std::max(8, svh / 28) - 16;
    while (total_h > fit_h && discover_visible > 1) { --discover_visible; layout(); }
    if (total_h > fit_h) {
        float k = std::max(0.6f, (float)fit_h / total_h);
        field_h = (int)(field_h * k); btn_h = (int)(btn_h * k); dev_row_h = (int)(dev_row_h * k);
        gap = std::max(4, (int)(gap * k)); pad = std::max(8, (int)(pad * k));
        layout();
    }

    int panel_x = svx + (svw - panel_w) / 2;
    int bottom_margin = std::max(8, svh / 28);
    int panel_y = svy + (svh - total_h) / 2 + svh / 18;
    if (panel_y + total_h > svy + svh - bottom_margin)
        panel_y = svy + svh - total_h - bottom_margin;
    if (panel_y < svy + 8) panel_y = svy + 8;
    panel_y -= (int)((1.0f - android_panel_anim_) * 30);
    uint8_t text_alpha = (uint8_t)(255 * android_panel_anim_);

    android_panel_rect_ = {panel_x, panel_y, panel_w, total_h};

    int mx, my;
    SDL_GetMouseState(&mx, &my);
    const Accent& ac = accent_for(settings_.accent_color);
    auto hov = [&](const BtnRect& r) { return in_rect(mx, my, r.x, r.y, r.w, r.h); };

    ui_shell_(panel_x, panel_y, panel_w, total_h, std::max(14, pad));
    int cy = panel_y + pad;
    int left = panel_x + pad, inner_w = panel_w - pad * 2;

    // Header: icon tile, title, help + close.
    {
        int hh = std::max(tile, round_b), mid = cy + hh / 2;
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl_renderer_, ac.r, ac.g, ac.b, 42);
        fill_round_rect(sdl_renderer_, left, mid - tile / 2, tile, tile, tile / 3);
        draw_glyph(sdl_renderer_, lucide::Smartphone, left + tile / 2, mid, tile / 2,
                   ac.lr, ac.lg, ac.lb, text_alpha);
        android_close_btn_ = {panel_x + panel_w - pad - round_b, mid - round_b / 2, round_b, round_b};
        android_help_btn_  = {android_close_btn_.x - round_b - 6, mid - round_b / 2, round_b, round_b};
        int title_x = left + tile + tile / 3;
        draw_text_mid(sdl_renderer_, tr("Add Android phone"),
                      fit_text_px(sdl_renderer_, tr("Add Android phone"), title_h,
                                  android_help_btn_.x - 6 - title_x, 700),
                      244, 246, 248, text_alpha, title_x, mid, 0, 700);
        ui_round_btn_(android_help_btn_, lucide::CircleHelp, hov(android_help_btn_));
        ui_round_btn_(android_close_btn_, lucide::X, hov(android_close_btn_));
        cy += hh + gap;
    }

    // Nearby: phones found over mDNS (Wireless debugging on).
    android_discover_btns_.clear();
    {
        int card_r = std::max(10, pad * 3 / 4);
        SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sdl_renderer_, 30, 33, 40, 255);
        fill_round_rect(sdl_renderer_, left - 1, cy - 1, inner_w + 2, near_h + 2, card_r + 1);
        SDL_SetRenderDrawColor(sdl_renderer_, 17, 19, 24, 255);
        fill_round_rect(sdl_renderer_, left, cy, inner_w, near_h, card_r);

        int hy = cy + 6 + near_head_h / 2;
        draw_text_mid(sdl_renderer_, tr("NEARBY"), label_h * 85 / 100, 127, 138, 156, text_alpha,
                      left + 14, hy, 0, 700);
        if (android_discover_running_.load()) {
            // "Scanning" + three accent dots blinking in turn.
            int dot_r = std::max(2, label_h / 6), step = dot_r * 3;
            int dx = left + inner_w - 14 - step * 2;
            uint32_t ms = SDL_GetTicks();
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
            for (int i = 0; i < 3; ++i) {
                float ph = std::fmod(ms / 1400.0f - i * 0.15f, 1.0f);
                if (ph < 0) ph += 1.0f;
                uint8_t a = (uint8_t)(70 + 185 * std::max(0.0f, 1.0f - std::abs(ph - 0.4f) * 3.0f));
                SDL_SetRenderDrawColor(sdl_renderer_, ac.lr, ac.lg, ac.lb, a);
                fill_circle(sdl_renderer_, dx + i * step, hy, dot_r);
            }
            draw_text_mid(sdl_renderer_, tr("Scanning"), label_h * 9 / 10, 127, 138, 156, text_alpha,
                          dx - step, hy, 2);
        }
        int ry = cy + 6 + near_head_h;
        if (discovered_snapshot.empty()) {
            draw_text_mid(sdl_renderer_, tr("No phones yet. Turn on Wireless debugging."),
                          label_h * 9 / 10, 127, 138, 156, text_alpha, left + 14,
                          ry + near_body_h / 2, 0);
        }
        for (int i = 0; i < discover_visible; ++i) {
            const auto& dev = discovered_snapshot[i];
            BtnRect row{left + 6, ry, inner_w - 12, dev_row_h};
            android_discover_btns_.push_back(row);
            bool selected = !android_ip_.empty() && dev.ip == android_ip_ &&
                            (android_connect_port_.empty() || dev.connect_port == android_connect_port_);
            if (selected || hov(row)) {
                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                if (selected) SDL_SetRenderDrawColor(sdl_renderer_, ac.r, ac.g, ac.b, 34);
                else          SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 12);
                fill_round_rect(sdl_renderer_, row.x, row.y, row.w, row.h, std::max(8, card_r - 4));
            }
            int it = dev_row_h * 62 / 100, imid = row.y + row.h / 2;
            SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(sdl_renderer_, 156, 201, 232, 30);
            fill_round_rect(sdl_renderer_, row.x + 8, imid - it / 2, it, it, it / 3);
            draw_glyph(sdl_renderer_, lucide::Wifi, row.x + 8 + it / 2, imid, it / 2,
                       207, 228, 244, text_alpha);
            int tx = row.x + 8 + it + 12;
            std::string name = dev.label.empty() ? std::string("Android phone") : dev.label;
            std::string detail = dev.ip;
            if (!dev.connect_port.empty()) detail += ":" + dev.connect_port;
            if (!dev.pair_port.empty()) detail += "  - ready to pair";
            draw_text_mid(sdl_renderer_, name, label_h, 244, 246, 248, text_alpha,
                          tx, imid - label_h * 6 / 10, 0, 600);
            draw_text_mid(sdl_renderer_, detail, label_h * 85 / 100, 127, 138, 156, text_alpha,
                          tx, imid + label_h * 6 / 10, 0);
            draw_glyph(sdl_renderer_, lucide::ChevronRight, row.x + row.w - 16, imid, label_h,
                       127, 138, 156, text_alpha);
            ry += dev_row_h;
        }
        cy += near_h + gap;
    }

    // Fields: two columns (IP + connect port, pair port + pairing code).
    {
        int col_w = (inner_w - gap) / 2;
        auto field = [&](int idx, const char* label, bool required, const std::string& val,
                         const char* placeholder, int fx, int fy) {
            int lw = 0, lh = 0;
            measure_text_c(sdl_renderer_, label, label_h, &lw, &lh, 600);
            draw_text_mid(sdl_renderer_, label, label_h, 154, 167, 186, text_alpha,
                          fx + 2, fy + label_h / 2, 0, 600);
            if (required) {
                SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(sdl_renderer_, ac.lr, ac.lg, ac.lb, 255);
                fill_circle(sdl_renderer_, fx + 2 + lw + 4, fy + label_h / 2, std::max(2, label_h / 6));
            }
            BtnRect fr{fx, fy + label_h + 7, col_w, field_h};
            android_field_rects_[idx] = fr;
            ui_field_(fr, val, placeholder, android_focus_ == idx, text_alpha);
        };
        field(0, tr("IP address"),   true,  android_ip_,           "192.168.1.20", left,             cy);
        field(1, tr("Connect port"), false, android_connect_port_, "46029",        left + col_w + gap, cy);
        cy += field_block + gap;
        field(2, tr("Pair port"),    false, android_port_,         "42379",        left,             cy);
        field(3, tr("Pairing code"), false, android_pin_,          "123456",       left + col_w + gap, cy);
        cy += field_block + gap;
    }

    // Status (errors in red).
    if (!status_lines.empty()) {
        cy += gap / 2;
        for (auto& ln : status_lines) {
            if (status_err) draw_text_mid(sdl_renderer_, ln, label_h, 255, 138, 143, text_alpha,
                                          left + 2, cy + label_h / 2, 0);
            else            draw_text_mid(sdl_renderer_, ln, label_h, 154, 167, 186, text_alpha,
                                          left + 2, cy + label_h / 2, 0);
            cy += label_h + 4;
        }
        cy += gap / 2;
    }

    // Buttons: QR / USB, Connect, Disconnect.
    {
        int bw = (inner_w - gap) / 2;
        android_qr_btn_  = {left, cy, bw, btn_h};
        android_usb_btn_ = {left + bw + gap, cy, bw, btn_h};
        ui_pill_(android_qr_btn_, tr("QR code"), lucide::QrCode, false, hov(android_qr_btn_), text_alpha);
        ui_pill_(android_usb_btn_, tr("USB cable"), lucide::Usb, false, hov(android_usb_btn_), text_alpha);
        cy += btn_h + gap;
        android_connect_btn_ = {left, cy, inner_w, btn_h + 4};
        ui_pill_(android_connect_btn_, android_busy_ ? tr("Connecting...") : tr("Connect"), 0, true,
                 hov(android_connect_btn_), text_alpha);
        cy += btn_h + 4;
        int dw = 0, dh = 0;
        measure_text_c(sdl_renderer_, tr("Disconnect"), label_h, &dw, &dh, 600);
        android_disconnect_btn_ = {panel_x + (panel_w - dw) / 2 - 10, cy, dw + 20, disc_h};
        bool dh_ = hov(android_disconnect_btn_);
        draw_text_mid(sdl_renderer_, tr("Disconnect"), label_h, dh_ ? 255 : 138, dh_ ? 138 : 148,
                      dh_ ? 143 : 166, text_alpha, panel_x + panel_w / 2, cy + disc_h / 2, 1, 600);
    }
}

bool Renderer::emit_screenshot_output(const uint8_t* rgba, int w, int h, int stride,
                                      const std::string& filename,
                                      const std::string& snagit_path,
                                      bool save, bool clip, bool snag) {
    std::vector<uint8_t> resized;
    const uint8_t* out_rgba = rgba;
    int out_w = w, out_h = h, out_stride = stride;
    std::string out_filename = filename;
    int out_dpi = 0; // 0 = no DPI metadata written (unresized path)
    if (settings_.screenshot_resize_enabled && settings_.screenshot_resize_height_cm >= 0.5f && h > 0) {
        // Render at print-quality DPI and embed that same DPI in the file
        // (pHYs chunk) so Office reports the correct physical height from
        // the metadata instead of assuming 96 DPI — this is what keeps the
        // image looking sharp instead of soft/blocky at the target size.
        const int dpi = 300;
        int target_h = std::max(1, (int)std::lround(settings_.screenshot_resize_height_cm / 2.54 * dpi));
        int target_w = std::max(1, (int)std::lround((double)w * target_h / (double)h));
        resized = resize_rgba_bilinear(rgba, w, h, stride, target_w, target_h);
        out_rgba = resized.data();
        out_w = target_w; out_h = target_h; out_stride = target_w * 4;
        out_dpi = dpi;
        char tag[32];
        float cm = settings_.screenshot_resize_height_cm;
        if (std::abs(cm - std::round(cm)) < 0.01f)
            snprintf(tag, sizeof(tag), "-h%dcm", (int)std::lround(cm));
        else
            snprintf(tag, sizeof(tag), "-h%.1fcm", cm);
        auto dot = filename.rfind('.');
        out_filename = (dot == std::string::npos) ? (filename + tag)
                                                    : (filename.substr(0, dot) + tag + filename.substr(dot));
    }

    bool wrote = true;
    if (save) {
        wrote = write_png_file_dpi(out_filename, out_w, out_h, out_rgba, out_stride, out_dpi);
        if (wrote && out_filename != filename && settings_.screenshot_save_original_too) {
            stbi_write_png(filename.c_str(), w, h, 4, rgba, stride);
        }
    } else if (snag && !snagit_path.empty()) {
        wrote = write_png_file_dpi(snagit_path, out_w, out_h, out_rgba, out_stride, out_dpi);
    }
    if (wrote) {
        if (clip) copy_to_clipboard(out_rgba, out_w, out_h, out_dpi);
        if (snag) open_in_snagit(save ? out_filename : snagit_path);
        if (save) std::cout << "[Screenshot] Saved: " << out_filename << " (" << out_w << "x" << out_h << ")\n";
        else if (clip) std::cout << "[Screenshot] Copied to clipboard (" << out_w << "x" << out_h << ")\n";
    }
    return wrote;
}

void Renderer::take_screenshot() {
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        std::cout << "[Screenshot] No frame data available\n";
        return;
    }

    const bool save = settings_.screenshot_save_to_folder;
    const bool clip = settings_.screenshot_copy_to_clipboard;
    const bool snag = settings_.screenshot_open_in_snagit;
    if (!save && !clip && !snag) {
        std::cout << "[Screenshot] All output options are disabled in settings\n";
        toast_text_ = "Screenshot disabled in Settings";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }

    if (save) std::filesystem::create_directories(screenshot_dir_);

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local_tm);
    // Primary filename in the user's screenshots folder. If "save" is off
    // but Snagit is on we write a transient copy into %TEMP% so Snagit
    // has something to open. Filename format is <date>_<time>_<kind>.png
    // so screenshots and annotated captures sort chronologically when
    // mixed in the same folder.
    std::string filename = screenshot_dir_ + "/" + timestamp + "_screenshot.png";
    std::string snagit_path; // path passed to Snagit when save==false
    if (!save && snag) {
        std::error_code ec;
        auto tmp = std::filesystem::temp_directory_path(ec);
        if (!ec) snagit_path = (tmp / ("PhoneMimi_" + std::string(timestamp) + ".png")).string();
    }

    bool saved = false;
    // M5 (WYSIWYG): if the webcam drawer is open, grow the screenshot
    // canvas downward and stitch in the same camera strip the user
    // sees on screen. Mirror toggle is honoured by paint_webcam_strip.
    const bool include_cam = should_include_webcam_in_capture();
    if (phone_frame_enabled_ && phone_frame_.is_generated()) {
        int out_w, out_h;
        uint8_t* composite = phone_frame_.composite_screenshot(
            sdl_renderer_,
            last_frame_data_.data(), last_frame_w_, last_frame_h_, last_frame_stride_,
            &out_w, &out_h);
        if (composite) {
            std::unique_ptr<uint8_t[]> composite_owned(composite);
            const uint8_t* save_rgba = composite;
            int save_w = out_w, save_h = out_h;
            std::vector<uint8_t> stitched;
            if (include_cam) {
                int panel_w = out_w - (out_w / 16) * 2;
                int panel_h = panel_w > 0 ? panel_w * 9 / 16 : 0;
                // Tuck the strip up behind the phone bezel by the corner
                // radius so the drawer connects flush to the phone (no
                // visible "air" band between the bezel curve and the
                // panel top).
                int overlap = panel_w > 0 ? std::max(0, panel_w / 30) : 0;
                if (panel_h > overlap) {
                    save_h = out_h + panel_h - overlap;
                    stitched.assign((size_t)out_w * save_h * 4, 0);
                    // Paint the webcam strip FIRST so its top rows sit
                    // inside the phone-bezel area, then composite the
                    // phone composite on top via alpha-over — the bezel
                    // hides the panel where opaque and lets the panel
                    // body bleed through the rounded corner cutouts.
                    paint_webcam_strip(stitched.data(), out_w,
                                       out_h - overlap, panel_h, overlap);
                    blit_phone_over_strip(stitched.data(), out_w * 4,
                                          composite, out_w * 4,
                                          out_w, out_h);
                    save_rgba = stitched.data();
                }
            }
            bool wrote = emit_screenshot_output(save_rgba, out_w, save_h, out_w * 4,
                                               filename, snagit_path, save, clip, snag);
            if (wrote) saved = true;
        }
    } else {
        const uint8_t* save_rgba = last_frame_data_.data();
        int save_w = last_frame_w_, save_h = last_frame_h_;
        int save_stride = last_frame_stride_;
        std::vector<uint8_t> stitched;
        if (include_cam) {
            int panel_w = save_w - (save_w / 16) * 2;
            int panel_h = panel_w > 0 ? panel_w * 9 / 16 : 0;
            if (panel_h > 0) {
                int new_h = save_h + panel_h;
                stitched.assign((size_t)save_w * new_h * 4, 0);
                for (int y = 0; y < save_h; ++y) {
                    std::memcpy(stitched.data() + (size_t)y * save_w * 4,
                                last_frame_data_.data() + (size_t)y * save_stride,
                                (size_t)save_w * 4);
                }
                paint_webcam_strip(stitched.data(), save_w, save_h, panel_h);
                save_rgba   = stitched.data();
                save_h      = new_h;
                save_stride = save_w * 4;
            }
        }
        bool wrote = emit_screenshot_output(save_rgba, save_w, save_h, save_stride,
                                           filename, snagit_path, save, clip, snag);
        if (wrote) {
            saved = true;
        }
    }

    if (saved) {
        std::string parts;
        auto add = [&](const char* p){ if (!parts.empty()) parts += " + "; parts += p; };
        if (save) add("saved to Pictures");
        if (clip) add("copied to clipboard");
        if (snag) add("opened in Snagit");
        toast_text_ = "Screenshot: " + parts;
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        opm::UsageLog::log_feature_use("screenshot", active_platform_fn_ ? active_platform_fn_() : std::string());
    }
}

void Renderer::open_screenshot_folder() {
#ifdef _WIN32
    std::filesystem::create_directories(screenshot_dir_);
    ShellExecuteA(nullptr, "explore", screenshot_dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

// ---- Screenshot annotation overlay (Ctrl+Shift+S) ----
//
// Snapshots the current composited frame (same buffer that take_screenshot
// would write), then enters a modal markup mode with a small floating
// toolbar: arrow / rectangle / highlight / pixelate, four colour swatches,
// undo, save and cancel. Save bakes the strokes back into the captured
// RGBA and re-uses the same write+clipboard logic as a plain screenshot.

namespace {

// Five-colour palette — kept tiny on purpose; v1 is a quick-markup tool,
// not a paint program. Order: red, yellow, green, black, white.
struct AnnoColor { uint8_t r, g, b; };
constexpr AnnoColor k_anno_palette[5] = {
    {235,  80,  80},  // red
    {245, 205,  60},  // yellow
    { 90, 200, 110},  // green
    { 20,  20,  20},  // black
    {245, 245, 245},  // white
};

// Pixelate block size in image-space pixels. 12 is coarse enough that
// faces / phone numbers / OTPs are unreadable but fine enough that the
// general layout is preserved.
constexpr int k_anno_pix_block = 12;

// Average a `block`x`block` patch of RGBA at (sx,sy) inside src into a
// packed 0xRRGGBB. Out-of-bounds reads are clamped.
inline uint32_t avg_block_rgb(const uint8_t* src, int w, int h, int stride,
                              int sx, int sy, int block) {
    uint32_t rsum = 0, gsum = 0, bsum = 0, n = 0;
    int x0 = std::max(0, sx);
    int y0 = std::max(0, sy);
    int x1 = std::min(w, sx + block);
    int y1 = std::min(h, sy + block);
    for (int yy = y0; yy < y1; ++yy) {
        const uint8_t* row = src + yy * stride;
        for (int xx = x0; xx < x1; ++xx) {
            const uint8_t* p = row + xx * 4;
            rsum += p[0]; gsum += p[1]; bsum += p[2]; ++n;
        }
    }
    if (n == 0) return 0;
    return ((rsum / n) << 16) | ((gsum / n) << 8) | (bsum / n);
}

// Bresenham-style thick line into RGBA. Used by the bake step so the
// annotated PNG matches what the user saw in the editor.
inline void plot_disc_rgba(uint8_t* rgba, int w, int h, int stride,
                           int cx, int cy, int rad,
                           uint8_t r, uint8_t g, uint8_t b) {
    int r2 = rad * rad;
    int x0 = std::max(0, cx - rad), x1 = std::min(w - 1, cx + rad);
    int y0 = std::max(0, cy - rad), y1 = std::min(h - 1, cy + rad);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r2) {
                uint8_t* p = rgba + y * stride + x * 4;
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
            }
        }
    }
}

inline void thick_line_rgba(uint8_t* rgba, int w, int h, int stride,
                            int x0, int y0, int x1, int y1, int thick,
                            uint8_t r, uint8_t g, uint8_t b) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int rad = std::max(1, thick / 2);
    while (true) {
        plot_disc_rgba(rgba, w, h, stride, x0, y0, rad, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

inline void rect_outline_rgba(uint8_t* rgba, int w, int h, int stride,
                              int rx, int ry, int rw, int rh, int thick,
                              uint8_t r, uint8_t g, uint8_t b) {
    thick_line_rgba(rgba, w, h, stride, rx, ry, rx + rw, ry, thick, r, g, b);
    thick_line_rgba(rgba, w, h, stride, rx, ry + rh, rx + rw, ry + rh, thick, r, g, b);
    thick_line_rgba(rgba, w, h, stride, rx, ry, rx, ry + rh, thick, r, g, b);
    thick_line_rgba(rgba, w, h, stride, rx + rw, ry, rx + rw, ry + rh, thick, r, g, b);
}

inline void filled_rect_blend_rgba(uint8_t* rgba, int w, int h, int stride,
                                   int rx, int ry, int rw, int rh,
                                   uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int x0 = std::max(0, rx), y0 = std::max(0, ry);
    int x1 = std::min(w, rx + rw), y1 = std::min(h, ry + rh);
    float alpha = a / 255.0f, inv = 1.0f - alpha;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            uint8_t* p = rgba + y * stride + x * 4;
            p[0] = (uint8_t)(r * alpha + p[0] * inv);
            p[1] = (uint8_t)(g * alpha + p[1] * inv);
            p[2] = (uint8_t)(b * alpha + p[2] * inv);
            // Keep alpha at 255 — captured frame is already opaque.
        }
    }
}

// Draw a filled triangle (arrow head) into RGBA. Standard half-space test.
inline void fill_triangle_rgba(uint8_t* rgba, int w, int h, int stride,
                               int ax, int ay, int bx, int by, int cx, int cy,
                               uint8_t r, uint8_t g, uint8_t b) {
    int xmin = std::max(0, std::min({ax, bx, cx}));
    int xmax = std::min(w - 1, std::max({ax, bx, cx}));
    int ymin = std::max(0, std::min({ay, by, cy}));
    int ymax = std::min(h - 1, std::max({ay, by, cy}));
    auto edge = [](int x0, int y0, int x1, int y1, int x, int y) {
        return (x - x0) * (y1 - y0) - (y - y0) * (x1 - x0);
    };
    for (int y = ymin; y <= ymax; ++y) {
        for (int x = xmin; x <= xmax; ++x) {
            int e0 = edge(ax, ay, bx, by, x, y);
            int e1 = edge(bx, by, cx, cy, x, y);
            int e2 = edge(cx, cy, ax, ay, x, y);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                uint8_t* p = rgba + y * stride + x * 4;
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
            }
        }
    }
}

// Render a single line of text into a freshly-allocated RGBA buffer, in
// the same colour `cr,cg,cb`. Alpha comes from the GDI ClearType
// luminance, so callers can alpha-blend the result onto another RGBA
// surface. Returns true on success and writes into `out`. Used by the
// annotator's save path to bake Text strokes into the PNG.
inline bool make_text_rgba_buffer(const std::string& text, int font_height,
                                  uint8_t cr, uint8_t cg, uint8_t cb,
                                  std::vector<uint8_t>& out, int& out_w, int& out_h) {
    out.clear();
    out_w = out_h = 0;
    if (text.empty() || font_height <= 0) return false;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;
    HFONT font = make_ui_font(font_height, 500);   // same face as the on-screen preview
    HFONT old_font = (HFONT)SelectObject(hdc, font);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text.c_str(), (int)text.size(), &sz);
    if (sz.cx <= 0 || sz.cy <= 0) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return false;
    }
    TEXTMETRICA tm;
    if (GetTextMetricsA(hdc, &tm)) sz.cx += tm.tmOverhang;
    sz.cx += (font_height + 3) / 4;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz.cx;
    bmi.bmiHeader.biHeight = -sz.cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        SelectObject(hdc, old_font); DeleteObject(font); DeleteDC(hdc);
        return false;
    }
    HBITMAP old_bm = (HBITMAP)SelectObject(hdc, hbm);
    memset(bits, 0, (size_t)sz.cx * sz.cy * 4);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutA(hdc, 0, 0, text.c_str(), (int)text.size());
    GdiFlush();
    auto* src = static_cast<uint8_t*>(bits);
    out.resize((size_t)sz.cx * sz.cy * 4);
    for (int i = 0; i < sz.cx * sz.cy; ++i) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        uint8_t alpha = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        out[i * 4 + 0] = cr;
        out[i * 4 + 1] = cg;
        out[i * 4 + 2] = cb;
        out[i * 4 + 3] = alpha;
    }
    out_w = sz.cx; out_h = sz.cy;
    SelectObject(hdc, old_bm);
    SelectObject(hdc, old_font);
    DeleteObject(hbm);
    DeleteObject(font);
    DeleteDC(hdc);
    return true;
}

// Alpha-blend an RGBA glyph buffer onto a destination RGBA buffer at
// (dx, dy). Source alpha modulates the per-pixel blend.
inline void blit_rgba_blend(uint8_t* dst, int dw, int dh, int dstride,
                            const uint8_t* src, int sw, int sh,
                            int dx, int dy) {
    int x0 = std::max(0, dx), y0 = std::max(0, dy);
    int x1 = std::min(dw, dx + sw), y1 = std::min(dh, dy + sh);
    for (int y = y0; y < y1; ++y) {
        const uint8_t* srow = src + (y - dy) * sw * 4;
        uint8_t* drow = dst + y * dstride;
        for (int x = x0; x < x1; ++x) {
            const uint8_t* sp = srow + (x - dx) * 4;
            uint8_t a = sp[3];
            if (a == 0) continue;
            uint8_t* dp = drow + x * 4;
            int inv = 255 - a;
            dp[0] = (uint8_t)((sp[0] * a + dp[0] * inv) / 255);
            dp[1] = (uint8_t)((sp[1] * a + dp[1] * inv) / 255);
            dp[2] = (uint8_t)((sp[2] * a + dp[2] * inv) / 255);
            dp[3] = 255;
        }
    }
}

} // namespace

void Renderer::begin_annotation() {
    if (annotator_active_) return;
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        std::cout << "[Annotate] No frame to annotate yet\n";
        return;
    }

    // Capture the same composite take_screenshot() would save, so what
    // the user marks up matches what they would have got with Ctrl+S
    // (including the webcam strip when the drawer is open).
    int cap_w = 0, cap_h = 0;
    std::vector<uint8_t> rgba;
    const bool include_cam = should_include_webcam_in_capture();
    if (phone_frame_enabled_ && phone_frame_.is_generated()) {
        uint8_t* composite = phone_frame_.composite_screenshot(
            sdl_renderer_,
            last_frame_data_.data(), last_frame_w_, last_frame_h_, last_frame_stride_,
            &cap_w, &cap_h);
        if (!composite) {
            std::cout << "[Annotate] composite_screenshot returned null\n";
            return;
        }
        std::unique_ptr<uint8_t[]> composite_owned(composite);
        int panel_h = 0;
        int overlap = 0;
        if (include_cam) {
            int panel_w = cap_w - (cap_w / 16) * 2;
            if (panel_w > 0) {
                panel_h = panel_w * 9 / 16;
                overlap = std::max(0, panel_w / 30);
                if (panel_h <= overlap) { panel_h = 0; overlap = 0; }
            }
        }
        int total_h = cap_h + std::max(0, panel_h - overlap);
        rgba.assign((size_t)cap_w * total_h * 4, 0);
        if (panel_h > 0) {
            // Strip first (tucked behind the phone bezel), then phone
            // composited on top via alpha-over so the bezel hides the
            // overlap and the panel bleeds through the corner cutouts.
            paint_webcam_strip(rgba.data(), cap_w, cap_h - overlap, panel_h,
                               overlap);
            blit_phone_over_strip(rgba.data(), cap_w * 4,
                                  composite, cap_w * 4, cap_w, cap_h);
            cap_h = total_h;
        } else {
            std::memcpy(rgba.data(), composite, (size_t)cap_w * cap_h * 4);
        }
    } else {
        cap_w = last_frame_w_;
        int panel_h = 0;
        if (include_cam) {
            int panel_w = cap_w - (cap_w / 16) * 2;
            if (panel_w > 0) panel_h = panel_w * 9 / 16;
        }
        int total_h = last_frame_h_ + panel_h;
        rgba.assign((size_t)cap_w * total_h * 4, 0);
        // last_frame_stride_ may include padding; copy row-by-row.
        for (int y = 0; y < last_frame_h_; ++y) {
            memcpy(rgba.data() + (size_t)y * cap_w * 4,
                   last_frame_data_.data() + (size_t)y * last_frame_stride_,
                   (size_t)cap_w * 4);
        }
        if (panel_h > 0) {
            paint_webcam_strip(rgba.data(), cap_w, last_frame_h_, panel_h);
        }
        cap_h = total_h;
    }

    // Build SDL texture from the captured RGBA. Kept around for the
    // duration of the editor session and re-used across frames.
    if (annotator_bg_tex_) {
        SDL_DestroyTexture(annotator_bg_tex_);
        annotator_bg_tex_ = nullptr;
    }
    annotator_bg_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_ABGR8888,
                                          SDL_TEXTUREACCESS_STATIC, cap_w, cap_h);
    if (!annotator_bg_tex_) {
        std::cerr << "[Annotate] SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        return;
    }
    SDL_UpdateTexture(annotator_bg_tex_, nullptr, rgba.data(), cap_w * 4);
    SDL_SetTextureBlendMode(annotator_bg_tex_, SDL_BLENDMODE_BLEND);

    annotator_bg_orig_ = std::move(rgba);
    annotator_bg_w_ = cap_w;
    annotator_bg_h_ = cap_h;
    annotator_strokes_.clear();
    annotator_drawing_ = false;
    annotator_active_ = true;
    std::cout << "[Annotate] Editor open (" << cap_w << "x" << cap_h << ")\n";
}

void Renderer::end_annotation() {
    if (annotator_bg_tex_) {
        SDL_DestroyTexture(annotator_bg_tex_);
        annotator_bg_tex_ = nullptr;
    }
    // Free per-stroke text textures before dropping the stroke list.
    for (auto& s : annotator_strokes_) {
        if (s.text_tex) { SDL_DestroyTexture(s.text_tex); s.text_tex = nullptr; }
    }
    if (annotator_text_preview_tex_) {
        SDL_DestroyTexture(annotator_text_preview_tex_);
        annotator_text_preview_tex_ = nullptr;
    }
    annotator_text_preview_w_ = annotator_text_preview_h_ = 0;
    annotator_text_preview_cached_.clear();
    if (annotator_text_input_active_) {
        SDL_StopTextInput();
        annotator_text_input_active_ = false;
    }
    annotator_text_buf_.clear();
    annotator_slider_drag_ = false;
    annotator_bg_orig_.clear();
    annotator_bg_orig_.shrink_to_fit();
    annotator_strokes_.clear();
    annotator_drawing_ = false;
    annotator_active_ = false;
    annotator_bg_w_ = annotator_bg_h_ = 0;
}

void Renderer::bake_pixelate_stroke(AnnoStroke& s) const {
    int rx = std::min(s.x0, s.x1);
    int ry = std::min(s.y0, s.y1);
    int rw = std::abs(s.x1 - s.x0);
    int rh = std::abs(s.y1 - s.y0);
    if (rw < k_anno_pix_block || rh < k_anno_pix_block) {
        // Too small to mosaic meaningfully — collapse to a tiny patch
        // covering at least one block so the user still gets feedback.
        rw = std::max(rw, k_anno_pix_block);
        rh = std::max(rh, k_anno_pix_block);
    }
    int cols = (rw + k_anno_pix_block - 1) / k_anno_pix_block;
    int rows = (rh + k_anno_pix_block - 1) / k_anno_pix_block;
    s.pix_origin_x = rx;
    s.pix_origin_y = ry;
    s.pix_block_size = k_anno_pix_block;
    s.pix_cols = cols;
    s.pix_rows = rows;
    s.pix_blocks.resize((size_t)cols * rows);
    for (int by = 0; by < rows; ++by) {
        for (int bx = 0; bx < cols; ++bx) {
            s.pix_blocks[(size_t)by * cols + bx] = avg_block_rgb(
                annotator_bg_orig_.data(),
                annotator_bg_w_, annotator_bg_h_, annotator_bg_w_ * 4,
                rx + bx * k_anno_pix_block, ry + by * k_anno_pix_block,
                k_anno_pix_block);
        }
    }
}

void Renderer::rebuild_text_preview() {
    // Recreate the in-progress preview texture so it tracks the typed
    // string. Uses the same Win32 helper as every other text label in
    // this file.
    if (annotator_text_preview_tex_) {
        SDL_DestroyTexture(annotator_text_preview_tex_);
        annotator_text_preview_tex_ = nullptr;
    }
    annotator_text_preview_w_ = annotator_text_preview_h_ = 0;
    annotator_text_preview_cached_ = annotator_text_buf_;
    if (annotator_text_buf_.empty() || annotator_text_font_px_ <= 0) return;
    AnnoColor c = k_anno_palette[annotator_color_idx_];
    int tw = 0, th = 0;
    annotator_text_preview_tex_ = make_text_texture(
        sdl_renderer_, annotator_text_buf_, annotator_text_font_px_,
        c.r, c.g, c.b, &tw, &th);
    annotator_text_preview_w_ = tw;
    annotator_text_preview_h_ = th;
}

void Renderer::commit_text_stroke() {
    if (!annotator_text_input_active_) return;
    if (!annotator_text_buf_.empty()) {
        AnnoStroke s;
        s.tool = AnnoTool::Text;
        s.x0 = annotator_text_x_;
        s.y0 = annotator_text_y_;
        s.x1 = annotator_text_x_;
        s.y1 = annotator_text_y_;
        s.text = annotator_text_buf_;
        s.font_px = annotator_text_font_px_;
        s.r = k_anno_palette[annotator_color_idx_].r;
        s.g = k_anno_palette[annotator_color_idx_].g;
        s.b = k_anno_palette[annotator_color_idx_].b;
        // Bake the on-screen texture immediately so the saved PNG and the
        // editor view come from the same glyph source.
        int tw = 0, th = 0;
        s.text_tex = make_text_texture(sdl_renderer_, s.text, s.font_px,
                                       s.r, s.g, s.b, &tw, &th);
        s.text_w = tw;
        s.text_h = th;
        annotator_strokes_.push_back(std::move(s));
    }
    annotator_text_buf_.clear();
    if (annotator_text_preview_tex_) {
        SDL_DestroyTexture(annotator_text_preview_tex_);
        annotator_text_preview_tex_ = nullptr;
    }
    annotator_text_preview_w_ = annotator_text_preview_h_ = 0;
    annotator_text_preview_cached_.clear();
    SDL_StopTextInput();
    annotator_text_input_active_ = false;
}

void Renderer::rasterize_strokes_to(uint8_t* rgba, int w, int h, int stride) const {
    for (const auto& s : annotator_strokes_) {
        int thick = std::max(2, s.thickness);
        switch (s.tool) {
        case AnnoTool::Arrow: {
            // Head — isoceles triangle pointing along the stroke. We compute
            // it first so we know where to stop the shaft.
            float dx = (float)(s.x1 - s.x0);
            float dy = (float)(s.y1 - s.y0);
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) break;
            float ux = dx / len, uy = dy / len;
            float px = -uy, py = ux; // perpendicular
            int head_len = std::max(thick * 4, 12);
            int head_w   = std::max(thick * 3, 9);
            // Shaft as a perpendicular quad so the tail cap is flat/square
            // instead of the diamond shape produced by stacked H/V lines.
            float back_x = s.x1 - ux * head_len * 0.85f;
            float back_y = s.y1 - uy * head_len * 0.85f;
            float hw = thick * 0.5f;
            int s0x = (int)std::round(s.x0  + px * hw);
            int s0y = (int)std::round(s.y0  + py * hw);
            int s1x = (int)std::round(s.x0  - px * hw);
            int s1y = (int)std::round(s.y0  - py * hw);
            int s2x = (int)std::round(back_x - px * hw);
            int s2y = (int)std::round(back_y - py * hw);
            int s3x = (int)std::round(back_x + px * hw);
            int s3y = (int)std::round(back_y + py * hw);
            fill_triangle_rgba(rgba, w, h, stride,
                               s0x, s0y, s1x, s1y, s2x, s2y,
                               s.r, s.g, s.b);
            fill_triangle_rgba(rgba, w, h, stride,
                               s0x, s0y, s2x, s2y, s3x, s3y,
                               s.r, s.g, s.b);
            int bx0 = (int)(s.x1 - ux * head_len + px * head_w);
            int by0 = (int)(s.y1 - uy * head_len + py * head_w);
            int bx1 = (int)(s.x1 - ux * head_len - px * head_w);
            int by1 = (int)(s.y1 - uy * head_len - py * head_w);
            fill_triangle_rgba(rgba, w, h, stride,
                               s.x1, s.y1, bx0, by0, bx1, by1,
                               s.r, s.g, s.b);
            break;
        }
        case AnnoTool::Rect: {
            int rx = std::min(s.x0, s.x1), ry = std::min(s.y0, s.y1);
            int rw = std::abs(s.x1 - s.x0), rh = std::abs(s.y1 - s.y0);
            rect_outline_rgba(rgba, w, h, stride, rx, ry, rw, rh, thick,
                              s.r, s.g, s.b);
            break;
        }
        case AnnoTool::Highlight: {
            int rx = std::min(s.x0, s.x1), ry = std::min(s.y0, s.y1);
            int rw = std::abs(s.x1 - s.x0), rh = std::abs(s.y1 - s.y0);
            filled_rect_blend_rgba(rgba, w, h, stride, rx, ry, rw, rh,
                                   s.r, s.g, s.b, 110);
            break;
        }
        case AnnoTool::Pixelate: {
            for (int by = 0; by < s.pix_rows; ++by) {
                for (int bx = 0; bx < s.pix_cols; ++bx) {
                    uint32_t c = s.pix_blocks[(size_t)by * s.pix_cols + bx];
                    uint8_t r = (c >> 16) & 0xFF;
                    uint8_t g = (c >> 8) & 0xFF;
                    uint8_t b = c & 0xFF;
                    int rx = s.pix_origin_x + bx * s.pix_block_size;
                    int ry = s.pix_origin_y + by * s.pix_block_size;
                    int x0 = std::max(0, rx);
                    int y0 = std::max(0, ry);
                    int x1 = std::min(w, rx + s.pix_block_size);
                    int y1 = std::min(h, ry + s.pix_block_size);
                    for (int y = y0; y < y1; ++y) {
                        uint8_t* row = rgba + y * stride;
                        for (int x = x0; x < x1; ++x) {
                            uint8_t* p = row + x * 4;
                            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                        }
                    }
                }
            }
            break;
        }
        case AnnoTool::Text: {
            if (s.text.empty() || s.font_px <= 0) break;
            std::vector<uint8_t> glyph;
            int gw = 0, gh = 0;
            if (make_text_rgba_buffer(s.text, s.font_px, s.r, s.g, s.b,
                                      glyph, gw, gh)) {
                blit_rgba_blend(rgba, w, h, stride, glyph.data(), gw, gh,
                                s.x0, s.y0);
            }
            break;
        }
        }
    }
}

void Renderer::save_annotated() {
    if (!annotator_active_ || annotator_bg_orig_.empty()) return;

    // Flush any in-progress text so it ends up in the saved image.
    if (annotator_text_input_active_) commit_text_stroke();

    const bool save = settings_.screenshot_save_to_folder;
    const bool clip = settings_.screenshot_copy_to_clipboard;
    const bool snag = settings_.screenshot_open_in_snagit;
    if (!save && !clip && !snag) {
        toast_text_ = "Screenshot disabled in Settings";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }

    // Bake strokes into a fresh copy of the original capture.
    std::vector<uint8_t> baked = annotator_bg_orig_;
    rasterize_strokes_to(baked.data(), annotator_bg_w_, annotator_bg_h_,
                         annotator_bg_w_ * 4);

    bool wrote = true;
    std::string filename;
    std::string snagit_path;
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local_tm);
    if (save) {
        std::filesystem::create_directories(screenshot_dir_);
        // <date>_<time>_<kind>.png — sorts chronologically alongside
        // plain screenshots saved to the same folder.
        filename = screenshot_dir_ + "/" + timestamp + "_annotated.png";
    } else if (snag) {
        std::error_code ec;
        auto tmp = std::filesystem::temp_directory_path(ec);
        if (!ec) snagit_path = (tmp / ("PhoneMimi_annot_" + std::string(timestamp) + ".png")).string();
    }
    wrote = emit_screenshot_output(baked.data(), annotator_bg_w_, annotator_bg_h_,
                                   annotator_bg_w_ * 4, filename, snagit_path,
                                   save, clip, snag);
    if (wrote) {
        std::string parts;
        auto add = [&](const char* p){ if (!parts.empty()) parts += " + "; parts += p; };
        if (save) add("saved");
        if (clip) add("copied");
        if (snag) add("Snagit");
        toast_text_ = "Annotated: " + parts;
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        if (save) std::cout << "[Annotate] Saved: " << filename << "\n";
        opm::UsageLog::log_feature_use("annotate", active_platform_fn_ ? active_platform_fn_() : std::string());
    }
    end_annotation();
}

void Renderer::draw_annotator() {
    if (!annotator_active_ || !annotator_bg_tex_) return;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    // Restrict the overlay to the PHONE area only (exclude the log drawer
    // if it is open) so the dim backdrop, picture and toolbar all stay
    // within the phone window and the user can still see/use the log on
    // the right. Same recipe as draw_ocr_overlay().
    int log_w = (int)(log_panel_full_w_ * log_panel_anim_);
    int phone_area_w = std::max(64, win_w - log_w);

    // Dim backdrop covering the phone area (the log drawer remains visible).
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 235);
    SDL_Rect full{0, 0, phone_area_w, win_h};
    SDL_RenderFillRect(sdl_renderer_, &full);

    // Toolbar reserves a strip at the top.
    int toolbar_h = std::max(44, win_h / 14);
    int img_top = toolbar_h + 12;
    int img_bottom_pad = 12;
    int avail_w = std::max(64, phone_area_w - 24);
    int avail_h = std::max(64, win_h - img_top - img_bottom_pad);
    float sx = (float)avail_w / annotator_bg_w_;
    float sy = (float)avail_h / annotator_bg_h_;
    float s = std::min(sx, sy);
    annotator_dst_w_ = (int)(annotator_bg_w_ * s);
    annotator_dst_h_ = (int)(annotator_bg_h_ * s);
    annotator_dst_x_ = (phone_area_w - annotator_dst_w_) / 2;
    annotator_dst_y_ = img_top + (avail_h - annotator_dst_h_) / 2;

    // Background image.
    SDL_Rect img_dst{annotator_dst_x_, annotator_dst_y_,
                     annotator_dst_w_, annotator_dst_h_};
    SDL_RenderCopy(sdl_renderer_, annotator_bg_tex_, nullptr, &img_dst);

    // Map image-space pixel coords onto the displayed rect.
    auto img_to_screen = [&](int ix, int iy, int& ox, int& oy) {
        ox = annotator_dst_x_ + (int)(ix * s);
        oy = annotator_dst_y_ + (int)(iy * s);
    };
    // Convert an image-space line width into screen-space pixels.
    auto img_thick_to_screen = [&](int img_thick) {
        return std::max(2, (int)std::round(img_thick * s));
    };

    auto draw_arrow_screen = [&](int x0, int y0, int x1, int y1, int thick,
                                 uint8_t r, uint8_t g, uint8_t b) {
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
        float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) return;
        float ux = dx / len, uy = dy / len;
        float px = -uy, py = ux;
        int head_len = std::max(thick * 4, 12);
        int head_w   = std::max(thick * 3, 9);
        // Stop the shaft at the base of the head so the line never pokes
        // through the tip. Pull back by ~80% of head_len which leaves a
        // small overlap that hides any rounding gap.
        float back_x = x1 - ux * head_len * 0.85f;
        float back_y = y1 - uy * head_len * 0.85f;

        // Generic scanline fill for an arbitrary triangle. Used both for
        // the shaft (split into two tris) and the arrow head.
        auto edge = [](float Ax, float Ay, float Bx, float By, float Px, float Py) {
            return (Bx - Ax) * (Py - Ay) - (By - Ay) * (Px - Ax);
        };
        auto fill_tri = [&](float ax, float ay, float bx, float by,
                            float cx2, float cy2) {
            int min_x = (int)std::floor(std::min({ax, bx, cx2}));
            int max_x = (int)std::ceil (std::max({ax, bx, cx2}));
            int min_y = (int)std::floor(std::min({ay, by, cy2}));
            int max_y = (int)std::ceil (std::max({ay, by, cy2}));
            for (int yy = min_y; yy <= max_y; ++yy) {
                for (int xx = min_x; xx <= max_x; ++xx) {
                    float w0 = edge(bx, by, cx2, cy2, (float)xx, (float)yy);
                    float w1 = edge(cx2, cy2, ax, ay,  (float)xx, (float)yy);
                    float w2 = edge(ax, ay, bx, by,    (float)xx, (float)yy);
                    if ((w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                        (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                        SDL_RenderDrawPoint(sdl_renderer_, xx, yy);
                    }
                }
            }
        };

        // Shaft as a perpendicular quad, giving a flat (square) tail cap.
        float hw = thick * 0.5f;
        float s0x = x0  + px * hw, s0y = y0  + py * hw;
        float s1x = x0  - px * hw, s1y = y0  - py * hw;
        float s2x = back_x - px * hw, s2y = back_y - py * hw;
        float s3x = back_x + px * hw, s3y = back_y + py * hw;
        fill_tri(s0x, s0y, s1x, s1y, s2x, s2y);
        fill_tri(s0x, s0y, s2x, s2y, s3x, s3y);

        // Solid arrow head: filled triangle with apex at the tip and base
        // perpendicular to the shaft `head_len` behind it.
        float ax = (float)x1, ay = (float)y1;
        float bx = x1 - ux * head_len + px * head_w;
        float by = y1 - uy * head_len + py * head_w;
        float cx2 = x1 - ux * head_len - px * head_w;
        float cy2 = y1 - uy * head_len - py * head_w;
        fill_tri(ax, ay, bx, by, cx2, cy2);
    };

    auto draw_rect_screen = [&](int x0, int y0, int x1, int y1, int thick,
                                uint8_t r, uint8_t g, uint8_t b) {
        int rx = std::min(x0, x1), ry = std::min(y0, y1);
        int rw = std::abs(x1 - x0), rh = std::abs(y1 - y0);
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
        for (int t = 0; t < thick; ++t) {
            SDL_Rect rr{rx - t / 2, ry - t / 2, rw + t, rh + t};
            SDL_RenderDrawRect(sdl_renderer_, &rr);
        }
    };

    auto draw_highlight_screen = [&](int x0, int y0, int x1, int y1,
                                     uint8_t r, uint8_t g, uint8_t b) {
        int rx = std::min(x0, x1), ry = std::min(y0, y1);
        int rw = std::abs(x1 - x0), rh = std::abs(y1 - y0);
        SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 110);
        SDL_Rect rr{rx, ry, rw, rh};
        SDL_RenderFillRect(sdl_renderer_, &rr);
    };

    auto draw_pixelate_screen = [&](const AnnoStroke& st) {
        for (int by = 0; by < st.pix_rows; ++by) {
            for (int bx = 0; bx < st.pix_cols; ++bx) {
                uint32_t c = st.pix_blocks[(size_t)by * st.pix_cols + bx];
                uint8_t r = (c >> 16) & 0xFF;
                uint8_t g = (c >> 8) & 0xFF;
                uint8_t b = c & 0xFF;
                int ix = st.pix_origin_x + bx * st.pix_block_size;
                int iy = st.pix_origin_y + by * st.pix_block_size;
                int sxp, syp;
                img_to_screen(ix, iy, sxp, syp);
                int sw = std::max(1, (int)(st.pix_block_size * s + 1));
                int sh = std::max(1, (int)(st.pix_block_size * s + 1));
                SDL_Rect rr{sxp, syp, sw, sh};
                SDL_SetRenderDrawColor(sdl_renderer_, r, g, b, 255);
                SDL_RenderFillRect(sdl_renderer_, &rr);
            }
        }
    };

    // Replay committed strokes on top of the background.
    SDL_RenderSetClipRect(sdl_renderer_, &img_dst);
    for (const auto& st : annotator_strokes_) {
        int sx0, sy0, sx1, sy1;
        img_to_screen(st.x0, st.y0, sx0, sy0);
        img_to_screen(st.x1, st.y1, sx1, sy1);
        int thk = img_thick_to_screen(st.thickness);
        switch (st.tool) {
        case AnnoTool::Arrow:     draw_arrow_screen(sx0, sy0, sx1, sy1, thk, st.r, st.g, st.b); break;
        case AnnoTool::Rect:      draw_rect_screen(sx0, sy0, sx1, sy1, thk, st.r, st.g, st.b); break;
        case AnnoTool::Highlight: draw_highlight_screen(sx0, sy0, sx1, sy1, st.r, st.g, st.b); break;
        case AnnoTool::Pixelate:  draw_pixelate_screen(st); break;
        case AnnoTool::Text: {
            if (!st.text_tex) break;
            // Scale the glyph texture so it grows/shrinks with the editor.
            int dw = std::max(1, (int)std::round(st.text_w * s));
            int dh = std::max(1, (int)std::round(st.text_h * s));
            SDL_Rect td{sx0, sy0, dw, dh};
            SDL_RenderCopy(sdl_renderer_, st.text_tex, nullptr, &td);
            break;
        }
        }
    }

    // Live preview of the in-progress stroke.
    if (annotator_drawing_) {
        AnnoColor c = k_anno_palette[annotator_color_idx_];
        int sx0, sy0, sx1, sy1;
        img_to_screen(annotator_drag_x0_, annotator_drag_y0_, sx0, sy0);
        img_to_screen(annotator_drag_x1_, annotator_drag_y1_, sx1, sy1);
        int thk = img_thick_to_screen(annotator_line_width_);
        switch (annotator_tool_) {
        case AnnoTool::Arrow:     draw_arrow_screen(sx0, sy0, sx1, sy1, thk, c.r, c.g, c.b); break;
        case AnnoTool::Rect:      draw_rect_screen(sx0, sy0, sx1, sy1, thk, c.r, c.g, c.b); break;
        case AnnoTool::Highlight: draw_highlight_screen(sx0, sy0, sx1, sy1, c.r, c.g, c.b); break;
        case AnnoTool::Pixelate: {
            // Show a translucent marker rect while dragging — actual mosaic
            // is computed on commit.
            int rx = std::min(sx0, sx1), ry = std::min(sy0, sy1);
            int rw = std::abs(sx1 - sx0), rh = std::abs(sy1 - sy0);
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 120);
            SDL_Rect rr{rx, ry, rw, rh};
            SDL_RenderFillRect(sdl_renderer_, &rr);
            SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 220);
            SDL_RenderDrawRect(sdl_renderer_, &rr);
            break;
        }
        case AnnoTool::Text: break; // Text uses its own preview path below.
        }
    }

    // Live preview of the in-progress text stroke (caret + glyphs).
    if (annotator_text_input_active_) {
        int sx0, sy0;
        img_to_screen(annotator_text_x_, annotator_text_y_, sx0, sy0);
        if (annotator_text_preview_tex_) {
            int dw = std::max(1, (int)std::round(annotator_text_preview_w_ * s));
            int dh = std::max(1, (int)std::round(annotator_text_preview_h_ * s));
            SDL_Rect td{sx0, sy0, dw, dh};
            SDL_RenderCopy(sdl_renderer_, annotator_text_preview_tex_, nullptr, &td);
            // Blinking caret after the last glyph.
            int caret_x = sx0 + dw + 1;
            int caret_h = dh;
            uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if ((ms / 500) % 2 == 0) {
                AnnoColor c = k_anno_palette[annotator_color_idx_];
                SDL_SetRenderDrawColor(sdl_renderer_, c.r, c.g, c.b, 255);
                SDL_Rect cr{caret_x, sy0, std::max(1, (int)std::round(s)), caret_h};
                SDL_RenderFillRect(sdl_renderer_, &cr);
            }
        } else {
            // Empty buffer — just show a caret so the user knows where they are.
            int caret_h = std::max(8, (int)std::round(annotator_text_font_px_ * s));
            uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if ((ms / 500) % 2 == 0) {
                AnnoColor c = k_anno_palette[annotator_color_idx_];
                SDL_SetRenderDrawColor(sdl_renderer_, c.r, c.g, c.b, 255);
                SDL_Rect cr{sx0, sy0, std::max(1, (int)std::round(s)), caret_h};
                SDL_RenderFillRect(sdl_renderer_, &cr);
            }
        }
    }
    SDL_RenderSetClipRect(sdl_renderer_, nullptr);

    // ---- Toolbar (centered, top of window) ----
    // Lay out the toolbar with explicit slot counts and shrink everything
    // proportionally if it would overflow the window. This keeps every
    // button reachable even on a narrow phone-shaped window.
    const int n_tools = 5, n_swatches = 5, n_actions = 3;
    int btn_sz = std::max(24, toolbar_h - 8);
    int gap = std::max(3, btn_sz / 6);
    int swatch_sz = std::max(18, btn_sz - 8);
    int slider_w = btn_sz * 5;
    auto compute_total = [&]() {
        return n_tools * btn_sz + (n_tools - 1) * gap + gap * 3
             + slider_w + gap * 3
             + n_swatches * swatch_sz + (n_swatches - 1) * gap + gap * 3
             + n_actions * btn_sz + (n_actions - 1) * gap;
    };
    int avail = std::max(64, phone_area_w - 16);
    int total_w = compute_total();
    if (total_w > avail) {
        // Scale all slot sizes down to fit. Recompute gaps from the new
        // btn_sz so visual proportions stay consistent.
        float s2 = (float)avail / total_w;
        btn_sz   = std::max(20, (int)(btn_sz * s2));
        swatch_sz = std::max(16, (int)(swatch_sz * s2));
        gap      = std::max(2, (int)(gap * s2));
        slider_w = std::max(60, (int)(slider_w * s2));
        total_w = compute_total();
        // After flooring, may still overflow by a couple px — just clamp.
        if (total_w > avail) total_w = avail;
    }
    int tx = (phone_area_w - total_w) / 2;
    if (tx < 8) tx = 8;
    int ty = (toolbar_h - btn_sz) / 2;

    int mx, my;
    SDL_GetMouseState(&mx, &my);
    auto in = [&](int x, int y, int w, int h) {
        return mx >= x && mx < x + w && my >= y && my < y + h;
    };

    auto draw_tool_btn = [&](BtnRect& slot, AnnoTool tool, const char* glyph_kind) {
        slot = {tx, ty, btn_sz, btn_sz};
        bool selected = (annotator_tool_ == tool);
        bool hov = in(tx, ty, btn_sz, btn_sz);
        uint8_t bg = selected ? 90 : (hov ? 70 : 45);
        SDL_SetRenderDrawColor(sdl_renderer_, bg, bg, bg + 8, 235);
        SDL_Rect br{tx, ty, btn_sz, btn_sz};
        SDL_RenderFillRect(sdl_renderer_, &br);
        if (selected) {
            SDL_SetRenderDrawColor(sdl_renderer_, 200, 200, 220, 255);
            SDL_RenderDrawRect(sdl_renderer_, &br);
        }
        int cx = tx + btn_sz / 2, cy = ty + btn_sz / 2;
        std::string k = glyph_kind;
        wchar_t cp = k == "arrow" ? lucide::ArrowUpRight : k == "rect" ? lucide::Square
                   : k == "highlight" ? lucide::Highlighter : k == "pixelate" ? lucide::Grid2x2
                   : lucide::Type;
        if (k == "highlight") draw_glyph(sdl_renderer_, cp, cx, cy, btn_sz * 3 / 5, 245, 205, 60, 255);
        else                  draw_glyph(sdl_renderer_, cp, cx, cy, btn_sz * 3 / 5, 235, 235, 240, 255);
        tx += btn_sz + gap;
    };
    draw_tool_btn(anno_btn_arrow_,     AnnoTool::Arrow,     "arrow");
    draw_tool_btn(anno_btn_rect_,      AnnoTool::Rect,      "rect");
    draw_tool_btn(anno_btn_highlight_, AnnoTool::Highlight, "highlight");
    draw_tool_btn(anno_btn_pixelate_,  AnnoTool::Pixelate,  "pixelate");
    draw_tool_btn(anno_btn_text_,      AnnoTool::Text,      "text");

    tx += gap * 3;

    // ---- Line-width slider ----
    // Track range maps to image-space pixels [k_min..k_max]. Affects
    // Arrow + Rect strokes (and the in-progress preview); Text uses it
    // as the glyph height.
    constexpr int k_lw_min = 1, k_lw_max = 32;
    int track_y = ty + btn_sz / 2;
    int track_h = std::max(3, btn_sz / 8);
    SDL_SetRenderDrawColor(sdl_renderer_, 60, 60, 70, 235);
    SDL_Rect track{tx, track_y - track_h / 2, slider_w, track_h};
    SDL_RenderFillRect(sdl_renderer_, &track);
    // Filled portion up to handle.
    float lw_t = (float)(annotator_line_width_ - k_lw_min) / (k_lw_max - k_lw_min);
    lw_t = std::clamp(lw_t, 0.0f, 1.0f);
    int handle_x = tx + (int)(lw_t * slider_w);
    SDL_SetRenderDrawColor(sdl_renderer_, 120, 160, 220, 255);
    SDL_Rect filled{tx, track_y - track_h / 2, handle_x - tx, track_h};
    SDL_RenderFillRect(sdl_renderer_, &filled);
    // Handle (circle approximated by filled rect — keeps it primitive).
    int handle_r = std::max(5, btn_sz / 4);
    SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 230, 255);
    SDL_Rect handle{handle_x - handle_r, track_y - handle_r,
                    handle_r * 2, handle_r * 2};
    SDL_RenderFillRect(sdl_renderer_, &handle);
    SDL_SetRenderDrawColor(sdl_renderer_, 40, 40, 50, 255);
    SDL_RenderDrawRect(sdl_renderer_, &handle);
    // Numeric value sits to the right of the track for quick feedback.
    {
        char val_buf[12];
        std::snprintf(val_buf, sizeof(val_buf), "%d", annotator_line_width_);
        int vw = 0, vh = 0;
        SDL_Texture* vt = make_text_texture(sdl_renderer_, val_buf,
                                            std::max(10, btn_sz / 2),
                                            220, 220, 230, &vw, &vh);
        if (vt) {
            SDL_Rect vd{tx + slider_w - vw - 4, track_y - vh / 2, vw, vh};
            SDL_RenderCopy(sdl_renderer_, vt, nullptr, &vd);
            SDL_DestroyTexture(vt);
        }
    }
    anno_slider_rect_ = {tx, ty, slider_w, btn_sz};
    tx += slider_w + gap * 3;

    // Colour swatches.
    int sw_y = ty + (btn_sz - swatch_sz) / 2;
    for (int i = 0; i < 5; ++i) {
        anno_swatch_btns_[i] = {tx, sw_y, swatch_sz, swatch_sz};
        bool selected = (annotator_color_idx_ == i);
        SDL_SetRenderDrawColor(sdl_renderer_,
                               k_anno_palette[i].r, k_anno_palette[i].g,
                               k_anno_palette[i].b, 255);
        SDL_Rect r{tx, sw_y, swatch_sz, swatch_sz};
        SDL_RenderFillRect(sdl_renderer_, &r);
        SDL_SetRenderDrawColor(sdl_renderer_, selected ? 255 : 60,
                               selected ? 255 : 60, selected ? 255 : 60, 255);
        SDL_Rect rr{tx - 1, sw_y - 1, swatch_sz + 2, swatch_sz + 2};
        SDL_RenderDrawRect(sdl_renderer_, &rr);
        tx += swatch_sz + gap;
    }
    tx += gap * 3;

    auto draw_action_btn = [&](BtnRect& slot, const char* glyph_kind,
                               uint8_t r, uint8_t g, uint8_t b) {
        slot = {tx, ty, btn_sz, btn_sz};
        bool hov = in(tx, ty, btn_sz, btn_sz);
        SDL_SetRenderDrawColor(sdl_renderer_, hov ? 80 : 50, hov ? 80 : 50,
                               hov ? 90 : 58, 235);
        SDL_Rect br{tx, ty, btn_sz, btn_sz};
        SDL_RenderFillRect(sdl_renderer_, &br);
        int cx = tx + btn_sz / 2, cy = ty + btn_sz / 2;
        std::string k = glyph_kind;
        wchar_t cp = k == "undo" ? lucide::Undo2 : k == "save" ? lucide::Check : lucide::X;
        draw_glyph(sdl_renderer_, cp, cx, cy, btn_sz * 3 / 5, r, g, b, 255);
        tx += btn_sz + gap;
    };
    draw_action_btn(anno_btn_undo_,   "undo",   220, 220, 220);
    draw_action_btn(anno_btn_save_,   "save",    90, 220, 110);
    draw_action_btn(anno_btn_cancel_, "cancel", 235, 110, 110);
}

bool Renderer::handle_annotator_event(const SDL_Event& ev) {
    if (!annotator_active_) return false;

    auto screen_to_image = [&](int sx, int sy, int& ix, int& iy) {
        if (annotator_dst_w_ <= 0 || annotator_dst_h_ <= 0) { ix = iy = 0; return false; }
        float fx = (float)(sx - annotator_dst_x_) * annotator_bg_w_ / annotator_dst_w_;
        float fy = (float)(sy - annotator_dst_y_) * annotator_bg_h_ / annotator_dst_h_;
        ix = (int)std::round(fx);
        iy = (int)std::round(fy);
        return ix >= 0 && iy >= 0 && ix < annotator_bg_w_ && iy < annotator_bg_h_;
    };

    auto in = [](int mx, int my, const BtnRect& r) {
        return r.w > 0 && mx >= r.x && my >= r.y && mx < r.x + r.w && my < r.y + r.h;
    };

    constexpr int k_lw_min = 1, k_lw_max = 32;
    auto slider_value_from_x = [&](int sx) {
        int rel = std::clamp(sx - anno_slider_rect_.x, 0, anno_slider_rect_.w);
        float t = (float)rel / std::max(1, anno_slider_rect_.w);
        return std::clamp((int)std::round(k_lw_min + t * (k_lw_max - k_lw_min)),
                          k_lw_min, k_lw_max);
    };

    // ---- Text input mode ----
    // While the user is typing into a text stroke, most keyboard events
    // belong to the input rather than to tool shortcuts.
    if (annotator_text_input_active_) {
        if (ev.type == SDL_TEXTINPUT) {
            annotator_text_buf_ += ev.text.text;
            rebuild_text_preview();
            return true;
        }
        if (ev.type == SDL_KEYDOWN) {
            SDL_Keycode k = ev.key.keysym.sym;
            if (k == SDLK_BACKSPACE) {
                if (!annotator_text_buf_.empty()) {
                    // Strip one byte; safe for ASCII typing. Multi-byte
                    // UTF-8 sequences can leave a partial codepoint, but
                    // the next render call just re-bakes the buffer.
                    annotator_text_buf_.pop_back();
                    // Remove any trailing UTF-8 continuation bytes.
                    while (!annotator_text_buf_.empty() &&
                           ((uint8_t)annotator_text_buf_.back() & 0xC0) == 0x80) {
                        annotator_text_buf_.pop_back();
                    }
                    rebuild_text_preview();
                }
                return true;
            }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                commit_text_stroke();
                return true;
            }
            if (k == SDLK_ESCAPE) {
                // Cancel the in-progress text without leaving the editor.
                annotator_text_buf_.clear();
                if (annotator_text_preview_tex_) {
                    SDL_DestroyTexture(annotator_text_preview_tex_);
                    annotator_text_preview_tex_ = nullptr;
                }
                annotator_text_preview_w_ = annotator_text_preview_h_ = 0;
                annotator_text_preview_cached_.clear();
                SDL_StopTextInput();
                annotator_text_input_active_ = false;
                return true;
            }
            // Swallow other keys (no tool shortcuts while typing).
            return true;
        }
        // Mouse handling continues below — clicking the toolbar or
        // outside the current text commits and starts a new operation.
    }

    if (ev.type == SDL_KEYDOWN) {
        SDL_Keymod m = SDL_GetModState();
        if (ev.key.keysym.sym == SDLK_ESCAPE) {
            std::cout << "[Annotate] Cancelled\n";
            end_annotation();
            return true;
        }
        if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) {
            save_annotated();
            return true;
        }
        if ((m & KMOD_CTRL) && ev.key.keysym.sym == SDLK_z) {
            if (!annotator_strokes_.empty()) {
                if (annotator_strokes_.back().text_tex) {
                    SDL_DestroyTexture(annotator_strokes_.back().text_tex);
                }
                annotator_strokes_.pop_back();
            }
            return true;
        }
        // Tool shortcuts.
        if (ev.key.keysym.sym == SDLK_a) { annotator_tool_ = AnnoTool::Arrow;     return true; }
        if (ev.key.keysym.sym == SDLK_r) { annotator_tool_ = AnnoTool::Rect;      return true; }
        if (ev.key.keysym.sym == SDLK_h) { annotator_tool_ = AnnoTool::Highlight; return true; }
        if (ev.key.keysym.sym == SDLK_p) { annotator_tool_ = AnnoTool::Pixelate;  return true; }
        if (ev.key.keysym.sym == SDLK_t) { annotator_tool_ = AnnoTool::Text;      return true; }
        // [/] adjust line width without touching the slider.
        if (ev.key.keysym.sym == SDLK_LEFTBRACKET) {
            annotator_line_width_ = std::max(k_lw_min, annotator_line_width_ - 1);
            return true;
        }
        if (ev.key.keysym.sym == SDLK_RIGHTBRACKET) {
            annotator_line_width_ = std::min(k_lw_max, annotator_line_width_ + 1);
            return true;
        }
        return true; // swallow everything else while modal
    }

    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        int mx = ev.button.x, my = ev.button.y;
        // Toolbar hits first.
        if (in(mx, my, anno_btn_arrow_))     { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Arrow;     return true; }
        if (in(mx, my, anno_btn_rect_))      { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Rect;      return true; }
        if (in(mx, my, anno_btn_highlight_)) { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Highlight; return true; }
        if (in(mx, my, anno_btn_pixelate_))  { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Pixelate;  return true; }
        if (in(mx, my, anno_btn_text_))      { if (annotator_text_input_active_) commit_text_stroke(); annotator_tool_ = AnnoTool::Text;      return true; }
        // Slider — start a drag and snap the value to the cursor.
        if (in(mx, my, anno_slider_rect_)) {
            annotator_slider_drag_ = true;
            annotator_line_width_ = slider_value_from_x(mx);
            // Live preview gets re-baked at new size on next render.
            return true;
        }
        for (int i = 0; i < 5; ++i) {
            if (in(mx, my, anno_swatch_btns_[i])) {
                annotator_color_idx_ = i;
                if (annotator_text_input_active_) rebuild_text_preview();
                return true;
            }
        }
        if (in(mx, my, anno_btn_undo_)) {
            if (!annotator_strokes_.empty()) {
                if (annotator_strokes_.back().text_tex) {
                    SDL_DestroyTexture(annotator_strokes_.back().text_tex);
                }
                annotator_strokes_.pop_back();
            }
            return true;
        }
        if (in(mx, my, anno_btn_save_))   { save_annotated();   return true; }
        if (in(mx, my, anno_btn_cancel_)) { end_annotation();   return true; }

        // Otherwise click is inside the image area.
        int ix, iy;
        if (screen_to_image(mx, my, ix, iy)) {
            if (annotator_tool_ == AnnoTool::Text) {
                // Commit any prior in-progress text and start a new one
                // anchored at the cursor.
                if (annotator_text_input_active_) commit_text_stroke();
                annotator_text_input_active_ = true;
                annotator_text_x_ = ix;
                annotator_text_y_ = iy;
                // Map the line-width slider to a glyph height, with a
                // minimum so even thickness=1 produces legible text.
                annotator_text_font_px_ = std::max(14, annotator_line_width_ * 6);
                annotator_text_buf_.clear();
                rebuild_text_preview();
                SDL_StartTextInput();
            } else {
                if (annotator_text_input_active_) commit_text_stroke();
                annotator_drawing_ = true;
                annotator_drag_x0_ = annotator_drag_x1_ = ix;
                annotator_drag_y0_ = annotator_drag_y1_ = iy;
            }
        } else if (annotator_text_input_active_) {
            // Click outside the image while typing also commits.
            commit_text_stroke();
        }
        return true;
    }

    if (ev.type == SDL_MOUSEMOTION) {
        if (annotator_slider_drag_) {
            annotator_line_width_ = slider_value_from_x(ev.motion.x);
            return true;
        }
        if (annotator_drawing_) {
            int ix, iy;
            screen_to_image(ev.motion.x, ev.motion.y, ix, iy);
            // Allow dragging slightly outside the image — clamp to bounds.
            ix = std::clamp(ix, 0, annotator_bg_w_ - 1);
            iy = std::clamp(iy, 0, annotator_bg_h_ - 1);
            annotator_drag_x1_ = ix;
            annotator_drag_y1_ = iy;
            return true;
        }
    }

    if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
        if (annotator_slider_drag_) {
            annotator_slider_drag_ = false;
            return true;
        }
        if (annotator_drawing_) {
            annotator_drawing_ = false;
            // Discard zero-length drags so a stray click doesn't litter the
            // stroke list with invisible entries.
            if (std::abs(annotator_drag_x1_ - annotator_drag_x0_) < 3 &&
                std::abs(annotator_drag_y1_ - annotator_drag_y0_) < 3) {
                return true;
            }
            AnnoStroke s;
            s.tool = annotator_tool_;
            s.x0 = annotator_drag_x0_; s.y0 = annotator_drag_y0_;
            s.x1 = annotator_drag_x1_; s.y1 = annotator_drag_y1_;
            s.r = k_anno_palette[annotator_color_idx_].r;
            s.g = k_anno_palette[annotator_color_idx_].g;
            s.b = k_anno_palette[annotator_color_idx_].b;
            s.thickness = annotator_line_width_;
            if (s.tool == AnnoTool::Pixelate) bake_pixelate_stroke(s);
            annotator_strokes_.push_back(std::move(s));
            return true;
        }
    }

    // Keep modal: swallow all other events (mouse wheel, right-click etc).
    if (ev.type == SDL_MOUSEWHEEL || ev.type == SDL_MOUSEBUTTONDOWN ||
        ev.type == SDL_MOUSEBUTTONUP) {
        return true;
    }
    return false;
}

void Renderer::show_toast(const std::string& text, int duration_ms) {
    toast_text_ = text;
    toast_duration_ms_ = std::max(600, duration_ms);
    toast_active_ = true;
    toast_start_ = std::chrono::steady_clock::now();
}

// Sample a sparse grid of pixels in last_frame_data_ and report whether
// every sample is "near black". Used to detect FLAG_SECURE / lock-screen
// content: Android's MediaProjection capture delivers an all-black surface
// for protected windows and the framework deliberately hides the cause.
// We sample only ~16x16 = 256 pixels regardless of resolution (effectively
// free at 1080p+) and require an extremely dark threshold so genuine
// dark-mode UI won't trigger the overlay.
bool Renderer::is_frame_near_black() const {
    if (last_frame_data_.empty() || last_frame_w_ <= 0 || last_frame_h_ <= 0) {
        return false;
    }
    constexpr int kGrid = 16;
    constexpr int kThreshold = 12; // 0..255 per channel
    // Restrict sampling to the central 60% of the frame so the top status
    // bar (clock, battery, signal icons) and bottom nav bar/keyboard
    // suggestion strip — which remain bright even when an app uses
    // FLAG_SECURE on its own window — don't disqualify an otherwise
    // blacked-out content area.
    const int x0 = last_frame_w_ * 20 / 100;
    const int x1 = last_frame_w_ * 80 / 100;
    const int y0 = last_frame_h_ * 20 / 100;
    const int y1 = last_frame_h_ * 80 / 100;
    const int rw = std::max(1, x1 - x0);
    const int rh = std::max(1, y1 - y0);
    const uint8_t* p = last_frame_data_.data();
    for (int gy = 0; gy < kGrid; ++gy) {
        int y = y0 + (gy * 2 + 1) * rh / (kGrid * 2);
        if (y >= last_frame_h_) y = last_frame_h_ - 1;
        const uint8_t* row = p + (size_t)y * last_frame_stride_;
        for (int gx = 0; gx < kGrid; ++gx) {
            int x = x0 + (gx * 2 + 1) * rw / (kGrid * 2);
            if (x >= last_frame_w_) x = last_frame_w_ - 1;
            const uint8_t* px = row + x * 4;
            if (px[0] > kThreshold || px[1] > kThreshold || px[2] > kThreshold) {
                return false;
            }
        }
    }
    return true;
}

void Renderer::check_for_update_async(bool show_when_up_to_date) {
    bool expected = false;
    if (!update_check_in_progress_.compare_exchange_strong(expected, true)) {
        // Already running — silently coalesce.
        return;
    }
    // Format the running version once on the calling thread; the worker
    // thread only touches its own locals plus the renderer fields it owns.
    char ver[32];
    std::snprintf(ver, sizeof(ver), "%d.%d.%d",
                  OPM_VERSION_MAJOR,
                  OPM_VERSION_MINOR,
                  OPM_VERSION_PATCH);
    std::string current = ver;

    std::thread([this, current, show_when_up_to_date]() {
        auto result = opm::network::check_for_update(current);
        {
            std::lock_guard<std::mutex> lk(update_check_mutex_);
            update_latest_version_ = result.latest_version;
            update_release_url_    = result.release_url;
        }
        // Surface the outcome via the existing toast banner. The renderer
        // event loop polls toast state, so just setting these strings from
        // a worker thread is safe enough for our purposes (single writer
        // because update_check_in_progress_ is the latch).
        if (!result.ok) {
            if (show_when_up_to_date) {
                show_toast("Update check failed (no internet?)", 3000);
            }
            // Silent on launch when the network is unreachable.
        } else if (result.update_available) {
            // Persistent two-line banner with a clickable GitHub link
            // (replaces the previous single-line toast). The renderer
            // event loop will pick up the new state next frame.
            update_banner_active_ = true;
            update_banner_start_ = std::chrono::steady_clock::now();
            // Force texture re-render in draw_update_banner() so the
            // version string in the cached texture matches the new value.
            update_line1_cached_.clear();
            std::cout << "[Update] New version available: " << result.latest_version
                      << " (current " << current << ") " << result.release_url << "\n";
        } else if (show_when_up_to_date) {
            show_toast("You're up to date (v" + current + ")", 3000);
        }
        update_check_in_progress_.store(false);
    }).detach();
}

std::string Renderer::make_recording_path() const {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &local_tm);
    const char* ext = (settings_.record_format == 1) ? ".gif" : ".mp4";
    // <date>_<time>_<kind>.<ext> so MP4/GIF recordings sort
    // chronologically alongside PNG screenshots in the same folder.
    return screenshot_dir_ + "/" + ts + "_recording" + ext;
}

void Renderer::start_recording() {
    if (recorder_.is_recording()) return;
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        toast_text_ = "No video to record yet";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }
    std::filesystem::create_directories(screenshot_dir_);

    media::RecordConfig cfg;
    cfg.format           = (settings_.record_format == 1) ? media::RecordFormat::GIF
                                                          : media::RecordFormat::MP4;
    cfg.output_path      = make_recording_path();

    // Match the on-screen composite when the phone bezel is on, so the
    // recording shows the same rounded-corner phone frame the user sees.
    int src_w = last_frame_w_;
    int src_h = last_frame_h_;
    if (phone_frame_enabled_ && phone_frame_.is_generated()) {
        src_w = phone_frame_.frame_width();
        src_h = phone_frame_.frame_height();
    }

    // M5: lock in the webcam panel decision for the whole recording.
    // Dimensions must stay constant for the entire clip (the encoder
    // can't change resolution mid-stream), so we capture the intent
    // here and keep the panel attached even if the user toggles the
    // drawer mid-recording.
    recording_with_webcam_    = false;
    recording_webcam_panel_h_ = 0;
    recording_canvas_w_       = src_w;
    recording_canvas_h_base_  = src_h;
    recording_webcam_overlap_ = 0;
#ifdef ENABLE_WEBCAM
    if (settings_.webcam_include_in_recording && webcam_.is_running()) {
        int rec_inset   = src_w / 16;
        int rec_panel_w = src_w - rec_inset * 2;
        int rec_panel_h = rec_panel_w * 9 / 16;
        // Same "tuck behind the bezel" amount used by screenshots so
        // the recorded composite matches what Ctrl+S produces.
        int rec_overlap = std::max(0, rec_panel_w / 30);
        if (rec_panel_h > rec_overlap && rec_panel_w > 0) {
            recording_with_webcam_    = true;
            recording_webcam_panel_h_ = rec_panel_h;
            recording_webcam_overlap_ = rec_overlap;
            src_h += rec_panel_h - rec_overlap;
        }
    }
#endif

    cfg.width            = src_w & ~1;
    cfg.height           = src_h & ~1;
    // Downscale GIFs aggressively — RGB8 already loses colour; smaller
    // dimensions keep the file under a few MB for 10-15 s clips.
    if (cfg.format == media::RecordFormat::GIF && cfg.width > 480) {
        int new_w = 480 & ~1;
        int new_h = (src_h * new_w / src_w) & ~1;
        cfg.width  = new_w;
        cfg.height = new_h;
    }
    cfg.target_fps       = (cfg.format == media::RecordFormat::GIF)
                            ? settings_.record_fps_gif
                            : settings_.record_fps_mp4;
    cfg.bitrate_kbps     = settings_.record_bitrate_kbps;
    cfg.max_duration_sec = (pending_record_duration_sec_ > 0)
                            ? pending_record_duration_sec_
                            : settings_.record_max_duration_sec;
    pending_record_duration_sec_ = 0;

    if (!recorder_.start(cfg)) {
        toast_text_ = std::string("Recording failed: ") + recorder_.last_error();
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }
    std::cout << "[Recorder] Started: " << cfg.output_path
              << " (" << cfg.width << "x" << cfg.height
              << " @ " << cfg.target_fps << " fps)\n";
    toast_text_ = (cfg.format == media::RecordFormat::GIF)
        ? "Recording GIF..."
        : "Recording MP4...";
    toast_active_ = true;
    toast_start_  = std::chrono::steady_clock::now();
}

void Renderer::stop_recording() {
    if (!recorder_.is_recording() && record_countdown_ms_ == 0) return;
    if (record_countdown_ms_ > 0 && !recorder_.is_recording()) {
        // Cancel countdown
        record_countdown_ms_ = 0;
        toast_text_ = "Recording cancelled";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }
    std::string out = recorder_.stop();
    std::string err = recorder_.last_error();
    if (!err.empty()) {
        toast_text_ = std::string("Recording failed: ") + err;
    } else if (!out.empty()) {
        std::filesystem::path p(out);
        toast_text_ = std::string("Saved ") + p.filename().string();
        std::cout << "[Recorder] Saved: " << out << "\n";
        opm::UsageLog::log_feature_use("recording", active_platform_fn_ ? active_platform_fn_() : std::string());
    } else {
        toast_text_ = "Recording stopped";
    }
    toast_active_ = true;
    toast_start_  = std::chrono::steady_clock::now();
}

void Renderer::copy_to_clipboard(const uint8_t* rgba, int w, int h, int dpi) {
#ifdef _WIN32
    std::vector<uint8_t> png_data;
    stbi_write_png_to_func(png_write_vec, &png_data, w, h, 4, rgba, w * 4);
    if (dpi > 0) png_set_dpi(png_data, dpi);

    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();

    UINT png_fmt = RegisterClipboardFormatA("PNG");
    if (png_fmt && !png_data.empty()) {
        HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, png_data.size());
        if (hPng) {
            void* p = GlobalLock(hPng);
            memcpy(p, png_data.data(), png_data.size());
            GlobalUnlock(hPng);
            SetClipboardData(png_fmt, hPng);
        }
    }

    size_t hdr_size = sizeof(BITMAPV5HEADER);
    size_t pixel_size = (size_t)w * h * 4;
    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, hdr_size + pixel_size);
    if (hDib) {
        void* pDib = GlobalLock(hDib);
        auto* hdr = reinterpret_cast<BITMAPV5HEADER*>(pDib);
        memset(hdr, 0, hdr_size);
        hdr->bV5Size = (DWORD)hdr_size;
        hdr->bV5Width = w;
        hdr->bV5Height = -h;
        hdr->bV5Planes = 1;
        hdr->bV5BitCount = 32;
        hdr->bV5Compression = BI_BITFIELDS;
        hdr->bV5SizeImage = (DWORD)pixel_size;
        hdr->bV5RedMask = 0x00FF0000;
        hdr->bV5GreenMask = 0x0000FF00;
        hdr->bV5BlueMask = 0x000000FF;
        hdr->bV5AlphaMask = 0xFF000000;
        hdr->bV5CSType = LCS_sRGB;
        hdr->bV5Intent = LCS_GM_IMAGES;
        if (dpi > 0) {
            LONG ppm = (LONG)std::lround(dpi / 0.0254);
            hdr->bV5XPelsPerMeter = ppm;
            hdr->bV5YPelsPerMeter = ppm;
        }

        auto* dst = reinterpret_cast<uint8_t*>(pDib) + hdr_size;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t a = rgba[i * 4 + 3];
            dst[i * 4 + 0] = (uint8_t)(rgba[i * 4 + 2] * a / 255);
            dst[i * 4 + 1] = (uint8_t)(rgba[i * 4 + 1] * a / 255);
            dst[i * 4 + 2] = (uint8_t)(rgba[i * 4 + 0] * a / 255);
            dst[i * 4 + 3] = a;
        }
        GlobalUnlock(hDib);
        SetClipboardData(CF_DIBV5, hDib);
    }

    CloseClipboard();
    std::cout << "[Clipboard] Copied " << w << "x" << h << " PNG + DIBv5\n";
#endif
}

void Renderer::update_window_shape() {
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window_, &info)) return;

    if (!phone_frame_enabled_ || frame_dst_w_ == 0) {
        SetWindowRgn(info.info.win.window, nullptr, FALSE);
        window_shape_last_lp_w_ = -1;
        window_shape_last_frame_w_ = -1;
        window_shape_last_frame_x_ = -1;
        window_shape_last_frame_y_ = -1;
        return;
    }

    // Compute current drawer pixel width and skip the (expensive)
    // SetWindowRgn call when nothing the region depends on has changed.
    // Calling SetWindowRgn every frame causes Windows to re-clip the
    // non-client area which produces visible stutter during animation.
    int cur_lp_w = (log_panel_anim_ > 0.01f)
                       ? (int)(log_panel_full_w_ * log_panel_anim_)
                       : 0;
    int cur_wc_h = (webcam_drawer_anim_ > 0.01f)
                       ? (int)(webcam_drawer_full_h_ * webcam_drawer_anim_)
                       : 0;
    if (cur_lp_w == window_shape_last_lp_w_ &&
        cur_wc_h == window_shape_last_wc_h_ &&
        frame_dst_w_ == window_shape_last_frame_w_ &&
        frame_dst_x_ == window_shape_last_frame_x_ &&
        frame_dst_y_ == window_shape_last_frame_y_) {
        return;
    }
    window_shape_last_lp_w_ = cur_lp_w;
    window_shape_last_wc_h_ = cur_wc_h;
    window_shape_last_frame_w_ = frame_dst_w_;
    window_shape_last_frame_x_ = frame_dst_x_;
    window_shape_last_frame_y_ = frame_dst_y_;

    float scale = (float)frame_dst_w_ / phone_frame_.frame_width();
    int cr = (int)(phone_frame_.corner_radius() * scale);
    if (cr < 4) cr = 4;

    HRGN phone_rgn = CreateRoundRectRgn(
        frame_dst_x_, frame_dst_y_,
        frame_dst_x_ + frame_dst_w_ + 1,
        frame_dst_y_ + frame_dst_h_ + 1,
        cr * 2, cr * 2);

    // Side buttons stick out of the phone outline (their rects reach a
    // little under the body, so only the outer corners end up rounded).
    for (const BtnRect& b : side_btn_) {
        if (b.w <= 0) continue;
        int ell = std::max(2, std::min(b.w, b.h) * 2 / 3);
        HRGN btn_rgn = CreateRoundRectRgn(b.x, b.y, b.x + b.w + 1, b.y + b.h + 1, ell, ell);
        if (btn_rgn) {
            CombineRgn(phone_rgn, phone_rgn, btn_rgn, RGN_OR);
            DeleteObject(btn_rgn);
        }
    }

    if (log_panel_anim_ > 0.01f) {
        // Combine phone frame region with log panel region (right side, drawer)
        // Flat left edge, rounded right edge only
        int lp_w = (int)(log_panel_full_w_ * log_panel_anim_);
        int lp_margin = std::max(4, frame_dst_h_ / 40);
        int drawer_inset = frame_dst_h_ / 16;
        int lp_cr = std::max(8, cr / 2);
        int lp_left = frame_dst_x_ + frame_dst_w_; // flush with phone
        int lp_right = frame_dst_x_ + frame_dst_w_ + lp_w - lp_margin;
        int lp_top = frame_dst_y_ + drawer_inset;
        int lp_bottom = frame_dst_y_ + frame_dst_h_ - drawer_inset + 1;
        int lp_width_px = lp_right - lp_left;
        if (lp_width_px > 0) {
            HRGN log_rgn = nullptr;
            if (lp_width_px > lp_cr * 2) {
                // Wide enough for rounded right edge
                HRGN log_round = CreateRoundRectRgn(
                    lp_left, lp_top,
                    lp_right + 1, lp_bottom,
                    lp_cr * 2, lp_cr * 2);
                // Flat-left rect to fill in left rounded corners
                HRGN log_flat = CreateRectRgn(
                    lp_left, lp_top,
                    lp_left + lp_cr, lp_bottom);
                if (log_round && log_flat) {
                    CombineRgn(log_round, log_round, log_flat, RGN_OR);
                    log_rgn = log_round;
                    DeleteObject(log_flat);
                } else {
                    if (log_round) DeleteObject(log_round);
                    if (log_flat) DeleteObject(log_flat);
                }
            } else {
                // Too narrow for rounded corners — use a plain rect so the
                // drawer is visible from the very first pixel of animation.
                log_rgn = CreateRectRgn(lp_left, lp_top, lp_right + 1, lp_bottom);
            }
            if (log_rgn) {
                CombineRgn(phone_rgn, phone_rgn, log_rgn, RGN_OR);
                DeleteObject(log_rgn);
            }
        }
    }

    // Webcam drawer region (hangs below the phone frame, inset L/R to
    // match the straight section of the phone's bottom bezel, rounded
    // bottom corners only, with a small bottom margin so the panel
    // doesn't hit the window edge \u2014 matches draw_webcam_drawer()).
    if (cur_wc_h > 0) {
        float wc_scale = phone_frame_.frame_width() > 0
            ? (float)frame_dst_w_ / (float)phone_frame_.frame_width()
            : 1.0f;
        int inset_x = std::max(frame_dst_w_ / 16,
                               (int)(phone_frame_.corner_radius() * wc_scale));
        int wd_margin = std::max(4, frame_dst_w_ / 40);
        int wc_left   = frame_dst_x_ + inset_x;
        int wc_right  = frame_dst_x_ + frame_dst_w_ - inset_x;
        int wc_top    = frame_dst_y_ + frame_dst_h_;
        int wc_bottom = wc_top + std::max(0, cur_wc_h - wd_margin);
        int wc_w_px   = wc_right - wc_left;
        int wc_h_px   = wc_bottom - wc_top;
        int wc_cr     = std::max(8, wc_w_px / 30);
        if (wc_w_px > 0 && wc_h_px > 0) {
            HRGN wc_rgn = nullptr;
            if (wc_w_px > wc_cr * 2 && wc_h_px > wc_cr) {
                // Rounded bottom corners; flat top edge (flush with phone).
                HRGN wc_round = CreateRoundRectRgn(
                    wc_left, wc_top,
                    wc_right + 1, wc_bottom + 1,
                    wc_cr * 2, wc_cr * 2);
                HRGN wc_flat_top = CreateRectRgn(
                    wc_left, wc_top,
                    wc_right + 1, wc_top + wc_cr);
                if (wc_round && wc_flat_top) {
                    CombineRgn(wc_round, wc_round, wc_flat_top, RGN_OR);
                    wc_rgn = wc_round;
                    DeleteObject(wc_flat_top);
                } else {
                    if (wc_round)    DeleteObject(wc_round);
                    if (wc_flat_top) DeleteObject(wc_flat_top);
                }
            } else {
                wc_rgn = CreateRectRgn(wc_left, wc_top,
                                       wc_right + 1, wc_bottom + 1);
            }
            if (wc_rgn) {
                CombineRgn(phone_rgn, phone_rgn, wc_rgn, RGN_OR);
                DeleteObject(wc_rgn);
            }
        }
    }

    SetWindowRgn(info.info.win.window, phone_rgn, FALSE);
#endif
}

void Renderer::begin_window_drag() {
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window_, &info)) {
        ReleaseCapture();
        SendMessage(info.info.win.window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
#endif
}

void Renderer::stop() { running_.store(false); }

void Renderer::set_pin_code(const std::string& pin) {
    std::lock_guard<std::mutex> lk(pin_mutex_);
    pin_code_ = pin;
}

void Renderer::draw_pin_overlay() {
    if (!sdl_renderer_ || !window_) return;
    std::string pin;
    {
        std::lock_guard<std::mutex> lk(pin_mutex_);
        pin = pin_code_;
    }
    if (pin.empty()) {
        // Free cached textures when not in use.
        if (pin_label_tex_)  { SDL_DestroyTexture(pin_label_tex_);  pin_label_tex_  = nullptr; }
        if (pin_digits_tex_) { SDL_DestroyTexture(pin_digits_tex_); pin_digits_tex_ = nullptr; }
        pin_digits_cached_.clear();
        return;
    }

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window_, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    // Build textures lazily / when PIN changes.
    if (!pin_label_tex_) {
        pin_label_tex_ = make_text_texture(sdl_renderer_, "AirPlay PIN",
                                           36, 220, 220, 220,
                                           &pin_label_w_, &pin_label_h_);
    }
    if (!pin_note_tex_) {
        pin_note_tex_ = make_text_texture(sdl_renderer_,
                                          "Note: PIN pairing is experimental and may not work on all devices.",
                                          14, 170, 170, 170,
                                          &pin_note_w_, &pin_note_h_);
    }
    if (pin != pin_digits_cached_) {
        if (pin_digits_tex_) { SDL_DestroyTexture(pin_digits_tex_); pin_digits_tex_ = nullptr; }
        // Letter-spaced for readability ("1 2 3 4")
        std::string spaced;
        for (size_t i = 0; i < pin.size(); ++i) {
            if (i) spaced += ' ';
            spaced += pin[i];
        }
        pin_digits_tex_ = make_text_texture(sdl_renderer_, spaced,
                                             140, 255, 255, 255,
                                             &pin_digits_w_, &pin_digits_h_);
        pin_digits_cached_ = pin;
    }

    // Centered card with semi-transparent dark backdrop.
    int card_w = std::max(420, std::max(pin_digits_w_, pin_note_w_) + 80);
    int card_h = pin_label_h_ + pin_digits_h_ + pin_note_h_ + 96;
    int card_x = (win_w - card_w) / 2;
    int card_y = (win_h - card_h) / 2;

    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    // Dim the screen behind the card.
    SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 160);
    SDL_Rect bg = {0, 0, win_w, win_h};
    SDL_RenderFillRect(sdl_renderer_, &bg);

    // Card background.
    SDL_SetRenderDrawColor(sdl_renderer_, 28, 28, 32, 235);
    SDL_Rect card = {card_x, card_y, card_w, card_h};
    SDL_RenderFillRect(sdl_renderer_, &card);

    // Card border.
    SDL_SetRenderDrawColor(sdl_renderer_, 80, 80, 90, 255);
    SDL_RenderDrawRect(sdl_renderer_, &card);

    // Label.
    if (pin_label_tex_) {
        SDL_Rect dst = {card_x + (card_w - pin_label_w_) / 2,
                        card_y + 18,
                        pin_label_w_, pin_label_h_};
        SDL_RenderCopy(sdl_renderer_, pin_label_tex_, nullptr, &dst);
    }
    // Digits.
    if (pin_digits_tex_) {
        SDL_Rect dst = {card_x + (card_w - pin_digits_w_) / 2,
                        card_y + 18 + pin_label_h_ + 12,
                        pin_digits_w_, pin_digits_h_};
        SDL_RenderCopy(sdl_renderer_, pin_digits_tex_, nullptr, &dst);
    }
    // Footnote.
    if (pin_note_tex_) {
        SDL_Rect dst = {card_x + (card_w - pin_note_w_) / 2,
                        card_y + 18 + pin_label_h_ + 12 + pin_digits_h_ + 16,
                        pin_note_w_, pin_note_h_};
        SDL_RenderCopy(sdl_renderer_, pin_note_tex_, nullptr, &dst);
    }
}

void Renderer::reset_window_to_default_size() {
    int fw = phone_frame_.frame_width();
    int fh = phone_frame_.frame_height();
    if (fw <= 0 || fh <= 0) return;
    SDL_DisplayMode dm;
    SDL_GetCurrentDisplayMode(0, &dm);
    float s = std::min(dm.w * 0.32f / fw, dm.h * 0.65f / fh);
    SDL_SetWindowSize(window_, (int)(fw * s), (int)(fh * s));
    window_shape_set_ = false;
    toast_text_ = "Window reset to default size";
    toast_start_ = std::chrono::steady_clock::now();
    toast_active_ = true;
    std::cout << "[Renderer] Reset window to default size\n";
}

#ifdef _WIN32
// =====================================================================
// OCR copy (Ctrl+Shift+T)
// =====================================================================
//
// Reuses the annotator's "freeze a composite, dim it, draw on top" idiom
// but stripped to a single tool: rectangle pick. On mouse-up the cropped
// RGBA region is shipped to a worker thread (see media/ocr.cpp), which
// runs Windows.Media.Ocr and writes the joined text back through a
// mutex-guarded result slot. The render loop polls
// `process_ocr_result()` each frame to publish to the clipboard.

void Renderer::begin_ocr() {
    if (ocr_active_ || annotator_active_) return;
    if (last_frame_data_.empty() || last_frame_w_ == 0 || last_frame_h_ == 0) {
        toast_text_ = "OCR: no frame yet";
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }

    // Capture the same composite the annotator does so the user picks a
    // region of exactly what they see (phone frame included when on).
    int cap_w = 0, cap_h = 0;
    std::vector<uint8_t> rgba;
    if (phone_frame_enabled_ && phone_frame_.is_generated()) {
        uint8_t* composite = phone_frame_.composite_screenshot(
            sdl_renderer_,
            last_frame_data_.data(), last_frame_w_, last_frame_h_, last_frame_stride_,
            &cap_w, &cap_h);
        if (!composite) {
            toast_text_ = "OCR: composite failed";
            toast_start_ = std::chrono::steady_clock::now();
            return;
        }
        rgba.assign(composite, composite + (size_t)cap_w * cap_h * 4);
        delete[] composite;
    } else {
        cap_w = last_frame_w_;
        cap_h = last_frame_h_;
        rgba.resize((size_t)cap_w * cap_h * 4);
        for (int y = 0; y < cap_h; ++y) {
            memcpy(rgba.data() + (size_t)y * cap_w * 4,
                   last_frame_data_.data() + (size_t)y * last_frame_stride_,
                   (size_t)cap_w * 4);
        }
    }

    if (ocr_bg_tex_) { SDL_DestroyTexture(ocr_bg_tex_); ocr_bg_tex_ = nullptr; }
    ocr_bg_tex_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_ABGR8888,
                                    SDL_TEXTUREACCESS_STATIC, cap_w, cap_h);
    if (!ocr_bg_tex_) {
        std::cerr << "[OCR] SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        return;
    }
    SDL_UpdateTexture(ocr_bg_tex_, nullptr, rgba.data(), cap_w * 4);

    ocr_bg_rgba_ = std::move(rgba);
    ocr_bg_w_ = cap_w;
    ocr_bg_h_ = cap_h;
    ocr_drawing_ = false;
    ocr_drag_x0_ = ocr_drag_y0_ = ocr_drag_x1_ = ocr_drag_y1_ = 0;
    ocr_active_ = true;
    toast_text_ = "OCR: drag a region (Esc to cancel)";
    toast_start_ = std::chrono::steady_clock::now();
    toast_active_ = true;
}

void Renderer::end_ocr() {
    if (ocr_bg_tex_) { SDL_DestroyTexture(ocr_bg_tex_); ocr_bg_tex_ = nullptr; }
    ocr_bg_rgba_.clear();
    ocr_bg_w_ = ocr_bg_h_ = 0;
    ocr_drawing_ = false;
    ocr_active_ = false;
}

void Renderer::draw_ocr_overlay() {
    if (!ocr_active_ || !ocr_bg_tex_) return;

    int win_w = 0, win_h = 0;
    SDL_GetRendererOutputSize(sdl_renderer_, &win_w, &win_h);

    // Restrict the overlay to the PHONE area only (exclude the log
    // drawer if it is open) so the dim backdrop, picture, hint and
    // selection rect all stay within the phone window and the user can
    // still see/use the log on the right.
    int log_w = (int)(log_panel_full_w_ * log_panel_anim_);
    int phone_area_w = std::max(64, win_w - log_w);

    // Dim backdrop — only over the phone area.
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 200);
    SDL_Rect full{0, 0, phone_area_w, win_h};
    SDL_RenderFillRect(sdl_renderer_, &full);

    // Fit the captured composite into the phone area with a small margin.
    int margin = 16;
    int avail_w = std::max(64, phone_area_w - 2 * margin);
    int avail_h = std::max(64, win_h - 2 * margin);
    float sx = (float)avail_w / ocr_bg_w_;
    float sy = (float)avail_h / ocr_bg_h_;
    float s = std::min(sx, sy);
    ocr_dst_w_ = (int)(ocr_bg_w_ * s);
    ocr_dst_h_ = (int)(ocr_bg_h_ * s);
    ocr_dst_x_ = (phone_area_w - ocr_dst_w_) / 2;
    ocr_dst_y_ = (win_h - ocr_dst_h_) / 2;

    SDL_Rect img_dst{ocr_dst_x_, ocr_dst_y_, ocr_dst_w_, ocr_dst_h_};
    SDL_RenderCopy(sdl_renderer_, ocr_bg_tex_, nullptr, &img_dst);

    auto img_to_screen = [&](int ix, int iy, int& ox, int& oy) {
        ox = ocr_dst_x_ + (int)(ix * s);
        oy = ocr_dst_y_ + (int)(iy * s);
    };

    // Selection rect.
    if (ocr_drawing_) {
        int x0s, y0s, x1s, y1s;
        img_to_screen(ocr_drag_x0_, ocr_drag_y0_, x0s, y0s);
        img_to_screen(ocr_drag_x1_, ocr_drag_y1_, x1s, y1s);
        int rx = std::min(x0s, x1s), ry = std::min(y0s, y1s);
        int rw = std::abs(x1s - x0s), rh = std::abs(y1s - y0s);

        // Cut the dimmer out of the selection so the user sees the source
        // pixels unobstructed inside the rect.
        SDL_Rect sel{rx, ry, rw, rh};
        SDL_RenderCopy(sdl_renderer_, ocr_bg_tex_, nullptr, &img_dst);
        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 0);
        // We can't punch a hole; redraw selected region of the composite
        // on top of the dimmer instead.
        // Compute matching source rect in image-space.
        auto screen_to_img = [&](int xs, int ys, int& ix, int& iy) {
            ix = (int)((xs - ocr_dst_x_) / s);
            iy = (int)((ys - ocr_dst_y_) / s);
        };
        int isx0, isy0, isx1, isy1;
        screen_to_img(rx, ry, isx0, isy0);
        screen_to_img(rx + rw, ry + rh, isx1, isy1);
        isx0 = std::clamp(isx0, 0, ocr_bg_w_);
        isy0 = std::clamp(isy0, 0, ocr_bg_h_);
        isx1 = std::clamp(isx1, 0, ocr_bg_w_);
        isy1 = std::clamp(isy1, 0, ocr_bg_h_);
        SDL_Rect src{isx0, isy0, isx1 - isx0, isy1 - isy0};
        SDL_RenderCopy(sdl_renderer_, ocr_bg_tex_, &src, &sel);

        // Marching-rectangle border (solid 2-px green).
        SDL_SetRenderDrawColor(sdl_renderer_, 80, 220, 120, 255);
        for (int t = 0; t < 2; ++t) {
            SDL_Rect r{rx - t, ry - t, rw + 2 * t, rh + 2 * t};
            SDL_RenderDrawRect(sdl_renderer_, &r);
        }
    }

    // Status hint at the top — phone-shaped windows are too narrow at the
    // bottom for one-line text, so anchor at the top and auto-shrink the
    // font until it fits the window width.
    int fh = std::max(14, win_h / 42);
    std::string hint;
    if (ocr_running_.load()) {
        hint = "OCR: recognizing\u2026";
    } else if (ocr_drawing_) {
        hint = "Release to recognize \u00B7 Esc to cancel";
    } else {
        hint = "Drag a region \u00B7 Esc to cancel";
    }
    int tw = 0, th = 0;
    SDL_Texture* tex = nullptr;
    // Try shrinking the font until it fits with a 24-px side margin.
    for (int attempt = 0; attempt < 6 && fh >= 10; ++attempt) {
        if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
        tex = make_text_texture(sdl_renderer_, hint, fh, 230, 230, 230, &tw, &th);
        if (tex && tw + 24 <= phone_area_w) break;
        fh = std::max(10, (int)(fh * 0.85f));
    }
    if (tex) {
        int bg_y = 12;
        SDL_Rect bg{(phone_area_w - tw) / 2 - 12, bg_y, tw + 24, th + 12};
        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 180);
        SDL_RenderFillRect(sdl_renderer_, &bg);
        SDL_Rect d{(phone_area_w - tw) / 2, bg_y + 6, tw, th};
        SDL_RenderCopy(sdl_renderer_, tex, nullptr, &d);
        SDL_DestroyTexture(tex);
    }
}

bool Renderer::handle_ocr_event(const SDL_Event& ev) {
    if (!ocr_active_) return false;

    auto screen_to_img = [&](int xs, int ys, int& ix, int& iy) -> bool {
        if (ocr_dst_w_ <= 0 || ocr_dst_h_ <= 0) return false;
        float s = (float)ocr_dst_w_ / ocr_bg_w_;
        ix = (int)((xs - ocr_dst_x_) / s);
        iy = (int)((ys - ocr_dst_y_) / s);
        ix = std::clamp(ix, 0, ocr_bg_w_ - 1);
        iy = std::clamp(iy, 0, ocr_bg_h_ - 1);
        // Inside the displayed image rect?
        return xs >= ocr_dst_x_ && xs < ocr_dst_x_ + ocr_dst_w_ &&
               ys >= ocr_dst_y_ && ys < ocr_dst_y_ + ocr_dst_h_;
    };

    switch (ev.type) {
    case SDL_KEYDOWN:
        if (ev.key.keysym.sym == SDLK_ESCAPE) {
            // If a job is running we still drop the modal; the worker
            // thread will publish its result silently or be ignored
            // depending on whether the user cares. Simpler: just close.
            end_ocr();
            return true;
        }
        return true; // swallow keys while OCR modal is up

    case SDL_MOUSEBUTTONDOWN:
        if (ev.button.button == SDL_BUTTON_LEFT && !ocr_running_.load()) {
            int ix, iy;
            if (screen_to_img(ev.button.x, ev.button.y, ix, iy)) {
                ocr_drawing_ = true;
                ocr_drag_x0_ = ocr_drag_x1_ = ix;
                ocr_drag_y0_ = ocr_drag_y1_ = iy;
            }
        }
        return true;

    case SDL_MOUSEMOTION:
        if (ocr_drawing_) {
            int ix, iy;
            screen_to_img(ev.motion.x, ev.motion.y, ix, iy);
            ocr_drag_x1_ = ix;
            ocr_drag_y1_ = iy;
        }
        return true;

    case SDL_MOUSEBUTTONUP:
        if (ev.button.button == SDL_BUTTON_LEFT && ocr_drawing_) {
            ocr_drawing_ = false;
            int ix0 = std::min(ocr_drag_x0_, ocr_drag_x1_);
            int iy0 = std::min(ocr_drag_y0_, ocr_drag_y1_);
            int ix1 = std::max(ocr_drag_x0_, ocr_drag_x1_);
            int iy1 = std::max(ocr_drag_y0_, ocr_drag_y1_);
            int iw = ix1 - ix0;
            int ih = iy1 - iy0;
            if (iw < 8 || ih < 8) {
                toast_text_ = "OCR: selection too small";
                toast_start_ = std::chrono::steady_clock::now();
            } else {
                launch_ocr_job(ix0, iy0, iw, ih);
            }
        }
        return true;

    case SDL_WINDOWEVENT:
        return false; // don't swallow window events
    }
    return false;
}

void Renderer::launch_ocr_job(int ix, int iy, int iw, int ih) {
    if (ocr_running_.exchange(true)) return;

    // Crop into a packed RGBA buffer that the worker owns.
    std::vector<uint8_t> crop((size_t)iw * ih * 4);
    for (int y = 0; y < ih; ++y) {
        memcpy(crop.data() + (size_t)y * iw * 4,
               ocr_bg_rgba_.data() + ((size_t)(iy + y) * ocr_bg_w_ + ix) * 4,
               (size_t)iw * 4);
    }

    int w = iw, h = ih;
    std::thread([this, buf = std::move(crop), w, h]() mutable {
        OcrJobResult r = run_ocr_rgba(buf.data(), w, h);
        {
            std::lock_guard<std::mutex> lk(ocr_result_mu_);
            ocr_result_ok_ = r.ok;
            ocr_result_text_ = std::move(r.text);
            ocr_result_error_ = std::move(r.error);
            ocr_result_pending_ = true;
        }
        ocr_running_.store(false);
    }).detach();
}

void Renderer::process_ocr_result() {
    bool pending = false;
    bool ok = false;
    std::string text, err;
    {
        std::lock_guard<std::mutex> lk(ocr_result_mu_);
        if (!ocr_result_pending_) return;
        pending = true;
        ok = ocr_result_ok_;
        text = std::move(ocr_result_text_);
        err = std::move(ocr_result_error_);
        ocr_result_pending_ = false;
    }
    if (!pending) return;

    if (ok) {
        if (text.empty()) {
            toast_text_ = "OCR: no text detected";
        } else {
            SDL_SetClipboardText(text.c_str());
            // Keep the toast short so it renders at the same font size as
            // the screenshot/clipboard confirmations (the pill auto-shrinks
            // when the text gets long). The recognised text is on the
            // clipboard — no need to echo it back here.
            toast_text_ = "Text copied to clipboard \u2014 ready to paste";
            opm::UsageLog::log_feature_use("ocr_copy", active_platform_fn_ ? active_platform_fn_() : std::string());
        }
    } else {
        toast_text_ = "OCR failed: " + err;
        std::cerr << "[OCR] " << err << "\n";
    }
    toast_start_ = std::chrono::steady_clock::now();
    toast_active_ = true;
    end_ocr();
}
#endif // _WIN32

void Renderer::toggle_webcam_drawer() {
    webcam_drawer_visible_   = !webcam_drawer_visible_;
    webcam_drawer_animating_ = true;
    webcam_drawer_anim_start_ = std::chrono::steady_clock::now();
    settings_.webcam_drawer_open = webcam_drawer_visible_;
    settings_.save();
#ifdef ENABLE_WEBCAM
    if (webcam_drawer_visible_) {
        if (webcam_requires_manual_pick_) {
            return;
        }
        if (!webcam_.is_running()) {
            // Detached so we never block the UI thread while MF spins up.
            std::string dev = settings_.webcam_device_id;
            opm::Settings::set_webcam_pending();
            webcam_pending_cleared_ = false;
            std::thread([this, dev]() {
                if (!webcam_.start(dev, 1280, 720)) {
                    std::cerr << "[Webcam] start failed: "
                              << webcam_.last_error() << "\n";
                    opm::Settings::clear_webcam_pending();
                    webcam_pending_cleared_ = true;
                }
            }).detach();
        }
    } else {
        if (webcam_.is_running()) {
            std::thread([this]() {
                webcam_.stop();
                opm::Settings::clear_webcam_pending();
                webcam_pending_cleared_ = true;
            }).detach();
        }
    }
#endif
}

void Renderer::save_webcam_screenshot() {
#ifdef ENABLE_WEBCAM
    if (webcam_last_rgba_.empty() ||
        webcam_last_w_ <= 0 || webcam_last_h_ <= 0) {
        toast_text_  = "Webcam: no frame to save yet";
        toast_active_ = true;
        toast_start_ = std::chrono::steady_clock::now();
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    struct tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H-%M-%S", &local_tm);

    std::error_code ec;
    std::filesystem::create_directories(screenshot_dir_, ec);
    std::string filename = screenshot_dir_ + "/" + timestamp + " Webcam.png";

    const int w = webcam_last_w_;
    const int h = webcam_last_h_;
    const bool mirror = settings_.webcam_mirror_h;
    std::vector<uint8_t> out(webcam_last_rgba_.size());
    if (mirror) {
        for (int y = 0; y < h; ++y) {
            const uint8_t* src = webcam_last_rgba_.data() + (size_t)y * w * 4;
            uint8_t* dst       = out.data()               + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) {
                const uint8_t* sp = src + (size_t)(w - 1 - x) * 4;
                uint8_t* dp       = dst + (size_t)x * 4;
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
            }
        }
    } else {
        std::memcpy(out.data(), webcam_last_rgba_.data(), out.size());
    }

    bool wrote = stbi_write_png(filename.c_str(), w, h, 4,
                                out.data(), w * 4) != 0;
    if (wrote) {
        std::cout << "[Webcam] Saved screenshot: " << filename << "\n";
        toast_text_ = std::string("Webcam screenshot saved \u2014 ") +
                      std::filesystem::path(filename).filename().string();
    } else {
        std::cerr << "[Webcam] Failed to write " << filename << "\n";
        toast_text_ = "Webcam screenshot failed (see log)";
    }
    toast_active_ = true;
    toast_start_  = std::chrono::steady_clock::now();
#endif
}

void Renderer::draw_webcam_drawer() {
    if (frame_dst_w_ == 0 || webcam_drawer_anim_ < 0.01f) return;

    // Design mirrors draw_log_panel() rotated 90 degrees:
    //   * The log panel sits flush against the phone's RIGHT edge, inset
    //     vertically by frame_dst_h_/16 from top and bottom so it stays
    //     clear of the phone's side corner curves, and its FAR edge
    //     (right) is rounded. It also leaves a small margin of window
    //     background between the panel's far edge and the window edge.
    //   * The webcam drawer sits flush against the phone's BOTTOM edge,
    //     inset HORIZONTALLY by frame_dst_w_/16 from left and right so it
    //     stays clear of the phone's bottom corner curves, and its FAR
    //     edge (bottom) is rounded. We leave a matching margin between
    //     the panel's bottom edge and the window bottom.
    //
    // The result reads as "a drawer slides DOWN out of the phone bottom"
    // exactly like the log panel reads as "a drawer slides RIGHT out of
    // the phone side".
    // Inset to match the STRAIGHT portion of the phone's bottom bezel
    // — i.e. the panel's flat top edge spans exactly the section of
    // the phone bottom that isn't curving away into the corner arcs.
    float frame_scale = phone_frame_.frame_width() > 0
        ? (float)frame_dst_w_ / (float)phone_frame_.frame_width()
        : 1.0f;
    int drawer_inset = std::max(frame_dst_w_ / 16,
                                (int)(phone_frame_.corner_radius() * frame_scale));
    int wd_margin    = std::max(4, frame_dst_w_ / 40);
    int panel_x = frame_dst_x_ + drawer_inset;
    int panel_w = frame_dst_w_ - drawer_inset * 2;
    int full_h  = webcam_drawer_full_h_;
    int panel_h = (int)(full_h * webcam_drawer_anim_) - wd_margin;
    if (panel_w < 40 || panel_h < 8) return;

    int phone_bottom_y = frame_dst_y_ + frame_dst_h_;
    int panel_y = phone_bottom_y;
    int pr      = std::max(8, panel_w / 30);
    if (pr > panel_h - 1) pr = std::max(2, panel_h - 1);

    uint8_t dr, dg, db; drawer_color(dr, dg, db);
    auto lite = [](uint8_t c, int d) -> uint8_t {
        int v = (int)c + d; if (v < 0) v = 0; if (v > 255) v = 255;
        return (uint8_t)v;
    };
    uint8_t br_ = phone_frame_.bezel_r();
    uint8_t bg_ = phone_frame_.bezel_g();
    uint8_t bb_ = phone_frame_.bezel_b();
    uint8_t er = lite(br_, 30), eg = lite(bg_, 30), eb = lite(bb_, 32);
    int bw = std::max(2, std::min(3, panel_w / 200 + 2));

    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);

    int cy_b  = panel_y + panel_h - 1 - pr;
    int cx_lc = panel_x + pr;
    int cx_rc = panel_x + panel_w - 1 - pr;

    // 1. Interior fill (everything above the corner-centre row).
    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
    SDL_Rect body_top  = {panel_x + bw, panel_y, panel_w - bw * 2, cy_b - panel_y};
    SDL_RenderFillRect(sdl_renderer_, &body_top);
    SDL_Rect body_strip = {cx_lc, cy_b, cx_rc - cx_lc + 1, pr - bw};
    SDL_RenderFillRect(sdl_renderer_, &body_strip);

    // 2. Border strips (left / right / bottom-straight segment).
    SDL_SetRenderDrawColor(sdl_renderer_, er, eg, eb, 255);
    SDL_Rect e_lft = {panel_x, panel_y, bw, cy_b - panel_y};
    SDL_RenderFillRect(sdl_renderer_, &e_lft);
    SDL_Rect e_rgt = {panel_x + panel_w - bw, panel_y, bw, cy_b - panel_y};
    SDL_RenderFillRect(sdl_renderer_, &e_rgt);
    SDL_Rect e_bot = {cx_lc, panel_y + panel_h - bw, cx_rc - cx_lc + 1, bw};
    SDL_RenderFillRect(sdl_renderer_, &e_bot);

    // 3. Two bottom corners — coverage-based AA, same approach as
    //    draw_log_panel() to match the visual language exactly. quad==2
    //    means bottom-left (dx<=0, dy>=0); quad==3 means bottom-right
    //    (dx>=0, dy>=0).
    auto paint_corner = [&](int cx, int cy, int quad) {
        float r_out = (float)pr;
        float r_in  = (float)(pr - bw);
        for (int dy = -pr - 1; dy <= pr + 1; dy++) {
            for (int dx = -pr - 1; dx <= pr + 1; dx++) {
                if (quad == 2 && !(dx <= 0 && dy >= 0)) continue;
                if (quad == 3 && !(dx >= 0 && dy >= 0)) continue;
                float d = std::sqrt((float)(dx * dx + dy * dy));
                float cov_out = std::clamp(r_out + 0.5f - d, 0.0f, 1.0f);
                if (cov_out <= 0.0f) continue;
                float cov_in  = std::clamp(r_in  + 0.5f - d, 0.0f, 1.0f);
                int px = cx + dx;
                int py = cy + dy;
                if (cov_in >= 1.0f) {
                    SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db, 255);
                    SDL_RenderDrawPoint(sdl_renderer_, px, py);
                } else if (cov_out >= 1.0f && cov_in <= 0.0f) {
                    SDL_SetRenderDrawColor(sdl_renderer_, er, eg, eb, 255);
                    SDL_RenderDrawPoint(sdl_renderer_, px, py);
                } else {
                    // Anti-aliased seam — blend drawer/border/destination.
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);
                    if (cov_in > 0.0f && cov_in < 1.0f) {
                        SDL_SetRenderDrawColor(sdl_renderer_, dr, dg, db,
                                               (uint8_t)(cov_in * 255));
                        SDL_RenderDrawPoint(sdl_renderer_, px, py);
                        float band = cov_out - cov_in;
                        if (band > 0.0f) {
                            SDL_SetRenderDrawColor(sdl_renderer_, er, eg, eb,
                                                   (uint8_t)(band * 255));
                            SDL_RenderDrawPoint(sdl_renderer_, px, py);
                        }
                    } else {
                        SDL_SetRenderDrawColor(sdl_renderer_, er, eg, eb,
                                               (uint8_t)(cov_out * 255));
                        SDL_RenderDrawPoint(sdl_renderer_, px, py);
                    }
                    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
                }
            }
        }
    };
    paint_corner(cx_lc, cy_b, 2);
    paint_corner(cx_rc, cy_b, 3);

    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_BLEND);

    // --- Live webcam image -------------------------------------------------
    // Inner content rect (sit inside the border so the image never
    // overlaps the rounded corners).
    int inner_x = panel_x + bw + 2;
    int inner_y = panel_y + bw + 2;
    int inner_w = panel_w - (bw + 2) * 2;
    int inner_h = panel_h - (bw + 2) * 2;
    if (inner_w <= 4 || inner_h <= 4) return;

    // The in-drawer controls (screenshot button + camera-switch dots)
    // are OVERLAID on top of the image instead of reserving horizontal
    // rails, so the camera frame keeps the full width it had before
    // the buttons were added. They're sized off the bottom-bezel dot
    // metrics so they read at the same visual weight as the rest of
    // the app and tucked into the inner edges of the drawer.
    webcam_shot_btn_ = {};
    webcam_cam_btns_.clear();
    int rail_btn_sz = std::max(14, ui_ref_width() / 22);
    if (rail_btn_sz > inner_h - 6) rail_btn_sz = std::max(12, inner_h - 6);
    // Tucked tight to the inner edge so they sit out of the way of the
    // camera frame. A 2 px hairline gap keeps them off the rounded
    // border without eating noticeable image area.
    int rail_pad   = 0;
    int img_x      = inner_x;
    int img_w      = inner_w;
    int img_y      = inner_y;
    int img_h      = inner_h;

    // Poll the capture worker once per render tick (no-op if no fresh
    // frame). poll_webcam_frame() also keeps webcam_last_rgba_ in sync
    // so the recorder composite path can grab the pixels even when the
    // drawer is collapsed mid-recording.
    poll_webcam_frame();

    if (webcam_texture_ && webcam_tex_w_ > 0 && webcam_tex_h_ > 0) {
        // Aspect-fit (letterbox) the camera frame into the image rect.
        float src_ar = (float)webcam_tex_w_ / (float)webcam_tex_h_;
        float dst_ar = (float)img_w / (float)img_h;
        int draw_w, draw_h;
        if (src_ar > dst_ar) {
            draw_w = img_w;
            draw_h = (int)(img_w / src_ar);
        } else {
            draw_h = img_h;
            draw_w = (int)(img_h * src_ar);
        }
        SDL_Rect dst = { img_x + (img_w - draw_w) / 2,
                         img_y + (img_h - draw_h) / 2,
                         draw_w, draw_h };
        SDL_RendererFlip flip = settings_.webcam_mirror_h
                                    ? SDL_FLIP_HORIZONTAL
                                    : SDL_FLIP_NONE;
        SDL_RenderCopyEx(sdl_renderer_, webcam_texture_,
                         nullptr, &dst, 0.0, nullptr, flip);
    } else if (webcam_.is_running()) {
        // Camera is warming up — leave the drawer blank (already filled
        // with the drawer body colour above).
    } else {
        // Not running: render a faint status hint so the user knows
        // why the drawer is dark.
        std::string msg;
        if (webcam_requires_manual_pick_) {
            msg = "Webcam crashed previously. Pick another camera from the menu or side dots.";
        } else {
            msg = webcam_.last_error().empty()
                      ? std::string("Right-click the bottom button to pick a camera")
                      : ("Webcam error: " + webcam_.last_error());
        }
        int eq_w = ui_ref_width();
        int font_h = std::max(11, eq_w / 44);
        int tw = 0, th = 0;
        SDL_Texture* tex = make_text_texture(
            sdl_renderer_, msg, font_h, 200, 200, 200, &tw, &th);
        if (tex) {
            SDL_Rect dst = { img_x + (img_w - tw) / 2,
                             img_y + (img_h - th) / 2,
                             tw, th };
            SDL_RenderCopy(sdl_renderer_, tex, nullptr, &dst);
            SDL_DestroyTexture(tex);
        }
    }

#ifdef ENABLE_WEBCAM
    if (inner_w > rail_btn_sz * 3 && inner_h >= rail_btn_sz + 4) {
        int mx, my;
        SDL_GetMouseState(&mx, &my);

        // ----- Left overlay: webcam-only screenshot button -----------------
        int sb_x = inner_x + rail_pad;
        int sb_y = inner_y + (inner_h - rail_btn_sz) / 2;
        webcam_shot_btn_ = {sb_x, sb_y, rail_btn_sz, rail_btn_sz};
        bool sb_hover = in_rect(mx, my, sb_x, sb_y, rail_btn_sz, rail_btn_sz);
        bool sb_enabled = webcam_texture_ && webcam_tex_w_ > 0;
        // Slightly more opaque than the bezel buttons because the
        // background here might be the live camera image rather than
        // the dark drawer body.
        uint8_t sb_bg = sb_hover ? 70 : 50;
        SDL_SetRenderDrawColor(sdl_renderer_, sb_bg, sb_bg, sb_bg + 5, 210);
        fill_circle(sdl_renderer_, sb_x + rail_btn_sz / 2,
                    sb_y + rail_btn_sz / 2, rail_btn_sz / 2);
        int ssr  = rail_btn_sz / 4;
        int sscx = sb_x + rail_btn_sz / 2;
        int sscy = sb_y + rail_btn_sz / 2;
        uint8_t dot_a = sb_enabled
                            ? (sb_hover ? 255 : 220)
                            : 90; // dimmed when no frame yet
        SDL_SetRenderDrawColor(sdl_renderer_, 240, 240, 240, dot_a);
        fill_circle(sdl_renderer_, sscx, sscy, ssr);
        if (sb_hover && tooltip_ready("wcdraw_shot")) {
            pending_tooltip_text_  = sb_enabled ? "Save webcam screenshot"
                                                : "Webcam not ready yet";
            // Anchor just inside the right edge of the button so the
            // tooltip extends INTO the frame instead of clipping past
            // the drawer's left edge.
            pending_tooltip_x_      = sb_x + rail_btn_sz;
            pending_tooltip_y_      = my; // vertically follow the mouse
            pending_tooltip_below_  = false;
            pending_tooltip_align_  = 2; // tooltip extends to the RIGHT
            pending_tooltip_valign_ = 1; // vertically centered on mouse Y
        }

        // ----- Right overlay: one dot per available camera ----------------
        // Same visual language as the resize-grip dots: small white
        // filled circles with hover-brightened alpha. The ACTIVE camera
        // is drawn brighter and wrapped in a thin ring so it reads as
        // selected without needing a colour change.
        auto cams = WebcamCapture::enumerate();
        if (!cams.empty()) {
            int dot_r    = std::max(3, ui_ref_width() / 110);
            int hit_sz   = std::max(rail_btn_sz / 2, dot_r * 5);
            int gap      = std::max(4, dot_r * 2);
            int total_h  = (int)cams.size() * hit_sz +
                           (int)std::max<size_t>(0, cams.size() - 1) * gap;
            int rail_x   = inner_x + inner_w - rail_pad - hit_sz;
            int rail_y0  = inner_y + (inner_h - total_h) / 2;
            for (size_t i = 0; i < cams.size(); ++i) {
                int dx_x = rail_x;
                int dx_y = rail_y0 + (int)i * (hit_sz + gap);
                BtnRect r{dx_x, dx_y, hit_sz, hit_sz};
                webcam_cam_btns_.push_back({cams[i].id, r});
                bool sel = (cams[i].id == settings_.webcam_device_id) ||
                           (settings_.webcam_device_id.empty() && i == 0);
                bool hov = in_rect(mx, my, dx_x, dx_y, hit_sz, hit_sz);
                // Tuck a darker chip behind the dot so it stays readable
                // when overlaid on a bright part of the camera frame.
                SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 35,
                                       (uint8_t)(hov ? 180 : 130));
                fill_circle(sdl_renderer_, dx_x + hit_sz / 2,
                            dx_y + hit_sz / 2, dot_r + 3);
                uint8_t a = sel ? 255 : (hov ? 230 : 170);
                SDL_SetRenderDrawColor(sdl_renderer_, 220, 220, 220, a);
                fill_circle(sdl_renderer_, dx_x + hit_sz / 2,
                            dx_y + hit_sz / 2, dot_r);
                if (sel) {
                    // Thin selection ring (1 px) just outside the dot.
                    int ring = dot_r + 2;
                    int cx = dx_x + hit_sz / 2;
                    int cy = dx_y + hit_sz / 2;
                    SDL_SetRenderDrawColor(sdl_renderer_, 240, 240, 240, 220);
                    for (int dy = -ring; dy <= ring; ++dy) {
                        for (int dx = -ring; dx <= ring; ++dx) {
                            int d2 = dx * dx + dy * dy;
                            if (d2 <= ring * ring &&
                                d2 >= (ring - 1) * (ring - 1))
                                SDL_RenderDrawPoint(sdl_renderer_, cx + dx, cy + dy);
                        }
                    }
                }
                if (hov && tooltip_ready("wcdraw_cam:" + cams[i].id)) {
                    // Strip non-ASCII so SDL_ttf renders cleanly (matches
                    // the right-click picker's sanitiser).
                    std::string label;
                    label.reserve(cams[i].name.size());
                    for (unsigned char c : cams[i].name) {
                        if (c >= 0x20 && c < 0x7F) label.push_back((char)c);
                        else if (c == ' ' || c == '\t') label.push_back(' ');
                    }
                    if (label.empty()) label = "(camera)";
                    pending_tooltip_text_  = (sel ? "Active: "
                                                  : "Switch to: ") + label;
                    // Anchor at the inside (left) edge of the dot so the
                    // tooltip extends INTO the frame instead of clipping
                    // off the drawer's right edge.
                    pending_tooltip_x_      = dx_x;
                    pending_tooltip_y_      = my; // vertically follow the mouse
                    pending_tooltip_below_  = false;
                    pending_tooltip_align_  = 1; // tooltip extends to the LEFT
                    pending_tooltip_valign_ = 1; // vertically centered on mouse Y
                }
            }
        }
    }
#endif
}

bool Renderer::should_include_webcam_in_capture() const {
#ifdef ENABLE_WEBCAM
    return webcam_drawer_visible_
        && webcam_.is_running()
        && !webcam_last_rgba_.empty()
        && webcam_last_w_ > 0 && webcam_last_h_ > 0;
#else
    return false;
#endif
}

void Renderer::paint_webcam_strip(uint8_t* canvas, int canvas_w,
                                  int base_h, int panel_h,
                                  int /*overlap_top*/) const {
    if (!canvas || canvas_w <= 0 || panel_h <= 0) return;

    // Mirrors draw_webcam_drawer(): the on-screen drawer is inset inside
    // the phone-bezel corner curves, body filled with drawer_color, then
    // bordered with a slightly-lightened bezel tint and rounded on the
    // BOTTOM corners only (top stays flush with the phone). We replicate
    // that exactly on the CPU canvas so screenshots and recordings show
    // the webcam framed the same way the user sees it.
    //
    // The strip's `base_h` row is the visual TOP of the panel. Callers
    // can place that row INSIDE the phone composite (passing
    // `phone_h - overlap`) so the panel slides up behind the phone
    // bezel — the bezel is alpha-composited on top afterwards and hides
    // the overlap rows.
    uint8_t dr, dg, db;
    const_cast<Renderer*>(this)->drawer_color(dr, dg, db);
    auto lite = [](uint8_t c, int d) -> uint8_t {
        int v = (int)c + d; if (v < 0) v = 0; if (v > 255) v = 255;
        return (uint8_t)v;
    };
    uint8_t bzr  = phone_frame_.bezel_r();
    uint8_t bzg  = phone_frame_.bezel_g();
    uint8_t bzb  = phone_frame_.bezel_b();
    uint8_t er   = lite(bzr, 30);
    uint8_t eg   = lite(bzg, 30);
    uint8_t eb   = lite(bzb, 32);

    int inset   = canvas_w / 16;
    int panel_x = inset;
    int panel_w = canvas_w - inset * 2;
    if (panel_w < 8) return;
    int bw = std::max(2, std::min(3, panel_w / 200 + 2));
    int pr = std::max(8, panel_w / 30);
    if (pr > panel_h - 1) pr = std::max(2, panel_h - 1);

    auto set_px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b,
                      uint8_t a) {
        if (x < 0 || x >= canvas_w) return;
        if (y < base_h || y >= base_h + panel_h) return;
        uint8_t* p = canvas + ((size_t)y * canvas_w + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    };

    // 0) Start the strip fully transparent so the inset bands beside the
    //    panel "hang" off the phone exactly like the on-screen drawer.
    for (int y = base_h; y < base_h + panel_h; ++y) {
        memset(canvas + (size_t)y * canvas_w * 4, 0, (size_t)canvas_w * 4);
    }

    int py_top = base_h;
    int py_bot = base_h + panel_h - 1;
    int cy_b   = py_bot - pr;
    int cx_lc  = panel_x + pr;
    int cx_rc  = panel_x + panel_w - 1 - pr;

    // 1) Panel body fill (above the corner-centre row, plus the strip
    //    between the two bottom corners).
    for (int y = py_top; y < cy_b; ++y)
        for (int x = panel_x + bw; x < panel_x + panel_w - bw; ++x)
            set_px(x, y, dr, dg, db, 255);
    for (int y = cy_b; y <= py_bot - bw; ++y)
        for (int x = cx_lc; x <= cx_rc; ++x)
            set_px(x, y, dr, dg, db, 255);

    // 2) Border strips (left / right / bottom-straight segment).
    for (int y = py_top; y < cy_b; ++y) {
        for (int dx = 0; dx < bw; ++dx) {
            set_px(panel_x + dx, y, er, eg, eb, 255);
            set_px(panel_x + panel_w - 1 - dx, y, er, eg, eb, 255);
        }
    }
    for (int dy = 0; dy < bw; ++dy)
        for (int x = cx_lc; x <= cx_rc; ++x)
            set_px(x, py_bot - dy, er, eg, eb, 255);

    // 3) Bottom corners — coverage-based AA, exactly the same recipe as
    //    draw_webcam_drawer()'s paint_corner lambda. Outer coverage drives
    //    alpha so the corner anti-aliases against the transparent matte.
    auto paint_corner = [&](int cx, int cy, int quad) {
        float r_out = (float)pr;
        float r_in  = (float)(pr - bw);
        for (int dy = -pr - 1; dy <= pr + 1; ++dy) {
            for (int dx = -pr - 1; dx <= pr + 1; ++dx) {
                if (quad == 2 && !(dx <= 0 && dy >= 0)) continue;
                if (quad == 3 && !(dx >= 0 && dy >= 0)) continue;
                float d = std::sqrt((float)(dx * dx + dy * dy));
                float cov_out = std::clamp(r_out + 0.5f - d, 0.0f, 1.0f);
                if (cov_out <= 0.0f) continue;
                float cov_in  = std::clamp(r_in + 0.5f - d, 0.0f, 1.0f);
                int px = cx + dx;
                int py = cy + dy;
                if (cov_in >= 1.0f) {
                    set_px(px, py, dr, dg, db, 255);
                } else if (cov_out >= 1.0f && cov_in <= 0.0f) {
                    set_px(px, py, er, eg, eb, 255);
                } else {
                    float body = cov_in;
                    float ring = std::max(0.0f, cov_out - cov_in);
                    uint8_t r = (uint8_t)(dr * body + er * ring);
                    uint8_t g = (uint8_t)(dg * body + eg * ring);
                    uint8_t b = (uint8_t)(db * body + eb * ring);
                    uint8_t a = (uint8_t)(cov_out * 255);
                    set_px(px, py, r, g, b, a);
                }
            }
        }
    };
    paint_corner(cx_lc, cy_b, 2);
    paint_corner(cx_rc, cy_b, 3);

#ifdef ENABLE_WEBCAM
    // 4) Letterbox the freshest webcam frame inside the bordered inner
    //    rect (same +2 padding as the on-screen drawer so the image never
    //    overlaps the rounded corners).
    if (webcam_last_rgba_.empty() ||
        webcam_last_w_ <= 0 || webcam_last_h_ <= 0) {
        return;
    }
    int inner_x = panel_x + bw + 2;
    int inner_y = py_top + bw + 2;
    int inner_w = panel_w - (bw + 2) * 2;
    int inner_h = panel_h - (bw + 2) * 2;
    if (inner_w <= 4 || inner_h <= 4) return;

    float src_ar = (float)webcam_last_w_ / (float)webcam_last_h_;
    float dst_ar = (float)inner_w / (float)inner_h;
    int draw_w, draw_h;
    if (src_ar > dst_ar) {
        draw_w = inner_w;
        draw_h = (int)(inner_w / src_ar);
    } else {
        draw_h = inner_h;
        draw_w = (int)(inner_h * src_ar);
    }
    int dst_x = inner_x + (inner_w - draw_w) / 2;
    int dst_y = inner_y + (inner_h - draw_h) / 2;
    const bool mirror = settings_.webcam_mirror_h;

    for (int y = 0; y < draw_h; ++y) {
        int sy = (int)((int64_t)y * webcam_last_h_ / draw_h);
        if (sy < 0) sy = 0;
        if (sy >= webcam_last_h_) sy = webcam_last_h_ - 1;
        const uint8_t* src_row =
            webcam_last_rgba_.data() + (size_t)sy * webcam_last_w_ * 4;
        uint8_t* dst_row =
            canvas + (size_t)(dst_y + y) * canvas_w * 4 + (size_t)dst_x * 4;
        for (int x = 0; x < draw_w; ++x) {
            int sx = (int)((int64_t)x * webcam_last_w_ / draw_w);
            if (mirror) sx = webcam_last_w_ - 1 - sx;
            if (sx < 0) sx = 0;
            if (sx >= webcam_last_w_) sx = webcam_last_w_ - 1;
            const uint8_t* sp = src_row + (size_t)sx * 4;
            dst_row[x * 4 + 0] = sp[0];
            dst_row[x * 4 + 1] = sp[1];
            dst_row[x * 4 + 2] = sp[2];
            dst_row[x * 4 + 3] = 255;
        }
    }
#endif
}

void Renderer::poll_webcam_frame() {
    WebcamFrame latest;
    if (!webcam_.get_latest(latest) ||
        latest.width <= 0 || latest.height <= 0 || latest.rgba.empty()) {
        return;
    }

    // Refresh the GPU texture used by the drawer.
    if (!webcam_texture_ ||
        webcam_tex_w_ != latest.width ||
        webcam_tex_h_ != latest.height) {
        if (webcam_texture_) {
            SDL_DestroyTexture(webcam_texture_);
            webcam_texture_ = nullptr;
        }
        webcam_texture_ = SDL_CreateTexture(
            sdl_renderer_, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING, latest.width, latest.height);
        if (webcam_texture_) {
            webcam_tex_w_ = latest.width;
            webcam_tex_h_ = latest.height;
            SDL_SetTextureBlendMode(webcam_texture_, SDL_BLENDMODE_NONE);
        } else {
            webcam_tex_w_ = 0;
            webcam_tex_h_ = 0;
        }
    }
    if (webcam_texture_) {
        SDL_UpdateTexture(webcam_texture_, nullptr,
                          latest.rgba.data(), latest.width * 4);
    }

    // First valid frame from the active device — clear the crash-guard
    // marker so the next launch will auto-open as normal.
    if (!webcam_pending_cleared_) {
        opm::Settings::clear_webcam_pending();
        webcam_pending_cleared_ = true;
    }

    // Keep a CPU copy for the recorder composite path.
    webcam_last_w_ = latest.width;
    webcam_last_h_ = latest.height;
    webcam_last_rgba_ = std::move(latest.rgba);
}

} // namespace opm::media
