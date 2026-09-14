// Lock free single producer, single consumer ring buffer.
//
// One thread calls push, one thread calls pop, and neither ever blocks. The
// two indices sit on separate cache lines, and each side keeps a private copy
// of the other side's index so the common case touches only its own line.
// Without that cache, every push would read the line the consumer is writing
// and the two cores would trade the line back and forth on every message.
//
// Capacity must be a power of two so the wrap is a mask instead of a modulo.
// The buffer holds Capacity - 1 items; leaving one slot empty is what
// distinguishes full from empty without a separate count.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace ltx {

inline constexpr std::size_t kCacheLine = 64;

template <typename T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>,
                "the ring copies raw bytes between threads");

 public:
  explicit SpscRing(std::size_t capacity_pow2) : mask_(capacity_pow2 - 1) {
    // Round up to a power of two.
    std::size_t cap = 2;
    while (cap < capacity_pow2) cap <<= 1;
    mask_ = cap - 1;
    buf_.resize(cap);
  }

  std::size_t capacity() const noexcept { return mask_; }

  // Producer side.
  bool push(const T& v) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & mask_;
    if (next == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (next == cached_tail_) return false;  // full
    }
    buf_[head] = v;
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Pushes as many of the n items as fit. Returns how many were written.
  std::size_t push_bulk(const T* src, std::size_t n) noexcept {
    std::size_t head = head_.load(std::memory_order_relaxed);
    std::size_t written = 0;
    while (written < n) {
      const std::size_t next = (head + 1) & mask_;
      if (next == cached_tail_) {
        cached_tail_ = tail_.load(std::memory_order_acquire);
        if (next == cached_tail_) break;
      }
      buf_[head] = src[written++];
      head = next;
    }
    if (written) head_.store(head, std::memory_order_release);
    return written;
  }

  // Consumer side.
  bool pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) return false;  // empty
    }
    out = buf_[tail];
    tail_.store((tail + 1) & mask_, std::memory_order_release);
    return true;
  }

  std::size_t pop_bulk(T* dst, std::size_t n) noexcept {
    std::size_t tail = tail_.load(std::memory_order_relaxed);
    std::size_t read = 0;
    while (read < n) {
      if (tail == cached_head_) {
        cached_head_ = head_.load(std::memory_order_acquire);
        if (tail == cached_head_) break;
      }
      dst[read++] = buf_[tail];
      tail = (tail + 1) & mask_;
    }
    if (read) tail_.store(tail, std::memory_order_release);
    return read;
  }

  // Approximate; only meaningful when one side is quiescent.
  std::size_t size_approx() const noexcept {
    const std::size_t h = head_.load(std::memory_order_acquire);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    return (h - t) & mask_;
  }
  bool empty_approx() const noexcept { return size_approx() == 0; }

 private:
  std::vector<T> buf_;
  std::size_t mask_;

  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  // Producer's private view of tail_.
  alignas(kCacheLine) std::size_t cached_tail_{0};

  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  // Consumer's private view of head_.
  alignas(kCacheLine) std::size_t cached_head_{0};
};

}  // namespace ltx
