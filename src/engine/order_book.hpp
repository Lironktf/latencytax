// Price-time priority limit order book.
//
// Layout
//   levels_   one contiguous array indexed directly by tick. A price level is
//             24 bytes: resting quantity, order count, and the head and tail
//             slot of its FIFO queue. Indexing by tick means no tree walk and
//             no hashing to reach a level.
//   occupied_ a bitmap with one bit per tick. Bids always sit at or below
//             best_bid_ and asks at or above best_ask_, and the two never
//             overlap, so a single bitmap serves both sides: to find the next
//             bid after the best one empties, scan the bitmap downward with
//             count-trailing-zeros instead of walking ticks one at a time.
//   pool_     order records in a flat vector with a free list. Queue links are
//             32 bit slot indices, not pointers, so an order is 32 bytes and
//             two of them share a cache line.
//   ids_      open addressing map from order id to pool slot.
//
// Everything is integer arithmetic. No allocation happens on the hot path once
// the pool and the id map have been sized.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "events.hpp"
#include "id_map.hpp"
#include "types.hpp"
#include "util/hugevec.hpp"

namespace ltx {

struct BookConfig {
  Tick min_tick = 1;
  Tick max_tick = 262144;       // 26214.4 USD at a 0.1 tick
  std::size_t max_orders = 1u << 20;
  std::size_t id_map_capacity = 1u << 21;
};

struct LevelView {
  Tick tick = 0;
  Qty qty = 0;
  std::uint32_t orders = 0;
};

class OrderBook {
 public:
  explicit OrderBook(const BookConfig& cfg = {}, EventSink* sink = nullptr);

  void set_sink(EventSink* sink) noexcept { sink_ = sink; }
  EventSink* sink() const noexcept { return sink_; }

  // --- client operations -------------------------------------------------
  // Each returns Reject::None on success. A rejected message leaves the book
  // exactly as it was.
  Reject add_limit(Ts ts, OrderId id, Side side, Tick tick, Qty qty, Tif tif = Tif::Gtc);
  Reject add_market(Ts ts, OrderId id, Side side, Qty qty);
  Reject cancel(Ts ts, OrderId id);
  // Queue position survives only when the price is unchanged and the size goes
  // down. Anything else is a cancel followed by a fresh order at the tail.
  Reject modify(Ts ts, OrderId id, Tick new_tick, Qty new_qty);

  // --- book builder operations -------------------------------------------
  // The two above describe what a client asks for. These two describe what the
  // exchange reports back on a market data feed: an order gave up some size, or
  // an order traded some size where it sat. ITCH calls them Order Cancel and
  // Order Executed. Both keep queue position, because neither is a new order.
  //
  // Having both roles in one book is what lets the same engine be checked twice
  // over: once by matching the tape as incoming orders, and once by applying
  // the exchange's own execution reports off the wire.
  Reject reduce(Ts ts, OrderId id, Qty shares_cancelled);
  Reject execute(Ts ts, OrderId id, Qty shares, OrderId match_number = kNoOrder);
  // ITCH Order Replace: retire one reference and put a new one in its place at
  // the back of its queue. The side is taken from the order being replaced,
  // because the message does not carry it.
  Reject replace(Ts ts, OrderId orig_id, OrderId new_id, Tick new_tick, Qty new_qty);

  // --- state -------------------------------------------------------------
  Tick best_bid() const noexcept { return best_bid_; }
  Tick best_ask() const noexcept { return best_ask_; }
  bool has_bid() const noexcept { return best_bid_ != kInvalidTick; }
  bool has_ask() const noexcept { return best_ask_ != kInvalidTick; }
  Qty qty_at(Tick t) const noexcept { return in_range(t) ? levels_[idx(t)].qty : 0; }
  std::uint32_t orders_at(Tick t) const noexcept {
    return in_range(t) ? levels_[idx(t)].orders : 0u;
  }
  std::size_t live_orders() const noexcept { return ids_.size(); }

  // Quantity resting ahead of `id` at its own price level.
  Qty queue_ahead(OrderId id) const noexcept;
  bool is_live(OrderId id) const noexcept { return ids_.find(id) != kNullSlot; }
  Qty order_qty(OrderId id) const noexcept;
  Tick order_tick(OrderId id) const noexcept;

  // Fills `out` with up to `depth` levels from the best price outward.
  // Returns the number written.
  std::size_t top_levels(Side side, std::size_t depth, LevelView* out) const;

  void clear();

  // Total resting quantity, used by tests as an invariant check.
  Qty total_qty(Side side) const;
  // Walks every level and order and verifies the bookkeeping. Debug only.
  bool check_invariants() const;

 private:
  struct Level {
    Qty qty = 0;
    Slot head = kNullSlot;
    Slot tail = kNullSlot;
    std::uint32_t orders = 0;
  };
  static_assert(sizeof(Level) == 24, "level should stay at 24 bytes");

  struct Order {
    OrderId id = kNoOrder;
    Qty qty = 0;
    Tick tick = 0;
    Slot prev = kNullSlot;
    Slot next = kNullSlot;
    Side side = Side::Buy;
    bool live = false;
    std::uint16_t pad = 0;
  };
  static_assert(sizeof(Order) == 32, "order should stay at 32 bytes");

  bool in_range(Tick t) const noexcept { return t >= cfg_.min_tick && t <= cfg_.max_tick; }
  std::size_t idx(Tick t) const noexcept {
    return static_cast<std::size_t>(t - cfg_.min_tick);
  }
  Tick tick_of(std::size_t i) const noexcept {
    return static_cast<Tick>(i) + cfg_.min_tick;
  }

  void bit_set(std::size_t i) noexcept { occupied_[i >> 6] |= (1ull << (i & 63)); }
  void bit_clear(std::size_t i) noexcept { occupied_[i >> 6] &= ~(1ull << (i & 63)); }
  // Highest set bit at index <= from, or npos.
  std::size_t scan_down(std::size_t from) const noexcept;
  // Lowest set bit at index >= from, or npos.
  std::size_t scan_up(std::size_t from) const noexcept;
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  Slot alloc_slot() noexcept;
  void free_slot(Slot s) noexcept;
  void link_back(std::size_t li, Slot s) noexcept;
  void unlink(std::size_t li, Slot s) noexcept;
  void rest(Ts ts, Slot s);
  void remove_resting(Slot s, CancelReason reason, Ts ts);
  // Trades `qty` against the opposite side up to `limit` (inclusive). Returns
  // the quantity that traded.
  Qty match(Ts ts, OrderId taker, Side side, Tick limit, Qty qty);
  Qty available(Side side, Tick limit) const;
  void refresh_best_after_removal(Side maker_side, std::size_t li) noexcept;

  BookConfig cfg_;
  EventSink* sink_;
  HugeVec<Level> levels_;
  HugeVec<std::uint64_t> occupied_;
  HugeVec<Order> pool_;
  Slot free_head_ = kNullSlot;
  std::size_t pool_used_ = 0;
  IdMap ids_;
  Tick best_bid_ = kInvalidTick;
  Tick best_ask_ = kInvalidTick;
};

}  // namespace ltx
