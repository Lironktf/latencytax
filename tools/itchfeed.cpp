// Reads an ITCH 5.0 style binary feed back into the engine.
//
// Three things it does.
//
//   --check    Applies the feed to a fresh book and compares against the
//              exchange's own snapshots at every five second boundary. This is
//              the same fidelity claim as build/replay, reached by a completely
//              different path: replay hands the engine C++ structs, this parses
//              big endian bytes out of framed packets. Both have to be exact.
//
//   default    Decodes and applies single threaded, reporting decode rate,
//              apply rate and bytes per second.
//
//   --shards=N Routes by the symbol index in each message header to N shards,
//              each a thread with its own queue and its own set of books, and
//              prints a digest per symbol. The digests must not depend on N.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/event_hash.hpp"
#include "engine/multi_book.hpp"
#include "replay/loader.hpp"
#include "util/affinity.hpp"
#include "util/hugevec.hpp"
#include "util/gzline.hpp"
#include "util/timing.hpp"
#include "wire/decoder.hpp"
#include "wire/journal.hpp"
#include "wire/sequencer.hpp"

using namespace ltx;
using namespace ltx::wire;

namespace {

struct Args {
  std::string file;
  std::string data = "data/raw";
  std::string days;
  bool check = false;
  int shards = 0;
  int core = -1;
  bool digest = false;
  bool quiet = false;
  std::string journal;        // write every packet here
  std::string from_journal;   // read packets from here instead of the file
  bool event_hash = false;
  int hugepages = 1;
};

void usage() {
  std::printf(
      "itchfeed - read an ITCH 5.0 style feed back into the engine\n"
      "\n"
      "usage: itchfeed [options] FILE.itch\n"
      "  --check            compare the resulting book against the exchange snapshots\n"
      "  --data=DIR         raw data directory, needed by --check\n"
      "  --days=a,b,c       days the file covers, in order, needed by --check\n"
      "  --shards=N         route to N threads by symbol and report per symbol digests\n"
      "  --core=N           pin the single threaded path to this cpu\n"
      "  --digest           print a digest per symbol\n"
      "  --journal=FILE     record every packet, in order, for later replay\n"
      "  --from-journal=F   replay packets from a journal instead of a feed file\n"
      "  --event-hash       print a rolling hash of every event the engine emits\n"
      "  --hugepages=0|1    ask for 2 MB pages for the book arrays, default 1\n"
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

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::vector<std::uint8_t> buf;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return buf;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    buf.resize(static_cast<std::size_t>(n));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) buf.clear();
  }
  std::fclose(f);
  return buf;
}

double now_s() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration<double>(clock::now() - t0).count();
}

BookConfig feed_cfg() {
  BookConfig c;
  c.min_tick = 1;
  c.max_tick = 262144;
  c.max_orders = 1u << 16;
  c.id_map_capacity = 1u << 17;
  return c;
}

// Compares one book against one snapshot. Returns the number of level positions
// that differ, counting a missing or extra level as one.
std::size_t compare(const OrderBook& b, const Snapshot& s) {
  LevelView lv[kSnapDepth];
  std::size_t bad = 0;
  const Side sides[2] = {Side::Buy, Side::Sell};
  const RawLevel* tgt[2] = {s.bids.data(), s.asks.data()};
  const std::size_t tn[2] = {s.n_bids, s.n_asks};
  for (int side = 0; side < 2; ++side) {
    const std::size_t got = b.top_levels(sides[side], kSnapDepth, lv);
    const std::size_t n = std::max(got, tn[side]);
    for (std::size_t k = 0; k < n; ++k) {
      if (k >= got || k >= tn[side]) { ++bad; continue; }
      if (lv[k].tick != tgt[side][k].tick || lv[k].qty != tgt[side][k].qty) ++bad;
    }
  }
  return bad;
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
    else if (s == "--check") a.check = true;
    else if (s == "--digest") a.digest = true;
    else if (str("--journal=", a.journal)) {}
    else if (str("--from-journal=", a.from_journal)) {}
    else if (s == "--event-hash") a.event_hash = true;
    else if (s.rfind("--hugepages=", 0) == 0) a.hugepages = std::atoi(s.c_str() + 12);
    else if (s == "--quiet") a.quiet = true;
    else if (s.rfind("--shards=", 0) == 0) a.shards = std::atoi(s.c_str() + 9);
    else if (s.rfind("--core=", 0) == 0) a.core = std::atoi(s.c_str() + 7);
    else if (!s.empty() && s[0] != '-') a.file = s;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
  }
  if (a.file.empty() && a.from_journal.empty()) { usage(); return 1; }
  huge_pages_enabled() = a.hugepages != 0;

  // A journal replays as a stream of packets with their original framing, so
  // everything downstream cannot tell which it was handed. That is the point:
  // the hashes have to match.
  std::vector<std::uint8_t> raw;
  if (!a.from_journal.empty()) {
    JournalReader jr;
    if (!jr.load(a.from_journal)) {
      std::fprintf(stderr, "cannot read journal %s\n", a.from_journal.c_str());
      return 1;
    }
    JournalRecord rec{};
    std::uint64_t n = 0;
    while (jr.next(rec)) {
      std::uint8_t len[4];
      store_be<std::uint32_t>(len, rec.len);
      raw.insert(raw.end(), len, len + 4);
      raw.insert(raw.end(), rec.data, rec.data + rec.len);
      ++n;
    }
    if (!a.quiet) {
      std::printf("journal              %s, %llu packets, %.1f MB\n", a.from_journal.c_str(),
                  static_cast<unsigned long long>(n), jr.size() / 1048576.0);
    }
    a.file = a.from_journal;
  } else {
    raw = read_file(a.file);
  }
  if (raw.empty()) { std::fprintf(stderr, "cannot read %s\n", a.file.c_str()); return 1; }
  if (a.core >= 0) pin_to_core(a.core);

  // --- decode only, to time the parser on its own ----------------------------
  Decoder probe;
  {
    std::vector<Routed> scratch;
    scratch.reserve(1 << 12);
    const double t0 = now_s();
    std::size_t off = 0;
    while (off + 4 <= raw.size()) {
      const std::uint32_t plen = load_be<std::uint32_t>(raw.data() + off);
      off += 4;
      if (plen == 0 || off + plen > raw.size()) break;
      scratch.clear();
      probe.decode_packet(raw.data() + off, plen, scratch);
      off += plen;
    }
    const double t = now_s() - t0;
    const DecodeStats& ds = probe.stats();
    if (!a.quiet) {
      std::printf("file                 %s (%.1f MB)\n", a.file.c_str(),
                  raw.size() / 1048576.0);
      std::printf("packets              %llu\n", static_cast<unsigned long long>(ds.packets));
      std::printf("messages             %llu -> %llu engine commands\n",
                  static_cast<unsigned long long>(ds.messages),
                  static_cast<unsigned long long>(ds.commands));
      std::printf("symbols              %zu\n",
                  probe.specs().size() ? probe.specs().size() - 1 : 0);
      std::printf("sequence gaps        %llu (%llu messages missed)\n",
                  static_cast<unsigned long long>(ds.sequence_gaps),
                  static_cast<unsigned long long>(ds.gap_messages));
      std::printf("malformed            %llu truncated, %llu bad length, %llu unknown type,"
                  " %llu unknown symbol\n",
                  static_cast<unsigned long long>(ds.truncated_packets),
                  static_cast<unsigned long long>(ds.bad_message_length),
                  static_cast<unsigned long long>(ds.unknown_type),
                  static_cast<unsigned long long>(ds.unknown_symbol));
      std::printf("decode alone         %.2f M msg/s (%.1f ns/msg), %.0f MB/s\n",
                  ds.messages / t / 1e6, t / ds.messages * 1e9,
                  raw.size() / t / 1048576.0);
    }
  }

  std::vector<SymbolSpec> specs;
  for (const auto& sp : probe.specs()) {
    specs.push_back(SymbolSpec{sp.symbol, sp.price_decimals, sp.size_decimals});
  }

  // --- check ----------------------------------------------------------------
  // Streams packet by packet with one small reusable buffer, the way a handler
  // reading a socket would, rather than materialising the whole feed.
  if (a.check) {
    const std::vector<std::string> days = split(a.days, ',');
    if (days.empty()) { std::fprintf(stderr, "--check needs --days\n"); return 1; }

    NullSink sink;
    Decoder dec;
    dec.track_sessions(true);
    std::vector<Routed> buf;
    buf.reserve(1 << 12);

    std::vector<Snapshot> snaps;
    std::int64_t session_epoch_ms = 0;
    std::size_t si = 0, day = static_cast<std::size_t>(-1), day_bad = 0;
    std::uint64_t compared = 0, mismatched = 0, windows = 0;
    MatchingEngine eng(feed_cfg(), &sink);

    auto snap_ts = [&](std::size_t k) {
      return static_cast<std::uint64_t>(snaps[k].time_ms - session_epoch_ms) * 1000000ull;
    };
    auto score_remaining = [&]() {
      while (si < snaps.size()) {
        const std::size_t bad = compare(eng.book(), snaps[si]);
        compared += static_cast<std::uint64_t>(snaps[si].n_bids) + snaps[si].n_asks;
        mismatched += bad;
        day_bad += bad;
        ++windows;
        ++si;
      }
    };
    auto start_day = [&]() {
      if (day != static_cast<std::size_t>(-1)) {
        score_remaining();
        if (!a.quiet) {
          std::printf("  %-12s %7zu snapshots  %8zu mismatched level positions\n",
                      days[day].c_str(), snaps.size(), day_bad);
        }
      }
      ++day;
      if (day >= days.size()) return;
      LoadStats bs{};
      snaps = load_snapshots(a.data + "/l2book_ETH", days[day], 1, bs);
      session_epoch_ms = snaps.empty() ? 0 : snaps.front().time_ms / 86400000 * 86400000;
      si = 0;
      day_bad = 0;
      eng.book().clear();
    };

    std::size_t off = 0;
    while (off + 4 <= raw.size()) {
      const std::uint32_t plen = load_be<std::uint32_t>(raw.data() + off);
      off += 4;
      if (plen == 0 || off + plen > raw.size()) break;
      buf.clear();
      dec.clear_session_marks();
      dec.decode_packet(raw.data() + off, plen, buf);
      off += plen;
      std::size_t mi = 0;
      const std::vector<std::size_t>& marks = dec.session_marks();
      for (std::size_t j = 0; j <= buf.size(); ++j) {
        while (mi < marks.size() && marks[mi] == j) { start_day(); ++mi; }
        if (j == buf.size()) break;
        if (day >= days.size()) break;
        while (si < snaps.size() &&
               static_cast<std::uint64_t>(buf[j].cmd.ts) > snap_ts(si)) {
          const std::size_t bad = compare(eng.book(), snaps[si]);
          compared += static_cast<std::uint64_t>(snaps[si].n_bids) + snaps[si].n_asks;
          mismatched += bad;
          day_bad += bad;
          ++windows;
          ++si;
        }
        eng.apply(buf[j].cmd);
      }
    }
    if (day != static_cast<std::size_t>(-1) && day < days.size()) {
      score_remaining();
      if (!a.quiet) {
        std::printf("  %-12s %7zu snapshots  %8zu mismatched level positions\n",
                    days[day].c_str(), snaps.size(), day_bad);
      }
    }

    std::printf("\nITCH path against the exchange snapshots\n");
    std::printf("  snapshots compared   %llu\n", static_cast<unsigned long long>(windows));
    std::printf("  level positions      %llu compared, %llu wrong (%.6f%%)\n",
                static_cast<unsigned long long>(compared),
                static_cast<unsigned long long>(mismatched),
                compared ? 100.0 * mismatched / compared : 0.0);
    return mismatched == 0 ? 0 : 2;
  }

  // --- apply, single thread -------------------------------------------------
  if (a.shards <= 1) {
    NullSink sink;
    MultiBook books(feed_cfg(), &sink);
    if (a.event_hash) books.enable_event_hashing();
    Decoder dec;
    std::unique_ptr<JournalWriter> jw;
    if (!a.journal.empty()) {
      jw = std::make_unique<JournalWriter>(a.journal);
      if (!jw->ok()) {
        std::fprintf(stderr, "cannot write journal %s\n", a.journal.c_str());
        return 1;
      }
    }
    std::vector<Routed> buf;
    buf.reserve(1 << 12);
    std::uint64_t applied = 0;
    const double t0 = now_s();
    std::size_t off = 0;
    std::uint64_t jts = 0;
    while (off + 4 <= raw.size()) {
      const std::uint32_t plen = load_be<std::uint32_t>(raw.data() + off);
      off += 4;
      if (plen == 0 || off + plen > raw.size()) break;
      if (jw) jw->append(++jts, raw.data() + off, plen);
      buf.clear();
      dec.decode_packet(raw.data() + off, plen, buf);
      off += plen;
      for (const Routed& r : buf) {
        if (!books.has(r.locate)) {
          books.ensure(r.locate, r.locate < specs.size() ? specs[r.locate] : SymbolSpec{});
        }
        books.apply(r.locate, r.cmd);
        ++applied;
      }
    }
    const double t = now_s() - t0;
    std::printf("decode and apply     %.2f M msg/s (%.1f ns/msg), %.0f MB/s, one thread\n",
                dec.stats().messages / t / 1e6, t / dec.stats().messages * 1e9,
                raw.size() / t / 1048576.0);
    std::printf("                     %llu commands applied\n",
                static_cast<unsigned long long>(applied));
    if (jw) {
      std::printf("journal              %s, %llu packets, %.1f MB\n", a.journal.c_str(),
                  static_cast<unsigned long long>(jw->records()), jw->bytes() / 1048576.0);
      jw->close();
    }
    if (a.event_hash) {
      for (std::size_t i = 1; i < books.size(); ++i) {
        const std::uint16_t loc = static_cast<std::uint16_t>(i);
        if (!books.has(loc)) continue;
        std::printf("EVENT HASH %-10s %016llx over %llu events\n",
                    books.spec(loc).symbol.c_str(),
                    static_cast<unsigned long long>(books.event_hash(loc)),
                    static_cast<unsigned long long>(books.event_count(loc)));
      }
    }
    if (a.digest) {
      for (std::size_t i = 1; i < books.size(); ++i) {
        const std::uint16_t loc = static_cast<std::uint16_t>(i);
        if (!books.has(loc)) continue;
        std::printf("  digest %-10s %016llx\n", books.spec(loc).symbol.c_str(),
                    static_cast<unsigned long long>(book_digest(books.engine(loc).book())));
      }
    }
    return 0;
  }

  // --- apply, sharded -------------------------------------------------------
  {
    std::vector<std::unique_ptr<Shard>> shards;
    for (int i = 0; i < a.shards; ++i) {
      shards.push_back(std::make_unique<Shard>(feed_cfg(), 1u << 15, -1));
      shards.back()->set_specs(specs);
      if (a.event_hash) shards.back()->books().enable_event_hashing();
    }
    for (auto& sh : shards) sh->start();

    Decoder dec;
    std::vector<Routed> buf;
    buf.reserve(1 << 12);
    const double t0 = now_s();
    std::size_t off = 0;
    while (off + 4 <= raw.size()) {
      const std::uint32_t plen = load_be<std::uint32_t>(raw.data() + off);
      off += 4;
      if (plen == 0 || off + plen > raw.size()) break;
      buf.clear();
      dec.decode_packet(raw.data() + off, plen, buf);
      off += plen;
      for (const Routed& r : buf) shards[r.locate % a.shards]->push(r);
    }
    for (auto& sh : shards) sh->drain_and_join();
    const double t = now_s() - t0;

    std::uint64_t applied = 0;
    for (auto& sh : shards) applied += sh->stats().applied;
    std::printf("decode and apply     %.2f M msg/s (%.1f ns/msg), %.0f MB/s, %d shards"
                " plus the feed thread\n",
                dec.stats().messages / t / 1e6, t / dec.stats().messages * 1e9,
                raw.size() / t / 1048576.0, a.shards);
    std::printf("                     %llu commands applied\n",
                static_cast<unsigned long long>(applied));
    if (a.event_hash) {
      for (std::size_t loc = 1; loc < specs.size(); ++loc) {
        Shard& sh = *shards[loc % a.shards];
        const std::uint16_t l = static_cast<std::uint16_t>(loc);
        if (!sh.books().has(l)) continue;
        std::printf("EVENT HASH %-10s %016llx over %llu events\n", specs[loc].symbol.c_str(),
                    static_cast<unsigned long long>(sh.books().event_hash(l)),
                    static_cast<unsigned long long>(sh.books().event_count(l)));
      }
    }
    if (a.digest) {
      for (std::size_t loc = 1; loc < specs.size(); ++loc) {
        Shard& sh = *shards[loc % a.shards];
        const std::uint16_t l = static_cast<std::uint16_t>(loc);
        if (!sh.books().has(l)) continue;
        std::printf("  digest %-10s %016llx\n", specs[loc].symbol.c_str(),
                    static_cast<unsigned long long>(book_digest(sh.books().engine(l).book())));
      }
    }
  }
  return 0;
}
