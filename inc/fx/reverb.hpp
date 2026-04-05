#pragma once

#include "fx/effect.hpp"
#include <vector>

class ReverbFX : public Effect {
public:
    std::atomic<float> room{0.35f};
    std::atomic<float> damp{0.5f};
    std::atomic<float> wet{0.2f};

    ReverbFX();
    const char *name() const override { return "Reverb"; }
    int effect_kind() const override { return 3; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    std::vector<float> c0_, c1_, c2_, c3_;
    unsigned p0_ = 0, p1_ = 0, p2_ = 0, p3_ = 0;
    float lp_[4]{};
};
