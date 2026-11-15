#include <opm/media/clip_player.h>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace opm::media {

ClipPlayer::~ClipPlayer() { close(); }

void ClipPlayer::close() {
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (dec_) avcodec_free_context(&dec_);
    if (fmt_) avformat_close_input(&fmt_);
    stream_ = -1;
    w_ = h_ = 0;
    shown_pts_ = -1.0;
}

bool ClipPlayer::open(const std::string& path) {
    close();
    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) { fmt_ = nullptr; return false; }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) { close(); return false; }
    stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_ < 0) { close(); return false; }
    AVStream* st = fmt_->streams[stream_];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { close(); return false; }
    dec_ = avcodec_alloc_context3(codec);
    if (!dec_ || avcodec_parameters_to_context(dec_, st->codecpar) < 0 ||
        avcodec_open2(dec_, codec, nullptr) < 0) { close(); return false; }
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    w_ = dec_->width;
    h_ = dec_->height;
    tb_ = av_q2d(st->time_base);
    dur_ = fmt_->duration > 0 ? fmt_->duration / (double)AV_TIME_BASE
                              : (st->duration > 0 ? st->duration * tb_ : 0.0);
    return w_ > 0 && h_ > 0 && dur_ > 0.0;
}

void ClipPlayer::rewind_() {
    av_seek_frame(fmt_, stream_, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_);
    shown_pts_ = -1.0;
}

bool ClipPlayer::decode_next_(double* pts_s) {
    for (;;) {
        int r = avcodec_receive_frame(dec_, frame_);
        if (r == 0) {
            int64_t p = frame_->best_effort_timestamp;
            *pts_s = p == AV_NOPTS_VALUE ? shown_pts_ + 1.0 / 30.0 : p * tb_;
            return true;
        }
        if (r != AVERROR(EAGAIN)) return false;
        if (av_read_frame(fmt_, pkt_) < 0) {
            avcodec_send_packet(dec_, nullptr);   // drain
            continue;
        }
        if (pkt_->stream_index == stream_) avcodec_send_packet(dec_, pkt_);
        av_packet_unref(pkt_);
    }
}

bool ClipPlayer::frame_at(double t, std::vector<uint8_t>* rgba) {
    if (!fmt_) return false;
    t = std::fmod(std::max(0.0, t), dur_);
    if (t + 1e-3 < shown_pts_) rewind_();      // looped back to the start
    bool got = false;
    double pts = 0.0;
    // Decode up to the frame on screen at `t` (at most a clip's worth).
    for (int guard = 0; guard < 600; ++guard) {
        if (shown_pts_ >= 0.0 && shown_pts_ + 1.0 / 60.0 > t) break;
        if (!decode_next_(&pts)) { rewind_(); break; }
        shown_pts_ = pts;
        got = true;
        if (pts + 1.0 / 60.0 >= t) break;
    }
    if (!got) return false;
    sws_ = sws_getCachedContext(sws_, w_, h_, (AVPixelFormat)frame_->format, w_, h_, AV_PIX_FMT_RGBA,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return false;
    rgba->resize((size_t)w_ * h_ * 4);
    uint8_t* dst[4] = {rgba->data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {w_ * 4, 0, 0, 0};
    sws_scale(sws_, frame_->data, frame_->linesize, 0, h_, dst, dst_stride);
    return true;
}

} // namespace opm::media
