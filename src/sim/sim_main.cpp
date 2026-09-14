// Runs market making agents inside the replayed book and reports what reaction
// latency costs them.
//
// Every agent in a sweep sees the same replay: one pass over a day drives all of
// them, so a latency curve is one reconstruction, not one per point. The agents
// do not interact with each other or with the book; each one is a shadow
// participant whose fills are inferred from the real tape by the queue model in
// fill_model.hpp.
//
// Output is one csv row per (day, agent). The bootstrap and the confidence
// intervals are in scripts/analyse.py; this binary only produces measurements.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/engine.hpp"
#include "replay/loader.hpp"
#include "replay/reconstruct.hpp"
#include "sim/agent.hpp"
#include "util/gzline.hpp"

using namespace ltx;

namespace {

struct Fees {
  double maker_bps = 1.5;   // Hyperliquid perps tier 0
  double taker_bps = 4.5;
};

struct Args {
  std::string data = "data/raw";
  std::string days, from, to;
  std::string latencies = "0.1,1,10,100";
  std::string gammas = "5";
  std::string offsets = "0";
  std::string betas = "0";
  std::string kappas = "1.0";
  std::string requotes = "0";
  std::string sweep = "through";
  double size_eth = 0.5;
  double max_inv_eth = 25.0;
  double k = 1.5;
  double tau_s = 60.0;
  double ofi_halflife_s = 60.0;
  Fees fees;
  std::string csv;
  std::string fills_csv;
  bool fit_beta = false;
  bool quiet = false;
};

void usage() {
  std::printf(
      "sim - market making agents inside the replayed Hyperliquid book\n"
      "\n"
      "usage: sim [options]\n"
      "  --data=DIR          directory holding l2book_ETH/ and trades_ETH/\n"
      "  --days=a,b,c        explicit days, or use --from/--to\n"
      "  --from=YYYY-MM-DD   first day inclusive\n"
      "  --to=YYYY-MM-DD     last day inclusive\n"
      "  --latency=LIST      reaction latencies in ms, default 0.1,1,10,100\n"
      "  --gamma=LIST        inventory risk aversion, default 5\n"
      "  --offset=LIST       ticks behind the touch, default 0\n"
      "  --beta=LIST         order flow coefficient in USD per ETH, default 0\n"
      "  --kappa=LIST        share of cancellations taken to be ahead, default 1.0\n"
      "  --requote=LIST      ticks of drift tolerated before giving up queue position\n"
      "  --sweep=through|queue  what a print through the quote price does, default through\n"
      "  --size=N            quote size in ETH, default 0.5\n"
      "  --max-inv=N         inventory limit in ETH, default 25\n"
      "  --k=N               AS order arrival decay, default 1.5\n"
      "  --tau=N             AS horizon in seconds, default 60\n"
      "  --maker-bps=N       maker fee, default 1.5 (Hyperliquid tier 0)\n"
      "  --taker-bps=N       taker fee, default 4.5 (Hyperliquid tier 0)\n"
      "  --csv=FILE          write one row per day and agent\n"
      "  --fills-csv=FILE    write every fill (large)\n"
      "  --fit-beta          report the order flow regression instead of running agents\n"
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

std::vector<double> split_d(const std::string& s) {
  std::vector<double> out;
  for (const std::string& t : split(s, ',')) out.push_back(std::atof(t.c_str()));
  return out;
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto str = [&](const char* k, std::string& out) {
      if (s.rfind(k, 0) == 0) { out = s.substr(std::strlen(k)); return true; }
      return false;
    };
    auto num = [&](const char* k, double& out) {
      if (s.rfind(k, 0) == 0) { out = std::atof(s.c_str() + std::strlen(k)); return true; }
      return false;
    };
    if (s == "--help" || s == "-h") { usage(); std::exit(0); }
    else if (str("--data=", a.data)) {}
    else if (str("--days=", a.days)) {}
    else if (str("--from=", a.from)) {}
    else if (str("--to=", a.to)) {}
    else if (str("--latency=", a.latencies)) {}
    else if (str("--gamma=", a.gammas)) {}
    else if (str("--offset=", a.offsets)) {}
    else if (str("--beta=", a.betas)) {}
    else if (str("--kappa=", a.kappas)) {}
    else if (str("--requote=", a.requotes)) {}
    else if (str("--sweep=", a.sweep)) {}
    else if (str("--csv=", a.csv)) {}
    else if (str("--fills-csv=", a.fills_csv)) {}
    else if (num("--size=", a.size_eth)) {}
    else if (num("--max-inv=", a.max_inv_eth)) {}
    else if (num("--k=", a.k)) {}
    else if (num("--tau=", a.tau_s)) {}
    else if (num("--maker-bps=", a.fees.maker_bps)) {}
    else if (num("--taker-bps=", a.fees.taker_bps)) {}
    else if (s == "--fit-beta") a.fit_beta = true;
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return false; }
  }
  return true;
}

// Drives every agent in the sweep from one replay.
class SimObserver final : public ReplayObserver {
 public:
  explicit SimObserver(std::vector<Agent>& agents) : agents_(agents) {}

  void on_snapshot(const Snapshot& s, const OrderBook& book) override {
    if (!book.has_bid() || !book.has_ask()) return;
    const Tick bb = book.best_bid();
    const Tick ba = book.best_ask();
    const Qty bq = book.qty_at(bb);
    const Qty aq = book.qty_at(ba);
    const std::int64_t us = s.time_ms * 1000;
    mid_ts_.push_back(s.time_ms);
    mid_px_.push_back(0.5 * (static_cast<double>(bb) + static_cast<double>(ba)) * 0.1);
    for (Agent& a : agents_) {
      const Tick abt = a.agent_bid_tick();
      const Tick aat = a.agent_ask_tick();
      a.on_snapshot(us, bb, ba, bq, aq, abt == kInvalidTick ? 0 : book.qty_at(abt),
                    aat == kInvalidTick ? 0 : book.qty_at(aat));
    }
    last_bb_ = bb;
    last_ba_ = ba;
  }

  void on_trade_print(const RawTrade& t, const OrderBook& book) override {
    if (!book.has_bid() || !book.has_ask()) return;
    const Qty bq = book.qty_at(book.best_bid());
    const Qty aq = book.qty_at(book.best_ask());
    const std::int64_t us = t.time_ms * 1000;
    for (Agent& a : agents_) a.on_print(us, t.aggressor, t.tick, t.qty, bq, aq);
  }

  const std::vector<std::int64_t>& mid_ts() const { return mid_ts_; }
  const std::vector<double>& mid_px() const { return mid_px_; }
  Tick last_bb() const { return last_bb_; }
  Tick last_ba() const { return last_ba_; }

 private:
  std::vector<Agent>& agents_;
  std::vector<std::int64_t> mid_ts_;
  std::vector<double> mid_px_;
  Tick last_bb_ = kInvalidTick;
  Tick last_ba_ = kInvalidTick;
};

struct Row {
  std::string day;
  double latency_ms;
  double gamma, offset, beta, kappa, requote;
  std::uint64_t maker_fills = 0;
  std::uint64_t swept_fills = 0;
  std::uint64_t requotes = 0;
  double maker_notional = 0;
  double taker_notional = 0;
  double gross_pnl = 0;
  double fees = 0;
  double net_pnl = 0;
  double edge_bps = 0;
  double mo1 = 0, mo5 = 0, mo30 = 0;
  std::uint64_t mo_n = 0;
  double max_abs_inv = 0;
  double end_inv = 0;
  double buy_eth = 0, sell_eth = 0;
};

double mid_at(const std::vector<std::int64_t>& ts, const std::vector<double>& px,
              std::int64_t want_ms, bool& ok) {
  const auto it = std::lower_bound(ts.begin(), ts.end(), want_ms);
  if (it == ts.end()) { ok = false; return 0; }
  ok = true;
  return px[static_cast<std::size_t>(it - ts.begin())];
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
    if (!a.from.empty())
      days.erase(std::remove_if(days.begin(), days.end(),
                                [&](const std::string& d) { return d < a.from; }),
                 days.end());
    if (!a.to.empty())
      days.erase(std::remove_if(days.begin(), days.end(),
                                [&](const std::string& d) { return d > a.to; }),
                 days.end());
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

  // --- order flow regression mode -----------------------------------------
  if (a.fit_beta) {
    // Regresses the mid return over the next snapshot interval on the decayed
    // signed traded volume, pooled over the requested days. Calibration only.
    double sxx = 0, sxy = 0, sx = 0, sy = 0, syy = 0;
    std::uint64_t n = 0;
    for (const std::string& day : days) {
      LoadStats bs{}, ts{};
      auto snaps = load_snapshots(book_root, day, 1, bs);
      auto trades = load_trades(trade_root, day, 1, ts);
      if (snaps.size() < 3) continue;
      std::size_t ti = 0;
      double ofi = 0;
      std::int64_t ofi_ts = 0;
      std::vector<double> x, y;
      for (std::size_t i = 0; i < snaps.size(); ++i) {
        while (ti < trades.size() && trades[ti].time_ms <= snaps[i].time_ms) {
          if (ofi_ts) {
            ofi *= std::exp(-std::log(2.0) *
                            static_cast<double>(trades[ti].time_ms - ofi_ts) / 1000.0 /
                            a.ofi_halflife_s);
          }
          ofi_ts = trades[ti].time_ms;
          ofi += (trades[ti].aggressor == Side::Buy ? 1.0 : -1.0) *
                 static_cast<double>(trades[ti].qty) / kQtyScale;
          ++ti;
        }
        if (snaps[i].n_bids == 0 || snaps[i].n_asks == 0) continue;
        const double mid = 0.5 * (snaps[i].bids[0].tick + snaps[i].asks[0].tick) * 0.1;
        x.push_back(ofi);
        y.push_back(mid);
      }
      for (std::size_t i = 0; i + 1 < x.size(); ++i) {
        const double dx = x[i];
        const double dy = y[i + 1] - y[i];
        sx += dx; sy += dy; sxx += dx * dx; sxy += dx * dy; syy += dy * dy; ++n;
      }
    }
    if (n < 2) { std::fprintf(stderr, "not enough data\n"); return 1; }
    const double mx = sx / n, my = sy / n;
    const double cov = sxy / n - mx * my;
    const double vx = sxx / n - mx * mx;
    const double vy = syy / n - my * my;
    const double beta = vx > 0 ? cov / vx : 0.0;
    const double corr = (vx > 0 && vy > 0) ? cov / std::sqrt(vx * vy) : 0.0;
    std::printf("order flow regression over %zu days, %llu snapshot pairs\n", days.size(),
                static_cast<unsigned long long>(n));
    std::printf("  next interval mid change = beta * decayed signed volume\n");
    std::printf("  beta = %.8f USD per ETH\n", beta);
    std::printf("  correlation = %.5f, r2 = %.6f\n", corr, corr * corr);
    std::printf("  ofi half life %.0f s\n", a.ofi_halflife_s);
    return 0;
  }

  // --- build the sweep -----------------------------------------------------
  const std::vector<double> lats = split_d(a.latencies);
  const std::vector<double> gammas = split_d(a.gammas);
  const std::vector<double> offsets = split_d(a.offsets);
  const std::vector<double> betas = split_d(a.betas);
  const std::vector<double> kappas = split_d(a.kappas);
  const std::vector<double> reqs = split_d(a.requotes);

  std::vector<AgentConfig> configs;
  for (double L : lats)
    for (double g : gammas)
      for (double off : offsets)
        for (double b : betas)
          for (double kp : kappas)
           for (double rq : reqs) {
            AgentConfig c;
            c.requote_ticks = static_cast<int>(rq);
            c.sweep_rule = a.sweep == "queue" ? SweepRule::Queue : SweepRule::Through;
            c.latency_us = static_cast<std::int64_t>(L * 1000.0 + 0.5);
            c.gamma = g;
            c.offset_ticks = static_cast<int>(off);
            c.beta = b;
            c.kappa = kp;
            c.k = a.k;
            c.tau_s = a.tau_s;
            c.ofi_halflife_s = a.ofi_halflife_s;
            c.quote_size = static_cast<Qty>(a.size_eth * kQtyScale + 0.5);
            c.max_inventory = static_cast<Qty>(a.max_inv_eth * kQtyScale + 0.5);
            configs.push_back(c);
          }

  std::FILE* csv = nullptr;
  if (!a.csv.empty()) {
    csv = std::fopen(a.csv.c_str(), "w");
    if (!csv) { std::fprintf(stderr, "cannot write %s\n", a.csv.c_str()); return 1; }
    std::fprintf(csv,
                 "day,latency_ms,gamma,offset_ticks,beta,kappa,maker_fills,swept_fills,"
                 "requote_ticks,requotes,maker_notional,taker_notional,gross_pnl,fees,net_pnl,edge_bps,"
                 "markout_1s_bps,markout_5s_bps,markout_30s_bps,markout_n,max_abs_inv_eth,"
                 "end_inv_eth,buy_eth,sell_eth,maker_bps,taker_bps\n");
  }
  std::FILE* fcsv = nullptr;
  if (!a.fills_csv.empty()) {
    fcsv = std::fopen(a.fills_csv.c_str(), "w");
    if (fcsv) std::fprintf(fcsv, "day,latency_ms,gamma,ts_ms,side,px,qty_eth,swept,taker\n");
  }

  if (!a.quiet) {
    std::printf("agents per day: %zu   days: %zu   fees: maker %.3f bps, taker %.3f bps   sweep rule: %s\n",
                configs.size(), days.size(), a.fees.maker_bps, a.fees.taker_bps,
                a.sweep.c_str());
    std::printf("%-12s %8s %6s %4s %4s %8s %7s %9s %9s %9s %9s %9s\n", "day", "lat(ms)", "gamma",
                "off", "rq", "fills", "swept%", "notional", "edge_bps", "mo1s", "mo5s", "mo30s");
  }

  std::vector<Row> all;
  for (const std::string& day : days) {
    LoadStats bs{}, ts{};
    std::vector<Snapshot> snaps = load_snapshots(book_root, day, 1, bs);
    std::vector<RawTrade> trades = load_trades(trade_root, day, 1, ts);
    if (snaps.size() < 2) {
      std::fprintf(stderr, "%s: %zu snapshots, skipped\n", day.c_str(), snaps.size());
      continue;
    }

    std::vector<Agent> agents;
    agents.reserve(configs.size());
    for (const AgentConfig& c : configs) agents.emplace_back(c);

    NullSink sink;
    MatchingEngine eng(cfg, &sink);
    Reconstructor rec(eng);
    SimObserver obs(agents);
    rec.set_observer(&obs);
    const ReplayStats rs = rec.run(snaps, trades);
    if (rs.total.recon_level_mismatch != 0) {
      std::fprintf(stderr, "%s: replay fidelity broke, %u level mismatches\n", day.c_str(),
                   rs.total.recon_level_mismatch);
      return 2;
    }

    const Tick fbb = obs.last_bb(), fba = obs.last_ba();
    const std::int64_t last_us = snaps.back().time_ms * 1000;

    for (std::size_t ci = 0; ci < agents.size(); ++ci) {
      Agent& ag = agents[ci];
      if (fbb != kInvalidTick && fba != kInvalidTick) ag.flatten(last_us, fbb, fba);

      Row r;
      r.day = day;
      r.latency_ms = static_cast<double>(configs[ci].latency_us) / 1000.0;
      r.gamma = configs[ci].gamma;
      r.offset = configs[ci].offset_ticks;
      r.beta = configs[ci].beta;
      r.kappa = configs[ci].kappa;
      r.requote = configs[ci].requote_ticks;
      r.requotes = ag.requotes();

      double mo1 = 0, mo5 = 0, mo30 = 0;
      std::uint64_t mo_n = 0;
      for (const Fill& f : ag.fills()) {
        const double px = static_cast<double>(f.tick) * 0.1;
        const double q = static_cast<double>(f.qty) / kQtyScale;
        const double notional = px * q;
        if (f.taker) {
          r.taker_notional += notional;
          r.fees += notional * a.fees.taker_bps / 1e4;
        } else {
          ++r.maker_fills;
          if (f.swept) ++r.swept_fills;
          r.maker_notional += notional;
          r.fees += notional * a.fees.maker_bps / 1e4;
          const double s = f.side == Side::Buy ? 1.0 : -1.0;
          const std::int64_t t_ms = f.ts_us / 1000;
          bool ok1 = false, ok5 = false, ok30 = false;
          const double m1 = mid_at(obs.mid_ts(), obs.mid_px(), t_ms + 1000, ok1);
          const double m5 = mid_at(obs.mid_ts(), obs.mid_px(), t_ms + 5000, ok5);
          const double m30 = mid_at(obs.mid_ts(), obs.mid_px(), t_ms + 30000, ok30);
          if (ok1 && ok5 && ok30) {
            mo1 += s * (m1 - px) / px * 1e4;
            mo5 += s * (m5 - px) / px * 1e4;
            mo30 += s * (m30 - px) / px * 1e4;
            ++mo_n;
          }
        }
        if (f.side == Side::Buy) r.buy_eth += q; else r.sell_eth += q;
        if (fcsv) {
          std::fprintf(fcsv, "%s,%.4f,%.4f,%lld,%s,%.1f,%.6f,%d,%d\n", day.c_str(),
                       r.latency_ms, r.gamma, static_cast<long long>(f.ts_us / 1000),
                       f.side == Side::Buy ? "buy" : "sell", px, q, f.swept ? 1 : 0,
                       f.taker ? 1 : 0);
        }
      }
      r.mo_n = mo_n;
      if (mo_n) { r.mo1 = mo1 / mo_n; r.mo5 = mo5 / mo_n; r.mo30 = mo30 / mo_n; }

      // Inventory is flat after the forced close, so cash is the whole result.
      r.gross_pnl = ag.cash();
      r.net_pnl = r.gross_pnl - r.fees;
      const double notional = r.maker_notional + r.taker_notional;
      r.edge_bps = notional > 0 ? r.net_pnl / notional * 1e4 : 0.0;
      r.max_abs_inv = static_cast<double>(ag.max_abs_inventory()) / kQtyScale;
      r.end_inv = static_cast<double>(ag.inventory()) / kQtyScale;

      if (!a.quiet) {
        std::printf("%-12s %8.2f %6.2f %4d %4d %8llu %6.1f%% %9.0f %9.4f %9.4f %9.4f %9.4f\n",
                    day.c_str(), r.latency_ms, r.gamma, static_cast<int>(r.offset),
                    static_cast<int>(r.requote),
                    static_cast<unsigned long long>(r.maker_fills),
                    r.maker_fills ? 100.0 * r.swept_fills / r.maker_fills : 0.0, notional,
                    r.edge_bps, r.mo1, r.mo5, r.mo30);
      }
      if (csv) {
        std::fprintf(csv,
                     "%s,%.4f,%.6f,%d,%.8f,%.4f,%d,%llu,%llu,%llu,%.4f,%.4f,%.6f,%.6f,%.6f,"
                     "%.8f,%.8f,%.8f,%.8f,%llu,%.6f,%.6f,%.6f,%.6f,%.4f,%.4f\n",
                     day.c_str(), r.latency_ms, r.gamma, static_cast<int>(r.offset), r.beta,
                     r.kappa, static_cast<int>(r.requote), static_cast<unsigned long long>(r.maker_fills),
                     static_cast<unsigned long long>(r.swept_fills),
                     static_cast<unsigned long long>(r.requotes), r.maker_notional,
                     r.taker_notional, r.gross_pnl, r.fees, r.net_pnl, r.edge_bps, r.mo1,
                     r.mo5, r.mo30, static_cast<unsigned long long>(r.mo_n), r.max_abs_inv,
                     r.end_inv, r.buy_eth, r.sell_eth, a.fees.maker_bps, a.fees.taker_bps);
      }
      all.push_back(r);
    }
  }
  if (csv) std::fclose(csv);
  if (fcsv) std::fclose(fcsv);

  // Pooled summary per agent config, so a sweep is readable without the csv.
  std::printf("\npooled over %zu days\n", days.size());
  std::printf("%8s %6s %4s %4s %8s %6s %9s %7s %12s %9s %9s %9s %9s\n", "lat(ms)", "gamma", "off",
              "rq", "beta", "kappa", "fills", "swept%", "notional", "net_pnl", "edge_bps", "mo5s", "mo30s");
  for (std::size_t ci = 0; ci < configs.size(); ++ci) {
    double notional = 0, net = 0, mo5 = 0, mo30 = 0;
    std::uint64_t fills = 0, mo_n = 0, swept = 0;
    for (const Row& r : all) {
      if (r.latency_ms != static_cast<double>(configs[ci].latency_us) / 1000.0) continue;
      if (r.gamma != configs[ci].gamma) continue;
      if (static_cast<int>(r.offset) != configs[ci].offset_ticks) continue;
      if (r.beta != configs[ci].beta) continue;
      if (r.kappa != configs[ci].kappa) continue;
      if (static_cast<int>(r.requote) != configs[ci].requote_ticks) continue;
      notional += r.maker_notional + r.taker_notional;
      net += r.net_pnl;
      fills += r.maker_fills;
      swept += r.swept_fills;
      mo5 += r.mo5 * r.mo_n;
      mo30 += r.mo30 * r.mo_n;
      mo_n += r.mo_n;
    }
    std::printf("%8.2f %6.2f %4d %4d %8.5f %6.2f %9llu %6.1f%% %12.0f %9.2f %9.4f %9.4f %9.4f\n",
                static_cast<double>(configs[ci].latency_us) / 1000.0, configs[ci].gamma,
                configs[ci].offset_ticks, configs[ci].requote_ticks, configs[ci].beta,
                configs[ci].kappa, static_cast<unsigned long long>(fills),
                fills ? 100.0 * swept / fills : 0.0, notional, net,
                notional > 0 ? net / notional * 1e4 : 0.0, mo_n ? mo5 / mo_n : 0.0,
                mo_n ? mo30 / mo_n : 0.0);
  }
  return 0;
}
