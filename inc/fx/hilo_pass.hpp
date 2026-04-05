#pragma once

#include "fx/effect.hpp"

// Optional 2nd-order high-pass and low-pass in series (Butterworth-style Q).
class HiLoPassFX : public Effect {
public:
    std::atomic<bool> hpf_on{false};
    std::atomic<bool> lpf_on{false};
    std::atomic<float> hpf_hz{120.0f};
    std::atomic<float> lpf_hz{12000.0f};

    const char *name() const override { return "Hi/Lo pass"; }
    int effect_kind() const override { return 7; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    static void lp_coefs(float sr, float f0, float &b0, float &b1, float &b2,
                         float &a1, float &a2);
    static void hp_coefs(float sr, float f0, float &b0, float &b1, float &b2,
                         float &a1, float &a2);

    float hb0_ = 1, hb1_ = 0, hb2_ = 0, ha1_ = 0, ha2_ = 0;
    float lb0_ = 1, lb1_ = 0, lb2_ = 0, la1_ = 0, la2_ = 0;
    float last_sr_ = 0;
    float last_h_ = 0, last_l_ = 0;
    bool last_h_on_ = false, last_l_on_ = false;
    float hx1_[2]{}, hx2_[2]{}, hy1_[2]{}, hy2_[2]{};
    float lx1_[2]{}, lx2_[2]{}, ly1_[2]{}, ly2_[2]{};
};
