// Per wallet, per day markout aggregates, for experiment 003.
//
// Hyperliquid names both counterparties on every print, which is a thing no
// centralised venue gives you: on a CEX you never learn who hit you. This turns
// that into the only input the experiment needs, a table of
//
//   day, wallet, role, prints, eth, and size weighted markout at three horizons
//
// Aggregating here rather than emitting a row per print keeps the file small
// enough to read in one go, and the pre-registration's statistics are all
// functions of these sums.
//
// Nothing in this tool decides anything. The walk forward, the thresholds and
// the tests are in scripts/wallet_analysis.py, so the analysis can be re-run
// without re-reading 39 days of gzip.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "replay/loader.hpp"
#include "util/gzline.hpp"

using namespace ltx;

namespace {

struct Args {
  std::string data = "data/raw";
  std::string days, from, to;
  std::string out = "results/wallets.csv";
  bool quiet = false;
};

void usage() {
  std::printf(
      "wallets - per wallet, per day markout aggregates from the trade tape\n"
      "\n"
      "usage: wallets [options]\n"
      "  --data=DIR         raw data directory\n"
      "  --days=a,b,c       explicit days, or use --from/--to\n"
      "  --from=YYYY-MM-DD  first day inclusive\n"
      "  --to=YYYY-MM-DD    last day inclusive\n"
      "  --out=FILE         output csv, default results/wallets.csv\n"
      "  --quiet\n");
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

struct Agg {
  std::uint64_t prints = 0;
  double eth = 0;
  double w_mo[3] = {0, 0, 0};   // size weighted markout sums, 5 s / 30 s / 300 s
  std::uint64_t scored = 0;     // prints with a full horizon available
  double scored_eth = 0;
  // The same, restricted to prints where the aggressor bought. The period this
  // dataset covers trends hard, so a wallet that happened to be long would look
  // informed. Splitting by direction is what tells the two apart.
  std::uint64_t buy_scored = 0;
  double buy_eth = 0;
  double buy_w_mo30 = 0;
};

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
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
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

  std::FILE* out = std::fopen(a.out.c_str(), "w");
  if (!out) { std::fprintf(stderr, "cannot write %s\n", a.out.c_str()); return 1; }
  std::fprintf(out, "day,wallet,role,prints,eth,scored,scored_eth,"
                    "w_mo5,w_mo30,w_mo300,buy_scored,buy_eth,buy_w_mo30\n");

  const std::int64_t H[3] = {5000, 30000, 300000};
  std::uint64_t total_prints = 0, total_scored = 0, no_users = 0;

  for (const std::string& day : days) {
    LoadStats bs{};
    const std::vector<Snapshot> snaps = load_snapshots(book_root, day, 1, bs);
    if (snaps.size() < 2) continue;

    // The mid series this day's markouts are measured against.
    std::vector<std::int64_t> mt;
    std::vector<double> mp;
    mt.reserve(snaps.size());
    mp.reserve(snaps.size());
    for (const Snapshot& s : snaps) {
      if (s.n_bids == 0 || s.n_asks == 0) continue;
      mt.push_back(s.time_ms);
      mp.push_back(0.5 * (s.bids[0].tick + s.asks[0].tick) * 0.1);
    }
    if (mt.size() < 2) continue;

    // The tape has to be read again here rather than through load_trades,
    // because the addresses are dropped on the way into RawTrade.
    std::unordered_map<std::string, Agg> aggressor, maker;
    aggressor.reserve(1 << 14);
    maker.reserve(1 << 14);
    std::uint64_t day_prints = 0, day_scored = 0;

    for (const std::string& f : list_gz_files(trade_root + "/date=" + day)) {
      GzLineReader r(f);
      if (!r.ok()) continue;
      std::string_view line;
      while (r.next(line)) {
        if (line.empty()) continue;
        RawTrade t;
        if (!parse_trade(line, 1, t)) continue;
        TradeUsers u;
        if (!parse_trade_users(line, u)) { ++no_users; continue; }
        ++day_prints;

        const bool buy = t.aggressor == Side::Buy;
        const std::string agg_w = buy ? u.buyer : u.seller;
        const std::string mak_w = buy ? u.seller : u.buyer;
        const double sz = static_cast<double>(t.qty) / kQtyScale;
        const double px = static_cast<double>(t.tick) * 0.1;
        const double sign = buy ? 1.0 : -1.0;

        Agg& A = aggressor[agg_w];
        Agg& M = maker[mak_w];
        ++A.prints;
        ++M.prints;
        A.eth += sz;
        M.eth += sz;

        // Every horizon must be available, or the print is counted but not
        // scored, so the three horizons always describe the same set of prints.
        double mo[3];
        bool full = true;
        for (int h = 0; h < 3 && full; ++h) {
          const auto it = std::lower_bound(mt.begin(), mt.end(), t.time_ms + H[h]);
          if (it == mt.end()) { full = false; break; }
          const double m = mp[static_cast<std::size_t>(it - mt.begin())];
          mo[h] = sign * (m - px) / px * 1e4;
        }
        if (!full) continue;
        ++day_scored;
        ++A.scored;
        ++M.scored;
        A.scored_eth += sz;
        M.scored_eth += sz;
        for (int h = 0; h < 3; ++h) {
          A.w_mo[h] += mo[h] * sz;
          M.w_mo[h] -= mo[h] * sz;   // the maker's side of the same print
        }
        if (buy) {
          ++A.buy_scored;
          A.buy_eth += sz;
          A.buy_w_mo30 += mo[1] * sz;
          ++M.buy_scored;
          M.buy_eth += sz;
          M.buy_w_mo30 -= mo[1] * sz;
        }
      }
    }

    auto dump = [&](const std::unordered_map<std::string, Agg>& m, const char* role) {
      for (const auto& [w, g] : m) {
        if (g.scored == 0) continue;
        std::fprintf(out, "%s,%s,%s,%llu,%.6f,%llu,%.6f,%.8f,%.8f,%.8f,%llu,%.6f,%.8f\n",
                     day.c_str(), w.c_str(), role,
                     static_cast<unsigned long long>(g.prints), g.eth,
                     static_cast<unsigned long long>(g.scored), g.scored_eth, g.w_mo[0],
                     g.w_mo[1], g.w_mo[2],
                     static_cast<unsigned long long>(g.buy_scored), g.buy_eth,
                     g.buy_w_mo30);
      }
    };
    dump(aggressor, "aggressor");
    dump(maker, "maker");

    total_prints += day_prints;
    total_scored += day_scored;
    if (!a.quiet) {
      std::printf("%-12s %8llu prints, %8llu scored, %6zu aggressors, %6zu makers\n",
                  day.c_str(), static_cast<unsigned long long>(day_prints),
                  static_cast<unsigned long long>(day_scored), aggressor.size(),
                  maker.size());
    }
  }
  std::fclose(out);
  std::printf("\n%s\n", a.out.c_str());
  std::printf("  days               %zu\n", days.size());
  std::printf("  prints             %llu, %llu with a full 300 s horizon (%.1f%%)\n",
              static_cast<unsigned long long>(total_prints),
              static_cast<unsigned long long>(total_scored),
              total_prints ? 100.0 * total_scored / total_prints : 0.0);
  std::printf("  prints with no counterparties: %llu\n",
              static_cast<unsigned long long>(no_users));
  return 0;
}
