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

// One Adam step, scalar. Kept as the reference the vector version is measured
// against, and as the fallback when AVX2 is not available.
inline void adam_step(float* w, float* m, float* v, const float* g, float b1, float b2,
                      float lr_t, float eps, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    m[i] = b1 * m[i] + (1.0f - b1) * g[i];
    v[i] = b2 * v[i] + (1.0f - b2) * g[i] * g[i];
    w[i] -= lr_t * m[i] / (std::sqrt(v[i]) + eps);
  }
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

// GCC 15 at -O3 reports -Warray-bounds on the loadu intrinsics below when these
// functions are inlined into a caller holding a small fixed size array, for
// example dot(a, b, 4) on a float[4]. The loads it complains about are inside
// loops guarded by i + 8 <= n, behind an early return for n < 8, so they cannot
// execute. It is the known false positive class around loadu in a provably dead
// loop. Verified rather than assumed: the tests were run under the address and
// undefined behaviour sanitizers at -O0, -O2 and -O3 with -march=native, so the
// vector path really was the one executing, and all three are clean. The
// suppression covers this header, which is nothing but these kernels and their
// dispatchers, and nothing else in the project turns the warning off.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"


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
  // The vector loops below are guarded by i + 8 <= n, so they cannot run when
  // n is small. The compiler cannot always prove that after inlining into a
  // caller with a fixed size array, and warns about a load it will never issue.
  // Saying it once here is cheaper than a pragma at every call site.
  if (n < 8) return scalar::dot(a, b, n);
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
  if (n < 8) { scalar::axpy(y, x, alpha, n); return; }
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

// Four rows at a time. A row at a time reloads the whole input vector for every
// row; blocking by four loads each chunk of x once and uses it four times, which
// on a 32 by 64 layer takes the loads of x from 32 passes down to 8.
inline void gemv4(const float* W, const float* x, float* y, std::size_t rows,
                  std::size_t cols) {
  std::size_t r = 0;
  for (; r + 4 <= rows; r += 4) {
    const float* w0 = W + (r + 0) * cols;
    const float* w1 = W + (r + 1) * cols;
    const float* w2 = W + (r + 2) * cols;
    const float* w3 = W + (r + 3) * cols;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
    std::size_t c = 0;
    for (; c + 8 <= cols; c += 8) {
      const __m256 xv = _mm256_loadu_ps(x + c);
      a0 = _mm256_fmadd_ps(_mm256_loadu_ps(w0 + c), xv, a0);
      a1 = _mm256_fmadd_ps(_mm256_loadu_ps(w1 + c), xv, a1);
      a2 = _mm256_fmadd_ps(_mm256_loadu_ps(w2 + c), xv, a2);
      a3 = _mm256_fmadd_ps(_mm256_loadu_ps(w3 + c), xv, a3);
    }
    float t0 = hsum256(a0), t1 = hsum256(a1), t2 = hsum256(a2), t3 = hsum256(a3);
    for (; c < cols; ++c) {
      t0 += w0[c] * x[c];
      t1 += w1[c] * x[c];
      t2 += w2[c] * x[c];
      t3 += w3[c] * x[c];
    }
    y[r + 0] = t0;
    y[r + 1] = t1;
    y[r + 2] = t2;
    y[r + 3] = t3;
  }
  for (; r < rows; ++r) y[r] = dot(W + r * cols, x, cols);
}

// One Adam step over a contiguous run of weights.
//
//   m = b1 m + (1-b1) g
//   v = b2 v + (1-b2) g^2
//   w -= lr_t * m / (sqrt(v) + eps)
//
// The scalar form of this was the slowest thing in the whole model by a wide
// margin, at about 12 ns per weight, because a square root and a division are
// both roughly fifteen cycles and neither pipelines with anything useful when
// they sit alone in a loop body. Eight lanes at a time hides almost all of it.
inline void adam_step(float* w, float* m, float* v, const float* g, float b1, float b2,
                      float lr_t, float eps, std::size_t n) {
  const __m256 vb1 = _mm256_set1_ps(b1);
  const __m256 vb2 = _mm256_set1_ps(b2);
  const __m256 v1m = _mm256_set1_ps(1.0f - b1);
  const __m256 v2m = _mm256_set1_ps(1.0f - b2);
  const __m256 vlr = _mm256_set1_ps(lr_t);
  const __m256 veps = _mm256_set1_ps(eps);
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256 gv = _mm256_loadu_ps(g + i);
    __m256 mv = _mm256_loadu_ps(m + i);
    __m256 vv = _mm256_loadu_ps(v + i);
    mv = _mm256_fmadd_ps(v1m, gv, _mm256_mul_ps(vb1, mv));
    vv = _mm256_fmadd_ps(v2m, _mm256_mul_ps(gv, gv), _mm256_mul_ps(vb2, vv));
    _mm256_storeu_ps(m + i, mv);
    _mm256_storeu_ps(v + i, vv);
    const __m256 denom = _mm256_add_ps(_mm256_sqrt_ps(vv), veps);
    const __m256 step = _mm256_div_ps(_mm256_mul_ps(vlr, mv), denom);
    _mm256_storeu_ps(w + i, _mm256_sub_ps(_mm256_loadu_ps(w + i), step));
  }
  for (; i < n; ++i) {
    m[i] = b1 * m[i] + (1.0f - b1) * g[i];
    v[i] = b2 * v[i] + (1.0f - b2) * g[i] * g[i];
    w[i] -= lr_t * m[i] / (std::sqrt(v[i]) + eps);
  }
}

#else

inline float dot(const float* a, const float* b, std::size_t n) { return scalar::dot(a, b, n); }
inline void axpy(float* y, const float* x, float alpha, std::size_t n) {
  scalar::axpy(y, x, alpha, n);
}
inline void gemv4(const float* W, const float* x, float* y, std::size_t rows,
                  std::size_t cols) {
  scalar::gemv(W, x, y, rows, cols);
}
using scalar::adam_step;

#endif

// A row at a time: vectorised, but it walks the whole input vector once per
// row. Kept so the benchmark can separate what vectorising bought from what
// blocking bought.
inline void gemv_rowwise(const float* W, const float* x, float* y, std::size_t rows,
                         std::size_t cols) {
  for (std::size_t r = 0; r < rows; ++r) y[r] = dot(W + r * cols, x, cols);
}

inline void gemv(const float* W, const float* x, float* y, std::size_t rows,
                 std::size_t cols) {
  gemv4(W, x, y, rows, cols);
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

// out = alpha * a + beta * b, the gradient of one weight row including decay.
inline void scale_add(float* out, const float* a, float alpha, const float* b, float beta,
                      std::size_t n) {
#if LTX_HAVE_AVX2
  if (n >= 8) {
    const __m256 va = _mm256_set1_ps(alpha);
    const __m256 vb = _mm256_set1_ps(beta);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
      _mm256_storeu_ps(out + i, _mm256_fmadd_ps(va, _mm256_loadu_ps(a + i),
                                                _mm256_mul_ps(vb, _mm256_loadu_ps(b + i))));
    }
    for (; i < n; ++i) out[i] = alpha * a[i] + beta * b[i];
    return;
  }
#endif
  for (std::size_t i = 0; i < n; ++i) out[i] = alpha * a[i] + beta * b[i];
}

// --- elementwise -----------------------------------------------------------
inline void relu(float* x, std::size_t n) {
#if LTX_HAVE_AVX2
  if (n < 8) {
    for (std::size_t i = 0; i < n; ++i) x[i] = x[i] > 0.0f ? x[i] : 0.0f;
    return;
  }
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

#if LTX_HAVE_AVX2
#pragma GCC diagnostic pop
#endif

}  // namespace ltx::ml
