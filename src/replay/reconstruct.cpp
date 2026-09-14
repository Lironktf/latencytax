#include "replay/reconstruct.hpp"

#include <algorithm>
#include <cmath>

namespace ltx {
namespace {

// Counts trades the engine produced, so the reconciliation step can assert it
// produced none. Wraps whatever sink the caller already had.
class TradeCounter final : public EventSink {
 public:
  explicit TradeCounter(EventSink* inner) : inner_(inner) {}
  std::uint64_t trades = 0;
  Qty qty = 0;
  Tick last_tick = 0;
  void on_trade(const TradeEvent& e) override {
    ++trades;
    qty += e.qty;
    last_tick = e.tick;
    if (inner_) inner_->on_trade(e);
  }
  void on_accept(const AcceptEvent& e) override { if (inner_) inner_->on_accept(e); }
  void on_cancel(const CancelEvent& e) override { if (inner_) inner_->on_cancel(e); }
  void on_reject(const RejectEvent& e) override { if (inner_) inner_->on_reject(e); }

 private:
  EventSink* inner_;
};

}  // namespace

void Reconstructor::submit(const Command& c) {
  ++commands_;
  eng_.apply(c);
}

void Reconstructor::seed(const Snapshot& s) {
  now_ns_ = s.time_ms * 1000000;
  if (obs_) obs_->on_reseed(now_ns_, eng_.book());
  eng_.book().clear();
  bid_.lv.assign(s.bids.begin(), s.bids.begin() + s.n_bids);
  ask_.lv.assign(s.asks.begin(), s.asks.begin() + s.n_asks);
  for (const RawLevel& l : bid_.lv) {
    submit(Command{.ts = now_ns_,
                   .id = level_id(Side::Buy, l.tick),
                   .qty = l.qty,
                   .tick = l.tick,
                   .type = CmdType::AddLimit,
                   .side = Side::Buy});
  }
  for (const RawLevel& l : ask_.lv) {
    submit(Command{.ts = now_ns_,
                   .id = level_id(Side::Sell, l.tick),
                   .qty = l.qty,
                   .tick = l.tick,
                   .type = CmdType::AddLimit,
                   .side = Side::Sell});
  }
}

void Reconstructor::apply_trade(const RawTrade& t, WindowStats& w) {
  // The observer sees the print before the book has been changed by it, which
  // is what a participant with a resting order at that price would see.
  if (obs_) obs_->on_trade_print(t, eng_.book());
  ++w.trades;
  w.tape_qty += t.qty;
  now_ns_ = t.time_ms * 1000000;

  // Shadow model: consume the resting side from the best price up to the
  // printed price. This mirrors what a marketable order does, and is computed
  // without looking at the engine.
  std::vector<RawLevel>& rest = (t.aggressor == Side::Buy) ? ask_.lv : bid_.lv;
  Qty left = t.qty;
  Qty matched = 0, swept_better = 0;
  std::size_t i = 0;
  for (; i < rest.size() && left > 0; ++i) {
    const bool crosses = (t.aggressor == Side::Buy) ? (rest[i].tick <= t.tick)
                                                    : (rest[i].tick >= t.tick);
    if (!crosses) break;
    const Qty take = std::min(left, rest[i].qty);
    rest[i].qty -= take;
    left -= take;
    matched += take;
    if (rest[i].tick != t.tick) swept_better += take;
  }
  rest.erase(std::remove_if(rest.begin(), rest.end(),
                            [](const RawLevel& l) { return l.qty == 0; }),
             rest.end());
  w.matched_qty += matched;
  w.swept_better_qty += swept_better;
  if (matched == 0) ++w.trades_no_liquidity;

  // Engine: the same print as a marketable immediate or cancel order.
  submit(Command{.ts = now_ns_,
                 .id = ++taker_seq_,
                 .qty = t.qty,
                 .tick = t.tick,
                 .type = CmdType::AddLimit,
                 .side = t.aggressor,
                 .tif = Tif::Ioc});
}

void Reconstructor::reconcile(const Snapshot& target, WindowStats& w) {
  now_ns_ = target.time_ms * 1000000;
  const RawLevel* tgt[2] = {target.bids.data(), target.asks.data()};
  const std::size_t tn[2] = {target.n_bids, target.n_asks};
  SideState* cur[2] = {&bid_, &ask_};
  const Side sides[2] = {Side::Buy, Side::Sell};

  // Pass one: everything that removes or shrinks liquidity. Doing these first
  // means a repriced book never has a stale level on the wrong side of the new
  // touch when the adds go in, so reconciliation cannot accidentally cross.
  for (int s = 0; s < 2; ++s) {
    for (const RawLevel& c : cur[s]->lv) {
      const RawLevel* found = nullptr;
      for (std::size_t j = 0; j < tn[s]; ++j) {
        if (tgt[s][j].tick == c.tick) { found = &tgt[s][j]; break; }
      }
      if (!found) {
        submit(Command{.ts = now_ns_,
                       .id = level_id(sides[s], c.tick),
                       .tick = c.tick,
                       .type = CmdType::Cancel,
                       .side = sides[s]});
        ++w.cancels;
      } else if (found->qty < c.qty) {
        submit(Command{.ts = now_ns_,
                       .id = level_id(sides[s], c.tick),
                       .qty = found->qty,
                       .tick = c.tick,
                       .type = CmdType::Modify,
                       .side = sides[s]});
        ++w.modifies;
      }
    }
  }
  // Pass two: grow and add.
  for (int s = 0; s < 2; ++s) {
    for (std::size_t j = 0; j < tn[s]; ++j) {
      const RawLevel& t = tgt[s][j];
      const RawLevel* found = nullptr;
      for (const RawLevel& c : cur[s]->lv) {
        if (c.tick == t.tick) { found = &c; break; }
      }
      if (!found) {
        submit(Command{.ts = now_ns_,
                       .id = level_id(sides[s], t.tick),
                       .qty = t.qty,
                       .tick = t.tick,
                       .type = CmdType::AddLimit,
                       .side = sides[s]});
        ++w.adds;
      } else if (t.qty > found->qty) {
        submit(Command{.ts = now_ns_,
                       .id = level_id(sides[s], t.tick),
                       .qty = t.qty,
                       .tick = t.tick,
                       .type = CmdType::Modify,
                       .side = sides[s]});
        ++w.modifies;
      }
    }
  }
  bid_.lv.assign(target.bids.begin(), target.bids.begin() + target.n_bids);
  ask_.lv.assign(target.asks.begin(), target.asks.begin() + target.n_asks);
}

void Reconstructor::score(const Snapshot& target, std::uint32_t& mismatch,
                          std::uint32_t& compared, double* top5_err,
                          double* top5_ref) const {
  LevelView eng[kSnapDepth];
  const Side sides[2] = {Side::Buy, Side::Sell};
  const RawLevel* tgt[2] = {target.bids.data(), target.asks.data()};
  const std::size_t tn[2] = {target.n_bids, target.n_asks};
  for (int s = 0; s < 2; ++s) {
    const std::size_t got = eng_.book().top_levels(sides[s], kSnapDepth, eng);
    const std::size_t n = std::max(got, tn[s]);
    for (std::size_t k = 0; k < n; ++k) {
      ++compared;
      const bool have_e = k < got, have_t = k < tn[s];
      if (!have_e || !have_t) { ++mismatch; continue; }
      if (eng[k].tick != tgt[s][k].tick || eng[k].qty != tgt[s][k].qty) ++mismatch;
    }
    if (top5_err) {
      for (std::size_t k = 0; k < 5; ++k) {
        const Qty e = k < got ? eng[k].qty : 0;
        const Qty t = k < tn[s] ? tgt[s][k].qty : 0;
        // Compare at the same rank. A price that moved shows up as error on
        // both the size and the price, which is what we want to count.
        const Tick et = k < got ? eng[k].tick : 0;
        const Tick tt = k < tn[s] ? tgt[s][k].tick : 0;
        *top5_err += (et == tt) ? std::abs(static_cast<double>(e - t))
                                : static_cast<double>(e + t);
        *top5_ref += static_cast<double>(t);
      }
    }
  }
}

ReplayStats Reconstructor::run(const std::vector<Snapshot>& snaps,
                               const std::vector<RawTrade>& trades) {
  ReplayStats st;
  if (snaps.size() < 2) return st;
  per_window_.clear();
  commands_ = 0;

  EventSink* const outer = eng_.book().sink();
  TradeCounter counter(outer);

  std::size_t ti = 0;
  seed(snaps[0]);
  if (obs_) obs_->on_snapshot(snaps[0], eng_.book());

  for (std::size_t i = 0; i + 1 < snaps.size(); ++i) {
    const Snapshot& a = snaps[i];
    const Snapshot& b = snaps[i + 1];
    while (ti < trades.size() && trades[ti].time_ms <= a.time_ms) ++ti;

    if (b.time_ms - a.time_ms > max_window_ms_) {
      // Feed gap. Reseed and score nothing across the hole.
      ++st.windows_skipped_gap;
      while (ti < trades.size() && trades[ti].time_ms <= b.time_ms) ++ti;
      seed(b);
      if (obs_) obs_->on_snapshot(b, eng_.book());
      continue;
    }

    WindowStats w;
    while (ti < trades.size() && trades[ti].time_ms <= b.time_ms) {
      apply_trade(trades[ti], w);
      ++ti;
    }
    score(b, w.tape_level_mismatch, w.tape_levels_compared, &w.tape_top5_abs_err,
          &w.tape_top5_ref);

    // The reconciliation must not produce trades. If it does, the engine and
    // the shadow model disagree about what is resting.
    const std::uint64_t before = counter.trades;
    eng_.book().set_sink(&counter);
    reconcile(b, w);
    eng_.book().set_sink(outer);
    w.recon_unexpected_trades = counter.trades - before;

    score(b, w.recon_level_mismatch, w.recon_levels_compared, nullptr, nullptr);

    ++st.windows;
    st.total.trades += w.trades;
    st.total.trades_no_liquidity += w.trades_no_liquidity;
    st.total.tape_qty += w.tape_qty;
    st.total.matched_qty += w.matched_qty;
    st.total.swept_better_qty += w.swept_better_qty;
    st.total.tape_level_mismatch += w.tape_level_mismatch;
    st.total.tape_levels_compared += w.tape_levels_compared;
    st.total.tape_top5_abs_err += w.tape_top5_abs_err;
    st.total.tape_top5_ref += w.tape_top5_ref;
    st.total.recon_level_mismatch += w.recon_level_mismatch;
    st.total.recon_levels_compared += w.recon_levels_compared;
    st.total.recon_unexpected_trades += w.recon_unexpected_trades;
    st.total.adds += w.adds;
    st.total.cancels += w.cancels;
    st.total.modifies += w.modifies;
    if (w.recon_level_mismatch) ++st.windows_with_recon_mismatch;
    if (w.tape_level_mismatch) ++st.windows_with_tape_mismatch;
    if (keep_per_window_) per_window_.push_back(w);

    if (obs_) obs_->on_snapshot(b, eng_.book());
  }
  st.commands = commands_;
  return st;
}

}  // namespace ltx
