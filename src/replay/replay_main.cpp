// Replays the raw Hyperliquid files through the matching engine and reports how
// closely the reconstructed book tracks the exchange's own snapshots.
//
// Two numbers come out of this, and they mean different things.
//
//   tape only
//     Take snapshot i, apply nothing but the trade prints that happened in the
//     next five seconds, and compare against snapshot i+1. This measures how
//     much of the book's evolution trades explain. It is expected to be poor,
//     because most of what changes in five seconds is quotes being pulled and
//     replaced, not volume.
//
//   reconstructed
//     Apply the trade prints and then the add, cancel and modify flow that the
//     reconstruction derives from the raw snapshots, and compare against
//     snapshot i+1. The reconstruction is computed from the data alone and
//     never reads engine state, so this number is a check on the engine. It has
//     to be zero.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/engine.hpp"
#include "replay/loader.hpp"
#include "replay/reconstruct.hpp"
#include "util/gzline.hpp"

using namespace ltx;

namespace {

struct Args {
  std::string data = "data/raw";
  std::string days;
  std::string from, to;
  int price_decimals = 1;
  bool quiet = false;
  std::string csv;
};

void usage() {
  std::printf(
      "replay - reconstruct the book from snapshots plus tape and score it\n"
      "\n"
      "usage: replay [options]\n"
      "  --data=DIR         directory holding l2book_ETH/ and trades_ETH/, default data/raw\n"
      "  --days=a,b,c       explicit days (YYYY-MM-DD), default every day present\n"
      "  --from=YYYY-MM-DD  first day, inclusive\n"
      "  --to=YYYY-MM-DD    last day, inclusive\n"
      "  --csv=FILE         write per day rows to FILE\n"
      "  --quiet            totals only\n"
      "  --help\n");
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i <= s.size()) {
    const std::size_t j = s.find(sep, i);
    if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
    out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto opt = [&](const char* k, std::string& out) {
      if (s.rfind(k, 0) == 0) { out = s.substr(std::strlen(k)); return true; }
      return false;
    };
    if (s == "--help" || s == "-h") { usage(); std::exit(0); }
    else if (opt("--data=", a.data)) {}
    else if (opt("--days=", a.days)) {}
    else if (opt("--from=", a.from)) {}
    else if (opt("--to=", a.to)) {}
    else if (opt("--csv=", a.csv)) {}
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return false; }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return 1;

  const std::string book_root = a.data + "/l2book_ETH";
  const std::string trade_root = a.data + "/trades_ETH";

  std::vector<std::string> days;
  if (!a.days.empty()) {
    days = split(a.days, ',');
  } else {
    days = list_dates(book_root);
    if (!a.from.empty()) {
      days.erase(std::remove_if(days.begin(), days.end(),
                                [&](const std::string& d) { return d < a.from; }),
                 days.end());
    }
    if (!a.to.empty()) {
      days.erase(std::remove_if(days.begin(), days.end(),
                                [&](const std::string& d) { return d > a.to; }),
                 days.end());
    }
  }
  if (days.empty()) {
    std::fprintf(stderr, "no days found under %s\n", book_root.c_str());
    return 1;
  }

  BookConfig cfg;
  cfg.min_tick = 1;
  cfg.max_tick = 262144;
  cfg.max_orders = 1u << 16;
  cfg.id_map_capacity = 1u << 17;

  std::FILE* csv = nullptr;
  if (!a.csv.empty()) {
    csv = std::fopen(a.csv.c_str(), "w");
    if (csv) {
      std::fprintf(csv,
                   "day,snapshots,expected,coverage_pct,trades,windows,gap_windows,"
                   "tape_mismatch_pct,tape_top5_err_pct,recon_mismatch_levels,"
                   "recon_unexpected_trades,tape_vol_matched_pct,swept_better_pct,"
                   "commands\n");
    }
  }

  if (!a.quiet) {
    std::printf("%-12s %8s %7s %8s %8s %9s %9s %8s %8s\n", "day", "snaps", "cover",
                "trades", "windows", "tape-mism", "tape-sz", "recon", "cmds");
  }

  ReplayStats grand{};
  std::uint64_t grand_snaps = 0, grand_expected = 0;
  double grand_tape_err = 0, grand_tape_ref = 0;

  for (const std::string& day : days) {
    LoadStats bs{}, ts{};
    std::vector<Snapshot> snaps = load_snapshots(book_root, day, a.price_decimals, bs);
    std::vector<RawTrade> trades = load_trades(trade_root, day, a.price_decimals, ts);
    if (snaps.size() < 2) {
      std::fprintf(stderr, "%s: %zu snapshots, skipped\n", day.c_str(), snaps.size());
      continue;
    }

    NullSink sink;
    MatchingEngine eng(cfg, &sink);
    Reconstructor rec(eng);
    const ReplayStats st = rec.run(snaps, trades);

    const std::uint64_t expected = 86400 / 5;
    const double coverage = 100.0 * static_cast<double>(snaps.size()) / expected;
    const double tape_mism =
        st.total.tape_levels_compared
            ? 100.0 * st.total.tape_level_mismatch / st.total.tape_levels_compared
            : 0.0;
    const double tape_sz =
        st.total.tape_top5_ref ? 100.0 * st.total.tape_top5_abs_err / st.total.tape_top5_ref
                               : 0.0;

    if (!a.quiet) {
      std::printf("%-12s %8zu %6.1f%% %8zu %8llu %8.2f%% %8.2f%% %8llu %8llu\n", day.c_str(),
                  snaps.size(), coverage, trades.size(),
                  static_cast<unsigned long long>(st.windows), tape_mism, tape_sz,
                  static_cast<unsigned long long>(st.total.recon_level_mismatch),
                  static_cast<unsigned long long>(st.commands));
    }
    if (csv) {
      std::fprintf(csv, "%s,%zu,%llu,%.4f,%zu,%llu,%llu,%.6f,%.6f,%llu,%llu,%.6f,%.6f,%llu\n",
                   day.c_str(), snaps.size(), static_cast<unsigned long long>(expected),
                   coverage, trades.size(), static_cast<unsigned long long>(st.windows),
                   static_cast<unsigned long long>(st.windows_skipped_gap), tape_mism, tape_sz,
                   static_cast<unsigned long long>(st.total.recon_level_mismatch),
                   static_cast<unsigned long long>(st.total.recon_unexpected_trades),
                   st.total.tape_qty
                       ? 100.0 * static_cast<double>(st.total.matched_qty) / st.total.tape_qty
                       : 0.0,
                   st.total.matched_qty ? 100.0 * static_cast<double>(st.total.swept_better_qty) /
                                              st.total.matched_qty
                                        : 0.0,
                   static_cast<unsigned long long>(st.commands));
    }

    grand.windows += st.windows;
    grand.windows_skipped_gap += st.windows_skipped_gap;
    grand.commands += st.commands;
    grand.windows_with_recon_mismatch += st.windows_with_recon_mismatch;
    grand.windows_with_tape_mismatch += st.windows_with_tape_mismatch;
    grand.total.trades += st.total.trades;
    grand.total.trades_no_liquidity += st.total.trades_no_liquidity;
    grand.total.tape_qty += st.total.tape_qty;
    grand.total.matched_qty += st.total.matched_qty;
    grand.total.swept_better_qty += st.total.swept_better_qty;
    grand.total.tape_level_mismatch += st.total.tape_level_mismatch;
    grand.total.tape_levels_compared += st.total.tape_levels_compared;
    grand.total.recon_level_mismatch += st.total.recon_level_mismatch;
    grand.total.recon_levels_compared += st.total.recon_levels_compared;
    grand.total.recon_unexpected_trades += st.total.recon_unexpected_trades;
    grand.total.adds += st.total.adds;
    grand.total.cancels += st.total.cancels;
    grand.total.modifies += st.total.modifies;
    grand_tape_err += st.total.tape_top5_abs_err;
    grand_tape_ref += st.total.tape_top5_ref;
    grand_snaps += snaps.size();
    grand_expected += expected;
    if (bs.parse_errors || ts.parse_errors) {
      std::fprintf(stderr, "%s: parse errors book=%zu trades=%zu\n", day.c_str(),
                   bs.parse_errors, ts.parse_errors);
    }
  }
  if (csv) std::fclose(csv);

  std::printf("\n");
  std::printf("days                     %zu\n", days.size());
  std::printf("snapshots                %llu of %llu expected (%.2f%% coverage)\n",
              static_cast<unsigned long long>(grand_snaps),
              static_cast<unsigned long long>(grand_expected),
              100.0 * grand_snaps / (grand_expected ? grand_expected : 1));
  std::printf("windows scored           %llu (%llu skipped as feed gaps)\n",
              static_cast<unsigned long long>(grand.windows),
              static_cast<unsigned long long>(grand.windows_skipped_gap));
  std::printf("engine commands applied  %llu (%llu adds, %llu cancels, %llu modifies,"
              " %llu tape prints)\n",
              static_cast<unsigned long long>(grand.commands),
              static_cast<unsigned long long>(grand.total.adds),
              static_cast<unsigned long long>(grand.total.cancels),
              static_cast<unsigned long long>(grand.total.modifies),
              static_cast<unsigned long long>(grand.total.trades));
  std::printf("\ntape only fidelity (trades explain this much of a 5 s window)\n");
  std::printf("  level positions wrong  %.2f%% of %llu compared\n",
              grand.total.tape_levels_compared
                  ? 100.0 * grand.total.tape_level_mismatch / grand.total.tape_levels_compared
                  : 0.0,
              static_cast<unsigned long long>(grand.total.tape_levels_compared));
  std::printf("  top 5 size error       %.2f%% of snapshot size\n",
              grand_tape_ref ? 100.0 * grand_tape_err / grand_tape_ref : 0.0);
  std::printf("  windows with any miss  %.2f%%\n",
              grand.windows ? 100.0 * grand.windows_with_tape_mismatch / grand.windows : 0.0);
  std::printf("\nreconstructed fidelity (the engine check)\n");
  std::printf("  level positions wrong  %llu of %llu compared (%.6f%%)\n",
              static_cast<unsigned long long>(grand.total.recon_level_mismatch),
              static_cast<unsigned long long>(grand.total.recon_levels_compared),
              grand.total.recon_levels_compared
                  ? 100.0 * grand.total.recon_level_mismatch / grand.total.recon_levels_compared
                  : 0.0);
  std::printf("  windows with any miss  %llu\n",
              static_cast<unsigned long long>(grand.windows_with_recon_mismatch));
  std::printf("  unexpected trades      %llu (reconciliation must never cross)\n",
              static_cast<unsigned long long>(grand.total.recon_unexpected_trades));
  std::printf("\ntape against the snapshot book\n");
  std::printf("  volume that found resting size at or better than the print: %.2f%%\n",
              grand.total.tape_qty
                  ? 100.0 * static_cast<double>(grand.total.matched_qty) / grand.total.tape_qty
                  : 0.0);
  std::printf("  of that, filled at a better price than printed:             %.2f%%\n",
              grand.total.matched_qty ? 100.0 *
                                            static_cast<double>(grand.total.swept_better_qty) /
                                            grand.total.matched_qty
                                      : 0.0);
  std::printf("  prints that found nothing resting:                          %llu of %llu\n",
              static_cast<unsigned long long>(grand.total.trades_no_liquidity),
              static_cast<unsigned long long>(grand.total.trades));

  return grand.total.recon_level_mismatch == 0 && grand.total.recon_unexpected_trades == 0 ? 0
                                                                                           : 2;
}
