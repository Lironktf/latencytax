// The wire format between a feed thread and the matching thread, and the thin
// dispatcher that turns a command into a book operation.
#pragma once

#include "order_book.hpp"
#include "spsc_ring.hpp"
#include "types.hpp"

namespace ltx {

enum class CmdType : std::uint8_t { AddLimit, AddMarket, Cancel, Modify, Nop, Stop };

// 32 bytes: two commands per cache line.
struct Command {
  Ts ts = 0;
  OrderId id = kNoOrder;
  Qty qty = 0;
  Tick tick = 0;
  CmdType type = CmdType::Nop;
  Side side = Side::Buy;
  Tif tif = Tif::Gtc;
  std::uint8_t pad = 0;
};
static_assert(sizeof(Command) == 32, "command should stay at 32 bytes");

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
