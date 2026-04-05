#pragma once

#include <cstddef>

#if defined(__AVX__)
#include <immintrin.h>

namespace aud10::simd {

inline __m128 load_lr(const float *interleaved) {
    __m128d d = _mm_loadu_pd(reinterpret_cast<const double *>(interleaved));
    return _mm_castpd_ps(d);
}

inline void store_lr(float *interleaved, __m128 v) {
    _mm_storeu_pd(reinterpret_cast<double *>(interleaved), _mm_castps_pd(v));
}

inline __m128 abs_ps(__m128 x) {
    static const __m128 sign_mask =
        _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
    return _mm_and_ps(x, sign_mask);
}

// Transposed DF-II / "graphic" biquad: y = b0*x+z1; z1 = b1*x - a1*y + z2; z2 = b2*x - a2*y
inline void biquad_df2_parallel(__m128 x, float b0, float b1, float b2, float a1, float a2,
                                __m128 &z1, __m128 &z2, __m128 &y_out) {
    __m128 vb0 = _mm_set1_ps(b0);
    __m128 vb1 = _mm_set1_ps(b1);
    __m128 vb2 = _mm_set1_ps(b2);
    __m128 va1 = _mm_set1_ps(a1);
    __m128 va2 = _mm_set1_ps(a2);
    __m128 y = _mm_add_ps(_mm_mul_ps(vb0, x), z1);
    z1 = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(vb1, x), _mm_mul_ps(va1, y)), z2);
    z2 = _mm_sub_ps(_mm_mul_ps(vb2, x), _mm_mul_ps(va2, y));
    y_out = y;
}

// Direct-form IIR (biquad): y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2; shift delays
inline __m128 iir_direct_parallel(__m128 x, float b0, float b1, float b2, float a1, float a2,
                                  __m128 &x1, __m128 &x2, __m128 &y1, __m128 &y2) {
    __m128 vb0 = _mm_set1_ps(b0);
    __m128 vb1 = _mm_set1_ps(b1);
    __m128 vb2 = _mm_set1_ps(b2);
    __m128 va1 = _mm_set1_ps(a1);
    __m128 va2 = _mm_set1_ps(a2);
    __m128 y = _mm_mul_ps(vb0, x);
    y = _mm_add_ps(y, _mm_mul_ps(vb1, x1));
    y = _mm_add_ps(y, _mm_mul_ps(vb2, x2));
    y = _mm_sub_ps(y, _mm_mul_ps(va1, y1));
    y = _mm_sub_ps(y, _mm_mul_ps(va2, y2));
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

inline float hmax2_ps(__m128 v) {
    __m128 t = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 1, 0));
    return _mm_cvtss_f32(_mm_max_ps(v, t));
}

inline float hsum256_ps(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    return _mm_cvtss_f32(s);
}

// Per-frame gains: stereo interleaved L0,R0,L1,R1,...
inline void mul_interleaved_stereo_avx(float *samples, const float *gains, unsigned frames) {
    unsigned f = 0;
    for (; f + 4 <= frames; f += 4) {
        __m256 g = _mm256_setr_ps(gains[f], gains[f], gains[f + 1], gains[f + 1], gains[f + 2],
                                  gains[f + 2], gains[f + 3], gains[f + 3]);
        __m256 s = _mm256_loadu_ps(samples + f * 2);
        _mm256_storeu_ps(samples + f * 2, _mm256_mul_ps(s, g));
    }
    for (; f < frames; ++f) {
        float g = gains[f];
        samples[f * 2 + 0] *= g;
        samples[f * 2 + 1] *= g;
    }
}

inline void mul_mono_avx(float *samples, const float *gains, unsigned frames) {
    unsigned f = 0;
    for (; f + 8 <= frames; f += 8) {
        __m256 g = _mm256_loadu_ps(gains + f);
        __m256 s = _mm256_loadu_ps(samples + f);
        _mm256_storeu_ps(samples + f, _mm256_mul_ps(s, g));
    }
    for (; f < frames; ++f)
        samples[f] *= gains[f];
}

} // namespace aud10::simd

#endif // __AVX__
