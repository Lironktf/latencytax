#include "replay/loader.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "util/gzline.hpp"

namespace ltx {
namespace {

// A cursor over one JSON line. Only the shapes these two files actually use
// are handled; anything else makes the line fail to parse and get counted.
struct Scan {
  const char* p;
  const char* e;

  bool done() const { return p >= e; }
  void ws() { while (p < e && (*p == ' ' || *p == '\t')) ++p; }
  bool eat(char c) { ws(); if (p < e && *p == c) { ++p; return true; } return false; }

  // Moves just past the colon following the next occurrence of "key".
  bool seek_key(std::string_view key) {
    const std::size_t n = key.size();
    while (p + n + 2 <= e) {
      const void* q = std::memchr(p, '"', static_cast<std::size_t>(e - p));
      if (!q) return false;
      p = static_cast<const char*>(q);
      if (static_cast<std::size_t>(e - p) >= n + 2 && p[n + 1] == '"' &&
          std::memcmp(p + 1, key.data(), n) == 0) {
        p += n + 2;
        ws();
        if (p < e && *p == ':') { ++p; ws(); return true; }
        continue;
      }
      ++p;
    }
    return false;
  }

  bool uint64(std::uint64_t& out) {
    ws();
    if (p >= e || *p < '0' || *p > '9') return false;
    std::uint64_t v = 0;
    while (p < e && *p >= '0' && *p <= '9') v = v * 10 + static_cast<std::uint64_t>(*p++ - '0');
    out = v;
    return true;
  }

  bool str(std::string_view& out) {
    ws();
    if (p >= e || *p != '"') return false;
    const char* s = ++p;
    while (p < e && *p != '"') ++p;
    if (p >= e) return false;
    out = std::string_view(s, static_cast<std::size_t>(p - s));
    ++p;
    return true;
  }
};

}  // namespace

bool parse_fixed(std::string_view s, int decimals, std::int64_t& out) {
  if (s.empty()) return false;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '-') { neg = true; i = 1; }
  else if (s[0] == '+') { i = 1; }
  std::int64_t whole = 0;
  bool any = false;
  for (; i < s.size() && s[i] != '.'; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    whole = whole * 10 + (s[i] - '0');
    any = true;
  }
  std::int64_t scale = 1;
  for (int d = 0; d < decimals; ++d) scale *= 10;
  std::int64_t frac = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    int used = 0;
    for (; i < s.size(); ++i) {
      if (s[i] < '0' || s[i] > '9') return false;
      any = true;
      if (used < decimals) { frac = frac * 10 + (s[i] - '0'); ++used; }
      // Digits past the scale are dropped. The raw data never has any: prices
      // carry one decimal and sizes at most four, against a 1e8 size scale.
    }
    while (used < decimals) { frac *= 10; ++used; }
  }
  if (!any) return false;
  const std::int64_t v = whole * scale + frac;
  out = neg ? -v : v;
  return true;
}

namespace {

bool parse_level_array(Scan& sc, int price_decimals, std::array<RawLevel, kSnapDepth>& out,
                       std::uint8_t& count) {
  count = 0;
  if (!sc.eat('[')) return false;
  sc.ws();
  if (sc.eat(']')) return true;
  while (true) {
    if (!sc.eat('{')) return false;
    std::string_view px, sz;
    std::uint64_t n = 0;
    if (!sc.seek_key("px") || !sc.str(px)) return false;
    if (!sc.seek_key("sz") || !sc.str(sz)) return false;
    if (!sc.seek_key("n") || !sc.uint64(n)) return false;
    std::int64_t t = 0, q = 0;
    if (!parse_fixed(px, price_decimals, t)) return false;
    if (!parse_fixed(sz, 8, q)) return false;
    if (count < kSnapDepth) {
      out[count] = RawLevel{static_cast<Tick>(t), q, static_cast<std::uint32_t>(n)};
      ++count;
    }
    // Past the object.
    while (!sc.done() && *sc.p != '}') ++sc.p;
    if (!sc.eat('}')) return false;
    sc.ws();
    if (sc.eat(',')) continue;
    if (sc.eat(']')) return true;
    return false;
  }
}

}  // namespace

bool parse_snapshot(std::string_view line, int price_decimals, Snapshot& out) {
  Scan sc{line.data(), line.data() + line.size()};
  std::uint64_t rx = 0;
  if (!sc.seek_key("rx") || !sc.uint64(rx)) return false;
  out.rx_ms = static_cast<std::int64_t>(rx);
  if (!sc.seek_key("time")) return false;
  std::uint64_t tm = 0;
  if (!sc.uint64(tm)) return false;
  out.time_ms = static_cast<std::int64_t>(tm);
  if (!sc.seek_key("levels")) return false;
  if (!sc.eat('[')) return false;
  if (!parse_level_array(sc, price_decimals, out.bids, out.n_bids)) return false;
  if (!sc.eat(',')) return false;
  if (!parse_level_array(sc, price_decimals, out.asks, out.n_asks)) return false;
  return true;
}

bool parse_trade(std::string_view line, int price_decimals, RawTrade& out) {
  Scan sc{line.data(), line.data() + line.size()};
  std::uint64_t rx = 0;
  if (!sc.seek_key("rx") || !sc.uint64(rx)) return false;
  out.rx_ms = static_cast<std::int64_t>(rx);
  std::string_view side;
  if (!sc.seek_key("side") || !sc.str(side)) return false;
  // "A": the aggressor sold into the bid. "B": the aggressor bought the offer.
  out.aggressor = (side == "A") ? Side::Sell : Side::Buy;
  std::string_view px, sz;
  if (!sc.seek_key("px") || !sc.str(px)) return false;
  if (!sc.seek_key("sz") || !sc.str(sz)) return false;
  std::int64_t t = 0, q = 0;
  if (!parse_fixed(px, price_decimals, t)) return false;
  if (!parse_fixed(sz, 8, q)) return false;
  out.tick = static_cast<Tick>(t);
  out.qty = q;
  std::uint64_t tm = 0;
  if (!sc.seek_key("time") || !sc.uint64(tm)) return false;
  out.time_ms = static_cast<std::int64_t>(tm);
  std::uint64_t tid = 0;
  if (!sc.seek_key("tid") || !sc.uint64(tid)) return false;
  out.tid = tid;
  return true;
}

bool parse_trade_users(std::string_view line, TradeUsers& out) {
  Scan sc{line.data(), line.data() + line.size()};
  if (!sc.seek_key("users")) return false;
  if (!sc.eat('[')) return false;
  std::string_view a, b;
  if (!sc.str(a)) return false;
  if (!sc.eat(',')) return false;
  if (!sc.str(b)) return false;
  if (a.size() != kAddrLen || b.size() != kAddrLen) return false;
  std::memcpy(out.buyer, a.data(), kAddrLen);
  out.buyer[kAddrLen] = '\0';
  std::memcpy(out.seller, b.data(), kAddrLen);
  out.seller[kAddrLen] = '\0';
  return true;
}

std::vector<Snapshot> load_snapshots(const std::string& root, const std::string& day,
                                     int price_decimals, LoadStats& stats) {
  std::vector<Snapshot> out;
  out.reserve(17400);
  for (const std::string& f : list_gz_files(root + "/date=" + day)) {
    GzLineReader r(f);
    if (!r.ok()) continue;
    std::string_view line;
    while (r.next(line)) {
      if (line.empty()) continue;
      ++stats.lines;
      Snapshot s;
      if (!parse_snapshot(line, price_decimals, s)) { ++stats.parse_errors; continue; }
      ++stats.parsed;
      if (!out.empty() && s.time_ms < out.back().time_ms) ++stats.out_of_order;
      out.push_back(s);
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Snapshot& a, const Snapshot& b) { return a.time_ms < b.time_ms; });
  return out;
}

std::vector<RawTrade> load_trades(const std::string& root, const std::string& day,
                                  int price_decimals, LoadStats& stats) {
  std::vector<RawTrade> out;
  out.reserve(40000);
  std::unordered_set<std::uint64_t> seen;
  for (const std::string& f : list_gz_files(root + "/date=" + day)) {
    GzLineReader r(f);
    if (!r.ok()) continue;
    std::string_view line;
    while (r.next(line)) {
      if (line.empty()) continue;
      ++stats.lines;
      RawTrade t;
      if (!parse_trade(line, price_decimals, t)) { ++stats.parse_errors; continue; }
      if (!seen.insert(t.tid).second) { ++stats.duplicate_tid; continue; }
      ++stats.parsed;
      if (!out.empty() && t.time_ms < out.back().time_ms) ++stats.out_of_order;
      out.push_back(t);
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const RawTrade& a, const RawTrade& b) { return a.time_ms < b.time_ms; });
  return out;
}

}  // namespace ltx
