#include "fx/reverb.hpp"
#include <algorithm>

ReverbFX::ReverbFX()
{
    static constexpr unsigned L0 = 3600, L1 = 3700, L2 = 3400, L3 = 3300;
    c0_.assign(L0, 0.0f);
    c1_.assign(L1, 0.0f);
    c2_.assign(L2, 0.0f);
    c3_.assign(L3, 0.0f);
}

void ReverbFX::reset() {
    std::fill(c0_.begin(), c0_.end(), 0.0f);
    std::fill(c1_.begin(), c1_.end(), 0.0f);
    std::fill(c2_.begin(), c2_.end(), 0.0f);
    std::fill(c3_.begin(), c3_.end(), 0.0f);
    p0_ = p1_ = p2_ = p3_ = 0;
    for (auto &x : lp_) x = 0.0f;
}

void ReverbFX::process(float *samples, unsigned frames, unsigned channels,
                       unsigned sample_rate)
{
    if (channels < 1 || channels > 2) return;
    const float room_amt = std::clamp(room.load(std::memory_order_relaxed), 0.0f, 0.98f);
    const float damp_amt = std::clamp(damp.load(std::memory_order_relaxed), 0.0f, 0.99f);
    const float wet_amt  = std::clamp(wet.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float dry_amt  = 1.0f - wet_amt;
    const float g        = 0.28f + 0.7f * room_amt;
    const float sr_scale = float(sample_rate) / 44100.0f;

    auto comb_step = [&](std::vector<float> &buf, unsigned &p, unsigned base_len,
                         float in, int idx) -> float {
        unsigned D = std::clamp(unsigned(float(base_len) * sr_scale), 8u,
                              unsigned(buf.size()) - 1u);
        unsigned n = unsigned(buf.size());
        unsigned rp = (p + n - D) % n;
        float d = buf[rp];
        lp_[idx] = d * (1.0f - damp_amt) + lp_[idx] * damp_amt;
        float y = in + g * lp_[idx];
        buf[p] = y;
        p = (p + 1) % n;
        return lp_[idx];
    };

    for (unsigned f = 0; f < frames; f++) {
        float dry_l = samples[f * channels + 0];
        float dry_r = channels > 1 ? samples[f * channels + 1] : dry_l;
        float mono  = 0.5f * (dry_l + dry_r);

        float s = 0.0f;
        s += comb_step(c0_, p0_, 1557, mono, 0);
        s += comb_step(c1_, p1_, 1617, mono, 1);
        s += comb_step(c2_, p2_, 1491, mono, 2);
        s += comb_step(c3_, p3_, 1422, mono, 3);
        s *= 0.22f;

        float w = s * wet_amt;
        samples[f * channels + 0] = dry_l * dry_amt + w;
        if (channels > 1)
            samples[f * channels + 1] = dry_r * dry_amt + w * 0.97f;
    }
}
