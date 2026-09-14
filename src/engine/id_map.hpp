// Open addressing hash map from OrderId to order pool slot.
//
// This exists instead of std::unordered_map because the order lookup is on the
// hot path of every cancel and modify. std::unordered_map chases a pointer per
// bucket and allocates per node; this is one contiguous array of 16 byte
// entries, so a lookup is usually a single cache miss.
//
// Deletion uses Knuth's backward shift (Algorithm R, TAOCP 6.4) rather than
// tombstones, so a long run of cancels does not degrade the table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.hpp"
#include "util/hugevec.hpp"

namespace ltx {

class IdMap {
 public:
  explicit IdMap(std::size_t capacity_pow2 = 1u << 16) { reserve_pow2(capacity_pow2); }

  void reserve_pow2(std::size_t n) {
    std::size_t cap = 16;
    while (cap < n) cap <<= 1;
    buckets_.assign(cap, Entry{});
    mask_ = cap - 1;
    size_ = 0;
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return buckets_.size(); }

  // Returns false if the id is already present.
  bool insert(OrderId id, Slot slot) {
    if ((size_ + 1) * 10 >= buckets_.size() * 7) grow();
    std::size_t i = index_of(id);
    while (buckets_[i].id != kNoOrder) {
      if (buckets_[i].id == id) return false;
      i = (i + 1) & mask_;
    }
    buckets_[i] = Entry{id, slot};
    ++size_;
    return true;
  }

  Slot find(OrderId id) const noexcept {
    std::size_t i = index_of(id);
    while (true) {
      const Entry& e = buckets_[i];
      if (e.id == id) return e.slot;
      if (e.id == kNoOrder) return kNullSlot;
      i = (i + 1) & mask_;
    }
  }

  // Returns the freed slot, or kNullSlot if the id was not present.
  Slot erase(OrderId id) {
    std::size_t i = index_of(id);
    while (true) {
      if (buckets_[i].id == id) break;
      if (buckets_[i].id == kNoOrder) return kNullSlot;
      i = (i + 1) & mask_;
    }
    const Slot freed = buckets_[i].slot;
    // Backward shift: walk the probe run and pull back any entry whose ideal
    // bucket is at or before the hole.
    std::size_t hole = i;
    std::size_t j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (buckets_[j].id == kNoOrder) break;
      const std::size_t ideal = index_of(buckets_[j].id);
      // Is ideal outside the open interval (hole, j] going forward?
      const std::size_t hole_to_j = (j - hole) & mask_;
      const std::size_t hole_to_ideal = (ideal - hole) & mask_;
      if (hole_to_ideal == 0 || hole_to_ideal > hole_to_j) {
        buckets_[hole] = buckets_[j];
        hole = j;
      }
    }
    buckets_[hole] = Entry{};
    --size_;
    return freed;
  }

  void clear() {
    buckets_.assign(buckets_.size(), Entry{});
    size_ = 0;
  }

 private:
  struct Entry {
    OrderId id = kNoOrder;
    Slot slot = kNullSlot;
    std::uint32_t pad = 0;
  };

  // Fibonacci hashing. Order ids from an exchange are often sequential, which
  // is the worst case for identity hashing with linear probing.
  std::size_t index_of(OrderId id) const noexcept {
    std::uint64_t h = id * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return static_cast<std::size_t>(h) & mask_;
  }

  void grow() {
    HugeVec<Entry> old;
    old.swap(buckets_);
    buckets_.assign(old.size() * 2, Entry{});
    mask_ = buckets_.size() - 1;
    size_ = 0;
    for (const Entry& e : old) {
      if (e.id != kNoOrder) insert(e.id, e.slot);
    }
  }

  HugeVec<Entry> buckets_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

}  // namespace ltx
