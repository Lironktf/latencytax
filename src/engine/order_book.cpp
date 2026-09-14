#include "order_book.hpp"

#include <algorithm>
#include <cassert>

namespace ltx {

OrderBook::OrderBook(const BookConfig& cfg, EventSink* sink)
    : cfg_(cfg), sink_(sink), ids_(cfg.id_map_capacity) {
  const std::size_t n = static_cast<std::size_t>(cfg_.max_tick - cfg_.min_tick) + 1;
  levels_.assign(n, Level{});
  occupied_.assign((n + 63) / 64, 0ull);
  pool_.resize(cfg_.max_orders);
  // Free list threaded through `next`. Slots are handed out in ascending order
  // on a fresh book so early orders land in contiguous memory.
  for (std::size_t i = 0; i + 1 < pool_.size(); ++i) {
    pool_[i].next = static_cast<Slot>(i + 1);
  }
  if (!pool_.empty()) pool_.back().next = kNullSlot;
  free_head_ = pool_.empty() ? kNullSlot : 0;
}

void OrderBook::clear() {
  std::fill(levels_.begin(), levels_.end(), Level{});
  std::fill(occupied_.begin(), occupied_.end(), 0ull);
  for (std::size_t i = 0; i + 1 < pool_.size(); ++i) {
    pool_[i] = Order{};
    pool_[i].next = static_cast<Slot>(i + 1);
  }
  if (!pool_.empty()) {
    pool_.back() = Order{};
    pool_.back().next = kNullSlot;
  }
  free_head_ = pool_.empty() ? kNullSlot : 0;
  pool_used_ = 0;
  ids_.clear();
  best_bid_ = kInvalidTick;
  best_ask_ = kInvalidTick;
}

std::size_t OrderBook::scan_down(std::size_t from) const noexcept {
  if (from == npos) return npos;
  std::size_t w = from >> 6;
  std::uint64_t word = occupied_[w] & ((from & 63) == 63 ? ~0ull
                                        : ((1ull << ((from & 63) + 1)) - 1));
  while (true) {
    if (word) return (w << 6) + (63 - static_cast<std::size_t>(__builtin_clzll(word)));
    if (w == 0) return npos;
    --w;
    word = occupied_[w];
  }
}

std::size_t OrderBook::scan_up(std::size_t from) const noexcept {
  const std::size_t n = levels_.size();
  if (from >= n) return npos;
  std::size_t w = from >> 6;
  std::uint64_t word = occupied_[w] & (~0ull << (from & 63));
  const std::size_t nw = occupied_.size();
  while (true) {
    if (word) {
      const std::size_t i = (w << 6) + static_cast<std::size_t>(__builtin_ctzll(word));
      return i < n ? i : npos;
    }
    if (++w >= nw) return npos;
    word = occupied_[w];
  }
}

Slot OrderBook::alloc_slot() noexcept {
  if (free_head_ == kNullSlot) return kNullSlot;
  const Slot s = free_head_;
  free_head_ = pool_[s].next;
  ++pool_used_;
  return s;
}

void OrderBook::free_slot(Slot s) noexcept {
  pool_[s].live = false;
  pool_[s].id = kNoOrder;
  pool_[s].next = free_head_;
  free_head_ = s;
  --pool_used_;
}

void OrderBook::link_back(std::size_t li, Slot s) noexcept {
  Level& lv = levels_[li];
  pool_[s].next = kNullSlot;
  pool_[s].prev = lv.tail;
  if (lv.tail != kNullSlot) {
    pool_[lv.tail].next = s;
  } else {
    lv.head = s;
  }
  lv.tail = s;
  ++lv.orders;
}

void OrderBook::unlink(std::size_t li, Slot s) noexcept {
  Level& lv = levels_[li];
  const Slot p = pool_[s].prev;
  const Slot n = pool_[s].next;
  if (p != kNullSlot) pool_[p].next = n; else lv.head = n;
  if (n != kNullSlot) pool_[n].prev = p; else lv.tail = p;
  --lv.orders;
}

void OrderBook::refresh_best_after_removal(Side maker_side, std::size_t li) noexcept {
  if (maker_side == Side::Buy) {
    if (tick_of(li) != best_bid_) return;
    const std::size_t nxt = li == 0 ? npos : scan_down(li - 1);
    best_bid_ = (nxt == npos) ? kInvalidTick : tick_of(nxt);
  } else {
    if (tick_of(li) != best_ask_) return;
    const std::size_t nxt = scan_up(li + 1);
    best_ask_ = (nxt == npos) ? kInvalidTick : tick_of(nxt);
  }
}

Qty OrderBook::available(Side side, Tick limit) const {
  // Quantity the incoming `side` order could consume at prices up to `limit`.
  Qty total = 0;
  if (side == Side::Buy) {
    if (!has_ask()) return 0;
    for (std::size_t i = scan_up(idx(best_ask_)); i != npos; i = scan_up(i + 1)) {
      if (tick_of(i) > limit) break;
      total += levels_[i].qty;
    }
  } else {
    if (!has_bid()) return 0;
    std::size_t i = idx(best_bid_);
    while (true) {
      i = scan_down(i);
      if (i == npos || tick_of(i) < limit) break;
      total += levels_[i].qty;
      if (i == 0) break;
      --i;
    }
  }
  return total;
}

Qty OrderBook::match(Ts ts, OrderId taker, Side side, Tick limit, Qty qty) {
  Qty filled = 0;
  const Side maker_side = opposite(side);
  while (qty > 0) {
    Tick best = (side == Side::Buy) ? best_ask_ : best_bid_;
    if (best == kInvalidTick) break;
    if (side == Side::Buy ? (best > limit) : (best < limit)) break;

    const std::size_t li = idx(best);
    Level& lv = levels_[li];
    while (qty > 0 && lv.head != kNullSlot) {
      const Slot ms = lv.head;
      Order& mo = pool_[ms];
      const Qty take = std::min(qty, mo.qty);
      mo.qty -= take;
      lv.qty -= take;
      qty -= take;
      filled += take;
      if (sink_) {
        sink_->on_trade(TradeEvent{ts, mo.id, taker, best, take, side, mo.qty});
      }
      if (mo.qty == 0) {
        const OrderId dead = mo.id;
        unlink(li, ms);
        ids_.erase(dead);
        free_slot(ms);
      }
    }
    if (lv.qty == 0) {
      // Level exhausted. Drop the occupancy bit and walk to the next price.
      lv.head = kNullSlot;
      lv.tail = kNullSlot;
      lv.orders = 0;
      bit_clear(li);
      refresh_best_after_removal(maker_side, li);
    }
  }
  return filled;
}

void OrderBook::rest(Ts ts, Slot s) {
  Order& o = pool_[s];
  const std::size_t li = idx(o.tick);
  Level& lv = levels_[li];
  if (lv.qty == 0 && lv.head == kNullSlot) bit_set(li);
  lv.qty += o.qty;
  link_back(li, s);
  o.live = true;
  if (o.side == Side::Buy) {
    if (best_bid_ == kInvalidTick || o.tick > best_bid_) best_bid_ = o.tick;
  } else {
    if (best_ask_ == kInvalidTick || o.tick < best_ask_) best_ask_ = o.tick;
  }
  if (sink_) sink_->on_accept(AcceptEvent{ts, o.id, o.tick, o.qty, o.side});
}

Reject OrderBook::add_limit(Ts ts, OrderId id, Side side, Tick tick, Qty qty, Tif tif) {
  if (qty <= 0) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::BadQty});
    return Reject::BadQty;
  }
  if (!in_range(tick)) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::PriceOutOfRange});
    return Reject::PriceOutOfRange;
  }
  if (id == kNoOrder || ids_.find(id) != kNullSlot) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::DuplicateId});
    return Reject::DuplicateId;
  }
  if (tif == Tif::Gtc && free_head_ == kNullSlot) {
    // Checked before matching so a reject leaves the book untouched. Matching
    // only frees slots, so if the list is non-empty now it is non-empty after.
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::PoolExhausted});
    return Reject::PoolExhausted;
  }
  if (tif == Tif::Fok && available(side, tick) < qty) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::FokUnfillable});
    return Reject::FokUnfillable;
  }

  const Qty filled = match(ts, id, side, tick, qty);
  const Qty rem = qty - filled;
  if (rem == 0) return Reject::None;

  if (tif != Tif::Gtc) {
    if (sink_) {
      sink_->on_cancel(CancelEvent{ts, id, tick, rem, side,
                                   tif == Tif::Ioc ? CancelReason::Ioc : CancelReason::Fok});
    }
    return Reject::None;
  }

  const Slot s = alloc_slot();
  assert(s != kNullSlot);
  Order& o = pool_[s];
  o.id = id;
  o.qty = rem;
  o.tick = tick;
  o.side = side;
  ids_.insert(id, s);
  rest(ts, s);
  return Reject::None;
}

Reject OrderBook::add_market(Ts ts, OrderId id, Side side, Qty qty) {
  if (qty <= 0) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::BadQty});
    return Reject::BadQty;
  }
  const Tick limit = (side == Side::Buy) ? cfg_.max_tick : cfg_.min_tick;
  const Qty filled = match(ts, id, side, limit, qty);
  const Qty rem = qty - filled;
  if (rem > 0 && sink_) {
    sink_->on_cancel(CancelEvent{ts, id, limit, rem, side, CancelReason::Ioc});
  }
  return Reject::None;
}

void OrderBook::remove_resting(Slot s, CancelReason reason, Ts ts) {
  Order& o = pool_[s];
  const std::size_t li = idx(o.tick);
  Level& lv = levels_[li];
  const Qty left = o.qty;
  const Side side = o.side;
  const Tick tick = o.tick;
  const OrderId id = o.id;
  lv.qty -= left;
  unlink(li, s);
  if (lv.orders == 0) {
    lv.qty = 0;
    lv.head = kNullSlot;
    lv.tail = kNullSlot;
    bit_clear(li);
    refresh_best_after_removal(side, li);
  }
  ids_.erase(id);
  free_slot(s);
  if (sink_) sink_->on_cancel(CancelEvent{ts, id, tick, left, side, reason});
}

Reject OrderBook::cancel(Ts ts, OrderId id) {
  const Slot s = ids_.find(id);
  if (s == kNullSlot) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::UnknownId});
    return Reject::UnknownId;
  }
  remove_resting(s, CancelReason::User, ts);
  return Reject::None;
}

Reject OrderBook::reduce(Ts ts, OrderId id, Qty shares_cancelled) {
  const Slot s = ids_.find(id);
  if (s == kNullSlot) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::UnknownId});
    return Reject::UnknownId;
  }
  if (shares_cancelled <= 0) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::BadQty});
    return Reject::BadQty;
  }
  Order& o = pool_[s];
  if (shares_cancelled >= o.qty) {
    // The feed says more went away than we think is there. Taking the whole
    // order is the only reading that leaves the book consistent.
    remove_resting(s, CancelReason::User, ts);
    return Reject::None;
  }
  o.qty -= shares_cancelled;
  levels_[idx(o.tick)].qty -= shares_cancelled;
  if (sink_) {
    sink_->on_cancel(
        CancelEvent{ts, id, o.tick, shares_cancelled, o.side, CancelReason::User});
  }
  return Reject::None;
}

Reject OrderBook::execute(Ts ts, OrderId id, Qty shares, OrderId match_number) {
  const Slot s = ids_.find(id);
  if (s == kNullSlot) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::UnknownId});
    return Reject::UnknownId;
  }
  if (shares <= 0) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::BadQty});
    return Reject::BadQty;
  }
  Order& o = pool_[s];
  const Qty take = std::min(shares, o.qty);
  const Tick tick = o.tick;
  const Side maker_side = o.side;
  o.qty -= take;
  levels_[idx(tick)].qty -= take;
  if (sink_) {
    // The aggressor is whoever took this resting order, so the opposite side.
    sink_->on_trade(
        TradeEvent{ts, id, match_number, tick, take, opposite(maker_side), o.qty});
  }
  if (o.qty == 0) {
    Level& lv = levels_[idx(tick)];
    unlink(idx(tick), s);
    if (lv.orders == 0) {
      lv.qty = 0;
      lv.head = kNullSlot;
      lv.tail = kNullSlot;
      bit_clear(idx(tick));
      refresh_best_after_removal(maker_side, idx(tick));
    }
    ids_.erase(id);
    free_slot(s);
  }
  return take == shares ? Reject::None : Reject::BadQty;
}

Reject OrderBook::replace(Ts ts, OrderId orig_id, OrderId new_id, Tick new_tick,
                          Qty new_qty) {
  const Slot s = ids_.find(orig_id);
  if (s == kNullSlot) {
    if (sink_) sink_->on_reject(RejectEvent{ts, orig_id, Reject::UnknownId});
    return Reject::UnknownId;
  }
  if (new_qty <= 0) {
    if (sink_) sink_->on_reject(RejectEvent{ts, new_id, Reject::BadQty});
    return Reject::BadQty;
  }
  if (!in_range(new_tick)) {
    if (sink_) sink_->on_reject(RejectEvent{ts, new_id, Reject::PriceOutOfRange});
    return Reject::PriceOutOfRange;
  }
  if (new_id != orig_id && ids_.find(new_id) != kNullSlot) {
    if (sink_) sink_->on_reject(RejectEvent{ts, new_id, Reject::DuplicateId});
    return Reject::DuplicateId;
  }
  const Side side = pool_[s].side;
  remove_resting(s, CancelReason::Replace, ts);
  return add_limit(ts, new_id, side, new_tick, new_qty, Tif::Gtc);
}

Reject OrderBook::modify(Ts ts, OrderId id, Tick new_tick, Qty new_qty) {
  const Slot s = ids_.find(id);
  if (s == kNullSlot) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::UnknownId});
    return Reject::UnknownId;
  }
  if (new_qty <= 0) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::BadQty});
    return Reject::BadQty;
  }
  if (!in_range(new_tick)) {
    if (sink_) sink_->on_reject(RejectEvent{ts, id, Reject::PriceOutOfRange});
    return Reject::PriceOutOfRange;
  }
  Order& o = pool_[s];
  if (new_tick == o.tick && new_qty <= o.qty) {
    // Size down in place. Queue position is kept, which is the whole reason a
    // venue offers modify instead of cancel and replace.
    const Qty delta = o.qty - new_qty;
    if (delta > 0) {
      o.qty = new_qty;
      levels_[idx(o.tick)].qty -= delta;
      if (sink_) {
        sink_->on_cancel(CancelEvent{ts, id, o.tick, delta, o.side, CancelReason::Replace});
      }
    }
    return Reject::None;
  }
  // Anything else loses priority: pull the order, then submit a fresh one at
  // the tail of the new level.
  const Side side = o.side;
  const Tick old_tick = o.tick;
  const Qty old_qty = o.qty;
  (void)old_tick;
  (void)old_qty;
  remove_resting(s, CancelReason::Replace, ts);
  const Reject r = add_limit(ts, id, side, new_tick, new_qty, Tif::Gtc);
  // Every reason add_limit could reject has already been ruled out above: the
  // price and size were validated before the order was pulled, the id was just
  // erased so it cannot be a duplicate, and removing the order returned a slot
  // to the free list so the pool cannot be exhausted. A modify therefore never
  // turns into a silent cancel.
  assert(r == Reject::None);
  return r;
}

Qty OrderBook::queue_ahead(OrderId id) const noexcept {
  const Slot s = ids_.find(id);
  if (s == kNullSlot) return -1;
  Qty ahead = 0;
  for (Slot c = levels_[idx(pool_[s].tick)].head; c != kNullSlot && c != s;
       c = pool_[c].next) {
    ahead += pool_[c].qty;
  }
  return ahead;
}

Qty OrderBook::order_qty(OrderId id) const noexcept {
  const Slot s = ids_.find(id);
  return s == kNullSlot ? 0 : pool_[s].qty;
}

Tick OrderBook::order_tick(OrderId id) const noexcept {
  const Slot s = ids_.find(id);
  return s == kNullSlot ? kInvalidTick : pool_[s].tick;
}

std::size_t OrderBook::top_levels(Side side, std::size_t depth, LevelView* out) const {
  std::size_t n = 0;
  if (side == Side::Buy) {
    if (!has_bid()) return 0;
    std::size_t i = idx(best_bid_);
    while (n < depth) {
      i = scan_down(i);
      if (i == npos) break;
      out[n++] = LevelView{tick_of(i), levels_[i].qty, levels_[i].orders};
      if (i == 0) break;
      --i;
    }
  } else {
    if (!has_ask()) return 0;
    std::size_t i = idx(best_ask_);
    while (n < depth) {
      i = scan_up(i);
      if (i == npos) break;
      out[n++] = LevelView{tick_of(i), levels_[i].qty, levels_[i].orders};
      ++i;
    }
  }
  return n;
}

Qty OrderBook::total_qty(Side side) const {
  Qty total = 0;
  if (side == Side::Buy) {
    if (!has_bid()) return 0;
    std::size_t i = idx(best_bid_);
    while (true) {
      i = scan_down(i);
      if (i == npos) break;
      total += levels_[i].qty;
      if (i == 0) break;
      --i;
    }
  } else {
    if (!has_ask()) return 0;
    for (std::size_t i = scan_up(idx(best_ask_)); i != npos; i = scan_up(i + 1)) {
      total += levels_[i].qty;
    }
  }
  return total;
}

bool OrderBook::check_invariants() const {
  std::size_t counted = 0;
  for (std::size_t i = 0; i < levels_.size(); ++i) {
    const Level& lv = levels_[i];
    const bool bit = (occupied_[i >> 6] >> (i & 63)) & 1ull;
    if ((lv.orders != 0) != bit) return false;
    if (lv.orders == 0) {
      if (lv.qty != 0 || lv.head != kNullSlot || lv.tail != kNullSlot) return false;
      continue;
    }
    Qty sum = 0;
    std::uint32_t cnt = 0;
    Slot prev = kNullSlot;
    for (Slot c = lv.head; c != kNullSlot; c = pool_[c].next) {
      if (pool_[c].prev != prev) return false;
      if (!pool_[c].live) return false;
      if (static_cast<std::size_t>(idx(pool_[c].tick)) != i) return false;
      if (pool_[c].qty <= 0) return false;
      if (ids_.find(pool_[c].id) != c) return false;
      sum += pool_[c].qty;
      ++cnt;
      prev = c;
    }
    if (lv.tail != prev) return false;
    if (sum != lv.qty || cnt != lv.orders) return false;
    counted += cnt;
  }
  if (counted != ids_.size() || counted != pool_used_) return false;
  if (has_bid() && has_ask() && best_bid_ >= best_ask_) return false;
  if (has_bid() && levels_[idx(best_bid_)].orders == 0) return false;
  if (has_ask() && levels_[idx(best_ask_)].orders == 0) return false;
  return true;
}

}  // namespace ltx
