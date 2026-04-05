#include "fx/saturator.hpp"
#include <algorithm>
#include <cmath>

void SaturatorFX::reset() {}

void SaturatorFX::process(float *samples, unsigned frames, unsigned channels,
                          unsigned sample_rate)
{
    (void)sample_rate;
    if (channels < 1 || channels > 2) return;
    const float g = std::clamp(drive.load(std::memory_order_relaxed), 0.25f, 14.0f);
    const float m = std::clamp(mix.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float dry = 1.0f - m;

    for (unsigned f = 0; f < frames; f++) {
        for (unsigned c = 0; c < channels; c++) {
            float &x = samples[f * channels + c];
            float w = std::tanh(g * x);
            x = w * m + x * dry;
        }
    }
}
