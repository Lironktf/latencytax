// Core value types for the matching engine.
//
// Prices and quantities are fixed point integers. Nothing in the engine ever
// touches a float: comparisons at a price level must be exact, and 0.1 is not
// representable in binary floating point.
#pragma once

#include <cstdint>
#include <limits>

namespace ltx {

// Price in ticks. For Hyperliquid ETH-perp the tick is 0.1 USD, so a price of
// 1916.3 is tick 19163. The engine itself is tick agnostic; the scale lives in
// InstrumentSpec.
using Tick = std::int32_t;

// Quantity in integer units of 1e-8 of the base asset. Hyperliquid publishes
// ETH sizes with at most 4 decimals, so 1e-8 leaves four spare digits and the
// largest plausible level (1e5 ETH) is 1e13, far inside int64.
using Qty = std::int64_t;

using OrderId = std::uint64_t;
using SeqNum = std::uint64_t;
// Nanoseconds since the Unix epoch.
using Ts = std::int64_t;

inline constexpr Qty kQtyScale = 100000000;   // 1e8
inline constexpr Tick kInvalidTick = std::numeric_limits<Tick>::min();
inline constexpr OrderId kNoOrder = 0;
// Slot index into the order pool. 32 bit keeps the intrusive list links small.
using Slot = std::uint32_t;
inline constexpr Slot kNullSlot = 0xFFFFFFFFu;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Time in force.
enum class Tif : std::uint8_t {
  Gtc,  // rest whatever does not trade
  Ioc,  // trade what you can, cancel the rest
  Fok,  // all or nothing
};

// Why an order left the book.
enum class CancelReason : std::uint8_t { User, Ioc, Fok, Replace };

// Reject codes. The engine never throws on a bad client message.
enum class Reject : std::uint8_t {
  None = 0,
  DuplicateId,
  UnknownId,
  PriceOutOfRange,
  BadQty,
  FokUnfillable,
  PoolExhausted,
};

struct InstrumentSpec {
  // price_ticks = round(price / tick_size); tick_size is 10^-price_decimals.
  std::int32_t price_decimals = 1;
  std::int32_t size_decimals = 4;
  Tick min_tick = 1;
  Tick max_tick = 1000000;  // 100k USD at a 0.1 tick
};

}  // namespace ltx
