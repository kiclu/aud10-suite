#include "fx/compressor.hpp"
#include <algorithm>
#include <cmath>

#if defined(__AVX__)
#include "simd_audio.hpp"
#endif

void Compressor::reset() {
    env_ = 0.0f;
    gr_db.store(0.0f, std::memory_order_relaxed);
}

void Compressor::process(float *samples, unsigned frames, unsigned channels,
                         unsigned sample_rate)
{
    const float thr  = threshold.load(std::memory_order_relaxed);
    const float rat  = ratio.load(std::memory_order_relaxed);
    const float kn   = knee.load(std::memory_order_relaxed);
    const float hk   = kn * 0.5f;
    const float mk   = std::pow(10.0f, makeup.load(std::memory_order_relaxed) / 20.0f);
    const float at_c = std::exp(-1.0f / (attack_ms.load(std::memory_order_relaxed)
                                          * sample_rate / 1000.0f));
    const float rl_c = std::exp(-1.0f / (release_ms.load(std::memory_order_relaxed)
                                          * sample_rate / 1000.0f));
    float worst_gr = 0.0f;

#if defined(__AVX__)
    if (channels == 2) {
        if (gain_wk_.size() < frames)
            gain_wk_.resize(frames);
        for (unsigned f = 0; f < frames; f++) {
            __m128 v = aud10::simd::load_lr(&samples[f * 2]);
            float peak = aud10::simd::hmax2_ps(aud10::simd::abs_ps(v));

            float x_db = (peak > 1e-7f) ? 20.0f * std::log10(peak) : -140.0f;

            float gc = 0.0f;
            if (x_db > thr + hk) {
                gc = thr + (x_db - thr) / rat - x_db;
            } else if (x_db > thr - hk && kn > 0.001f) {
                float d = x_db - thr + hk;
                gc = (1.0f / rat - 1.0f) * d * d / (2.0f * kn);
            }

            if (gc < env_)
                env_ = at_c * env_ + (1.0f - at_c) * gc;
            else
                env_ = rl_c * env_ + (1.0f - rl_c) * gc;

            worst_gr = std::min(worst_gr, env_);
            gain_wk_[f] = std::pow(10.0f, env_ / 20.0f) * mk;
        }
        aud10::simd::mul_interleaved_stereo_avx(samples, gain_wk_.data(), frames);
        gr_db.store(worst_gr, std::memory_order_relaxed);
        return;
    }
    if (channels == 1) {
        if (gain_wk_.size() < frames)
            gain_wk_.resize(frames);
        for (unsigned f = 0; f < frames; f++) {
            float peak = std::fabs(samples[f]);

            float x_db = (peak > 1e-7f) ? 20.0f * std::log10(peak) : -140.0f;

            float gc = 0.0f;
            if (x_db > thr + hk) {
                gc = thr + (x_db - thr) / rat - x_db;
            } else if (x_db > thr - hk && kn > 0.001f) {
                float d = x_db - thr + hk;
                gc = (1.0f / rat - 1.0f) * d * d / (2.0f * kn);
            }

            if (gc < env_)
                env_ = at_c * env_ + (1.0f - at_c) * gc;
            else
                env_ = rl_c * env_ + (1.0f - rl_c) * gc;

            worst_gr = std::min(worst_gr, env_);
            gain_wk_[f] = std::pow(10.0f, env_ / 20.0f) * mk;
        }
        aud10::simd::mul_mono_avx(samples, gain_wk_.data(), frames);
        gr_db.store(worst_gr, std::memory_order_relaxed);
        return;
    }
#endif

    for (unsigned f = 0; f < frames; f++) {
        float peak = 0.0f;
        for (unsigned c = 0; c < channels; c++)
            peak = std::max(peak, std::fabs(samples[f * channels + c]));

        float x_db = (peak > 1e-7f) ? 20.0f * std::log10(peak) : -140.0f;

        float gc = 0.0f;
        if (x_db > thr + hk) {
            gc = thr + (x_db - thr) / rat - x_db;
        } else if (x_db > thr - hk && kn > 0.001f) {
            float d = x_db - thr + hk;
            gc = (1.0f / rat - 1.0f) * d * d / (2.0f * kn);
        }

        if (gc < env_)
            env_ = at_c * env_ + (1.0f - at_c) * gc;
        else
            env_ = rl_c * env_ + (1.0f - rl_c) * gc;

        worst_gr = std::min(worst_gr, env_);

        float gain = std::pow(10.0f, env_ / 20.0f) * mk;
        for (unsigned c = 0; c < channels; c++)
            samples[f * channels + c] *= gain;
    }

    gr_db.store(worst_gr, std::memory_order_relaxed);
}
