#pragma once

#include "fx/effect.hpp"

class GraphicEQ : public Effect {
public:
    std::atomic<float> low_db{0.0f};
    std::atomic<float> mid_db{0.0f};
    std::atomic<float> high_db{0.0f};

    const char *name() const override { return "EQ"; }
    int effect_kind() const override { return 2; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    static void peaking_coefs(float sr, float f0, float Q, float dB,
                              float &b0, float &b1, float &b2, float &a1, float &a2);

    float b0_[3]{}, b1_[3]{}, b2_[3]{}, a1_[3]{}, a2_[3]{};
    float last_g_[3]{0.0f, 0.0f, 0.0f};
    unsigned last_sr_ = 0;
    float z1_[6]{}, z2_[6]{};
};
