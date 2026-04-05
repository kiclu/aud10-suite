#pragma once

#include "constants.hpp"
#include "fx/effect.hpp"
#include <atomic>
#include <vector>

/** Time-domain pitch shift (variable read rate + crossfaded lag resets). RT-safe. */
class PitchShiftFX : public Effect {
public:
    /** Index 0..PS_SEMI_STEPS-1 → semitones (idx - PS_SEMI_CENTER). */
    std::atomic<int> semitones_idx{PS_SEMI_CENTER};
    std::atomic<float> wet_mix{1.0f};

    PitchShiftFX();
    const char *name() const override { return "Pitch shift"; }
    int effect_kind() const override { return FX_PSHIFT; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    static constexpr unsigned NFR        = 32768;
    static constexpr float    TARGET_LAG = 12000.f;
    static constexpr float    MIN_LAG    = 400.f;
    static constexpr float    MAX_LAG    = 22000.f;
    static constexpr float    XF_LEN     = 160.f;

    std::vector<float> buf_;
    unsigned wi_ = 0;
    float lag_a_ = TARGET_LAG;
    float lag_b_ = TARGET_LAG;
    float xf_    = 0.f;
    bool xf_on_  = false;
};
