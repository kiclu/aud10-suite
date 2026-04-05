#pragma once

#include "fx/effect.hpp"

// Band-limited sibilance detector + subtractive reduction (RT-safe, no allocs).
class DeesserFX : public Effect {
public:
    std::atomic<float> center_hz{6000.0f};
    std::atomic<float> threshold_db{-22.0f};
    std::atomic<float> amount{0.45f};
    std::atomic<float> attack_ms{5.0f};
    std::atomic<float> release_ms{80.0f};

    const char *name() const override { return "De-esser"; }
    int effect_kind() const override { return 5; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    static void bandpass_coefs(float sr, float f0, float Q,
                               float &b0, float &b1, float &b2, float &a1, float &a2);

    float b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float last_sr_ = 0, last_f0_ = 0;
    float bp_x1_[2]{}, bp_x2_[2]{}, bp_y1_[2]{}, bp_y2_[2]{};
    float env_[2]{};
};
