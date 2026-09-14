// Turning 5 second L2 snapshots plus a trade tape into an order flow the
// matching engine can consume, and measuring how faithful the result is.
//
// What the data is
//   The book feed is a snapshot of the top 20 levels every 5 seconds. It is not
//   an order by order feed. Individual orders, their ids, their arrival times
//   and their queue positions are not observable. The only per event data with
//   millisecond resolution is the trade tape.
//
// What the reconstruction does, for each window (snapshot i, snapshot i+1]
//   1. Replay every tape print in the window as a marketable immediate or
//      cancel order at the printed price, in time order.
//   2. Diff the resulting book against snapshot i+1 and emit the adds, cancels
//      and modifies that close the gap.
//
// How the engine gets checked
//   Step 1 and step 2 both work on a shadow model built only from the raw data.
//   The shadow model never reads engine state. The engine is then required to
//   agree with snapshot i+1 exactly. Any bookkeeping error in the engine, in
//   the level array, the FIFO queues, the occupancy bitmap or the id map, shows
//   up as a mismatch instead of being silently corrected.
//
// What it cannot do
//   Orders that are added and cancelled entirely inside one 5 second window are
//   invisible. Queue position inside a level is not observable, so each level is
//   represented by one synthetic order holding the whole level quantity. Where
//   queue position matters, in the market making simulation, it is modelled
//   explicitly from level quantity rather than taken from the engine.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/engine.hpp"
#include "replay/loader.hpp"

namespace ltx {

struct WindowStats {
  // Tape application.
  std::uint64_t trades = 0;
  std::uint64_t trades_no_liquidity = 0;   // printed price had nothing resting
  Qty tape_qty = 0;
  Qty matched_qty = 0;
  Qty swept_better_qty = 0;                // filled at a better price than printed

  // Fidelity of the tape alone, before reconciliation.
  std::uint32_t tape_level_mismatch = 0;   // of 40 compared positions
  std::uint32_t tape_levels_compared = 0;
  double tape_top5_abs_err = 0.0;          // in base units, summed over both sides
  double tape_top5_ref = 0.0;              // snapshot size over the same levels

  // Fidelity after reconciliation. This is the engine check and must be zero.
  std::uint32_t recon_level_mismatch = 0;
  std::uint32_t recon_levels_compared = 0;
  std::uint64_t recon_unexpected_trades = 0;

  // Emitted flow.
  std::uint32_t adds = 0;
  std::uint32_t cancels = 0;
  std::uint32_t modifies = 0;
};

struct ReplayStats {
  std::uint64_t windows = 0;
  std::uint64_t windows_skipped_gap = 0;
  std::uint64_t commands = 0;
  WindowStats total;
  std::uint64_t windows_with_recon_mismatch = 0;
  std::uint64_t windows_with_tape_mismatch = 0;
};

// Order ids are derived from (side, tick) so a level keeps its identity across
// windows for as long as it stays in the book.
inline OrderId level_id(Side s, Tick t) {
  return (static_cast<OrderId>(s == Side::Buy ? 1u : 2u) << 32) |
         static_cast<std::uint32_t>(t);
}

// Observers that want to see the market evolve hook in here. The simulator uses
// it to step its agent on every tape print and every snapshot boundary.
class ReplayObserver {
 public:
  virtual ~ReplayObserver() = default;
  // Before any of the window's flow is applied. `book` holds snapshot i.
  virtual void on_snapshot(const Snapshot& s, const OrderBook& book) { (void)s; (void)book; }
  // After the print has been applied to the book.
  virtual void on_trade_print(const RawTrade& t, const OrderBook& book) {
    (void)t; (void)book;
  }
  // The book is about to be cleared and rebuilt from the snapshot at `ts`,
  // either at the start of a day or after a feed gap. Anything mirroring the
  // book has to throw its own copy away at the same moment, and stamp whatever
  // it emits with `ts` rather than with the last thing it saw: the teardown
  // belongs to the snapshot that replaces the book, not to the one before the
  // hole.
  virtual void on_reseed(Ts ts, const OrderBook& book) { (void)ts; (void)book; }
};

class Reconstructor {
 public:
  explicit Reconstructor(MatchingEngine& eng) : eng_(eng) {}

  void set_observer(ReplayObserver* o) { obs_ = o; }
  // Windows longer than this are treated as a feed gap: the book is reseeded
  // from the next snapshot and no fidelity is scored across the hole.
  void set_max_window_ms(std::int64_t ms) { max_window_ms_ = ms; }

  // Replays one day. `trades` must be sorted by time_ms.
  ReplayStats run(const std::vector<Snapshot>& snaps, const std::vector<RawTrade>& trades);

  // --- incremental -------------------------------------------------------
  // The same machinery, fed one event at a time, for the live shadow. run()
  // is written in terms of these, so the live path and the replay path are the
  // same code and a difference between them would be a bug in one place rather
  // than a divergence between two.
  //
  // Call push_snapshot first. Then push_trade for everything that happened
  // since, then push_snapshot again to close the window.
  void push_snapshot(const Snapshot& s);
  void push_trade(const RawTrade& t);
  const ReplayStats& stats() const { return live_; }
  bool started() const { return started_; }

  const std::vector<WindowStats>& per_window() const { return per_window_; }
  void keep_per_window(bool k) { keep_per_window_ = k; }

 private:
  struct SideState {
    std::vector<RawLevel> lv;  // best first
  };

  void seed(const Snapshot& s);
  void apply_trade(const RawTrade& t, WindowStats& w);
  void reconcile(const Snapshot& target, WindowStats& w);
  void score(const Snapshot& target, std::uint32_t& mismatch, std::uint32_t& compared,
             double* top5_err, double* top5_ref) const;
  void submit(const Command& c);

  MatchingEngine& eng_;
  ReplayObserver* obs_ = nullptr;
  SideState bid_, ask_;
  std::int64_t max_window_ms_ = 30000;
  std::vector<WindowStats> per_window_;
  bool keep_per_window_ = false;
  std::uint64_t commands_ = 0;
  std::uint64_t taker_seq_ = 0;
  Ts now_ns_ = 0;
  // Incremental state: the window that is open, and the snapshot that opened it.
  bool started_ = false;
  Snapshot open_{};
  WindowStats win_{};
  ReplayStats live_{};
  EventSink* outer_sink_ = nullptr;
};

}  // namespace ltx
