#pragma once

#include "fx/effect.hpp"
#include <vector>

class DelayFX : public Effect {
public:
    std::atomic<float> time_ms{250.0f};
    std::atomic<float> feedback{0.25f};
    std::atomic<float> wet{0.35f};

    DelayFX();
    const char *name() const override { return "Delay"; }
    int effect_kind() const override { return 4; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    static constexpr unsigned MAX_SAMPLES = 262144;
    std::vector<float> buf_;
    unsigned wpos_ = 0;
};
