#include "fx/delay.hpp"
#include <algorithm>

#if defined(__AVX__)
#include "simd_audio.hpp"
#endif

DelayFX::DelayFX() : buf_(MAX_SAMPLES * 2, 0.0f) {}

void DelayFX::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    wpos_ = 0;
}

void DelayFX::process(float *samples, unsigned frames, unsigned channels,
                      unsigned sample_rate)
{
    if (channels < 1 || channels > 2) return;
    const float fb = std::clamp(feedback.load(std::memory_order_relaxed), 0.0f, 0.95f);
    const float mx = std::clamp(wet.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float dry = 1.0f - mx;
    unsigned d_samp = unsigned(time_ms.load(std::memory_order_relaxed)
                               * float(sample_rate) / 1000.0f);
    d_samp = std::clamp(d_samp, 1u, MAX_SAMPLES - 2u);

#if defined(__AVX__)
    if (channels == 2) {
        __m128 vfb = _mm_set1_ps(fb);
        __m128 vdry = _mm_set1_ps(dry);
        __m128 vmx = _mm_set1_ps(mx);
        for (unsigned f = 0; f < frames; f++) {
            unsigned rp = (wpos_ + MAX_SAMPLES - d_samp) % MAX_SAMPLES;
            __m128 rd = aud10::simd::load_lr(&buf_[rp * 2]);
            __m128 vin = aud10::simd::load_lr(&samples[f * 2]);
            __m128 nw = _mm_add_ps(vin, _mm_mul_ps(vfb, rd));
            aud10::simd::store_lr(&buf_[wpos_ * 2], nw);
            __m128 out = _mm_add_ps(_mm_mul_ps(vdry, vin), _mm_mul_ps(vmx, rd));
            aud10::simd::store_lr(&samples[f * 2], out);
            wpos_ = (wpos_ + 1) % MAX_SAMPLES;
        }
        return;
    }
#endif

    for (unsigned f = 0; f < frames; f++) {
        for (unsigned c = 0; c < channels; c++) {
            unsigned rp = (wpos_ + MAX_SAMPLES - d_samp) % MAX_SAMPLES;
            float rd = buf_[rp * 2 + c];
            float in = samples[f * channels + c];
            float nw = in + fb * rd;
            buf_[wpos_ * 2 + c] = nw;
            samples[f * channels + c] = in * dry + rd * mx;
        }
        wpos_ = (wpos_ + 1) % MAX_SAMPLES;
    }
}
