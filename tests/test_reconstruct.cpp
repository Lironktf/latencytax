// Parser and reconstruction tests.
//
// The reconstruction is what turns 5 second snapshots into engine flow, so the
// cases here pin down the parts that are easy to get wrong: exact fixed point
// parsing, which side of the book a tape print consumes, the mapping from level
// changes to add, cancel and modify, and that a reconstructed window lands on
// the target snapshot exactly.
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "engine/engine.hpp"
#include "replay/loader.hpp"
#include "replay/reconstruct.hpp"

using namespace ltx;

namespace {

constexpr Qty Q(double x) { return static_cast<Qty>(x * kQtyScale + 0.5); }

void test_parse_fixed() {
  std::int64_t v = 0;
  CHECK(parse_fixed("1916.3", 1, v)); CHECK_EQ(v, std::int64_t(19163));
  CHECK(parse_fixed("1916", 1, v));   CHECK_EQ(v, std::int64_t(19160));
  CHECK(parse_fixed("1916.0", 1, v)); CHECK_EQ(v, std::int64_t(19160));
  CHECK(parse_fixed("0.0286", 8, v)); CHECK_EQ(v, std::int64_t(2860000));
  CHECK(parse_fixed("260.9149", 8, v)); CHECK_EQ(v, std::int64_t(26091490000));
  CHECK(parse_fixed("-1.5", 1, v));   CHECK_EQ(v, std::int64_t(-15));
  CHECK(parse_fixed(".5", 1, v));     CHECK_EQ(v, std::int64_t(5));
  CHECK(!parse_fixed("", 1, v));
  CHECK(!parse_fixed("abc", 1, v));
  CHECK(!parse_fixed("1.2x", 1, v));
  // 1916.35 at one decimal truncates rather than rounding; the raw feed never
  // carries a second decimal, and a silent round would be worse than a shift.
  CHECK(parse_fixed("1916.35", 1, v)); CHECK_EQ(v, std::int64_t(19163));
}

void test_parse_snapshot() {
  const std::string line =
      R"({"rx": 1786233586649, "book": {"coin": "ETH", "time": 1786233586148, "levels": )"
      R"([[{"px": "1916.0", "sz": "260.9149", "n": 23}, {"px": "1915.9", "sz": "60.9933", "n": 9}],)"
      R"([{"px": "1916.1", "sz": "128.2341", "n": 23}, {"px": "1916.2", "sz": "46.1247", "n": 6}]]}})";
  Snapshot s;
  CHECK(parse_snapshot(line, 1, s));
  CHECK_EQ(s.rx_ms, std::int64_t(1786233586649));
  CHECK_EQ(s.time_ms, std::int64_t(1786233586148));
  CHECK_EQ(int(s.n_bids), 2);
  CHECK_EQ(int(s.n_asks), 2);
  CHECK_EQ(s.bids[0].tick, Tick(19160));
  CHECK_EQ(s.bids[0].qty, std::int64_t(26091490000));
  CHECK_EQ(s.bids[0].orders, 23u);
  CHECK_EQ(s.bids[1].tick, Tick(19159));
  CHECK_EQ(s.asks[0].tick, Tick(19161));
  CHECK_EQ(s.asks[1].tick, Tick(19162));
  CHECK(!parse_snapshot("{\"rx\": 1}", 1, s));
  CHECK(!parse_snapshot("", 1, s));
}

void test_parse_trade() {
  const std::string line =
      R"({"rx":1786233611997,"trade":{"coin":"ETH","side":"A","px":"1916.0","sz":"0.0286",)"
      R"("time":1786233592009,"hash":"0x00","tid":414374466238233,"users":["0xaa","0xbb"]}})";
  RawTrade t;
  CHECK(parse_trade(line, 1, t));
  CHECK_EQ(t.rx_ms, std::int64_t(1786233611997));
  CHECK_EQ(t.time_ms, std::int64_t(1786233592009));
  CHECK_EQ(t.tick, Tick(19160));
  CHECK_EQ(t.qty, std::int64_t(2860000));
  CHECK_EQ(t.tid, std::uint64_t(414374466238233));
  CHECK(t.aggressor == Side::Sell);  // "A" hits the bid

  const std::string buy =
      R"({"rx":1,"trade":{"coin":"ETH","side":"B","px":"1916.1","sz":"1.0","time":2,)"
      R"("hash":"0x00","tid":7,"users":[]}})";
  CHECK(parse_trade(buy, 1, t));
  CHECK(t.aggressor == Side::Buy);
  CHECK_EQ(t.tick, Tick(19161));
}

Snapshot make_snap(std::int64_t t, Tick bid_top, std::initializer_list<double> bid_sz,
                   Tick ask_top, std::initializer_list<double> ask_sz) {
  Snapshot s;
  s.time_ms = t;
  s.rx_ms = t + 300;
  for (double x : bid_sz) {
    s.bids[s.n_bids] = RawLevel{static_cast<Tick>(bid_top - s.n_bids), Q(x), 1};
    ++s.n_bids;
  }
  for (double x : ask_sz) {
    s.asks[s.n_asks] = RawLevel{static_cast<Tick>(ask_top + s.n_asks), Q(x), 1};
    ++s.n_asks;
  }
  return s;
}

BookConfig cfg() {
  BookConfig c;
  c.min_tick = 1;
  c.max_tick = 65536;
  c.max_orders = 4096;
  c.id_map_capacity = 8192;
  return c;
}

// Any reconstructed window must land on the target snapshot exactly, for every
// side of the book and whatever the tape did.
void expect_matches(const MatchingEngine& eng, const Snapshot& s) {
  LevelView lv[kSnapDepth];
  const std::size_t nb = eng.book().top_levels(Side::Buy, kSnapDepth, lv);
  CHECK_EQ(nb, std::size_t(s.n_bids));
  for (std::size_t i = 0; i < std::min(nb, std::size_t(s.n_bids)); ++i) {
    CHECK_EQ(lv[i].tick, s.bids[i].tick);
    CHECK_EQ(lv[i].qty, s.bids[i].qty);
  }
  const std::size_t na = eng.book().top_levels(Side::Sell, kSnapDepth, lv);
  CHECK_EQ(na, std::size_t(s.n_asks));
  for (std::size_t i = 0; i < std::min(na, std::size_t(s.n_asks)); ++i) {
    CHECK_EQ(lv[i].tick, s.asks[i].tick);
    CHECK_EQ(lv[i].qty, s.asks[i].qty);
  }
}

void test_reconstruct_no_trades() {
  std::vector<Snapshot> s;
  s.push_back(make_snap(0, 1000, {10, 20, 30}, 1001, {15, 25}));
  // A level shrinks, one grows, one disappears, one appears.
  s.push_back(make_snap(5000, 1000, {4, 20, 30, 40}, 1001, {15, 60}));
  NullSink sink;
  MatchingEngine eng(cfg(), &sink);
  Reconstructor rec(eng);
  const ReplayStats st = rec.run(s, {});
  CHECK_EQ(st.windows, std::uint64_t(1));
  CHECK_EQ(st.total.recon_level_mismatch, 0u);
  CHECK_EQ(st.total.recon_unexpected_trades, std::uint64_t(0));
  CHECK_EQ(st.total.adds, 1u);       // the new bid at 997
  CHECK_EQ(st.total.modifies, 2u);   // bid top down, ask second up
  CHECK_EQ(st.total.cancels, 0u);
  expect_matches(eng, s[1]);
  CHECK(eng.book().check_invariants());
}

void test_reconstruct_with_trades() {
  std::vector<Snapshot> s;
  s.push_back(make_snap(0, 1000, {10, 20}, 1001, {15, 25}));
  s.push_back(make_snap(5000, 1000, {3, 20}, 1001, {15, 25}));
  std::vector<RawTrade> t;
  // A seller takes 7 off the top bid inside the window.
  t.push_back(RawTrade{1200, 1200, 1, 1000, Q(7), Side::Sell});
  NullSink sink;
  MatchingEngine eng(cfg(), &sink);
  Reconstructor rec(eng);
  rec.keep_per_window(true);
  const ReplayStats st = rec.run(s, t);
  CHECK_EQ(st.windows, std::uint64_t(1));
  CHECK_EQ(st.total.recon_level_mismatch, 0u);
  CHECK_EQ(st.total.matched_qty, Q(7));
  CHECK_EQ(st.total.swept_better_qty, Qty(0));
  // The tape fully explains this window, so the tape only score is clean and
  // the reconciliation has nothing to do.
  CHECK_EQ(st.total.tape_level_mismatch, 0u);
  CHECK_EQ(st.total.adds + st.total.cancels + st.total.modifies, 0u);
  expect_matches(eng, s[1]);
}

void test_trade_sweeps_through_a_level() {
  std::vector<Snapshot> s;
  s.push_back(make_snap(0, 1000, {10, 20}, 1001, {5, 25}));
  s.push_back(make_snap(5000, 999, {20}, 1002, {17}));
  std::vector<RawTrade> t;
  // A buyer takes all 5 at 1001 and 8 of the 25 at 1002, printed at 1002.
  t.push_back(RawTrade{1000, 1000, 1, 1002, Q(13), Side::Buy});
  NullSink sink;
  MatchingEngine eng(cfg(), &sink);
  Reconstructor rec(eng);
  const ReplayStats st = rec.run(s, t);
  CHECK_EQ(st.total.matched_qty, Q(13));
  CHECK_EQ(st.total.swept_better_qty, Q(5));  // the part that filled at 1001
  CHECK_EQ(st.total.recon_level_mismatch, 0u);
  expect_matches(eng, s[1]);
}

void test_trade_with_no_liquidity() {
  std::vector<Snapshot> s;
  s.push_back(make_snap(0, 1000, {10}, 1001, {10}));
  s.push_back(make_snap(5000, 1000, {10}, 1001, {10}));
  std::vector<RawTrade> t;
  // A print far above the visible book: the snapshot was stale.
  t.push_back(RawTrade{1000, 1000, 1, 1050, Q(1), Side::Sell});
  NullSink sink;
  MatchingEngine eng(cfg(), &sink);
  Reconstructor rec(eng);
  const ReplayStats st = rec.run(s, t);
  CHECK_EQ(st.total.trades_no_liquidity, std::uint64_t(1));
  CHECK_EQ(st.total.matched_qty, Qty(0));
  CHECK_EQ(st.total.recon_level_mismatch, 0u);
  expect_matches(eng, s[1]);
}

void test_gap_is_skipped_not_scored() {
  std::vector<Snapshot> s;
  s.push_back(make_snap(0, 1000, {10}, 1001, {10}));
  s.push_back(make_snap(600000, 900, {10}, 901, {10}));  // ten minutes later
  s.push_back(make_snap(605000, 900, {12}, 901, {10}));
  NullSink sink;
  MatchingEngine eng(cfg(), &sink);
  Reconstructor rec(eng);
  const ReplayStats st = rec.run(s, {});
  CHECK_EQ(st.windows_skipped_gap, std::uint64_t(1));
  CHECK_EQ(st.windows, std::uint64_t(1));
  CHECK_EQ(st.total.recon_level_mismatch, 0u);
  expect_matches(eng, s[2]);
}

// A book that moves a long way in one window must not have the reconciliation
// cross the new touch against stale levels on the other side.
void test_large_reprice_does_not_cross() {
  std::vector<Snapshot> s;
  s.push_back(make_snap(0, 1000, {10, 10, 10, 10}, 1001, {10, 10, 10, 10}));
  s.push_back(make_snap(5000, 1030, {10, 10, 10, 10}, 1031, {10, 10, 10, 10}));
  s.push_back(make_snap(10000, 970, {10, 10, 10, 10}, 971, {10, 10, 10, 10}));
  NullSink sink;
  MatchingEngine eng(cfg(), &sink);
  Reconstructor rec(eng);
  const ReplayStats st = rec.run(s, {});
  CHECK_EQ(st.total.recon_unexpected_trades, std::uint64_t(0));
  CHECK_EQ(st.total.recon_level_mismatch, 0u);
  expect_matches(eng, s[2]);
  CHECK(eng.book().check_invariants());
}

void test_many_random_windows() {
  // Random books that stay well formed. Every window must reconstruct exactly.
  std::mt19937_64 rng(99);
  std::vector<Snapshot> s;
  Tick mid = 20000;
  for (int i = 0; i < 2000; ++i) {
    mid += static_cast<Tick>(rng() % 5) - 2;
    Snapshot snap;
    snap.time_ms = i * 5000;
    snap.rx_ms = snap.time_ms + 300;
    for (int k = 0; k < 20; ++k) {
      snap.bids[k] = RawLevel{static_cast<Tick>(mid - k),
                              static_cast<Qty>(1 + rng() % 100000) * 1000, 1};
      snap.asks[k] = RawLevel{static_cast<Tick>(mid + 1 + k),
                              static_cast<Qty>(1 + rng() % 100000) * 1000, 1};
    }
    snap.n_bids = 20;
    snap.n_asks = 20;
    s.push_back(snap);
  }
  std::vector<RawTrade> t;
  for (int i = 0; i < 4000; ++i) {
    const std::size_t w = rng() % (s.size() - 1);
    const bool buy = rng() & 1;
    const Tick base = buy ? s[w].asks[0].tick : s[w].bids[0].tick;
    t.push_back(RawTrade{0, static_cast<std::int64_t>(s[w].time_ms + 1 + rng() % 4000),
                         static_cast<std::uint64_t>(i),
                         static_cast<Tick>(base + (buy ? 1 : -1) * static_cast<Tick>(rng() % 3)),
                         static_cast<Qty>(1 + rng() % 50000) * 1000,
                         buy ? Side::Buy : Side::Sell});
  }
  std::sort(t.begin(), t.end(),
            [](const RawTrade& a, const RawTrade& b) { return a.time_ms < b.time_ms; });

  NullSink sink;
  MatchingEngine eng(cfg(), &sink);
  Reconstructor rec(eng);
  const ReplayStats st = rec.run(s, t);
  CHECK_EQ(st.windows, std::uint64_t(1999));
  CHECK_EQ(st.total.recon_level_mismatch, 0u);
  CHECK_EQ(st.total.recon_unexpected_trades, std::uint64_t(0));
  CHECK(st.total.trades > 3000);
  expect_matches(eng, s.back());
  CHECK(eng.book().check_invariants());
}

}  // namespace

int main() {
  RUN(test_parse_fixed);
  RUN(test_parse_snapshot);
  RUN(test_parse_trade);
  RUN(test_reconstruct_no_trades);
  RUN(test_reconstruct_with_trades);
  RUN(test_trade_sweeps_through_a_level);
  RUN(test_trade_with_no_liquidity);
  RUN(test_gap_is_skipped_not_scored);
  RUN(test_large_reprice_does_not_cross);
  RUN(test_many_random_windows);
  return ltxtest::summary();
}
