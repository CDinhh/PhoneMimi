#include <opm/android/scrcpy_util.h>

#include <cmath>

namespace opm::android {

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
} // namespace

size_t parse_device_messages(const uint8_t* buf, size_t n, std::vector<std::string>* clips) {
    size_t off = 0;
    while (off < n) {
        const uint8_t* p = buf + off;
        size_t left = n - off;
        switch (p[0]) {
            case 0: {  // clipboard: type, u32 length, UTF-8 text
                if (left < 5) return off;
                size_t len = be32(p + 1);
                if (left < 5 + len) return off;
                clips->emplace_back(reinterpret_cast<const char*>(p + 5), len);
                off += 5 + len;
                break;
            }
            case 1:  // ack clipboard: type, u64 sequence
                if (left < 9) return off;
                off += 9;
                break;
            case 2: {  // UHID output: type, u16 id, u16 size, data
                if (left < 5) return off;
                size_t len = be16(p + 3);
                if (left < 5 + len) return off;
                off += 5 + len;
                break;
            }
            default:
                return SIZE_MAX;
        }
    }
    return off;
}

void PcmResampler48to441::feed(const int16_t* in, size_t frames, std::vector<int16_t>* out) {
    const double step = 48000.0 / 44100.0;
    // Sample i of this call is input frame i; index -1 is last_.
    auto at = [&](long i, int ch) -> double {
        if (i < 0) return have_last_ ? last_[ch] : (frames ? in[ch] : 0);
        return in[size_t(i) * 2 + ch];
    };
    while (pos_ + 1 < double(frames)) {
        long i0 = long(std::floor(pos_));
        double f = pos_ - double(i0);
        for (int ch = 0; ch < 2; ++ch) {
            double v = at(i0, ch) * (1.0 - f) + at(i0 + 1, ch) * f;
            out->push_back(int16_t(std::lround(v)));
        }
        pos_ += step;
    }
    if (frames) {
        last_[0] = in[(frames - 1) * 2];
        last_[1] = in[(frames - 1) * 2 + 1];
        have_last_ = true;
        pos_ -= double(frames);   // carry the fraction into the next call
    }
}

} // namespace opm::android
