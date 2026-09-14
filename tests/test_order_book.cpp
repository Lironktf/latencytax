// Matching engine tests.
//
// The last case is a differential fuzz: the same random command stream is fed
// to the fast book and to a deliberately naive reference book built from
// std::map and std::list, and the resulting trades and book state must agree
// exactly. That covers the interactions between price time priority, level
// bookkeeping and the occupancy bitmap that hand written cases tend to miss.
#include <cstdint>
#include <list>
#include <map>
#include <random>
#include <vector>

#include "check.hpp"
#include "engine/order_book.hpp"

using namespace ltx;

namespace {

constexpr Qty Q(double x) { return static_cast<Qty>(x * kQtyScale + 0.5); }

struct Recorder final : EventSink {
  std::vector<TradeEvent> trades;
  std::vector<CancelEvent> cancels;
  std::vector<RejectEvent> rejects;
  std::vector<AcceptEvent> accepts;
  void on_trade(const TradeEvent& e) override { trades.push_back(e); }
  void on_cancel(const CancelEvent& e) override { cancels.push_back(e); }
  void on_reject(const RejectEvent& e) override { rejects.push_back(e); }
  void on_accept(const AcceptEvent& e) override { accepts.push_back(e); }
  void clear() { trades.clear(); cancels.clear(); rejects.clear(); accepts.clear(); }
};

BookConfig small_cfg() {
  BookConfig c;
  c.min_tick = 1;
  c.max_tick = 4096;
  c.max_orders = 4096;
  c.id_map_capacity = 8192;
  return c;
}

void test_empty_book() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  CHECK(!b.has_bid());
  CHECK(!b.has_ask());
  CHECK_EQ(b.qty_at(100), Qty(0));
  CHECK_EQ(static_cast<int>(b.cancel(1, 42)), static_cast<int>(Reject::UnknownId));
  CHECK_EQ(r.rejects.size(), size_t(1));
  CHECK(b.check_invariants());
}

void test_rest_and_best() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 1, Side::Buy, 100, Q(5));
  b.add_limit(2, 2, Side::Buy, 101, Q(3));
  b.add_limit(3, 3, Side::Sell, 105, Q(2));
  b.add_limit(4, 4, Side::Sell, 104, Q(7));
  CHECK_EQ(b.best_bid(), Tick(101));
  CHECK_EQ(b.best_ask(), Tick(104));
  CHECK_EQ(b.qty_at(100), Q(5));
  CHECK_EQ(b.orders_at(100), 1u);
  CHECK_EQ(r.trades.size(), size_t(0));
  CHECK_EQ(r.accepts.size(), size_t(4));
  CHECK(b.check_invariants());

  LevelView lv[4];
  CHECK_EQ(b.top_levels(Side::Buy, 4, lv), size_t(2));
  CHECK_EQ(lv[0].tick, Tick(101));
  CHECK_EQ(lv[1].tick, Tick(100));
  CHECK_EQ(b.top_levels(Side::Sell, 4, lv), size_t(2));
  CHECK_EQ(lv[0].tick, Tick(104));
  CHECK_EQ(lv[1].tick, Tick(105));
}

void test_price_time_priority() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  // Three makers at the same price; the earliest must fill first.
  b.add_limit(1, 10, Side::Sell, 100, Q(1));
  b.add_limit(2, 11, Side::Sell, 100, Q(1));
  b.add_limit(3, 12, Side::Sell, 100, Q(1));
  // A better price arriving later still trades first.
  b.add_limit(4, 13, Side::Sell, 99, Q(1));
  r.clear();

  b.add_limit(5, 20, Side::Buy, 100, Q(2.5));
  CHECK_EQ(r.trades.size(), size_t(3));
  CHECK_EQ(r.trades[0].maker_id, OrderId(13));
  CHECK_EQ(r.trades[0].tick, Tick(99));
  CHECK_EQ(r.trades[1].maker_id, OrderId(10));
  CHECK_EQ(r.trades[2].maker_id, OrderId(11));
  CHECK_EQ(r.trades[2].qty, Q(0.5));
  CHECK_EQ(r.trades[2].maker_remaining, Q(0.5));
  // Order 11 is half filled and still at the head; 12 is untouched behind it.
  CHECK_EQ(b.qty_at(100), Q(1.5));
  CHECK_EQ(b.orders_at(100), 2u);
  CHECK_EQ(b.queue_ahead(12), Q(0.5));
  CHECK(!b.has_bid());
  CHECK(b.check_invariants());
}

void test_partial_fill_rests_remainder() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 1, Side::Sell, 100, Q(2));
  r.clear();
  b.add_limit(2, 2, Side::Buy, 100, Q(5));
  CHECK_EQ(r.trades.size(), size_t(1));
  CHECK_EQ(r.trades[0].qty, Q(2));
  CHECK_EQ(b.best_bid(), Tick(100));
  CHECK_EQ(b.qty_at(100), Q(3));
  CHECK(!b.has_ask());
  CHECK_EQ(r.accepts.size(), size_t(1));
  CHECK_EQ(r.accepts[0].resting_qty, Q(3));
  CHECK(b.check_invariants());
}

void test_limit_does_not_cross_past_its_price() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 1, Side::Sell, 100, Q(1));
  b.add_limit(2, 2, Side::Sell, 101, Q(1));
  b.add_limit(3, 3, Side::Sell, 102, Q(1));
  r.clear();
  b.add_limit(4, 4, Side::Buy, 101, Q(5));
  CHECK_EQ(r.trades.size(), size_t(2));
  CHECK_EQ(b.best_ask(), Tick(102));
  CHECK_EQ(b.best_bid(), Tick(101));
  CHECK_EQ(b.qty_at(101), Q(3));
  CHECK(b.check_invariants());
}

void test_market_order() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 1, Side::Sell, 100, Q(1));
  b.add_limit(2, 2, Side::Sell, 120, Q(1));
  r.clear();
  b.add_market(3, 3, Side::Buy, Q(1.5));
  CHECK_EQ(r.trades.size(), size_t(2));
  CHECK_EQ(r.trades[0].tick, Tick(100));
  CHECK_EQ(r.trades[1].tick, Tick(120));
  CHECK_EQ(r.trades[1].qty, Q(0.5));
  CHECK(b.check_invariants());

  // A market order that runs out of book cancels the remainder and rests
  // nothing.
  r.clear();
  b.add_market(4, 4, Side::Buy, Q(10));
  CHECK_EQ(r.trades.size(), size_t(1));
  CHECK_EQ(r.cancels.size(), size_t(1));
  CHECK_EQ(r.cancels[0].cancelled_qty, Q(9.5));
  CHECK(!b.has_ask());
  CHECK(!b.has_bid());
  CHECK(b.check_invariants());
}

void test_ioc_and_fok() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 1, Side::Sell, 100, Q(1));
  r.clear();
  b.add_limit(2, 2, Side::Buy, 100, Q(3), Tif::Ioc);
  CHECK_EQ(r.trades.size(), size_t(1));
  CHECK_EQ(r.cancels.size(), size_t(1));
  CHECK_EQ(r.cancels[0].cancelled_qty, Q(2));
  CHECK(!b.has_bid());
  CHECK_EQ(b.live_orders(), size_t(0));

  b.add_limit(3, 3, Side::Sell, 100, Q(1));
  r.clear();
  // Not enough at the limit: nothing trades.
  CHECK_EQ(static_cast<int>(b.add_limit(4, 4, Side::Buy, 100, Q(2), Tif::Fok)),
           static_cast<int>(Reject::FokUnfillable));
  CHECK_EQ(r.trades.size(), size_t(0));
  CHECK_EQ(b.qty_at(100), Q(1));
  // Exactly enough: fills completely.
  r.clear();
  CHECK_EQ(static_cast<int>(b.add_limit(5, 5, Side::Buy, 100, Q(1), Tif::Fok)),
           static_cast<int>(Reject::None));
  CHECK_EQ(r.trades.size(), size_t(1));
  CHECK(b.check_invariants());
}

void test_cancel() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 1, Side::Buy, 100, Q(1));
  b.add_limit(2, 2, Side::Buy, 100, Q(2));
  b.add_limit(3, 3, Side::Buy, 99, Q(1));
  r.clear();
  CHECK_EQ(static_cast<int>(b.cancel(4, 1)), static_cast<int>(Reject::None));
  CHECK_EQ(b.qty_at(100), Q(2));
  CHECK_EQ(b.orders_at(100), 1u);
  CHECK_EQ(b.best_bid(), Tick(100));
  // Cancelling the last order at the best price moves the best price down.
  b.cancel(5, 2);
  CHECK_EQ(b.best_bid(), Tick(99));
  CHECK_EQ(b.qty_at(100), Qty(0));
  b.cancel(6, 3);
  CHECK(!b.has_bid());
  // Cancelling twice is a reject, not a crash.
  CHECK_EQ(static_cast<int>(b.cancel(7, 3)), static_cast<int>(Reject::UnknownId));
  CHECK(b.check_invariants());
  // The id is free for reuse after a cancel.
  CHECK_EQ(static_cast<int>(b.add_limit(8, 3, Side::Buy, 98, Q(1))),
           static_cast<int>(Reject::None));
}

void test_duplicate_id_rejected() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 7, Side::Buy, 100, Q(1));
  CHECK_EQ(static_cast<int>(b.add_limit(2, 7, Side::Buy, 101, Q(1))),
           static_cast<int>(Reject::DuplicateId));
  CHECK_EQ(b.best_bid(), Tick(100));
  CHECK_EQ(b.live_orders(), size_t(1));
  CHECK(b.check_invariants());
}

void test_modify_keeps_priority_only_on_size_down() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 1, Side::Buy, 100, Q(5));
  b.add_limit(2, 2, Side::Buy, 100, Q(5));
  // Size down at the same price: order 1 stays in front.
  CHECK_EQ(static_cast<int>(b.modify(3, 1, 100, Q(2))), static_cast<int>(Reject::None));
  CHECK_EQ(b.qty_at(100), Q(7));
  CHECK_EQ(b.queue_ahead(1), Qty(0));
  CHECK_EQ(b.queue_ahead(2), Q(2));

  // Size up at the same price: order 1 goes to the back.
  CHECK_EQ(static_cast<int>(b.modify(4, 1, 100, Q(6))), static_cast<int>(Reject::None));
  CHECK_EQ(b.qty_at(100), Q(11));
  CHECK_EQ(b.queue_ahead(2), Qty(0));
  CHECK_EQ(b.queue_ahead(1), Q(5));

  // Price change: new level, back of that queue.
  CHECK_EQ(static_cast<int>(b.modify(5, 1, 99, Q(6))), static_cast<int>(Reject::None));
  CHECK_EQ(b.qty_at(100), Q(5));
  CHECK_EQ(b.qty_at(99), Q(6));
  CHECK_EQ(b.best_bid(), Tick(100));

  // Same price, same size: a no op that keeps position.
  b.add_limit(6, 3, Side::Buy, 99, Q(1));
  CHECK_EQ(static_cast<int>(b.modify(7, 1, 99, Q(6))), static_cast<int>(Reject::None));
  CHECK_EQ(b.queue_ahead(1), Qty(0));
  CHECK_EQ(b.queue_ahead(3), Q(6));

  CHECK_EQ(static_cast<int>(b.modify(8, 999, 99, Q(1))),
           static_cast<int>(Reject::UnknownId));
  CHECK(b.check_invariants());
}

void test_modify_can_cross() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  b.add_limit(1, 1, Side::Sell, 101, Q(4));
  b.add_limit(2, 2, Side::Buy, 100, Q(4));
  r.clear();
  // Repricing the bid up through the offer trades.
  b.modify(3, 2, 101, Q(4));
  CHECK_EQ(r.trades.size(), size_t(1));
  CHECK_EQ(r.trades[0].qty, Q(4));
  CHECK_EQ(r.trades[0].maker_id, OrderId(1));
  CHECK(!b.has_bid());
  CHECK(!b.has_ask());
  CHECK(b.check_invariants());
}

void test_out_of_range_price() {
  Recorder r;
  BookConfig c = small_cfg();
  OrderBook b(c, &r);
  CHECK_EQ(static_cast<int>(b.add_limit(1, 1, Side::Buy, 0, Q(1))),
           static_cast<int>(Reject::PriceOutOfRange));
  CHECK_EQ(static_cast<int>(b.add_limit(2, 2, Side::Buy, c.max_tick + 1, Q(1))),
           static_cast<int>(Reject::PriceOutOfRange));
  CHECK_EQ(static_cast<int>(b.add_limit(3, 3, Side::Buy, 100, 0)),
           static_cast<int>(Reject::BadQty));
  CHECK_EQ(b.live_orders(), size_t(0));
  CHECK(b.check_invariants());
}

void test_sweep_many_levels() {
  Recorder r;
  OrderBook b(small_cfg(), &r);
  // 200 ask levels one tick apart, so the sweep crosses many bitmap words.
  for (int i = 0; i < 200; ++i) {
    b.add_limit(1, 1000 + i, Side::Sell, static_cast<Tick>(500 + i), Q(1));
  }
  CHECK_EQ(b.best_ask(), Tick(500));
  r.clear();
  b.add_limit(2, 1, Side::Buy, 699, Q(500));
  CHECK_EQ(r.trades.size(), size_t(200));
  CHECK(!b.has_ask());
  CHECK_EQ(b.best_bid(), Tick(699));
  CHECK_EQ(b.qty_at(699), Q(300));
  CHECK(b.check_invariants());
}

// ---------------------------------------------------------------------------
// Differential fuzz against a naive reference implementation.
// ---------------------------------------------------------------------------

struct RefOrder {
  OrderId id;
  Qty qty;
};

class RefBook {
 public:
  std::map<Tick, std::list<RefOrder>> bids;  // descending via rbegin
  std::map<Tick, std::list<RefOrder>> asks;
  std::map<OrderId, std::pair<Side, Tick>> where;
  std::vector<TradeEvent> trades;

  bool has(OrderId id) const { return where.count(id) > 0; }

  void add(Ts ts, OrderId id, Side side, Tick tick, Qty qty, Tif tif) {
    if (qty <= 0 || has(id)) return;
    if (tif == Tif::Fok && avail(side, tick) < qty) return;
    Qty rem = cross(ts, id, side, tick, qty);
    if (rem > 0 && tif == Tif::Gtc) {
      auto& book = side == Side::Buy ? bids : asks;
      book[tick].push_back(RefOrder{id, rem});
      where[id] = {side, tick};
    }
  }

  void market(Ts ts, OrderId id, Side side, Qty qty) {
    if (qty <= 0) return;
    cross(ts, id, side, side == Side::Buy ? 2000000000 : -2000000000, qty);
  }

  void cancel(OrderId id) {
    auto it = where.find(id);
    if (it == where.end()) return;
    auto& book = it->second.first == Side::Buy ? bids : asks;
    auto lit = book.find(it->second.second);
    for (auto o = lit->second.begin(); o != lit->second.end(); ++o) {
      if (o->id == id) { lit->second.erase(o); break; }
    }
    if (lit->second.empty()) book.erase(lit);
    where.erase(it);
  }

  void modify(Ts ts, OrderId id, Tick tick, Qty qty) {
    auto it = where.find(id);
    if (it == where.end() || qty <= 0) return;
    const Side side = it->second.first;
    const Tick old_tick = it->second.second;
    auto& book = side == Side::Buy ? bids : asks;
    auto& lst = book[old_tick];
    if (tick == old_tick) {
      for (auto& o : lst) {
        if (o.id == id) {
          if (qty <= o.qty) { o.qty = qty; return; }
          break;
        }
      }
    }
    cancel(id);
    add(ts, id, side, tick, qty, Tif::Gtc);
  }

  Qty qty_at(Side side, Tick t) const {
    const auto& book = side == Side::Buy ? bids : asks;
    auto it = book.find(t);
    if (it == book.end()) return 0;
    Qty s = 0;
    for (const auto& o : it->second) s += o.qty;
    return s;
  }

 private:
  Qty avail(Side side, Tick limit) const {
    Qty s = 0;
    if (side == Side::Buy) {
      for (const auto& [t, l] : asks) {
        if (t > limit) break;
        for (const auto& o : l) s += o.qty;
      }
    } else {
      for (auto it = bids.rbegin(); it != bids.rend(); ++it) {
        if (it->first < limit) break;
        for (const auto& o : it->second) s += o.qty;
      }
    }
    return s;
  }

  Qty cross(Ts ts, OrderId taker, Side side, Tick limit, Qty qty) {
    auto& opp = side == Side::Buy ? asks : bids;
    while (qty > 0 && !opp.empty()) {
      auto lit = side == Side::Buy ? opp.begin() : std::prev(opp.end());
      const Tick t = lit->first;
      if (side == Side::Buy ? (t > limit) : (t < limit)) break;
      auto& lst = lit->second;
      while (qty > 0 && !lst.empty()) {
        RefOrder& m = lst.front();
        const Qty take = std::min(qty, m.qty);
        m.qty -= take;
        qty -= take;
        trades.push_back(TradeEvent{ts, m.id, taker, t, take, side, m.qty});
        if (m.qty == 0) { where.erase(m.id); lst.pop_front(); }
      }
      if (lst.empty()) opp.erase(lit);
    }
    return qty;
  }
};

void test_differential_fuzz() {
  BookConfig cfg;
  cfg.min_tick = 1;
  cfg.max_tick = 2048;
  cfg.max_orders = 1u << 16;
  cfg.id_map_capacity = 1u << 17;

  for (unsigned seed = 1; seed <= 8; ++seed) {
    Recorder rec;
    OrderBook fast(cfg, &rec);
    RefBook ref;
    std::mt19937_64 rng(seed);
    std::vector<OrderId> live;
    OrderId next_id = 1;
    const Tick mid = 1000;

    for (int step = 0; step < 40000; ++step) {
      const int roll = static_cast<int>(rng() % 100);
      const Ts ts = step;
      if (roll < 55 || live.empty()) {
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const int off = static_cast<int>(rng() % 12) - 5;
        const Tick tick = static_cast<Tick>(mid + (side == Side::Buy ? -off : off));
        const Qty qty = static_cast<Qty>(1 + rng() % 500) * 1000;
        const unsigned tr = static_cast<unsigned>(rng() % 20);
        const Tif tif = tr < 16 ? Tif::Gtc : (tr < 18 ? Tif::Ioc : Tif::Fok);
        const OrderId id = next_id++;
        fast.add_limit(ts, id, side, tick, qty, tif);
        ref.add(ts, id, side, tick, qty, tif);
        if (fast.is_live(id)) live.push_back(id);
      } else if (roll < 80) {
        const std::size_t i = rng() % live.size();
        const OrderId id = live[i];
        live[i] = live.back();
        live.pop_back();
        fast.cancel(ts, id);
        ref.cancel(id);
      } else if (roll < 92) {
        const std::size_t i = rng() % live.size();
        const OrderId id = live[i];
        const Tick t = fast.order_tick(id);
        const Qty q = fast.order_qty(id);
        const bool reprice = (rng() % 3) == 0;
        const Tick nt = reprice ? static_cast<Tick>(t + (int)(rng() % 7) - 3) : t;
        const Qty nq = (rng() % 2) ? q / 2 + 1 : q * 2;
        fast.modify(ts, id, nt, nq);
        ref.modify(ts, id, nt, nq);
        if (!fast.is_live(id)) { live[i] = live.back(); live.pop_back(); }
      } else {
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const Qty qty = static_cast<Qty>(1 + rng() % 2000) * 1000;
        const OrderId id = next_id++;
        fast.add_market(ts, id, side, qty);
        ref.market(ts, id, side, qty);
      }

      // Drop ids the reference says are gone.
      if ((step & 255) == 0) {
        std::vector<OrderId> keep;
        keep.reserve(live.size());
        for (OrderId id : live) if (fast.is_live(id)) keep.push_back(id);
        live.swap(keep);
      }
    }

    CHECK(fast.check_invariants());
    // Trade streams must match event for event.
    CHECK_EQ(rec.trades.size(), ref.trades.size());
    const std::size_t n = std::min(rec.trades.size(), ref.trades.size());
    std::size_t mismatch = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const auto& a = rec.trades[i];
      const auto& b = ref.trades[i];
      if (a.maker_id != b.maker_id || a.taker_id != b.taker_id || a.tick != b.tick ||
          a.qty != b.qty || a.aggressor != b.aggressor ||
          a.maker_remaining != b.maker_remaining) {
        ++mismatch;
      }
    }
    CHECK_EQ(mismatch, size_t(0));

    // Final book state must match level for level.
    CHECK_EQ(fast.live_orders(), ref.where.size());
    std::size_t level_mismatch = 0;
    for (Tick t = cfg.min_tick; t <= cfg.max_tick; ++t) {
      const Qty want = ref.qty_at(Side::Buy, t) + ref.qty_at(Side::Sell, t);
      if (fast.qty_at(t) != want) ++level_mismatch;
    }
    CHECK_EQ(level_mismatch, size_t(0));
    const Tick ref_bid = ref.bids.empty() ? kInvalidTick : ref.bids.rbegin()->first;
    const Tick ref_ask = ref.asks.empty() ? kInvalidTick : ref.asks.begin()->first;
    CHECK_EQ(fast.best_bid(), ref_bid);
    CHECK_EQ(fast.best_ask(), ref_ask);
  }
}

}  // namespace

int main() {
  RUN(test_empty_book);
  RUN(test_rest_and_best);
  RUN(test_price_time_priority);
  RUN(test_partial_fill_rests_remainder);
  RUN(test_limit_does_not_cross_past_its_price);
  RUN(test_market_order);
  RUN(test_ioc_and_fok);
  RUN(test_cancel);
  RUN(test_duplicate_id_rejected);
  RUN(test_modify_keeps_priority_only_on_size_down);
  RUN(test_modify_can_cross);
  RUN(test_out_of_range_price);
  RUN(test_sweep_many_levels);
  RUN(test_differential_fuzz);
  return ltxtest::summary();
}
