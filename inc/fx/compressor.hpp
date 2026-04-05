#pragma once

#include "fx/effect.hpp"
#include <vector>

class Compressor : public Effect {
public:
    std::atomic<float> threshold{-20.0f};
    std::atomic<float> ratio{4.0f};
    std::atomic<float> attack_ms{10.0f};
    std::atomic<float> release_ms{100.0f};
    std::atomic<float> knee{6.0f};
    std::atomic<float> makeup{0.0f};
    std::atomic<float> gr_db{0.0f};

    const char *name() const override { return "Compressor"; }
    int effect_kind() const override { return 1; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    float env_ = 0.0f;
    std::vector<float> gain_wk_;
};
