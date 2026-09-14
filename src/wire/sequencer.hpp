// Routes a decoded feed to per symbol books, optionally across shards.
//
// Every ITCH message carries the symbol index in its header, so routing is a
// field read at a fixed offset and a modulo. A symbol therefore lives entirely
// inside one shard for the whole session, and the messages for that symbol
// reach it in feed order through a single queue. Nothing about the result can
// depend on how many shards there are, which is exactly what the determinism
// check asserts.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "engine/multi_book.hpp"
#include "engine/spsc_ring.hpp"
#include "util/affinity.hpp"
#include "wire/decoder.hpp"

namespace ltx::wire {

// Applies one routed command, creating the book for a symbol on first sight.
inline Reject apply_routed(MultiBook& books, const Routed& r, const SymbolSpec& spec) {
  if (!books.has(r.locate)) books.ensure(r.locate, spec);
  return books.apply(r.locate, r.cmd);
}

struct ShardStats {
  std::uint64_t applied = 0;
  std::uint64_t rejects = 0;
  std::uint64_t spins = 0;
};

class Shard {
 public:
  Shard(const BookConfig& cfg, std::size_t ring_pow2, int core)
      : books_(cfg, nullptr), ring_(ring_pow2), core_(core) {}

  MultiBook& books() { return books_; }
  const ShardStats& stats() const { return stats_; }

  void set_specs(const std::vector<SymbolSpec>& s) { specs_ = s; }

  // Producer side. Blocks only when the ring is full.
  void push(const Routed& r) {
    while (!ring_.push(r)) {
      // Spin rather than sleep: a full ring means the matcher is the
      // bottleneck, and parking the feed thread would only make it worse.
    }
  }

  void start() {
    stop_.store(false);
    thread_ = std::thread([this] {
      if (core_ >= 0) pin_to_core(core_);
      Routed batch[512];
      while (true) {
        const std::size_t n = ring_.pop_bulk(batch, 512);
        if (n == 0) {
          if (stop_.load(std::memory_order_acquire) && ring_.empty_approx()) break;
          ++stats_.spins;
          continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
          const std::uint16_t loc = batch[i].locate;
          if (!books_.has(loc)) {
            books_.ensure(loc, loc < specs_.size() ? specs_[loc] : SymbolSpec{});
          }
          if (books_.apply(loc, batch[i].cmd) != Reject::None) ++stats_.rejects;
          ++stats_.applied;
        }
      }
    });
  }

  void drain_and_join() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
  }

 private:
  MultiBook books_;
  SpscRing<Routed> ring_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  int core_;
  ShardStats stats_;
  std::vector<SymbolSpec> specs_;
};

}  // namespace ltx::wire
