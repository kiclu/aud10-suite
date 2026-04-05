#include "fx/parametric_eq.hpp"
#include <algorithm>
#include <cmath>

#if defined(__AVX__)
#include "simd_audio.hpp"
#endif

void ParametricEQ::peaking_coefs(float sr, float f0, float Qv, float dB,
                                 float &b0, float &b1, float &b2, float &a1, float &a2)
{
    const float A = std::pow(10.0f, dB / 40.0f);
    const float w0 = 2.0f * 3.14159265f * f0 / sr;
    const float sn = std::sin(w0);
    const float cs = std::cos(w0);
    const float alpha = sn / (2.0f * Qv);
    const float a0i = 1.0f / (1.0f + alpha / A);
    b0 = (1.0f + alpha * A) * a0i;
    b1 = (-2.0f * cs) * a0i;
    b2 = (1.0f - alpha * A) * a0i;
    a1 = (-2.0f * cs) * a0i;
    a2 = (1.0f - alpha / A) * a0i;
}

void ParametricEQ::reset() {
    for (auto &z : z1_) z = 0.0f;
    for (auto &z : z2_) z = 0.0f;
    last_sr_ = 0;
}

void ParametricEQ::process(float *samples, unsigned frames, unsigned channels,
                           unsigned sample_rate)
{
    if (channels < 1 || channels > 2) return;
    const float f0 = std::clamp(freq_hz.load(std::memory_order_relaxed), 20.0f, 20000.0f);
    const float Qv = std::clamp(Q.load(std::memory_order_relaxed), 0.35f, 8.0f);
    const float g = gain_db.load(std::memory_order_relaxed);
    if (float(sample_rate) != last_sr_ || f0 != last_f_ || Qv != last_Q_ || g != last_g_) {
        last_sr_ = float(sample_rate);
        last_f_ = f0;
        last_Q_ = Qv;
        last_g_ = g;
        peaking_coefs(float(sample_rate), f0, Qv, g, b0_, b1_, b2_, a1_, a2_);
    }

#if defined(__AVX__)
    if (channels == 2) {
        for (unsigned f = 0; f < frames; f++) {
            __m128 x = aud10::simd::load_lr(&samples[f * 2]);
            __m128 z1 = aud10::simd::load_lr(&z1_[0]);
            __m128 z2 = aud10::simd::load_lr(&z2_[0]);
            __m128 y;
            aud10::simd::biquad_df2_parallel(x, b0_, b1_, b2_, a1_, a2_, z1, z2, y);
            aud10::simd::store_lr(&z1_[0], z1);
            aud10::simd::store_lr(&z2_[0], z2);
            aud10::simd::store_lr(&samples[f * 2], y);
        }
        return;
    }
#endif

    for (unsigned f = 0; f < frames; f++) {
        for (unsigned c = 0; c < channels; c++) {
            float x = samples[f * channels + c];
            float &z1 = z1_[c];
            float &z2 = z2_[c];
            float y = b0_ * x + z1;
            z1 = b1_ * x - a1_ * y + z2;
            z2 = b2_ * x - a2_ * y;
            samples[f * channels + c] = y;
        }
    }
}
