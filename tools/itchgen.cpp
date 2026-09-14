// Turns the Hyperliquid replay into an ITCH 5.0 style binary feed.
//
// The replay in src/replay/ produces a stream of engine events: an order rested,
// an order gave up size, an order traded, an order went away. Those map onto
// ITCH message types one for one, so this listens to the events and writes the
// wire form.
//
//   order rested                        -> A  Add Order
//   order lost size at the same price    -> X  Order Cancel
//   order repriced or grew               -> U  Order Replace
//   order traded where it sat            -> E  Order Executed
//   order withdrawn                      -> D  Order Delete
//   book reseeded after a feed gap       -> D  for every live reference
//
// The result is a file that build/itchfeed can read back into a fresh engine
// and land on the same book the exchange published, by a completely different
// path from the one src/replay uses.
//
// With --symbols=N the same day is written N times under N symbol codes, with
// disjoint order references, interleaved by timestamp. That is not more market
// data, and it is not presented as any: it is a load and isolation fixture for
// the sharded feed handler, and the fidelity numbers are always quoted on the
// single real symbol.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/engine.hpp"
#include "replay/loader.hpp"
#include "replay/reconstruct.hpp"
#include "util/gzline.hpp"
#include "wire/encoder.hpp"
#include "wire/itch.hpp"

using namespace ltx;
using namespace ltx::wire;

namespace {

struct Args {
  std::string data = "data/raw";
  std::string days, from, to;
  std::string out = "results/eth.itch";
  int symbols = 1;
  int price_decimals = 1;
  std::size_t mtu = 1400;
  bool quiet = false;
};

void usage() {
  std::printf(
      "itchgen - write the Hyperliquid replay as an ITCH 5.0 style binary feed\n"
      "\n"
      "usage: itchgen [options]\n"
      "  --data=DIR         directory holding l2book_ETH/ and trades_ETH/\n"
      "  --days=a,b,c       explicit days, or use --from/--to\n"
      "  --from=YYYY-MM-DD  first day inclusive\n"
      "  --to=YYYY-MM-DD    last day inclusive\n"
      "  --out=FILE         output file, default results/eth.itch\n"
      "  --symbols=N        write the day under N symbol codes, default 1\n"
      "  --mtu=N            target packet size in bytes, default 1400\n"
      "  --quiet\n"
      "  --help\n");
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  std::size_t i = 0;
  while (true) {
    const std::size_t j = s.find(sep, i);
    if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
    out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

// One message before it has been given a symbol. The payload is written with a
// locate of 0 and patched per symbol at output time, which keeps the generator
// from re-encoding N times.
struct Pending {
  std::uint64_t ts_ns;
  std::uint8_t len;
  std::uint8_t body[48];
};

// Mirrors what the engine has resting, so the generator knows each order's
// current wire reference and size without asking the engine.
struct WireOrder {
  std::uint64_t ref;
  Qty qty;
  Tick tick;
  Side side;
};

class ItchEmitter final : public EventSink, public ReplayObserver {
 public:
  ItchEmitter(std::vector<Pending>& out, int price_decimals)
      : out_(out), pdec_(price_decimals) {}

  std::uint64_t counts[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // A X U E D, in order below
  enum { kAdd = 0, kCancel, kReplace, kExecuted, kDelete };

  void on_reseed(Ts ts, const OrderBook&) override {
    // Everything the wire believes is resting has to be retired explicitly,
    // otherwise a reader would carry orders across the gap that the exchange no
    // longer shows. These are stamped with the replacing snapshot's time, not
    // with the last event before the hole. Stamping them with the older time
    // put the teardown and the rebuild on opposite sides of a snapshot
    // boundary, so a reader scoring itself against that snapshot saw a book
    // that had already been emptied. That cost 200 wrong level positions on
    // each of the six days in this dataset that contain a feed gap, and nothing
    // on any other day.
    last_ts_ = static_cast<std::uint64_t>(ts);
    for (const auto& [id, w] : live_) {
      emit_delete(last_ts_, w.ref);
    }
    live_.clear();
    pending_replace_.clear();
  }

  void on_accept(const AcceptEvent& e) override {
    last_ts_ = static_cast<std::uint64_t>(e.ts);
    auto pr = pending_replace_.find(e.id);
    if (pr != pending_replace_.end()) {
      const std::uint64_t old_ref = pr->second;
      pending_replace_.erase(pr);
      const std::uint64_t ref = ++next_ref_;
      emit_replace(last_ts_, old_ref, ref, e.resting_qty, e.tick);
      live_[e.id] = WireOrder{ref, e.resting_qty, e.tick, e.side};
      return;
    }
    const std::uint64_t ref = ++next_ref_;
    emit_add(last_ts_, ref, e.side, e.resting_qty, e.tick);
    live_[e.id] = WireOrder{ref, e.resting_qty, e.tick, e.side};
  }

  void on_cancel(const CancelEvent& e) override {
    last_ts_ = static_cast<std::uint64_t>(e.ts);
    if (e.reason == CancelReason::Ioc || e.reason == CancelReason::Fok) {
      return;   // never rested, so it was never on the wire
    }
    auto it = live_.find(e.id);
    if (it == live_.end()) return;
    if (e.reason == CancelReason::Replace && e.cancelled_qty < it->second.qty) {
      // Size came down where it stood.
      emit_cancel(last_ts_, it->second.ref, e.cancelled_qty);
      it->second.qty -= e.cancelled_qty;
      return;
    }
    if (e.reason == CancelReason::Replace) {
      // The removal half of a reprice or a size increase. Hold the old
      // reference until the matching accept arrives and send one U.
      pending_replace_[e.id] = it->second.ref;
      live_.erase(it);
      return;
    }
    emit_delete(last_ts_, it->second.ref);
    live_.erase(it);
  }

  void on_trade(const TradeEvent& e) override {
    last_ts_ = static_cast<std::uint64_t>(e.ts);
    auto it = live_.find(e.maker_id);
    if (it == live_.end()) return;
    emit_executed(last_ts_, it->second.ref, e.qty, ++match_);
    if (e.maker_remaining == 0) {
      live_.erase(it);
    } else {
      it->second.qty = e.maker_remaining;
    }
  }

  std::size_t live_orders() const { return live_.size(); }

 private:
  void push(std::uint64_t ts, std::size_t len, const std::uint8_t* body) {
    Pending p;
    p.ts_ns = ts;
    p.len = static_cast<std::uint8_t>(len);
    std::memcpy(p.body, body, len);
    out_.push_back(p);
  }

  void emit_add(std::uint64_t ts, std::uint64_t ref, Side side, Qty qty, Tick tick) {
    std::uint8_t b[48];
    const std::size_t n =
        encode_add(b, 0, 0, ts, ref, side == Side::Buy ? 'B' : 'S', qty_to_wire(qty),
                   "SYMBOL  ", tick_to_wire_price(tick, pdec_));
    push(ts, n, b);
    ++counts[kAdd];
  }
  void emit_cancel(std::uint64_t ts, std::uint64_t ref, Qty qty) {
    std::uint8_t b[48];
    const std::size_t n = encode_cancel(b, 0, 0, ts, ref, qty_to_wire(qty));
    push(ts, n, b);
    ++counts[kCancel];
  }
  void emit_replace(std::uint64_t ts, std::uint64_t old_ref, std::uint64_t ref, Qty qty,
                    Tick tick) {
    std::uint8_t b[48];
    const std::size_t n = encode_replace(b, 0, 0, ts, old_ref, ref, qty_to_wire(qty),
                                         tick_to_wire_price(tick, pdec_));
    push(ts, n, b);
    ++counts[kReplace];
  }
  void emit_executed(std::uint64_t ts, std::uint64_t ref, Qty qty, std::uint64_t match) {
    std::uint8_t b[48];
    const std::size_t n = encode_executed(b, 0, 0, ts, ref, qty_to_wire(qty), match);
    push(ts, n, b);
    ++counts[kExecuted];
  }
  void emit_delete(std::uint64_t ts, std::uint64_t ref) {
    std::uint8_t b[48];
    const std::size_t n = encode_delete(b, 0, 0, ts, ref);
    push(ts, n, b);
    ++counts[kDelete];
  }

  std::vector<Pending>& out_;
  int pdec_;
  std::unordered_map<OrderId, WireOrder> live_;
  std::unordered_map<OrderId, std::uint64_t> pending_replace_;
  std::uint64_t next_ref_ = 0;
  std::uint64_t match_ = 0;
  std::uint64_t last_ts_ = 0;
};

// Patches a message body with its symbol locate, symbol text and a per symbol
// order reference, then writes it.
void write_for_symbol(PacketWriter& w, const Pending& p, std::uint16_t locate,
                      const char* symbol, std::uint64_t session_epoch_ns) {
  std::uint8_t b[48];
  std::memcpy(b, p.body, p.len);
  store_be<std::uint16_t>(b + 1, locate);
  // ITCH timestamps are 48 bits counted from the start of the session. The spec
  // says midnight, because a NASDAQ session sits inside one calendar day. This
  // collector partitions files by the time it received a message, not by the
  // venue's own clock, so the first few records of a file can carry a venue
  // timestamp from a second or two before midnight. Counting from midnight
  // would make the field wrap in the middle of a session and put the whole day
  // out of order. The session therefore starts at midnight of the day its first
  // message falls in, which for those files is the previous midnight. The field
  // still counts nanoseconds and still fits: two days is 1.73e14, against a 48
  // bit ceiling of 2.81e14. Both the generator and the reader derive the same
  // origin the same way, from the first snapshot of the day.
  store_u48(b + 5, p.ts_ns - session_epoch_ns);
  // Order references are made disjoint per symbol by putting the symbol index
  // in the top byte. The mapping is injective, so nothing collides.
  const std::uint64_t tag = static_cast<std::uint64_t>(locate) << 56;
  const char type = static_cast<char>(b[0]);
  if (type == 'A' || type == 'F') {
    store_be<std::uint64_t>(b + 11, load_be<std::uint64_t>(b + 11) | tag);
    std::memcpy(b + 24, symbol, kSymbolLen);
  } else if (type == 'U') {
    store_be<std::uint64_t>(b + 11, load_be<std::uint64_t>(b + 11) | tag);
    store_be<std::uint64_t>(b + 19, load_be<std::uint64_t>(b + 19) | tag);
  } else if (type == 'E' || type == 'C' || type == 'X' || type == 'D') {
    store_be<std::uint64_t>(b + 11, load_be<std::uint64_t>(b + 11) | tag);
  }
  w.write(b, p.len);
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto str = [&](const char* k, std::string& o) {
      if (s.rfind(k, 0) == 0) { o = s.substr(std::strlen(k)); return true; }
      return false;
    };
    if (s == "--help" || s == "-h") { usage(); return 0; }
    else if (str("--data=", a.data)) {}
    else if (str("--days=", a.days)) {}
    else if (str("--from=", a.from)) {}
    else if (str("--to=", a.to)) {}
    else if (str("--out=", a.out)) {}
    else if (s.rfind("--symbols=", 0) == 0) a.symbols = std::atoi(s.c_str() + 10);
    else if (s.rfind("--mtu=", 0) == 0) a.mtu = static_cast<std::size_t>(std::atoi(s.c_str() + 6));
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
  }
  if (a.symbols < 1 || a.symbols > 255) {
    std::fprintf(stderr, "--symbols must be between 1 and 255\n");
    return 1;
  }

  const std::string book_root = a.data + "/l2book_ETH";
  const std::string trade_root = a.data + "/trades_ETH";
  std::vector<std::string> days = a.days.empty() ? list_dates(book_root) : split(a.days, ',');
  if (a.days.empty()) {
    if (!a.from.empty())
      days.erase(std::remove_if(days.begin(), days.end(),
                                [&](const std::string& d) { return d < a.from; }),
                 days.end());
    if (!a.to.empty())
      days.erase(std::remove_if(days.begin(), days.end(),
                                [&](const std::string& d) { return d > a.to; }),
                 days.end());
  }
  if (days.empty()) { std::fprintf(stderr, "no days found\n"); return 1; }

  std::FILE* out = std::fopen(a.out.c_str(), "wb");
  if (!out) { std::fprintf(stderr, "cannot write %s\n", a.out.c_str()); return 1; }
  PacketWriter w(out, "LTXSESS01", a.mtu);

  // Symbol directory first, so a reader knows the scales before any order.
  std::vector<std::string> symbols;
  for (int i = 0; i < a.symbols; ++i) {
    // Fixed 8 byte field, space padded, as on the wire.
    char buf[kSymbolLen];
    std::memset(buf, ' ', kSymbolLen);
    if (a.symbols == 1) {
      std::memcpy(buf, "ETH", 3);
    } else {
      char tmp[16];
      const int n = std::snprintf(tmp, sizeof(tmp), "ETH%d", i);
      std::memcpy(buf, tmp, static_cast<std::size_t>(n) < kSymbolLen
                                ? static_cast<std::size_t>(n)
                                : kSymbolLen);
    }
    symbols.emplace_back(buf, kSymbolLen);
    std::uint8_t b[48];
    const std::size_t n = encode_symbol_directory(
        b, static_cast<std::uint16_t>(i + 1), 0, symbols.back().c_str(),
        static_cast<std::uint8_t>(a.price_decimals), 4);
    w.write(b, n);
  }
  {
    std::uint8_t b[48];
    w.write(b, encode_system_event(b, 0, static_cast<char>(SystemEventCode::StartOfMessages)));
  }

  BookConfig cfg;
  cfg.min_tick = 1;
  cfg.max_tick = 262144;
  cfg.max_orders = 1u << 16;
  cfg.id_map_capacity = 1u << 17;

  std::uint64_t total_msgs = 0, total_windows = 0;
  std::uint64_t kinds[5] = {0, 0, 0, 0, 0};

  for (const std::string& day : days) {
    LoadStats bs{}, ts{};
    std::vector<Snapshot> snaps = load_snapshots(book_root, day, a.price_decimals, bs);
    std::vector<RawTrade> trades = load_trades(trade_root, day, a.price_decimals, ts);
    if (snaps.size() < 2) continue;

    {
      // One session per day, as on a real feed.
      std::uint8_t b[48];
      w.write(b, encode_system_event(b, 0,
                                     static_cast<char>(SystemEventCode::StartOfSystemHours)));
    }
    std::vector<Pending> pend;
    pend.reserve(1u << 20);
    ItchEmitter em(pend, a.price_decimals);
    MatchingEngine eng(cfg, &em);
    Reconstructor rec(eng);
    rec.set_observer(&em);
    const ReplayStats rs = rec.run(snaps, trades);
    if (rs.total.recon_level_mismatch != 0) {
      std::fprintf(stderr, "%s: replay fidelity broke, refusing to write a feed\n",
                   day.c_str());
      return 2;
    }
    total_windows += rs.windows;
    for (int i = 0; i < 5; ++i) kinds[i] += em.counts[i];

    // Stable sort keeps messages for one symbol in the order the engine
    // produced them, which is what an exchange guarantees per instrument.
    const std::uint64_t session_epoch_ns =
        static_cast<std::uint64_t>(snaps.front().time_ms / 86400000) * 86400000ull * 1000000ull;
    if (a.symbols > 1) {
      for (std::size_t i = 0; i < pend.size(); ++i) {
        for (int sidx = 0; sidx < a.symbols; ++sidx) {
          write_for_symbol(w, pend[i], static_cast<std::uint16_t>(sidx + 1),
                           symbols[sidx].c_str(), session_epoch_ns);
        }
      }
    } else {
      for (const Pending& p : pend) {
        write_for_symbol(w, p, 1, symbols[0].c_str(), session_epoch_ns);
      }
    }
    total_msgs += pend.size() * static_cast<std::uint64_t>(a.symbols);
    if (!a.quiet) {
      std::printf("%-12s %8zu snapshots  %8zu trades  %9zu messages\n", day.c_str(),
                  snaps.size(), trades.size(), pend.size());
    }
  }

  {
    std::uint8_t b[48];
    w.write(b, encode_system_event(b, 0, static_cast<char>(SystemEventCode::EndOfMessages)));
  }
  w.end_of_session();
  std::fclose(out);

  std::printf("\n%s\n", a.out.c_str());
  std::printf("  symbols            %d\n", a.symbols);
  std::printf("  days               %zu, %llu windows\n", days.size(),
              static_cast<unsigned long long>(total_windows));
  std::printf("  messages           %llu (%llu add, %llu cancel, %llu replace,"
              " %llu executed, %llu delete, before the symbol fan out)\n",
              static_cast<unsigned long long>(w.messages()),
              static_cast<unsigned long long>(kinds[0]),
              static_cast<unsigned long long>(kinds[1]),
              static_cast<unsigned long long>(kinds[2]),
              static_cast<unsigned long long>(kinds[3]),
              static_cast<unsigned long long>(kinds[4]));
  std::printf("  packets            %llu\n", static_cast<unsigned long long>(w.packets()));
  std::printf("  bytes              %llu (%.1f MB, %.1f bytes per message)\n",
              static_cast<unsigned long long>(w.bytes()), w.bytes() / 1048576.0,
              w.messages() ? static_cast<double>(w.bytes()) / w.messages() : 0.0);
  (void)total_msgs;
  return 0;
}
