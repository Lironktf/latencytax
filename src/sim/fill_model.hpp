// Queue position model for a resting quote the market never saw.
//
// The agent's order is not in the reconstructed book. Whether it would have
// filled has to be inferred from what did happen at its price, which means
// tracking how much of the queue in front of it went away.
//
// Two things remove the queue ahead of a resting order.
//
//   Trades. A print at the order's own price consumes the queue in front of it
//   first, because that is what price-time priority means. A print that trades
//   through the order's price means everything at that price, including the
//   order, had to go first, so the order fills completely.
//
//   Cancellations. Between two snapshots a level can shrink by more than the
//   volume that traded at it. The difference was cancelled. Which part of the
//   queue those cancellations came from is not observable, so the model assumes
//   they are spread uniformly along the queue: a level that loses a fifth of its
//   size to cancellations takes a fifth off the quantity ahead of any given
//   order. kappa scales that assumption, from 0 (every cancellation came from
//   behind the order, the pessimistic case) to 1 (uniform).
//
// Additions go to the back of the queue and never reduce queue_ahead.
#pragma once

#include <algorithm>

#include "engine/types.hpp"

namespace ltx {

struct RestingQuote {
  bool live = false;
  Side side = Side::Buy;
  Tick tick = 0;
  Qty size = 0;          // original
  Qty remaining = 0;
  Qty queue_ahead = 0;   // quantity still in front at this price
  Qty level_at_join = 0; // level size when the order went live
  std::int64_t join_us = 0;
  // Volume that has printed at this exact price since the last snapshot, used
  // to separate trading from cancelling when the next snapshot arrives.
  Qty traded_at_level_this_window = 0;
  Qty level_at_last_snapshot = 0;
};

struct FillResult {
  Qty qty = 0;
  bool swept = false;  // filled because the tape traded through the price
};

// What to do when the tape prints at a better price than the resting order.
//
//   Through   Price-time priority says everything at the order's price had to
//             trade before anything beyond it, so the order is filled. This is
//             the usual assumption in queue position backtests. It is right
//             when the queue estimate is right, and it over-fills when the
//             5 second snapshot has left the estimate stale.
//   Queue     Treat the through print as if it had happened at the order's own
//             price: it eats the modelled queue first and only then fills. This
//             never fills ahead of the model, and under-fills when the model is
//             carrying queue that is no longer there.
//
// Neither is verifiable from an L2 feed. Results are reported under both.
enum class SweepRule { Through, Queue };

// Applies one print to one resting quote. `print_tick` and `print_qty` are the
// tape's, `aggressor` is the side that took liquidity.
inline FillResult apply_print(RestingQuote& q, Side aggressor, Tick print_tick, Qty print_qty,
                              SweepRule rule = SweepRule::Through) {
  FillResult r;
  if (!q.live || q.remaining <= 0) return r;
  // A print only touches a resting order on the opposite side of the aggressor.
  if (aggressor == q.side) return r;

  const bool through = (q.side == Side::Buy) ? (print_tick < q.tick) : (print_tick > q.tick);
  const bool at = (print_tick == q.tick);
  if (!through && !at) return r;

  if (through && rule == SweepRule::Through) {
    r.qty = q.remaining;
    r.swept = true;
    q.remaining = 0;
    q.queue_ahead = 0;
    return r;
  }

  if (!through) q.traded_at_level_this_window += print_qty;
  Qty v = print_qty;
  const Qty eat = std::min(v, q.queue_ahead);
  q.queue_ahead -= eat;
  v -= eat;
  if (v > 0) {
    r.qty = std::min(v, q.remaining);
    r.swept = through;
    q.remaining -= r.qty;
  }
  return r;
}

// Called once per snapshot with the level's size in the new snapshot. Moves
// queue_ahead down by the share of the level's unexplained shrinkage that the
// model attributes to orders in front.
inline void apply_snapshot_decay(RestingQuote& q, Qty level_size_now, double kappa) {
  if (!q.live || q.remaining <= 0) return;
  const Qty prev = q.level_at_last_snapshot;
  if (prev > 0 && q.queue_ahead > 0) {
    const Qty shrink = prev - q.traded_at_level_this_window - level_size_now;
    if (shrink > 0) {
      const double frac = static_cast<double>(q.queue_ahead) / static_cast<double>(prev);
      const double removed = kappa * frac * static_cast<double>(shrink);
      const Qty d = static_cast<Qty>(removed);
      q.queue_ahead = q.queue_ahead > d ? q.queue_ahead - d : 0;
    }
  }
  q.level_at_last_snapshot = level_size_now;
  q.traded_at_level_this_window = 0;
}

}  // namespace ltx
