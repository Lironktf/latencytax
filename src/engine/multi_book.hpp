// A book per symbol, addressed by the stock locate that every ITCH message
// carries in its header.
//
// The locate is the reason this is cheap. ITCH puts a 2 byte symbol index in
// the header of every message, including the ones that otherwise name only an
// order reference, so a router never has to look up which symbol an order
// belongs to before deciding where to send it. Routing is an array index on a
// field 1 byte into the message.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/engine.hpp"
#include "engine/event_hash.hpp"

namespace ltx {

struct SymbolSpec {
  std::string symbol;        // trimmed, no padding
  int price_decimals = 1;
  int size_decimals = 4;
};

class MultiBook {
 public:
  MultiBook(const BookConfig& cfg, EventSink* sink) : cfg_(cfg), sink_(sink) {}

  // Locates are dense and assigned by the feed's directory messages.
  void ensure(std::uint16_t locate, const SymbolSpec& spec) {
    if (locate >= books_.size()) {
      books_.resize(static_cast<std::size_t>(locate) + 1);
      specs_.resize(static_cast<std::size_t>(locate) + 1);
      hashers_.resize(static_cast<std::size_t>(locate) + 1);
    }
    if (!books_[locate]) {
      if (hashing_) {
        hashers_[locate] = std::make_unique<EventHasher>(sink_);
        books_[locate] = std::make_unique<MatchingEngine>(cfg_, hashers_[locate].get());
      } else {
        books_[locate] = std::make_unique<MatchingEngine>(cfg_, sink_);
      }
      specs_[locate] = spec;
    }
  }

  // One hash per symbol rather than one for the whole feed. A global hash would
  // depend on how events from different symbols interleaved, which is a property
  // of the thread schedule and not of the engine. Per symbol, the hash cannot
  // depend on the shard count, and that is the invariant worth asserting.
  void enable_event_hashing() { hashing_ = true; }
  bool hashing() const { return hashing_; }
  std::uint64_t event_hash(std::uint16_t locate) const {
    return (locate < hashers_.size() && hashers_[locate]) ? hashers_[locate]->value() : 0;
  }
  std::uint64_t event_count(std::uint16_t locate) const {
    return (locate < hashers_.size() && hashers_[locate]) ? hashers_[locate]->events() : 0;
  }

  bool has(std::uint16_t locate) const {
    return locate < books_.size() && books_[locate] != nullptr;
  }
  std::size_t size() const { return books_.size(); }

  MatchingEngine& engine(std::uint16_t locate) { return *books_[locate]; }
  const MatchingEngine& engine(std::uint16_t locate) const { return *books_[locate]; }
  const SymbolSpec& spec(std::uint16_t locate) const { return specs_[locate]; }

  Reject apply(std::uint16_t locate, const Command& c) {
    if (!has(locate)) return Reject::UnknownId;
    return books_[locate]->apply(c);
  }

  void set_sink(EventSink* s) {
    sink_ = s;
    for (auto& b : books_) {
      if (b) b->book().set_sink(s);
    }
  }

 private:
  BookConfig cfg_;
  EventSink* sink_;
  std::vector<std::unique_ptr<MatchingEngine>> books_;
  std::vector<std::unique_ptr<EventHasher>> hashers_;
  std::vector<SymbolSpec> specs_;
  bool hashing_ = false;
};

// Order independent digest of one book's visible state. Two runs that produce
// the same books produce the same digest, which is how the determinism check
// compares a one shard run against a three shard run without holding both.
inline std::uint64_t book_digest(const OrderBook& b, std::size_t depth = 20) {
  std::uint64_t h = 1469598103934665603ull;   // FNV-1a offset basis
  auto mix = [&h](std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xFF;
      h *= 1099511628211ull;
    }
  };
  LevelView lv[64];
  for (Side s : {Side::Buy, Side::Sell}) {
    const std::size_t n = b.top_levels(s, depth < 64 ? depth : 64, lv);
    mix(static_cast<std::uint64_t>(s == Side::Buy ? 1 : 2));
    mix(n);
    for (std::size_t i = 0; i < n; ++i) {
      mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(lv[i].tick)));
      mix(static_cast<std::uint64_t>(lv[i].qty));
      mix(lv[i].orders);
    }
  }
  return h;
}

}  // namespace ltx
