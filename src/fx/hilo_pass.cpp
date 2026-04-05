#include "fx/hilo_pass.hpp"
#include <algorithm>
#include <cmath>

#if defined(__AVX__)
#include "simd_audio.hpp"
#endif

static constexpr float kButterQ = 0.70710678f;

void HiLoPassFX::lp_coefs(float sr, float f0, float &b0, float &b1, float &b2,
                          float &a1, float &a2)
{
    f0 = std::clamp(f0, 20.0f, sr * 0.45f);
    const float w0 = 2.0f * 3.14159265f * f0 / sr;
    const float sn = std::sin(w0);
    const float cs = std::cos(w0);
    const float alpha = sn / (2.0f * kButterQ);
    const float a0 = 1.0f + alpha;
    const float b0n = (1.0f - cs) * 0.5f;
    b0 = b0n / a0;
    b1 = (1.0f - cs) / a0;
    b2 = b0n / a0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha) / a0;
}

void HiLoPassFX::hp_coefs(float sr, float f0, float &b0, float &b1, float &b2,
                          float &a1, float &a2)
{
    f0 = std::clamp(f0, 20.0f, sr * 0.45f);
    const float w0 = 2.0f * 3.14159265f * f0 / sr;
    const float sn = std::sin(w0);
    const float cs = std::cos(w0);
    const float alpha = sn / (2.0f * kButterQ);
    const float a0 = 1.0f + alpha;
    const float b0n = (1.0f + cs) * 0.5f;
    b0 = b0n / a0;
    b1 = -(1.0f + cs) / a0;
    b2 = b0n / a0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha) / a0;
}

void HiLoPassFX::reset() {
    for (int c = 0; c < 2; c++) {
        hx1_[c] = hx2_[c] = hy1_[c] = hy2_[c] = 0.0f;
        lx1_[c] = lx2_[c] = ly1_[c] = ly2_[c] = 0.0f;
    }
    last_sr_ = 0;
}

void HiLoPassFX::process(float *samples, unsigned frames, unsigned channels,
                         unsigned sample_rate)
{
    if (channels < 1 || channels > 2) return;
    const bool hon = hpf_on.load(std::memory_order_relaxed);
    const bool lon = lpf_on.load(std::memory_order_relaxed);
    const float fh = hpf_hz.load(std::memory_order_relaxed);
    const float fl = lpf_hz.load(std::memory_order_relaxed);
    const float sr = float(sample_rate);

    if (sr != last_sr_ || fh != last_h_ || fl != last_l_ || hon != last_h_on_
        || lon != last_l_on_) {
        last_sr_ = sr;
        last_h_ = fh;
        last_l_ = fl;
        last_h_on_ = hon;
        last_l_on_ = lon;
        if (hon)
            hp_coefs(sr, fh, hb0_, hb1_, hb2_, ha1_, ha2_);
        if (lon)
            lp_coefs(sr, fl, lb0_, lb1_, lb2_, la1_, la2_);
    }

#if defined(__AVX__)
    if (channels == 2) {
        for (unsigned f = 0; f < frames; f++) {
            __m128 x = aud10::simd::load_lr(&samples[f * 2]);
            if (hon) {
                __m128 hx1 = aud10::simd::load_lr(&hx1_[0]);
                __m128 hx2 = aud10::simd::load_lr(&hx2_[0]);
                __m128 hy1 = aud10::simd::load_lr(&hy1_[0]);
                __m128 hy2 = aud10::simd::load_lr(&hy2_[0]);
                x = aud10::simd::iir_direct_parallel(x, hb0_, hb1_, hb2_, ha1_, ha2_, hx1, hx2, hy1,
                                                     hy2);
                aud10::simd::store_lr(&hx1_[0], hx1);
                aud10::simd::store_lr(&hx2_[0], hx2);
                aud10::simd::store_lr(&hy1_[0], hy1);
                aud10::simd::store_lr(&hy2_[0], hy2);
            }
            if (lon) {
                __m128 lx1 = aud10::simd::load_lr(&lx1_[0]);
                __m128 lx2 = aud10::simd::load_lr(&lx2_[0]);
                __m128 ly1 = aud10::simd::load_lr(&ly1_[0]);
                __m128 ly2 = aud10::simd::load_lr(&ly2_[0]);
                x = aud10::simd::iir_direct_parallel(x, lb0_, lb1_, lb2_, la1_, la2_, lx1, lx2, ly1,
                                                     ly2);
                aud10::simd::store_lr(&lx1_[0], lx1);
                aud10::simd::store_lr(&lx2_[0], lx2);
                aud10::simd::store_lr(&ly1_[0], ly1);
                aud10::simd::store_lr(&ly2_[0], ly2);
            }
            aud10::simd::store_lr(&samples[f * 2], x);
        }
        return;
    }
#endif

    for (unsigned f = 0; f < frames; f++) {
        for (unsigned c = 0; c < channels; c++) {
            float x = samples[f * channels + c];
            if (hon) {
                float y = hb0_ * x + hb1_ * hx1_[c] + hb2_ * hx2_[c]
                        - ha1_ * hy1_[c] - ha2_ * hy2_[c];
                hx2_[c] = hx1_[c];
                hx1_[c] = x;
                hy2_[c] = hy1_[c];
                hy1_[c] = y;
                x = y;
            }
            if (lon) {
                float y = lb0_ * x + lb1_ * lx1_[c] + lb2_ * lx2_[c]
                        - la1_ * ly1_[c] - la2_ * ly2_[c];
                lx2_[c] = lx1_[c];
                lx1_[c] = x;
                ly2_[c] = ly1_[c];
                ly1_[c] = y;
                x = y;
            }
            samples[f * channels + c] = x;
        }
    }
}
