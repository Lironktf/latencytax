// Events the matching engine publishes.
#pragma once

#include "types.hpp"

namespace ltx {

struct TradeEvent {
  Ts ts;
  OrderId maker_id;
  OrderId taker_id;
  Tick tick;
  Qty qty;
  Side aggressor;   // side of the incoming order
  Qty maker_remaining;
};

struct AcceptEvent {
  Ts ts;
  OrderId id;
  Tick tick;
  Qty resting_qty;
  Side side;
};

struct CancelEvent {
  Ts ts;
  OrderId id;
  Tick tick;
  Qty cancelled_qty;
  Side side;
  CancelReason reason;
};

struct RejectEvent {
  Ts ts;
  OrderId id;
  Reject reason;
};

// The engine pushes through this interface. One virtual call per event, not per
// book operation; the benchmark numbers in the README include this cost because
// a real engine has to get its events onto a wire too.
class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void on_trade(const TradeEvent&) {}
  virtual void on_accept(const AcceptEvent&) {}
  virtual void on_cancel(const CancelEvent&) {}
  virtual void on_reject(const RejectEvent&) {}
};

// Discards everything. Used where the caller only wants book state.
class NullSink final : public EventSink {};

// Counts events without storing them. Used by the benchmark so the measured
// cost is engine work plus dispatch, not allocation.
class CountingSink final : public EventSink {
 public:
  std::uint64_t trades = 0, accepts = 0, cancels = 0, rejects = 0;
  Qty traded_qty = 0;
  void on_trade(const TradeEvent& e) override { ++trades; traded_qty += e.qty; }
  void on_accept(const AcceptEvent&) override { ++accepts; }
  void on_cancel(const CancelEvent&) override { ++cancels; }
  void on_reject(const RejectEvent&) override { ++rejects; }
};

}  // namespace ltx
