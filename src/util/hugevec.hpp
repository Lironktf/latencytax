// A flat array backed by anonymous mmap, with huge pages asked for explicitly.
//
// Why
//   The engine's three big arrays are the level table, the order pool and the
//   order id map. At a steady state of 200,000 resting orders those come to
//   about 70 MB, which is 17,000 pages of 4 KB against a data TLB of roughly a
//   thousand entries. Every cancel then pays a page walk on top of its cache
//   miss, and the benchmark measures the page tables as much as the book.
//
//   Measured on this machine, 200,000 resting orders, 12 second runs: median
//   operation 170 ns and 4.32 M msg/s with 4 KB pages, against 125 ns and
//   6.33 M msg/s with 2 MB pages. Same code, same flow, 46% more throughput.
//
// Why not just set the system flag
//   Transparent huge pages are a machine wide setting that a reader of this
//   repository does not control and may not be allowed to change. Asking for
//   them per mapping with madvise works under the common `madvise` policy,
//   leaves the rest of the machine alone, and lets the benchmark turn them off
//   again so the comparison is one binary and one flag rather than two system
//   states.
#pragma once

#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace ltx {

inline constexpr std::size_t kHugePageBytes = 2u << 20;

// Process wide, read at allocation time. The benchmark flips it before building
// a book so both sides of the comparison come out of one binary.
inline bool& huge_pages_enabled() {
  static bool on = true;
  return on;
}

template <typename T>
class HugeVec {
  static_assert(std::is_trivially_copyable_v<T>, "HugeVec holds plain data");

 public:
  HugeVec() = default;
  ~HugeVec() { release(); }
  HugeVec(const HugeVec&) = delete;
  HugeVec& operator=(const HugeVec&) = delete;
  HugeVec(HugeVec&& o) noexcept { steal(o); }
  HugeVec& operator=(HugeVec&& o) noexcept {
    if (this != &o) { release(); steal(o); }
    return *this;
  }

  T* data() noexcept { return p_; }
  const T* data() const noexcept { return p_; }
  std::size_t size() const noexcept { return n_; }
  bool empty() const noexcept { return n_ == 0; }
  T& operator[](std::size_t i) noexcept { return p_[i]; }
  const T& operator[](std::size_t i) const noexcept { return p_[i]; }
  T* begin() noexcept { return p_; }
  T* end() noexcept { return p_ + n_; }
  const T* begin() const noexcept { return p_; }
  const T* end() const noexcept { return p_ + n_; }
  T& back() noexcept { return p_[n_ - 1]; }
  bool huge() const noexcept { return huge_; }

  void assign(std::size_t n, const T& v) {
    allocate(n);
    std::fill(p_, p_ + n_, v);
  }
  void resize(std::size_t n) { assign(n, T{}); }
  void swap(HugeVec& o) noexcept {
    std::swap(p_, o.p_);
    std::swap(n_, o.n_);
    std::swap(bytes_, o.bytes_);
    std::swap(mapped_, o.mapped_);
    std::swap(huge_, o.huge_);
  }

 private:
  void steal(HugeVec& o) noexcept {
    p_ = o.p_; n_ = o.n_; bytes_ = o.bytes_; mapped_ = o.mapped_; huge_ = o.huge_;
    o.p_ = nullptr; o.n_ = 0; o.bytes_ = 0; o.mapped_ = false; o.huge_ = false;
  }

  void release() noexcept {
    if (!p_) return;
    if (mapped_) ::munmap(p_, bytes_);
    else std::free(p_);
    p_ = nullptr; n_ = 0; bytes_ = 0; mapped_ = false; huge_ = false;
  }

  void allocate(std::size_t n) {
    release();
    if (n == 0) return;
    const std::size_t want = n * sizeof(T);
    // Round the mapping to a huge page so the kernel can actually back it with
    // one; a mapping that is not aligned and sized to the boundary gets 4 KB
    // pages however politely it asks.
    const std::size_t rounded = ((want + kHugePageBytes - 1) / kHugePageBytes) * kHugePageBytes;
    // Rounding the length is not enough. A huge page has to be backed at a huge
    // page boundary, so a 2 MB sized mapping that happens to start at an odd
    // 4 KB offset gets small pages however politely it asks. The first version
    // of this rounded the length only and bought 14% where the machine wide
    // setting bought 46%, which is what sent me looking. So: over allocate by
    // one huge page, align the start up, and hand the slack back.
    const std::size_t span = rounded + kHugePageBytes;
    void* raw = ::mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
    if (raw != MAP_FAILED) {
      auto base = reinterpret_cast<std::uintptr_t>(raw);
      const std::uintptr_t aligned = (base + kHugePageBytes - 1) & ~(kHugePageBytes - 1);
      const std::size_t head = aligned - base;
      if (head) ::munmap(raw, head);
      const std::size_t tail = span - head - rounded;
      if (tail) ::munmap(reinterpret_cast<void*>(aligned + rounded), tail);
      void* m = reinterpret_cast<void*>(aligned);
#ifdef MADV_HUGEPAGE
      // Asked either way rather than only when enabled, so the comparison does
      // not depend on what the machine's default policy happens to be.
      ::madvise(m, rounded, huge_pages_enabled() ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
#endif
      // Touch every huge page once so the mapping is backed now rather than
      // being collapsed later by khugepaged, halfway through a measurement.
      auto* touch = static_cast<volatile unsigned char*>(m);
      for (std::size_t off = 0; off < rounded; off += kHugePageBytes) touch[off] = 0;
      p_ = static_cast<T*>(m);
      bytes_ = rounded;
      mapped_ = true;
      huge_ = huge_pages_enabled();
    } else {
      p_ = static_cast<T*>(std::malloc(want));
      if (!p_) throw std::bad_alloc();
      bytes_ = want;
      mapped_ = false;
      huge_ = false;
    }
    n_ = n;
  }

  T* p_ = nullptr;
  std::size_t n_ = 0;
  std::size_t bytes_ = 0;
  bool mapped_ = false;
  bool huge_ = false;
};

}  // namespace ltx
