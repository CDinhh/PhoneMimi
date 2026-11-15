#pragma once

// Windows.Media.Ocr (WinRT) wrapper.
//
// Synchronous, single-shot recognition for a packed RGBA8 buffer. Designed
// to be called from a worker thread spawned per OCR job by the renderer —
// initializing the WinRT apartment + creating an OcrEngine takes long
// enough that we never want to block the render thread on it.

#include <cstdint>
#include <string>
#include <vector>

namespace opm::media {

struct OcrLine {
    std::string text;   // UTF-8
    int x = 0, y = 0, w = 0, h = 0;   // pixels of the input buffer
};

struct OcrJobResult {
    bool ok = false;
    std::string text;   // joined lines, '\n'-separated, UTF-8
    std::string error;  // populated when ok == false
    std::vector<OcrLine> lines;
};

// Run OCR on a tightly-packed RGBA8 buffer (no row padding). Caller must
// keep `rgba` alive for the duration of the call.
OcrJobResult run_ocr_rgba(const uint8_t* rgba, int w, int h);

} // namespace opm::media
