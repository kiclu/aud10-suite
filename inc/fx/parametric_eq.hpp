#pragma once

#include "fx/effect.hpp"

// One peaking EQ band (frequency, Q, gain).
class ParametricEQ : public Effect {
public:
    std::atomic<float> freq_hz{2500.0f};
    std::atomic<float> Q{1.0f};
    std::atomic<float> gain_db{0.0f};

    const char *name() const override { return "Param EQ"; }
    int effect_kind() const override { return 6; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    static void peaking_coefs(float sr, float f0, float Qv, float dB,
                              float &b0, float &b1, float &b2, float &a1, float &a2);

    float b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float last_sr_ = 0, last_f_ = 0, last_Q_ = 0, last_g_ = 0;
    float z1_[2]{}, z2_[2]{};
};
