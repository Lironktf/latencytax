// Readers for the two raw Hyperliquid files.
//
// l2book record: {"rx": <ms>, "book": {"coin","time": <ms>,
//                 "levels": [[{"px","sz","n"} x20],[... x20]]}}
// trades record: {"rx": <ms>, "trade": {"coin","side","px","sz","time": <ms>,
//                 "hash","tid","users"}}
//
// Both timestamps are kept. `time` is the venue's own clock, `rx` is when the
// collector received the message. Everything downstream orders on `time` and
// uses `rx` only to measure collection lag.
//
// The parsing is a hand written scan rather than a general JSON library: the
// schema is fixed, and prices and sizes have to land in fixed point integers
// without passing through a double.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/types.hpp"

namespace ltx {

inline constexpr std::size_t kSnapDepth = 20;

struct RawLevel {
  Tick tick = 0;
  Qty qty = 0;
  std::uint32_t orders = 0;
};

struct Snapshot {
  std::int64_t rx_ms = 0;
  std::int64_t time_ms = 0;
  std::uint8_t n_bids = 0;
  std::uint8_t n_asks = 0;
  std::array<RawLevel, kSnapDepth> bids{};  // descending price
  std::array<RawLevel, kSnapDepth> asks{};  // ascending price
};

struct RawTrade {
  std::int64_t rx_ms = 0;
  std::int64_t time_ms = 0;
  std::uint64_t tid = 0;
  Tick tick = 0;
  Qty qty = 0;
  // Side of the aggressor. The raw feed writes "A" or "B". Measured against the
  // prevailing snapshot best bid and offer over all 39 days, 89.14% of "A"
  // prints land at or below the bid and 89.72% of "B" prints at or above the
  // ask, so "A" is a seller hitting the bid and "B" is a buyer lifting the
  // offer. The residual is the five second snapshot going stale, not ambiguity
  // about the convention. The check is scripts/tape_structure.py.
  Side aggressor = Side::Buy;
};

struct LoadStats {
  std::size_t lines = 0;
  std::size_t parsed = 0;
  std::size_t parse_errors = 0;
  std::size_t out_of_order = 0;
  std::size_t duplicate_tid = 0;
};

// price_decimals scales the px string into ticks (1 for a 0.1 tick).
bool parse_snapshot(std::string_view line, int price_decimals, Snapshot& out);
bool parse_trade(std::string_view line, int price_decimals, RawTrade& out);

// Loads every *.jsonl.gz under <root>/date=<day>/, sorted by file then line.
std::vector<Snapshot> load_snapshots(const std::string& root, const std::string& day,
                                     int price_decimals, LoadStats& stats);
std::vector<RawTrade> load_trades(const std::string& root, const std::string& day,
                                  int price_decimals, LoadStats& stats);

// "1916.3" with decimals=1 -> 19163. Exact; never touches a double.
bool parse_fixed(std::string_view s, int decimals, std::int64_t& out);

}  // namespace ltx
