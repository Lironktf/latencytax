// Features for the queue model, and the streaming state they are computed from.
//
// What the model is asked
//   A market maker joining a price level wants to know whether the quantity
//   sitting in front of it will trade away. Everything here describes the level
//   and the recent tape at the moment of joining, and nothing here is allowed to
//   see anything later than that moment. The state below is updated by the same
//   event stream the replay drives the engine with, in the same order, so a
//   feature cannot accidentally contain the answer.
//
// Layout
//   32 base features are measured. The remaining 32 are one hot buckets of the
//   three quantities that carry most of the signal, which is what gives a linear
//   model a shape it could not otherwise fit. 64 in total, which is eight AVX2
//   registers and no loop tail.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ml/kernels.hpp"
#include "replay/loader.hpp"

namespace ltx::ml {

inline constexpr std::size_t kBaseFeatures = 32;
static_assert(kDim == 64, "feature expansion assumes a 64 wide vector");

// Exponentially time decayed accumulator. Decay is in wall clock time rather
// than in event count, because the tape arrives in bursts and an event counted
// decay would make a quiet hour look like a busy second.
class TimeEwma {
 public:
  explicit TimeEwma(double half_life_s) : lambda_(std::log(2.0) / half_life_s) {}
  void add(std::int64_t ts_ms, double v) {
    decay_to(ts_ms);
    value_ += v;
  }
  double value(std::int64_t ts_ms) const {
    if (!have_) return 0.0;
    const double dt = static_cast<double>(ts_ms - ts_) / 1000.0;
    return dt > 0 ? value_ * std::exp(-lambda_ * dt) : value_;
  }
  void decay_to(std::int64_t ts_ms) {
    if (have_ && ts_ms > ts_) {
      value_ *= std::exp(-lambda_ * static_cast<double>(ts_ms - ts_) / 1000.0);
    }
    ts_ = ts_ms;
    have_ = true;
  }

 private:
  double lambda_;
  double value_ = 0.0;
  std::int64_t ts_ = 0;
  bool have_ = false;
};

// Everything the feature vector needs, maintained causally.
class MarketState {
 public:
  MarketState()
      : vol_buy_{TimeEwma(5.0), TimeEwma(30.0), TimeEwma(300.0)},
        vol_sell_{TimeEwma(5.0), TimeEwma(30.0), TimeEwma(300.0)},
        ofi_fast_(5.0), ofi_slow_(60.0), arrivals_(30.0) {}

  void on_snapshot(const Snapshot& s) {
    if (s.n_bids == 0 || s.n_asks == 0) return;
    const double mid = 0.5 * (s.bids[0].tick + s.asks[0].tick);
    if (have_mid_) {
      const double d = mid - last_mid_;
      const double w = 1.0 - std::exp(-std::log(2.0) / 240.0);   // 20 minutes
      var_ = have_var_ ? (1 - w) * var_ + w * d * d : d * d;
      have_var_ = true;
    }
    last_mid_ = mid;
    have_mid_ = true;
    mids_.push_back(mid);
    mid_ts_.push_back(s.time_ms);
    if (mids_.size() > 64) {
      mids_.erase(mids_.begin());
      mid_ts_.erase(mid_ts_.begin());
    }
    snap_ = s;
    have_snap_ = true;
  }

  void on_trade(const RawTrade& t) {
    const double q = static_cast<double>(t.qty) / kQtyScale;
    if (t.aggressor == Side::Buy) {
      for (auto& e : vol_buy_) e.add(t.time_ms, q);
      ofi_fast_.add(t.time_ms, q);
      ofi_slow_.add(t.time_ms, q);
    } else {
      for (auto& e : vol_sell_) e.add(t.time_ms, q);
      ofi_fast_.add(t.time_ms, -q);
      ofi_slow_.add(t.time_ms, -q);
    }
    arrivals_.add(t.time_ms, 1.0);
  }

  bool ready() const { return have_snap_ && have_var_ && mids_.size() >= 8; }
  const Snapshot& snapshot() const { return snap_; }

  // `side` is the side of the resting order. Volume that consumes it comes from
  // the opposite aggressor.
  double consuming_volume(Side side, int which, std::int64_t ts_ms) const {
    return side == Side::Buy ? vol_sell_[which].value(ts_ms) : vol_buy_[which].value(ts_ms);
  }
  double same_side_volume(Side side, int which, std::int64_t ts_ms) const {
    return side == Side::Buy ? vol_buy_[which].value(ts_ms) : vol_sell_[which].value(ts_ms);
  }
  double ofi_fast(std::int64_t ts) const { return ofi_fast_.value(ts); }
  double ofi_slow(std::int64_t ts) const { return ofi_slow_.value(ts); }
  double arrivals(std::int64_t ts) const { return arrivals_.value(ts); }
  double vol_ticks() const { return have_var_ ? std::sqrt(var_) : 0.0; }
  double mid() const { return last_mid_; }
  // Mid change over roughly `back` snapshots, in ticks.
  double mid_drift(std::size_t back) const {
    if (mids_.size() <= back) return 0.0;
    return mids_.back() - mids_[mids_.size() - 1 - back];
  }

 private:
  TimeEwma vol_buy_[3];
  TimeEwma vol_sell_[3];
  TimeEwma ofi_fast_, ofi_slow_, arrivals_;
  Snapshot snap_{};
  bool have_snap_ = false;
  bool have_mid_ = false, have_var_ = false;
  double last_mid_ = 0.0, var_ = 0.0;
  std::vector<double> mids_;
  std::vector<std::int64_t> mid_ts_;
};

// One thing the model is asked about: a level, and a quantity in front.
struct Query {
  Side side;
  int offset_ticks;   // 0 is the touch
  Tick tick;
  double level_eth;   // resting quantity at that price
  std::uint32_t orders;
  double q_eth;       // quantity in front of the hypothetical order
};

// Fills the 32 measured features. Documented by index so a reader can tie a
// weight back to a quantity.
inline void base_features(const MarketState& st, const Query& q, std::int64_t ts_ms,
                          float* f) {
  const Snapshot& s = st.snapshot();
  const double mid = st.mid();
  const double spread = static_cast<double>(s.asks[0].tick - s.bids[0].tick);
  const double sign = q.side == Side::Buy ? 1.0 : -1.0;

  double top5_same = 0, top5_opp = 0;
  std::uint32_t orders5_same = 0;
  const RawLevel* same = q.side == Side::Buy ? s.bids.data() : s.asks.data();
  const RawLevel* opp = q.side == Side::Buy ? s.asks.data() : s.bids.data();
  const std::size_t n_same = q.side == Side::Buy ? s.n_bids : s.n_asks;
  const std::size_t n_opp = q.side == Side::Buy ? s.n_asks : s.n_bids;
  for (std::size_t i = 0; i < 5 && i < n_same; ++i) {
    top5_same += static_cast<double>(same[i].qty) / kQtyScale;
    orders5_same += same[i].orders;
  }
  for (std::size_t i = 0; i < 5 && i < n_opp; ++i) {
    top5_opp += static_cast<double>(opp[i].qty) / kQtyScale;
  }
  const double top1_same = static_cast<double>(same[0].qty) / kQtyScale;
  const double top1_opp = static_cast<double>(opp[0].qty) / kQtyScale;

  const double cons5 = st.consuming_volume(q.side, 0, ts_ms);
  const double cons30 = st.consuming_volume(q.side, 1, ts_ms);
  const double cons300 = st.consuming_volume(q.side, 2, ts_ms);
  const double same5 = st.same_side_volume(q.side, 0, ts_ms);
  const double same30 = st.same_side_volume(q.side, 1, ts_ms);
  const double same300 = st.same_side_volume(q.side, 2, ts_ms);
  const double avg_order = q.orders ? q.level_eth / q.orders : q.level_eth;
  const double tod = static_cast<double>(ts_ms % 86400000) / 86400000.0;

  f[0] = 1.0f;                                                    // bias
  f[1] = static_cast<float>(std::log1p(q.level_eth));             // level size
  f[2] = static_cast<float>(std::log1p(q.orders));                // orders at the level
  f[3] = static_cast<float>(std::log1p(avg_order));               // mean order size there
  f[4] = static_cast<float>(q.offset_ticks);                      // ticks behind the touch
  f[5] = static_cast<float>(spread);                              // spread in ticks
  f[6] = static_cast<float>(q.level_eth > 0 ? q.q_eth / q.level_eth : 1.0);  // queue fraction
  f[7] = static_cast<float>(std::log1p(q.q_eth));                 // queue ahead
  f[8] = static_cast<float>(sign);                                // bid or ask
  f[9] = static_cast<float>((top1_same - top1_opp) / (top1_same + top1_opp + 1e-9));
  f[10] = static_cast<float>((top5_same - top5_opp) / (top5_same + top5_opp + 1e-9));
  f[11] = static_cast<float>(top5_same > 0 ? q.level_eth / top5_same : 0.0);
  f[12] = static_cast<float>(mid > 0 ? (q.tick - mid) * sign / mid * 1e4 : 0.0);
  f[13] = static_cast<float>(std::log1p(cons5));                  // volume that could fill it
  f[14] = static_cast<float>(std::log1p(cons30));
  f[15] = static_cast<float>(std::log1p(cons300));
  f[16] = static_cast<float>(std::log1p(same5));                  // volume on the other side
  f[17] = static_cast<float>(std::log1p(same30));
  f[18] = static_cast<float>(std::log1p(same300));
  f[19] = static_cast<float>(std::tanh(st.ofi_fast(ts_ms) * sign / 20.0));
  f[20] = static_cast<float>(std::tanh(st.ofi_slow(ts_ms) * sign / 100.0));
  f[21] = static_cast<float>(std::log1p(st.arrivals(ts_ms)));     // arrival rate
  f[22] = static_cast<float>(st.vol_ticks());                     // realised vol, ticks
  f[23] = static_cast<float>(st.mid_drift(6) * sign);             // 30 s drift toward the side
  // How many 30 second windows of consuming volume the queue ahead represents.
  // This is the quantity the whole question reduces to, so it appears raw, in
  // logs, and bucketed below.
  const double windows = q.q_eth / (cons30 + 1e-6);
  f[24] = static_cast<float>(windows > 50.0 ? 50.0 : windows);
  f[25] = static_cast<float>(std::log1p(windows));
  f[26] = static_cast<float>(top5_opp > 0 ? top5_same / top5_opp : 1.0);
  f[27] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * tod));
  f[28] = static_cast<float>(std::cos(2.0 * 3.14159265358979 * tod));
  f[29] = static_cast<float>(orders5_same ? static_cast<double>(q.orders) / orders5_same : 0.0);
  const double level_windows = q.level_eth / (cons30 + 1e-6);
  f[30] = static_cast<float>(std::log1p(level_windows));
  f[31] = static_cast<float>(st.mid_drift(1) * sign);   // one interval drift, ticks

  // Nothing downstream can recover from a non finite feature: it poisons every
  // weight it touches on the first update and every prediction after that. The
  // first version of f[31] took log1p of a signed drift and produced NaN on any
  // sample where the mid had fallen, which was about half of them, and the
  // models trained on it came out entirely NaN while the single feature
  // baseline beside them was fine. Cheaper to check here than to debug there.
  for (std::size_t i = 0; i < kBaseFeatures; ++i) {
    if (!std::isfinite(f[i])) f[i] = 0.0f;
  }
}

// Buckets the three quantities that matter into one hot indicators, which is
// what lets a linear model bend.
inline void expand(float* f) {
  for (std::size_t i = kBaseFeatures; i < kDim; ++i) f[i] = 0.0f;
  // 32..47: log1p(queue ahead in 30 second windows), 16 buckets from 0 to 4.
  {
    const float v = f[25];
    int b = static_cast<int>(v / 0.25f);
    if (b < 0) b = 0;
    if (b > 15) b = 15;
    f[32 + b] = 1.0f;
  }
  // 48..53: offset crossed with side, six combinations.
  {
    const int off = static_cast<int>(f[4]);
    const int o = off < 0 ? 0 : (off > 2 ? 2 : off);
    const int side = f[8] > 0 ? 0 : 1;
    f[48 + side * 3 + o] = 1.0f;
  }
  // 54..55: spread wider than one tick, and the level standing alone.
  f[54] = f[5] > 1.5f ? 1.0f : 0.0f;
  f[55] = f[2] < 0.75f ? 1.0f : 0.0f;   // log1p(orders) < 0.75 means one order
  // 56..63: queue fraction in eight buckets.
  {
    int b = static_cast<int>(f[6] * 8.0f);
    if (b < 0) b = 0;
    if (b > 7) b = 7;
    f[56 + b] = 1.0f;
  }
}

inline void build(const MarketState& st, const Query& q, std::int64_t ts_ms, float* f) {
  base_features(st, q, ts_ms, f);
  expand(f);
}

}  // namespace ltx::ml
