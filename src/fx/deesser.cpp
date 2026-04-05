#include "fx/deesser.hpp"
#include <algorithm>
#include <cmath>

#if defined(__AVX__)
#include "simd_audio.hpp"
#endif

void DeesserFX::bandpass_coefs(float sr, float f0, float Q,
                               float &b0, float &b1, float &b2, float &a1, float &a2)
{
    const float w0 = 2.0f * 3.14159265f * f0 / sr;
    const float sn = std::sin(w0);
    const float cs = std::cos(w0);
    const float alpha = sn / (2.0f * Q);
    const float a0 = 1.0f + alpha;
    const float qalpha = Q * alpha;
    b0 = qalpha / a0;
    b1 = 0.0f;
    b2 = -qalpha / a0;
    a1 = (-2.0f * cs) / a0;
    a2 = (1.0f - alpha) / a0;
}

void DeesserFX::reset() {
    for (int c = 0; c < 2; c++) {
        bp_x1_[c] = bp_x2_[c] = bp_y1_[c] = bp_y2_[c] = 0.0f;
        env_[c] = 0.0f;
    }
    last_sr_ = 0;
}

void DeesserFX::process(float *samples, unsigned frames, unsigned channels,
                        unsigned sample_rate)
{
    if (channels < 1 || channels > 2) return;
    const float f0 = std::clamp(center_hz.load(std::memory_order_relaxed), 2000.0f, 12000.0f);
    const float thr_db = threshold_db.load(std::memory_order_relaxed);
    const float amt = std::clamp(amount.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float atk_ms = std::max(0.1f, attack_ms.load(std::memory_order_relaxed));
    const float rel_ms = std::max(1.0f, release_ms.load(std::memory_order_relaxed));
    const float ca = std::exp(-1.0f / (atk_ms * float(sample_rate) / 1000.0f));
    const float cr = std::exp(-1.0f / (rel_ms * float(sample_rate) / 1000.0f));
    constexpr float Q = 2.2f;

    if (float(sample_rate) != last_sr_ || f0 != last_f0_) {
        last_sr_ = float(sample_rate);
        last_f0_ = f0;
        bandpass_coefs(float(sample_rate), f0, Q, b0_, b1_, b2_, a1_, a2_);
    }

#if defined(__AVX__)
    if (channels == 2) {
        __m128 envv = aud10::simd::load_lr(env_);
        __m128 ca_v = _mm_set1_ps(ca);
        __m128 cr_v = _mm_set1_ps(cr);
        alignas(16) float lane[4];
        for (unsigned f = 0; f < frames; f++) {
            __m128 xv = aud10::simd::load_lr(&samples[f * 2]);
            __m128 bx1 = aud10::simd::load_lr(bp_x1_);
            __m128 bx2 = aud10::simd::load_lr(bp_x2_);
            __m128 by1 = aud10::simd::load_lr(bp_y1_);
            __m128 by2 = aud10::simd::load_lr(bp_y2_);
            __m128 bp = aud10::simd::iir_direct_parallel(xv, b0_, b1_, b2_, a1_, a2_, bx1, bx2, by1,
                                                         by2);
            aud10::simd::store_lr(bp_x1_, bx1);
            aud10::simd::store_lr(bp_x2_, bx2);
            aud10::simd::store_lr(bp_y1_, by1);
            aud10::simd::store_lr(bp_y2_, by2);

            __m128 mv = aud10::simd::abs_ps(bp);
            __m128 gt = _mm_cmpgt_ps(mv, envv);
            __m128 coeff = _mm_blendv_ps(cr_v, ca_v, gt);
            envv = _mm_add_ps(mv, _mm_mul_ps(coeff, _mm_sub_ps(envv, mv)));

            _mm_storeu_ps(lane, xv);
            float x0 = lane[0], x1 = lane[1];
            _mm_storeu_ps(lane, bp);
            float bp0 = lane[0], bp1 = lane[1];
            _mm_storeu_ps(lane, envv);
            float e0 = lane[0], e1 = lane[1];

            float env_db0 = 20.0f * std::log10(e0 + 1e-9f);
            float red0 = 0.0f;
            if (env_db0 > thr_db) {
                float over = env_db0 - thr_db;
                red0 = std::clamp(std::tanh(over * 0.12f) * amt, 0.0f, 1.0f);
            }
            float env_db1 = 20.0f * std::log10(e1 + 1e-9f);
            float red1 = 0.0f;
            if (env_db1 > thr_db) {
                float over = env_db1 - thr_db;
                red1 = std::clamp(std::tanh(over * 0.12f) * amt, 0.0f, 1.0f);
            }
            samples[f * 2 + 0] = x0 - red0 * bp0;
            samples[f * 2 + 1] = x1 - red1 * bp1;
        }
        aud10::simd::store_lr(env_, envv);
        return;
    }
#endif

    for (unsigned f = 0; f < frames; f++) {
        for (unsigned c = 0; c < channels; c++) {
            float x = samples[f * channels + c];
            float bp = b0_ * x + b1_ * bp_x1_[c] + b2_ * bp_x2_[c]
                     - a1_ * bp_y1_[c] - a2_ * bp_y2_[c];
            bp_x2_[c] = bp_x1_[c];
            bp_x1_[c] = x;
            bp_y2_[c] = bp_y1_[c];
            bp_y1_[c] = bp;

            float m = std::fabs(bp);
            if (m > env_[c])
                env_[c] = m + ca * (env_[c] - m);
            else
                env_[c] = m + cr * (env_[c] - m);

            float env_db = 20.0f * std::log10(env_[c] + 1e-9f);
            float red = 0.0f;
            if (env_db > thr_db) {
                float over = env_db - thr_db;
                red = std::clamp(std::tanh(over * 0.12f) * amt, 0.0f, 1.0f);
            }
            samples[f * channels + c] = x - red * bp;
        }
    }
}
