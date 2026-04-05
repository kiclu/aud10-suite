#include "fx/graphic_eq.hpp"
#include <cmath>

#if defined(__AVX__)
#include "simd_audio.hpp"
#endif

void GraphicEQ::peaking_coefs(float sr, float f0, float Q, float dB,
                              float &b0, float &b1, float &b2, float &a1, float &a2)
{
    const float A = std::pow(10.0f, dB / 40.0f);
    const float w0 = 2.0f * 3.14159265f * f0 / sr;
    const float sn = std::sin(w0);
    const float cs = std::cos(w0);
    const float alpha = sn / (2.0f * Q);
    const float a0i = 1.0f / (1.0f + alpha / A);
    b0 = (1.0f + alpha * A) * a0i;
    b1 = (-2.0f * cs) * a0i;
    b2 = (1.0f - alpha * A) * a0i;
    a1 = (-2.0f * cs) * a0i;
    a2 = (1.0f - alpha / A) * a0i;
}

void GraphicEQ::reset() {
    for (auto &z : z1_) z = 0.0f;
    for (auto &z : z2_) z = 0.0f;
    last_sr_ = 0;
}

void GraphicEQ::process(float *samples, unsigned frames, unsigned channels,
                        unsigned sample_rate)
{
    if (channels < 1 || channels > 2) return;
    const float g0 = low_db.load(std::memory_order_relaxed);
    const float g1 = mid_db.load(std::memory_order_relaxed);
    const float g2 = high_db.load(std::memory_order_relaxed);
    if (sample_rate != last_sr_ || g0 != last_g_[0] || g1 != last_g_[1] || g2 != last_g_[2]) {
        last_sr_ = sample_rate;
        last_g_[0] = g0;
        last_g_[1] = g1;
        last_g_[2] = g2;
        float sr = float(sample_rate);
        peaking_coefs(sr, 180.0f,  0.7f, g0, b0_[0], b1_[0], b2_[0], a1_[0], a2_[0]);
        peaking_coefs(sr, 1200.f,  0.9f, g1, b0_[1], b1_[1], b2_[1], a1_[1], a2_[1]);
        peaking_coefs(sr, 6500.f, 0.85f, g2, b0_[2], b1_[2], b2_[2], a1_[2], a2_[2]);
    }

#if defined(__AVX__)
    if (channels == 2) {
        for (unsigned f = 0; f < frames; f++) {
            __m128 x = aud10::simd::load_lr(&samples[f * 2]);
            for (int k = 0; k < 3; k++) {
                unsigned base = unsigned(k * 2);
                __m128 z1 = aud10::simd::load_lr(&z1_[base]);
                __m128 z2 = aud10::simd::load_lr(&z2_[base]);
                __m128 y;
                aud10::simd::biquad_df2_parallel(x, b0_[k], b1_[k], b2_[k], a1_[k], a2_[k], z1, z2,
                                                 y);
                aud10::simd::store_lr(&z1_[base], z1);
                aud10::simd::store_lr(&z2_[base], z2);
                x = y;
            }
            aud10::simd::store_lr(&samples[f * 2], x);
        }
        return;
    }
#endif

    for (unsigned f = 0; f < frames; f++) {
        for (unsigned c = 0; c < channels; c++) {
            float x = samples[f * channels + c];
            for (int k = 0; k < 3; k++) {
                unsigned zi = unsigned(k * int(channels) + int(c));
                float &z1 = z1_[zi];
                float &z2 = z2_[zi];
                float y = b0_[k] * x + z1;
                z1 = b1_[k] * x - a1_[k] * y + z2;
                z2 = b2_[k] * x - a2_[k] * y;
                x = y;
            }
            samples[f * channels + c] = x;
        }
    }
}
