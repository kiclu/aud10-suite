#pragma once

#include "constants.hpp"
#include "fx/effect.hpp"
#include <atomic>

// Tanh waveshaping with parallel dry blend (drive + mix).
class SaturatorFX : public Effect {
public:
    std::atomic<float> drive{1.0f}; // ~0.25–14 input gain into tanh
    std::atomic<float> mix{1.0f};   // 0 = dry, 1 = full wet (shaped only)

    const char *name() const override { return "Saturator"; }
    int effect_kind() const override { return FX_SAT; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;
};
