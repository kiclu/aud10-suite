#pragma once

#include "fx/effect.hpp"
#include <vector>

// Monophonic pitch-to-scale snap with YIN-style detection and lag read-pointer shift.
// Best on solo vocals; polyphonic sources confuse the detector.
class PitchCorrectFX : public Effect {
public:
    std::atomic<float> wet{0.7f};
    std::atomic<float> speed_ms{90.0f};
    std::atomic<int> key_root{0};
    std::atomic<int> scale_{0};
    std::atomic<float> pull_semi{1.2f};
    std::atomic<bool> low_latency_x{false};
    std::atomic<unsigned> exp_yin_samples{512};
    std::atomic<unsigned> exp_detect_period{128};
    std::atomic<unsigned> exp_warmup_smps{2048};
    std::atomic<unsigned> exp_yin_gate_smps{640};
    std::atomic<float> exp_lag_min{380.0f};
    std::atomic<float> exp_lag_max{3000.0f};

    // Updated from the audio thread when pitch is analyzed (for slot-editor UI).
    std::atomic<float> meter_detect_hz{0.0f};
    std::atomic<float> meter_target_hz{0.0f};

    PitchCorrectFX();
    const char *name() const override { return "Pitch correct"; }
    int effect_kind() const override { return 8; }
    void reset() override;
    void process(float *samples, unsigned frames, unsigned channels,
                 unsigned sample_rate) override;

private:
    static constexpr unsigned N = 8192;
    std::vector<float> ringL_, ringR_;
    uint64_t wp_ = 0;
    float read_lag_ = 2048.0f;
    float ratio_smooth_ = 1.0f;
    float last_ratio_tgt_ = 1.0f;
    unsigned detect_counter_ = 0;
    float yin_d_[1024]{};

    static float read_ring(const std::vector<float> &b, uint64_t wp, float lag);
    float yin_detect_hz(const float *x, int n, int sr);
    static float hz_to_midi(float hz);
    static float midi_to_hz(float m);
    static int nearest_scale_degree(float midi, int root, const int *deg, int nd);
    static float quantize_hz(float hz, int key, int scale);
};
