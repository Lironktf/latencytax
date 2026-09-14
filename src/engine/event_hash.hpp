// A rolling hash over everything the engine emits.
//
// Comparing final book state proves the two runs ended in the same place. It
// does not prove they got there the same way, and for a matching engine the way
// is the product: the order fills happen in, which maker was hit first, what a
// participant was told and when. So this folds every field of every event, in
// order, into one 64 bit value.
//
// What it is for: one input, many configurations, one hash. Single threaded and
// sharded across two or three threads. Straight from a file and replayed from a
// journal. Compiled at -O0 and at -O3 with -march=native. Huge pages on and off.
// If any of those disagree, something depends on timing, on thread interleaving,
// or on undefined behaviour that the optimiser is reading differently, and the
// engine is not the deterministic thing it claims to be.
//
// FNV-1a rather than anything cryptographic: this is a change detector, not a
// commitment, and it has to be cheap enough to leave switched on.
#pragma once

#include <cstdint>

#include "engine/events.hpp"

namespace ltx {

class EventHasher final : public EventSink {
 public:
  explicit EventHasher(EventSink* inner = nullptr) : inner_(inner) {}

  std::uint64_t value() const { return h_; }
  std::uint64_t events() const { return n_; }
  void set_inner(EventSink* s) { inner_ = s; }

  void on_trade(const TradeEvent& e) override {
    mix(1);
    mix(static_cast<std::uint64_t>(e.ts));
    mix(e.maker_id);
    mix(e.taker_id);
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.tick)));
    mix(static_cast<std::uint64_t>(e.qty));
    mix(static_cast<std::uint64_t>(e.aggressor));
    mix(static_cast<std::uint64_t>(e.maker_remaining));
    ++n_;
    if (inner_) inner_->on_trade(e);
  }
  void on_accept(const AcceptEvent& e) override {
    mix(2);
    mix(static_cast<std::uint64_t>(e.ts));
    mix(e.id);
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.tick)));
    mix(static_cast<std::uint64_t>(e.resting_qty));
    mix(static_cast<std::uint64_t>(e.side));
    ++n_;
    if (inner_) inner_->on_accept(e);
  }
  void on_cancel(const CancelEvent& e) override {
    mix(3);
    mix(static_cast<std::uint64_t>(e.ts));
    mix(e.id);
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.tick)));
    mix(static_cast<std::uint64_t>(e.cancelled_qty));
    mix(static_cast<std::uint64_t>(e.side));
    mix(static_cast<std::uint64_t>(e.reason));
    ++n_;
    if (inner_) inner_->on_cancel(e);
  }
  void on_reject(const RejectEvent& e) override {
    mix(4);
    mix(static_cast<std::uint64_t>(e.ts));
    mix(e.id);
    mix(static_cast<std::uint64_t>(e.reason));
    ++n_;
    if (inner_) inner_->on_reject(e);
  }

 private:
  void mix(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h_ ^= (v >> (i * 8)) & 0xFF;
      h_ *= 1099511628211ull;
    }
  }

  EventSink* inner_;
  std::uint64_t h_ = 1469598103934665603ull;
  std::uint64_t n_ = 0;
};

}  // namespace ltx
