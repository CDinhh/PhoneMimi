#pragma once

// Small pure pieces of the scrcpy client, kept apart so the self-test can
// check them without a phone.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opm::android {

// Splits scrcpy device->computer messages (control socket). Appends the
// text of every complete clipboard message to *clips, skips ack and UHID
// messages, and returns how many bytes of `buf` were consumed (a partial
// message at the end is left for the next read). Returns SIZE_MAX on an
// unknown message type (the stream can no longer be followed).
size_t parse_device_messages(const uint8_t* buf, size_t n, std::vector<std::string>* clips);

// Linear resampler for interleaved stereo s16: 48000 Hz -> 44100 Hz.
class PcmResampler48to441 {
public:
    // Appends the output frames for `frames` input frames to *out.
    void feed(const int16_t* in, size_t frames, std::vector<int16_t>* out);

private:
    double pos_ = 0.0;          // position of the next output frame, in input frames
    int16_t last_[2] = {0, 0};  // last input frame of the previous call
    bool have_last_ = false;
};

} // namespace opm::android
