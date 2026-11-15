#pragma once

// Plays a short MP4 (the first-connect tip videos) frame by frame, looping.
// Decoding happens on the caller's thread; the clips are tiny (720x800,
// a few seconds), so it is cheap enough to do while rendering.

#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace opm::media {

class ClipPlayer {
public:
    ClipPlayer() = default;
    ~ClipPlayer();
    ClipPlayer(const ClipPlayer&) = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;

    bool open(const std::string& path_utf8);
    void close();
    bool is_open() const { return fmt_ != nullptr; }
    int width() const { return w_; }
    int height() const { return h_; }

    // Advances to the frame shown at `t` seconds (wrapped to the clip
    // length). Returns true and fills *rgba (w*h*4) when the frame changed.
    bool frame_at(double t, std::vector<uint8_t>* rgba);

private:
    bool decode_next_(double* pts_s);   // one frame; false at end of file
    void rewind_();

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;
    int stream_ = -1;
    int w_ = 0, h_ = 0;
    double tb_ = 0.0, dur_ = 0.0;
    double shown_pts_ = -1.0;
};

} // namespace opm::media
