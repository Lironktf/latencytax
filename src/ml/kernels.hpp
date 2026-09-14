// Hand written float32 kernels for the queue model, with a scalar reference
// beside every one of them.
//
// Why by hand
//   The model is small and runs inside the replay loop, once per resting quote
//   per market event. At that call rate the useful thing is not peak throughput,
//   it is that a single 64 wide dot product costs a handful of cycles instead of
//   a function call into a library that does not know the length at compile
//   time. 64 floats is eight AVX2 registers, so the whole feature vector fits in
//   the register file and the loop has no tail to handle.
//
// Why four accumulators
//   An FMA on this class of core has a latency of about five cycles and a
//   throughput of one or two per cycle. A single accumulator serialises on that
//   latency and leaves most of the issue width idle. Four independent
//   accumulators, summed at the end, keep the pipeline fed. The reduction order
//   differs from the scalar version because of that, so the tests compare within
//   a tolerance rather than bit for bit, which is the honest thing to assert
//   about reassociated float arithmetic.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(__AVX2__) && defined(__FMA__)
#define LTX_HAVE_AVX2 1
#include <immintrin.h>
#else
#define LTX_HAVE_AVX2 0
#endif

namespace ltx::ml {

inline constexpr std::size_t kDim = 64;   // feature vector width, including bias

// --- scalar reference ------------------------------------------------------
namespace scalar {

inline float dot(const float* a, const float* b, std::size_t n) {
  float s = 0.0f;
  for (std::size_t i = 0; i < n; ++i) s += a[i] * b[i];
  return s;
}

inline void axpy(float* y, const float* x, float alpha, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) y[i] += alpha * x[i];
}

// y = W x, W is rows x cols, row major.
inline void gemv(const float* W, const float* x, float* y, std::size_t rows,
                 std::size_t cols) {
  for (std::size_t r = 0; r < rows; ++r) y[r] = dot(W + r * cols, x, cols);
}

// W += alpha * (g outer x), the weight update for one layer.
inline void ger(float* W, const float* g, const float* x, float alpha, std::size_t rows,
                std::size_t cols) {
  for (std::size_t r = 0; r < rows; ++r) axpy(W + r * cols, x, alpha * g[r], cols);
}

// y = W^T g, the gradient flowing back to the input of a layer.
inline void gemv_t(const float* W, const float* g, float* y, std::size_t rows,
                   std::size_t cols) {
  for (std::size_t c = 0; c < cols; ++c) y[c] = 0.0f;
  for (std::size_t r = 0; r < rows; ++r) axpy(y, W + r * cols, g[r], cols);
}

}  // namespace scalar

// --- vector ----------------------------------------------------------------
#if LTX_HAVE_AVX2

inline float hsum256(__m256 v) {
  __m128 lo = _mm256_castps256_ps128(v);
  __m128 hi = _mm256_extractf128_ps(v, 1);
  lo = _mm_add_ps(lo, hi);
  __m128 shuf = _mm_movehdup_ps(lo);
  __m128 sums = _mm_add_ps(lo, shuf);
  shuf = _mm_movehl_ps(shuf, sums);
  sums = _mm_add_ss(sums, shuf);
  return _mm_cvtss_f32(sums);
}

inline float dot(const float* a, const float* b, std::size_t n) {
  __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
  __m256 s2 = _mm256_setzero_ps(), s3 = _mm256_setzero_ps();
  std::size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
    s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), s1);
    s2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), s2);
    s3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), s3);
  }
  for (; i + 8 <= n; i += 8) {
    s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
  }
  float tail = 0.0f;
  for (; i < n; ++i) tail += a[i] * b[i];
  s0 = _mm256_add_ps(s0, s1);
  s2 = _mm256_add_ps(s2, s3);
  return hsum256(_mm256_add_ps(s0, s2)) + tail;
}

inline void axpy(float* y, const float* x, float alpha, std::size_t n) {
  const __m256 va = _mm256_set1_ps(alpha);
  std::size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    _mm256_storeu_ps(y + i,
                     _mm256_fmadd_ps(va, _mm256_loadu_ps(x + i), _mm256_loadu_ps(y + i)));
    _mm256_storeu_ps(
        y + i + 8, _mm256_fmadd_ps(va, _mm256_loadu_ps(x + i + 8), _mm256_loadu_ps(y + i + 8)));
  }
  for (; i + 8 <= n; i += 8) {
    _mm256_storeu_ps(y + i,
                     _mm256_fmadd_ps(va, _mm256_loadu_ps(x + i), _mm256_loadu_ps(y + i)));
  }
  for (; i < n; ++i) y[i] += alpha * x[i];
}

#else

inline float dot(const float* a, const float* b, std::size_t n) { return scalar::dot(a, b, n); }
inline void axpy(float* y, const float* x, float alpha, std::size_t n) {
  scalar::axpy(y, x, alpha, n);
}

#endif

inline void gemv(const float* W, const float* x, float* y, std::size_t rows,
                 std::size_t cols) {
  for (std::size_t r = 0; r < rows; ++r) y[r] = dot(W + r * cols, x, cols);
}

inline void ger(float* W, const float* g, const float* x, float alpha, std::size_t rows,
                std::size_t cols) {
  for (std::size_t r = 0; r < rows; ++r) axpy(W + r * cols, x, alpha * g[r], cols);
}

inline void gemv_t(const float* W, const float* g, float* y, std::size_t rows,
                   std::size_t cols) {
  for (std::size_t c = 0; c < cols; ++c) y[c] = 0.0f;
  for (std::size_t r = 0; r < rows; ++r) axpy(y, W + r * cols, g[r], cols);
}

// --- elementwise -----------------------------------------------------------
inline void relu(float* x, std::size_t n) {
#if LTX_HAVE_AVX2
  const __m256 z = _mm256_setzero_ps();
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) _mm256_storeu_ps(x + i, _mm256_max_ps(z, _mm256_loadu_ps(x + i)));
  for (; i < n; ++i) x[i] = x[i] > 0.0f ? x[i] : 0.0f;
#else
  for (std::size_t i = 0; i < n; ++i) x[i] = x[i] > 0.0f ? x[i] : 0.0f;
#endif
}

// g *= (pre > 0), the ReLU backward pass.
inline void relu_grad(float* g, const float* pre, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) g[i] = pre[i] > 0.0f ? g[i] : 0.0f;
}

// Numerically stable logistic. Written out rather than called from a library so
// the large negative branch cannot overflow.
inline float sigmoid(float z) {
  if (z >= 0.0f) {
    const float e = std::exp(-z);
    return 1.0f / (1.0f + e);
  }
  const float e = std::exp(z);
  return e / (1.0f + e);
}

inline float log_loss(float p, float y) {
  const float eps = 1e-7f;
  const float q = p < eps ? eps : (p > 1.0f - eps ? 1.0f - eps : p);
  return -(y * std::log(q) + (1.0f - y) * std::log(1.0f - q));
}

}  // namespace ltx::ml
