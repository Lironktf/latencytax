// The wire format between a feed thread and the matching thread, and the thin
// dispatcher that turns a command into a book operation.
#pragma once

#include "order_book.hpp"
#include "spsc_ring.hpp"
#include "types.hpp"

namespace ltx {

enum class CmdType : std::uint8_t {
  AddLimit, AddMarket, Cancel, Modify,
  // Book builder side: what the exchange reports, not what a client asks.
  Reduce, Execute, Replace,
  Nop, Stop
};

// 40 bytes, naturally aligned with no wasted padding. `id2` exists for one
// message: ITCH Order Replace names both the reference being retired and the
// reference that takes its place, and carrying them together is cheaper and far
// less error prone than splitting the message into two commands and having the
// second one depend on state the first one left behind.
struct Command {
  Ts ts = 0;
  OrderId id = kNoOrder;
  OrderId id2 = kNoOrder;
  Qty qty = 0;
  Tick tick = 0;
  CmdType type = CmdType::Nop;
  Side side = Side::Buy;
  Tif tif = Tif::Gtc;
  std::uint8_t pad = 0;
};
static_assert(sizeof(Command) == 40, "command should stay at 40 bytes");

using CommandRing = SpscRing<Command>;

class MatchingEngine {
 public:
  explicit MatchingEngine(const BookConfig& cfg = {}, EventSink* sink = nullptr)
      : book_(cfg, sink) {}

  OrderBook& book() noexcept { return book_; }
  const OrderBook& book() const noexcept { return book_; }
  std::uint64_t applied() const noexcept { return applied_; }

  Reject apply(const Command& c) {
    ++applied_;
    switch (c.type) {
      case CmdType::AddLimit:
        return book_.add_limit(c.ts, c.id, c.side, c.tick, c.qty, c.tif);
      case CmdType::AddMarket:
        return book_.add_market(c.ts, c.id, c.side, c.qty);
      case CmdType::Cancel:
        return book_.cancel(c.ts, c.id);
      case CmdType::Modify:
        return book_.modify(c.ts, c.id, c.tick, c.qty);
      case CmdType::Reduce:
        return book_.reduce(c.ts, c.id, c.qty);
      case CmdType::Execute:
        return book_.execute(c.ts, c.id, c.qty);
      case CmdType::Replace:
        return book_.replace(c.ts, c.id, c.id2, c.tick, c.qty);
      case CmdType::Nop:
      case CmdType::Stop:
      default:
        return Reject::None;
    }
  }

 private:
  OrderBook book_;
  std::uint64_t applied_ = 0;
};

}  // namespace ltx
