// Timing helpers for the benchmark.
//
// Per operation costs here are tens of nanoseconds, which is the same order as
// the timer itself, so the harness measures the timer's own cost during warm up
// and subtracts it. The raw and corrected numbers are both reported.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace ltx {

inline std::uint64_t rdtsc_begin() noexcept {
#if defined(__x86_64__)
  unsigned aux;
  // lfence keeps earlier work from drifting past the read.
  _mm_lfence();
  return __rdtscp(&aux);
#else
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline std::uint64_t rdtsc_end() noexcept {
#if defined(__x86_64__)
  unsigned aux;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#else
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Cycles per nanosecond, measured against the monotonic clock.
inline double measure_tsc_ghz(int millis = 200) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const std::uint64_t c0 = rdtsc_begin();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count() <
         millis) {
  }
  const std::uint64_t c1 = rdtsc_end();
  const auto t1 = clock::now();
  const double ns =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  return static_cast<double>(c1 - c0) / ns;
}

// Cycle histogram with one bin per cycle up to kMax, and a decade spaced tail
// above it. At a TSC around 2.8 GHz one cycle is about 0.36 ns, so percentiles
// come out finer than the thing being measured, in half a megabyte of memory
// that does not grow with the sample count.
class CycleHist {
 public:
  static constexpr std::uint64_t kMax = 1u << 16;   // 65536 cycles, about 23 us
  static constexpr std::size_t kTail = 64;          // powers of two above kMax

  CycleHist() : bins_(kMax, 0), tail_(kTail, 0) {}

  inline void add(std::uint64_t cycles) {
    ++n_;
    sum_ += cycles;
    if (cycles > max_) max_ = cycles;
    if (cycles < kMax) {
      ++bins_[cycles];
    } else {
      std::size_t d = 0;
      std::uint64_t c = cycles >> 16;
      while (c > 1 && d + 1 < kTail) { c >>= 1; ++d; }
      ++tail_[d];
    }
  }

  std::uint64_t count() const { return n_; }
  std::uint64_t max_cycles() const { return max_; }
  double mean_cycles() const { return n_ ? static_cast<double>(sum_) / n_ : 0.0; }

  // Upper edge of the bin holding the p-th percentile, in cycles.
  double pct_cycles(double p) const {
    if (!n_) return 0.0;
    const std::uint64_t want = static_cast<std::uint64_t>(p / 100.0 * n_);
    std::uint64_t seen = 0;
    for (std::uint64_t c = 0; c < kMax; ++c) {
      seen += bins_[c];
      if (seen > want) return static_cast<double>(c);
    }
    for (std::size_t d = 0; d < kTail; ++d) {
      seen += tail_[d];
      if (seen > want) return static_cast<double>(kMax << (d + 1));
    }
    return static_cast<double>(max_);
  }

  void merge(const CycleHist& o) {
    for (std::uint64_t c = 0; c < kMax; ++c) bins_[c] += o.bins_[c];
    for (std::size_t d = 0; d < kTail; ++d) tail_[d] += o.tail_[d];
    n_ += o.n_;
    sum_ += o.sum_;
    if (o.max_ > max_) max_ = o.max_;
  }

 private:
  std::vector<std::uint64_t> bins_;
  std::vector<std::uint64_t> tail_;
  std::uint64_t n_ = 0, sum_ = 0, max_ = 0;
};

// Formats cycles as nanoseconds after removing the measured cost of the timer
// itself.
inline double to_ns(double cycles, double ghz, double overhead_cycles) {
  const double c = cycles - overhead_cycles;
  return (c > 0 ? c : 0.0) / ghz;
}

}  // namespace ltx
