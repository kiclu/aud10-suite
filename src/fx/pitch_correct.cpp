#include "fx/pitch_correct.hpp"
#include <algorithm>
#include <cmath>

#if defined(__AVX__)
#include "simd_audio.hpp"
#endif

PitchCorrectFX::PitchCorrectFX() : ringL_(N, 0.0f), ringR_(N, 0.0f) {}

void PitchCorrectFX::reset() {
    std::fill(ringL_.begin(), ringL_.end(), 0.0f);
    std::fill(ringR_.begin(), ringR_.end(), 0.0f);
    wp_ = 0;
    read_lag_ = 2048.0f;
    ratio_smooth_ = 1.0f;
    last_ratio_tgt_ = 1.0f;
    detect_counter_ = 0;
    meter_detect_hz.store(0.0f, std::memory_order_relaxed);
    meter_target_hz.store(0.0f, std::memory_order_relaxed);
}

float PitchCorrectFX::read_ring(const std::vector<float> &b, uint64_t wp, float lag) {
    const unsigned sz = (unsigned)b.size();
    double pos = (double)wp - (double)lag;
    if (pos < 0.0)
        return 0.0f;
    long long pi = (long long)std::floor(pos);
    unsigned i = (unsigned)((pi % (long long)sz + sz) % sz);
    unsigned j = (i + 1) % sz;
    float f = (float)(pos - std::floor(pos));
    return b[i] * (1.0f - f) + b[j] * f;
}

float PitchCorrectFX::hz_to_midi(float hz) {
    return 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f);
}

float PitchCorrectFX::midi_to_hz(float m) {
    return 440.0f * std::pow(2.0f, (m - 69.0f) / 12.0f);
}

int PitchCorrectFX::nearest_scale_degree(float midi, int root, const int *deg, int nd) {
    int m0 = (int)std::lround(midi);
    int best = m0;
    float best_d = 1e9f;
    for (int d = -6; d <= 6; ++d) {
        int cand = m0 + d;
        int pc = ((cand % 12) + 12) % 12;
        int rel = (pc - root + 12) % 12;
        for (int i = 0; i < nd; ++i) {
            if (deg[i] == rel) {
                float dist = std::fabs((float)cand - midi);
                if (dist < best_d) {
                    best_d = dist;
                    best = cand;
                }
                break;
            }
        }
    }
    if (best_d > 50.f)
        return (int)std::lround(midi);
    return best;
}

float PitchCorrectFX::quantize_hz(float hz, int key, int scale) {
    if (hz < 65.0f || hz > 1000.0f)
        return hz;
    key = std::clamp(key, 0, 11);
    float m = hz_to_midi(hz);
    int q;
    if (scale <= 0) {
        q = (int)std::lround(m);
    } else if (scale == 1) {
        static const int maj[] = {0, 2, 4, 5, 7, 9, 11};
        q = nearest_scale_degree(m, key, maj, 7);
    } else {
        static const int min_nat[] = {0, 2, 3, 5, 7, 8, 10};
        q = nearest_scale_degree(m, key, min_nat, 7);
    }
    return midi_to_hz((float)q);
}

float PitchCorrectFX::yin_detect_hz(const float *x, int n, int sr) {
    double rms = 0.0;
#if defined(__AVX__)
    {
        __m256 acc8 = _mm256_setzero_ps();
        int i = 0;
        for (; i + 8 <= n; i += 8) {
            __m256 xi = _mm256_loadu_ps(x + i);
            __m256 sq = _mm256_mul_ps(xi, xi);
            acc8 = _mm256_add_ps(acc8, sq);
        }
        rms += (double)aud10::simd::hsum256_ps(acc8);
        for (; i < n; i++)
            rms += (double)x[i] * (double)x[i];
    }
#else
    for (int i = 0; i < n; i++)
        rms += (double)x[i] * (double)x[i];
#endif
    rms = std::sqrt(rms / std::max(1, n));
    if (rms < 0.0015)
        return 0.0f;

    int tau_max = std::min(n / 2 - 2, sr * 2 / 55);
    int tau_min = std::max(3, sr / 1200);
    if (tau_max <= tau_min + 8)
        return 0.0f;

    float raw[1024];
    for (int tau = 1; tau < tau_max && tau < 1023; ++tau) {
        double acc = 0.0;
#if defined(__AVX__)
        {
            const int lim = n - tau;
            int j = 0;
            __m256 acc256 = _mm256_setzero_ps();
            for (; j + 8 <= lim; j += 8) {
                __m256 xj = _mm256_loadu_ps(x + j);
                __m256 xjt = _mm256_loadu_ps(x + j + tau);
                __m256 d = _mm256_sub_ps(xj, xjt);
                __m256 sq = _mm256_mul_ps(d, d);
                acc256 = _mm256_add_ps(acc256, sq);
            }
            acc += (double)aud10::simd::hsum256_ps(acc256);
            for (; j < lim; ++j) {
                float d = x[j] - x[j + tau];
                acc += (double)d * (double)d;
            }
        }
#else
        for (int j = 0; j < n - tau; ++j) {
            float d = x[j] - x[j + tau];
            acc += (double)d * (double)d;
        }
#endif
        raw[tau] = (float)acc;
    }

    float cumsum = 0.0f;
    for (int tau = 1; tau < tau_max && tau < 1023; ++tau) {
        cumsum += raw[tau];
        yin_d_[tau] = cumsum > 1e-12f ? raw[tau] * (float)tau / cumsum : 1.0f;
    }

    constexpr float thresh = 0.15f;
    for (int tau = tau_min; tau < tau_max - 1; ++tau) {
        float y = yin_d_[tau];
        if (y < thresh && y < yin_d_[tau - 1] && y < yin_d_[tau + 1]) {
            float y0 = yin_d_[tau - 1], y1 = y, y2 = yin_d_[tau + 1];
            float denom = y0 - 2.0f * y1 + y2;
            float delta = std::fabs(denom) > 1e-8f ? 0.5f * (y0 - y2) / denom : 0.0f;
            float t0 = (float)tau + delta;
            if (t0 > 0.0f)
                return (float)sr / t0;
        }
    }
    return 0.0f;
}

void PitchCorrectFX::process(float *samples, unsigned frames, unsigned channels,
                             unsigned sample_rate)
{
    if (channels < 1 || channels > 2) return;

    const float mx = std::clamp(wet.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float spd = std::clamp(speed_ms.load(std::memory_order_relaxed), 25.0f, 500.0f);
    const int key = std::clamp(key_root.load(std::memory_order_relaxed), 0, 11);
    const int sc = std::clamp(scale_.load(std::memory_order_relaxed), 0, 2);
    const float pull = std::clamp(pull_semi.load(std::memory_order_relaxed), 0.15f, 3.5f);
    const bool low_x = low_latency_x.load(std::memory_order_relaxed);

    int yin_n;
    unsigned detect_period;
    uint64_t warmup_writes;
    uint64_t yin_gate;
    float lag_min;
    float lag_max;
    if (!low_x) {
        yin_n          = 1024;
        detect_period  = 256u;
        warmup_writes  = 4096ull;
        yin_gate       = 3072ull;
        lag_min        = 900.0f;
        lag_max        = 5600.0f;
    } else {
        yin_n = (int)std::clamp(exp_yin_samples.load(std::memory_order_relaxed), 256u, 1024u);
        detect_period = std::max(1u, exp_detect_period.load(std::memory_order_relaxed));
        warmup_writes =
            (uint64_t)std::max(64u, exp_warmup_smps.load(std::memory_order_relaxed));
        yin_gate = (uint64_t)std::max(32u, exp_yin_gate_smps.load(std::memory_order_relaxed));
        lag_min = exp_lag_min.load(std::memory_order_relaxed);
        lag_max = exp_lag_max.load(std::memory_order_relaxed);
        if (!(lag_max > lag_min))
            lag_max = lag_min + 64.0f;
    }
    yin_gate      = std::max(yin_gate, (uint64_t)std::max(0, yin_n - 1));
    warmup_writes = std::max({warmup_writes, yin_gate, (uint64_t)yin_n});

    const float tau_sec = spd / 1000.0f;
    const float a = std::exp(-1.0f / (tau_sec * std::max(1u, sample_rate)));

    float xmono[1024];

    for (unsigned f = 0; f < frames; ++f) {
        float L = samples[f * channels + 0];
        float R = channels > 1 ? samples[f * channels + 1] : L;

        const uint64_t wcur = wp_;
        ringL_[wcur % N] = L;
        ringR_[wcur % N] = R;

        if ((++detect_counter_ % detect_period) == 0u && wcur >= yin_gate) {
            for (int i = 0; i < yin_n; ++i) {
                uint64_t gi = wcur - (uint64_t)(yin_n - 1) + (uint64_t)i;
                unsigned k = (unsigned)(gi % N);
                xmono[i] = 0.5f * (ringL_[k] + ringR_[k]);
            }
            float hz = yin_detect_hz(xmono, yin_n, (int)sample_rate);
            float r_tgt = 1.0f;
            if (hz > 70.0f && hz < 950.0f) {
                float qhz = quantize_hz(hz, key, sc);
                meter_detect_hz.store(hz, std::memory_order_relaxed);
                meter_target_hz.store(qhz, std::memory_order_relaxed);
                float err_semi = std::fabs(12.0f * std::log2(qhz / hz));
                if (err_semi <= pull)
                    r_tgt = std::clamp(qhz / hz, 0.82f, 1.22f);
            } else {
                meter_detect_hz.store(0.0f, std::memory_order_relaxed);
                meter_target_hz.store(0.0f, std::memory_order_relaxed);
            }
            last_ratio_tgt_ = r_tgt;
        }

        ratio_smooth_ = ratio_smooth_ * a + last_ratio_tgt_ * (1.0f - a);

        float cL, cR;
        if (wcur < warmup_writes) {
            cL = L;
            cR = R;
        } else {
            read_lag_ += (1.0f - ratio_smooth_);
            read_lag_ = std::clamp(read_lag_, lag_min, lag_max);
            cL = read_ring(ringL_, wcur, read_lag_);
            cR = read_ring(ringR_, wcur, read_lag_);
        }

        samples[f * channels + 0] = L * (1.0f - mx) + cL * mx;
        if (channels > 1)
            samples[f * channels + 1] = R * (1.0f - mx) + cR * mx;

        wp_ = wcur + 1;
    }
}
