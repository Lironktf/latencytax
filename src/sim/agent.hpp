// Avellaneda-Stoikov style quoting with an inventory skew, a latency pipeline
// and a book that is only observable every five seconds.
//
//   reservation  = mid - inventory * gamma * sigma^2 * tau + beta * ofi
//   half_spread  = 0.5 * (gamma * sigma^2 * tau + (2/gamma) * ln(1 + gamma/k))
//
// sigma is an exponentially weighted estimate of the standard deviation of the
// mid over one snapshot interval, scaled to the horizon tau. ofi is an
// exponentially time-decayed sum of signed traded size.
//
// On this instrument the spread is one tick 99.8% of the time, so the computed
// half spread is almost always smaller than a tick and the quotes clamp to the
// touch. The offset parameter moves them a whole number of ticks behind it.
//
// Latency is a pipeline, not a single pending slot. A decision made in response
// to an event at venue time t takes effect at t + L; decisions made while an
// earlier one is still in flight queue up behind it, so a busy period does not
// leave the agent frozen.
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <vector>

#include "engine/types.hpp"
#include "sim/fill_model.hpp"

namespace ltx {

struct AgentConfig {
  double gamma = 5.0;          // inventory risk aversion, 1/USD
  double k = 1.5;              // order arrival decay in the AS spread term
  double tau_s = 60.0;         // horizon for the inventory term, seconds
  double beta = 0.0;           // reservation shift per unit of decayed signed volume, USD/ETH
  double ofi_halflife_s = 60.0;
  double vol_halflife_snaps = 240.0;   // 240 snapshots = 20 minutes
  int offset_ticks = 0;        // how far behind the touch to quote
  // Queue position is worth more than a tick of price here, so the agent only
  // gives it up when the resting quote is further than this from where it now
  // wants to be. A quote that has become crossable, or that the inventory limit
  // says to pull, is always acted on.
  int requote_ticks = 0;
  Qty quote_size = kQtyScale / 2;      // 0.5 ETH
  Qty max_inventory = 25 * kQtyScale;
  double tick_usd = 0.1;
  std::int64_t latency_us = 1000;
  double kappa = 1.0;
  SweepRule sweep_rule = SweepRule::Through;
};

struct Fill {
  std::int64_t ts_us = 0;
  Side side = Side::Buy;   // the agent's side
  Tick tick = 0;
  Qty qty = 0;
  bool swept = false;
  bool taker = false;      // end of day flatten
};

class Agent {
 public:
  explicit Agent(const AgentConfig& c) : c_(c) {}

  const AgentConfig& config() const { return c_; }
  Qty inventory() const { return inv_; }
  double cash() const { return cash_; }
  const std::vector<Fill>& fills() const { return fills_; }
  Qty max_abs_inventory() const { return max_abs_inv_; }
  std::uint64_t requotes() const { return requotes_; }
  std::uint64_t pending_high_water() const { return pending_hw_; }

  void reset_day() {
    bid_ = RestingQuote{};
    ask_ = RestingQuote{};
    pending_.clear();
    inv_ = 0;
    cash_ = 0;
    have_mid_ = false;
    ofi_ = 0;
    ofi_ts_ = 0;
    var_ = 0;
    have_var_ = false;
  }

  // --- market events ------------------------------------------------------

  // Called with the snapshot's own best bid and ask, in ticks, and the resting
  // size at whatever price the agent is quoting.
  void on_snapshot(std::int64_t ts_us, Tick best_bid, Tick best_ask, Qty bid_level_size,
                   Qty ask_level_size, Qty level_at_agent_bid, Qty level_at_agent_ask) {
    activate(ts_us, bid_level_size, ask_level_size, best_bid, best_ask);
    apply_snapshot_decay(bid_, level_at_agent_bid, c_.kappa);
    apply_snapshot_decay(ask_, level_at_agent_ask, c_.kappa);

    const double mid = 0.5 * (static_cast<double>(best_bid) + static_cast<double>(best_ask)) *
                       c_.tick_usd;
    if (have_mid_) {
      const double d = mid - last_mid_;
      const double w = 1.0 - std::exp(-std::log(2.0) / c_.vol_halflife_snaps);
      var_ = have_var_ ? (1 - w) * var_ + w * d * d : d * d;
      have_var_ = true;
    }
    last_mid_ = mid;
    have_mid_ = true;
    best_bid_ = best_bid;
    best_ask_ = best_ask;
    decide(ts_us);
  }

  // Returns the quantity the agent filled on this print, signed by the agent's
  // side (positive bought, negative sold).
  Qty on_print(std::int64_t ts_us, Side aggressor, Tick tick, Qty qty, Qty bid_level_size,
               Qty ask_level_size) {
    activate(ts_us, bid_level_size, ask_level_size, best_bid_, best_ask_);
    decay_ofi(ts_us);
    ofi_ += (aggressor == Side::Buy ? 1.0 : -1.0) * static_cast<double>(qty) / kQtyScale;

    Qty signed_fill = 0;
    const FillResult fb = apply_print(bid_, aggressor, tick, qty, c_.sweep_rule);
    if (fb.qty > 0) {
      book_fill(ts_us, Side::Buy, bid_.tick, fb.qty, fb.swept);
      signed_fill += fb.qty;
      if (bid_.remaining <= 0) bid_.live = false;
    }
    const FillResult fa = apply_print(ask_, aggressor, tick, qty, c_.sweep_rule);
    if (fa.qty > 0) {
      book_fill(ts_us, Side::Sell, ask_.tick, fa.qty, fa.swept);
      signed_fill -= fa.qty;
      if (ask_.remaining <= 0) ask_.live = false;
    }
    decide(ts_us);
    return signed_fill;
  }

  // Trades out of whatever is left at the touch, as a taker.
  void flatten(std::int64_t ts_us, Tick best_bid, Tick best_ask) {
    if (inv_ == 0) return;
    const Side side = inv_ > 0 ? Side::Sell : Side::Buy;
    const Tick tick = inv_ > 0 ? best_bid : best_ask;
    const Qty q = inv_ > 0 ? inv_ : -inv_;
    book_fill(ts_us, side, tick, q, false, true);
    bid_.live = false;
    ask_.live = false;
    pending_.clear();
  }

  double mid_usd() const { return last_mid_; }
  Tick agent_bid_tick() const { return bid_.live ? bid_.tick : kInvalidTick; }
  Tick agent_ask_tick() const { return ask_.live ? ask_.tick : kInvalidTick; }

 private:
  struct Decision {
    std::int64_t effective_us;
    Tick bid;   // kInvalidTick means do not quote this side
    Tick ask;
  };

  void decay_ofi(std::int64_t ts_us) {
    if (ofi_ts_ != 0 && ts_us > ofi_ts_) {
      const double dt = static_cast<double>(ts_us - ofi_ts_) / 1e6;
      ofi_ *= std::exp(-std::log(2.0) * dt / c_.ofi_halflife_s);
    }
    ofi_ts_ = ts_us;
  }

  void book_fill(std::int64_t ts_us, Side side, Tick tick, Qty qty, bool swept,
                 bool taker = false) {
    const double px = static_cast<double>(tick) * c_.tick_usd;
    const double notional = px * static_cast<double>(qty) / kQtyScale;
    if (side == Side::Buy) {
      cash_ -= notional;
      inv_ += qty;
    } else {
      cash_ += notional;
      inv_ -= qty;
    }
    const Qty a = inv_ < 0 ? -inv_ : inv_;
    if (a > max_abs_inv_) max_abs_inv_ = a;
    fills_.push_back(Fill{ts_us, side, tick, qty, swept, taker});
  }

  // Puts any decision whose time has come into the book.
  void activate(std::int64_t ts_us, Qty bid_level_size, Qty ask_level_size, Tick best_bid,
                Tick best_ask) {
    (void)best_bid;
    (void)best_ask;
    while (!pending_.empty() && pending_.front().effective_us <= ts_us) {
      const Decision d = pending_.front();
      pending_.pop_front();
      place(ts_us, Side::Buy, d.bid, bid_level_size);
      place(ts_us, Side::Sell, d.ask, ask_level_size);
    }
  }

  void place(std::int64_t ts_us, Side side, Tick tick, Qty level_size) {
    RestingQuote& q = side == Side::Buy ? bid_ : ask_;
    if (tick == kInvalidTick) {
      q.live = false;
      return;
    }
    if (q.live && q.tick == tick) return;  // already there, keep the queue slot
    ++requotes_;
    q = RestingQuote{};
    q.live = true;
    q.side = side;
    q.tick = tick;
    q.size = c_.quote_size;
    q.remaining = c_.quote_size;
    // Everything resting at the price when the order arrives is ahead of it.
    q.queue_ahead = level_size;
    q.level_at_join = level_size;
    q.level_at_last_snapshot = level_size;
    q.join_us = ts_us;
  }

  void decide(std::int64_t ts_us) {
    if (!have_mid_ || best_bid_ == kInvalidTick || best_ask_ == kInvalidTick) return;
    const double sigma2_tau = (have_var_ ? var_ : 0.0) * (c_.tau_s / 5.0);
    const double inv_eth = static_cast<double>(inv_) / kQtyScale;
    const double reservation = last_mid_ - inv_eth * c_.gamma * sigma2_tau + c_.beta * ofi_;
    double half;
    if (c_.gamma > 0) {
      half = 0.5 * (c_.gamma * sigma2_tau + (2.0 / c_.gamma) * std::log1p(c_.gamma / c_.k));
    } else {
      half = 0.5 / c_.k;  // the gamma -> 0 limit of the second term
    }

    Tick want_bid = static_cast<Tick>(std::floor((reservation - half) / c_.tick_usd));
    Tick want_ask = static_cast<Tick>(std::ceil((reservation + half) / c_.tick_usd));
    // Never cross, never improve on a one tick market: sit at the touch or
    // behind it.
    const Tick cap_bid = best_bid_ - c_.offset_ticks;
    const Tick cap_ask = best_ask_ + c_.offset_ticks;
    if (want_bid > cap_bid) want_bid = cap_bid;
    if (want_ask < cap_ask) want_ask = cap_ask;

    // Stop adding to a position that is already at the limit.
    if (inv_ >= c_.max_inventory) want_bid = kInvalidTick;
    if (inv_ <= -c_.max_inventory) want_ask = kInvalidTick;

    Tick have_bid = bid_.live ? bid_.tick : kInvalidTick;
    Tick have_ask = ask_.live ? ask_.tick : kInvalidTick;
    // Hold the existing quote when it is close enough to the new target and
    // still safe to leave resting.
    if (want_bid != kInvalidTick && have_bid != kInvalidTick &&
        have_bid < best_ask_ && std::abs(have_bid - want_bid) <= c_.requote_ticks) {
      want_bid = have_bid;
    }
    if (want_ask != kInvalidTick && have_ask != kInvalidTick &&
        have_ask > best_bid_ && std::abs(have_ask - want_ask) <= c_.requote_ticks) {
      want_ask = have_ask;
    }
    if (!pending_.empty()) {
      const Decision& back = pending_.back();
      if (back.bid == want_bid && back.ask == want_ask) return;
    } else if (have_bid == want_bid && have_ask == want_ask) {
      return;
    }
    pending_.push_back(Decision{ts_us + c_.latency_us, want_bid, want_ask});
    if (pending_.size() > pending_hw_) pending_hw_ = pending_.size();
  }

  AgentConfig c_;
  RestingQuote bid_, ask_;
  std::deque<Decision> pending_;
  Qty inv_ = 0;
  double cash_ = 0;
  std::vector<Fill> fills_;
  Qty max_abs_inv_ = 0;
  std::uint64_t requotes_ = 0;
  std::size_t pending_hw_ = 0;

  bool have_mid_ = false;
  double last_mid_ = 0;
  bool have_var_ = false;
  double var_ = 0;
  double ofi_ = 0;
  std::int64_t ofi_ts_ = 0;
  Tick best_bid_ = kInvalidTick;
  Tick best_ask_ = kInvalidTick;
};

}  // namespace ltx
