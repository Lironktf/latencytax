// Builds the queue model dataset from the raw files.
//
// The question, for one sample: an order joins a price level at time t with
// `q` already resting in front of it. Within the next H seconds, does enough
// volume trade at that price to work through `q`?
//
// The label is read off the tape and nothing else. Volume printing at the level's
// own price counts toward q; a print through the level's price means everything
// at that price had to go first, so the answer is yes immediately. There is no
// simulation and no fill model between the data and the label, which is the
// point: this is the piece of the fill model that can be checked against
// something real.
//
// Features are built from state that has seen the snapshot at t and every trade
// up to t, and nothing after. The label is the only thing that looks forward.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ml/dataset.hpp"
#include "ml/features.hpp"
#include "replay/loader.hpp"
#include "util/gzline.hpp"

using namespace ltx;
using namespace ltx::ml;

namespace {

struct Args {
  std::string data = "data/raw";
  std::string days, from, to;
  std::string out = "results/queue_train.bin";
  int horizon_s = 30;
  int stride = 1;              // take every Nth snapshot
  bool quiet = false;
};

void usage() {
  std::printf(
      "mlgen - build the queue model dataset from the raw files\n"
      "\n"
      "usage: mlgen [options]\n"
      "  --data=DIR         raw data directory\n"
      "  --days=a,b,c       explicit days, or use --from/--to\n"
      "  --from=YYYY-MM-DD  first day inclusive\n"
      "  --to=YYYY-MM-DD    last day inclusive\n"
      "  --out=FILE         output dataset, default results/queue_train.bin\n"
      "  --horizon=N        label horizon in seconds, default 30\n"
      "  --stride=N         use every Nth snapshot, default 1\n"
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
    else if (s.rfind("--horizon=", 0) == 0) a.horizon_s = std::atoi(s.c_str() + 10);
    else if (s.rfind("--stride=", 0) == 0) a.stride = std::atoi(s.c_str() + 9);
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
  }
  if (a.stride < 1) a.stride = 1;

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

  const std::int64_t H = static_cast<std::int64_t>(a.horizon_s) * 1000;
  const double q_fracs[3] = {0.25, 0.5, 1.0};
  const int offsets[2] = {0, 1};

  std::vector<Record> rows;
  rows.reserve(1u << 21);
  std::uint64_t positives = 0;
  std::uint64_t nonfinite = 0;

  for (std::size_t di = 0; di < days.size(); ++di) {
    LoadStats bs{}, ts{};
    const std::vector<Snapshot> snaps = load_snapshots(book_root, days[di], 1, bs);
    const std::vector<RawTrade> trades = load_trades(trade_root, days[di], 1, ts);
    if (snaps.size() < 2) continue;

    MarketState st;
    std::size_t ti = 0;       // trades fed into the state
    std::size_t day_rows = 0;

    for (std::size_t si = 0; si < snaps.size(); ++si) {
      const Snapshot& s = snaps[si];
      // Everything that happened at or before this snapshot, and nothing else.
      while (ti < trades.size() && trades[ti].time_ms <= s.time_ms) {
        st.on_trade(trades[ti]);
        ++ti;
      }
      st.on_snapshot(s);
      if (!st.ready()) continue;
      if (static_cast<int>(si) % a.stride != 0) continue;
      if (s.n_bids < 2 || s.n_asks < 2) continue;
      // A sample needs a full horizon of tape after it.
      if (s.time_ms + H > snaps.back().time_ms) break;

      for (int soff = 0; soff < 2; ++soff) {
        const Side side = soff == 0 ? Side::Buy : Side::Sell;
        const RawLevel* levels = side == Side::Buy ? s.bids.data() : s.asks.data();
        for (int oi = 0; oi < 2; ++oi) {
          const RawLevel& lv = levels[offsets[oi]];
          const double level_eth = static_cast<double>(lv.qty) / kQtyScale;
          if (level_eth <= 0) continue;

          // One forward scan of the tape serves every queue fraction at this
          // level, because the cumulative volume only grows.
          double consumed = 0.0;
          bool swept = false;
          for (std::size_t k = ti; k < trades.size(); ++k) {
            const RawTrade& t = trades[k];
            if (t.time_ms > s.time_ms + H) break;
            if (t.aggressor == side) continue;   // same side as the resting order
            if (side == Side::Buy ? (t.tick < lv.tick) : (t.tick > lv.tick)) {
              swept = true;
              break;
            }
            if (t.tick == lv.tick) consumed += static_cast<double>(t.qty) / kQtyScale;
          }

          for (int qi = 0; qi < 3; ++qi) {
            Record r{};
            Query qy;
            qy.side = side;
            qy.offset_ticks = offsets[oi];
            qy.tick = lv.tick;
            qy.level_eth = level_eth;
            qy.orders = lv.orders;
            qy.q_eth = q_fracs[qi] * level_eth;
            base_features(st, qy, s.time_ms, r.base);
            for (std::size_t fi = 0; fi < kBaseFeatures; ++fi) {
              if (!std::isfinite(r.base[fi])) ++nonfinite;
            }
            r.ts_ms = s.time_ms;
            r.consumed_eth = static_cast<float>(swept ? level_eth * 4.0 : consumed);
            r.label = (swept || consumed >= qy.q_eth) ? 1 : 0;
            r.side = static_cast<std::uint8_t>(soff);
            r.offset = static_cast<std::uint8_t>(offsets[oi]);
            r.day = static_cast<std::uint8_t>(di);
            positives += r.label;
            rows.push_back(r);
            ++day_rows;
          }
        }
      }
    }
    if (!a.quiet) {
      std::printf("%-12s %8zu snapshots  %9zu samples\n", days[di].c_str(), snaps.size(),
                  day_rows);
    }
  }

  if (!write_dataset(a.out, rows, static_cast<std::uint32_t>(H),
                     static_cast<std::uint32_t>(days.size()))) {
    std::fprintf(stderr, "cannot write %s\n", a.out.c_str());
    return 1;
  }
  std::printf("\n%s\n", a.out.c_str());
  std::printf("  samples            %zu (%.1f MB)\n", rows.size(),
              rows.size() * sizeof(Record) / 1048576.0);
  std::printf("  positives          %llu (%.2f%%)\n", static_cast<unsigned long long>(positives),
              rows.empty() ? 0.0 : 100.0 * positives / rows.size());
  std::printf("  horizon            %d s\n", a.horizon_s);
  std::printf("  days               %zu\n", days.size());
  if (nonfinite) {
    std::fprintf(stderr, "refusing to ship a dataset with %llu non finite features\n",
                 static_cast<unsigned long long>(nonfinite));
    return 1;
  }
  return 0;
}
