#include "fx/pitch_shift.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

PitchShiftFX::PitchShiftFX() : buf_(static_cast<std::size_t>(NFR) * 2u, 0.f) {}

void PitchShiftFX::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.f);
    wi_    = 0;
    lag_a_ = TARGET_LAG;
    lag_b_ = TARGET_LAG;
    xf_    = 0.f;
    xf_on_ = false;
}

static float read_lin2(const float *buf, unsigned N, unsigned wi, float lag, unsigned ch) {
    float pos = float(static_cast<int>(wi)) - lag;
    while (pos < 0.f)
        pos += float(N);
    while (pos >= float(N))
        pos -= float(N);
    unsigned i0 = static_cast<unsigned>(pos);
    if (i0 >= N)
        i0 %= N;
    unsigned i1 = (i0 + 1u) % N;
    float t = pos - std::floor(pos);
    return buf[i0 * 2u + ch] * (1.f - t) + buf[i1 * 2u + ch] * t;
}

void PitchShiftFX::process(float *samples, unsigned frames, unsigned channels,
                           unsigned sample_rate) {
    (void)sample_rate;
    if (channels < 1 || channels > 2)
        return;

    const float wet = std::clamp(wet_mix.load(std::memory_order_relaxed), 0.f, 1.f);
    const float dry = 1.f - wet;
    int si          = semitones_idx.load(std::memory_order_relaxed);
    si              = std::clamp(si, 0, PS_SEMI_STEPS - 1);
    const float semi = float(si - PS_SEMI_CENTER);
    const float ratio = std::pow(2.f, semi / 12.f);
    const float dx    = 1.f / XF_LEN;

    for (unsigned f = 0; f < frames; f++) {
        const float in_l = samples[f * channels + 0];
        const float in_r = channels > 1 ? samples[f * channels + 1] : in_l;

        auto shifted = [&](unsigned ch, float in) {
            const float ya = read_lin2(buf_.data(), NFR, wi_, lag_a_, ch);
            const float yb = read_lin2(buf_.data(), NFR, wi_, lag_b_, ch);
            const float y  = ya * (1.f - xf_) + yb * xf_;
            return in * dry + y * wet;
        };

        const float out_l = shifted(0, in_l);
        const float out_r = shifted(1, in_r);

        buf_[static_cast<std::size_t>(wi_) * 2u + 0u] = in_l;
        buf_[static_cast<std::size_t>(wi_) * 2u + 1u] = in_r;
        wi_                                             = (wi_ + 1u) % NFR;

        lag_a_ += ratio - 1.f;
        lag_b_ += ratio - 1.f;

        if (xf_on_) {
            xf_ += dx;
            if (xf_ >= 1.f) {
                lag_a_ = lag_b_;
                xf_    = 0.f;
                xf_on_ = false;
            }
        } else if (lag_a_ < MIN_LAG || lag_a_ > MAX_LAG) {
            lag_b_ = TARGET_LAG;
            xf_    = 0.f;
            xf_on_ = true;
        }

        samples[f * channels + 0] = out_l;
        if (channels > 1)
            samples[f * channels + 1] = out_r;
    }
}
