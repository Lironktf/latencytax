// Matching engine benchmark.
//
// Three measurements, reported separately because they answer different
// questions.
//
//   per operation latency
//     Every book call is bracketed by rdtscp with lfence on both sides. The
//     fences stop the CPU reordering work across the measurement, which is what
//     makes a per call number meaningful, and they also make it pessimistic:
//     the cost of the timer pair is measured during warm up and subtracted, but
//     the serialisation it forces is not removable. Treat these as an upper
//     bound.
//
//   single thread throughput
//     The same flow with no timers in the loop. Commands are generated into a
//     block first, then the block is applied under one wall clock reading, so
//     generation is not counted. This is the honest cost per message.
//
//   queue pipeline
//     A feed thread pushes commands into the lock free ring on one core and the
//     matcher pops and applies them on another. Reports end to end throughput
//     and how long a message sits in the ring.
//
// The synthetic flow is shaped like a liquid futures book: most orders arrive
// near the touch, most are cancelled rather than filled, and a small fraction
// are marketable.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "engine/engine.hpp"
#include "util/affinity.hpp"
#include "util/timing.hpp"

using namespace ltx;

namespace {

struct Args {
  double seconds = 30.0;
  int core = -1;
  int feed_core = -1;
  int depth_levels = 40;
  std::uint64_t seed = 20260914;
  std::size_t block = 1u << 20;
  double pct_add = 45.0;
  double pct_cancel = 40.0;
  double pct_modify = 10.0;
  double pct_marketable = 5.0;
  std::size_t max_live = 200000;
  bool csv = false;
};

void usage() {
  std::printf(
      "bench - matching engine throughput and latency\n"
      "\n"
      "usage: bench [options]\n"
      "  --seconds=N        timed work per phase, default 30\n"
      "  --core=N           pin the matcher to this cpu (default: leave alone)\n"
      "  --feed-core=N      pin the feed thread to this cpu\n"
      "  --levels=N         price levels the synthetic flow spans, default 40\n"
      "  --block=N          commands generated per block, default 1048576\n"
      "  --max-live=N       steady state resting orders, default 200000\n"
      "  --mix=a,c,m,t      percent add, cancel, modify, marketable (default 45,40,10,5)\n"
      "  --seed=N           flow generator seed\n"
      "  --csv              also print the table as csv\n"
      "  --help\n");
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto num = [&](const char* k, double& out) {
      if (s.rfind(k, 0) == 0) { out = std::atof(s.c_str() + std::strlen(k)); return true; }
      return false;
    };
    double d = 0;
    if (s == "--help" || s == "-h") { usage(); return false; }
    else if (num("--seconds=", a.seconds)) {}
    else if (num("--core=", d)) a.core = static_cast<int>(d);
    else if (num("--feed-core=", d)) a.feed_core = static_cast<int>(d);
    else if (num("--levels=", d)) a.depth_levels = static_cast<int>(d);
    else if (num("--block=", d)) a.block = static_cast<std::size_t>(d);
    else if (num("--max-live=", d)) a.max_live = static_cast<std::size_t>(d);
    else if (num("--seed=", d)) a.seed = static_cast<std::uint64_t>(d);
    else if (s == "--csv") a.csv = true;
    else if (s.rfind("--mix=", 0) == 0) {
      if (std::sscanf(s.c_str() + 6, "%lf,%lf,%lf,%lf", &a.pct_add, &a.pct_cancel,
                      &a.pct_modify, &a.pct_marketable) != 4) {
        std::fprintf(stderr, "bad --mix\n");
        return false;
      }
    } else {
      std::fprintf(stderr, "unknown argument %s\n", s.c_str());
      usage();
      return false;
    }
  }
  return true;
}

enum class OpKind : int { AddRest = 0, AddMarketable, Cancel, Modify, Count };
const char* kOpName[] = {"add (rests)", "add (marketable)", "cancel", "modify"};

// Generates a stream of commands against its own model of which orders it has
// sent, so cancels and modifies name real ids without the generator ever
// reading engine state. Some of them will already have been filled by the time
// they are cancelled; the engine rejects those, which is what happens on a real
// venue too.
class FlowGen {
 public:
  FlowGen(const Args& a, Tick mid) : a_(a), rng_(a.seed), mid_(mid) {
    live_.reserve(a.max_live * 2);
  }

  void fill(Command* out, OpKind* kinds, std::size_t n) {
    const double tot = a_.pct_add + a_.pct_cancel + a_.pct_modify + a_.pct_marketable;
    const double c1 = a_.pct_add / tot;
    const double c2 = c1 + a_.pct_cancel / tot;
    const double c3 = c2 + a_.pct_modify / tot;
    for (std::size_t i = 0; i < n; ++i) {
      const bool over = live_.size() >= a_.max_live;
      const double u = uniform();
      if (live_.size() < 1024) {
        emit_add(out[i], kinds[i], false);
      } else if (over && u < c2) {
        emit_cancel(out[i], kinds[i]);
      } else if (u < c1) {
        emit_add(out[i], kinds[i], false);
      } else if (u < c2) {
        emit_cancel(out[i], kinds[i]);
      } else if (u < c3) {
        emit_modify(out[i], kinds[i]);
      } else {
        emit_add(out[i], kinds[i], true);
      }
      // A slow random walk in the mid keeps the hot region of the level array
      // moving instead of sitting on one cache line for the whole run.
      if ((rng_() & 2047) == 0) mid_ += (rng_() & 1) ? 1 : -1;
    }
  }

  std::size_t live() const { return live_.size(); }
  Tick mid() const { return mid_; }

 private:
  struct LiveOrder {
    OrderId id;
    Tick tick;
    Qty qty;
    Side side;
  };

  double uniform() { return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0); }

  // Most orders land within a couple of ticks of the touch.
  Tick offset() {
    const double u = uniform();
    const int lv = static_cast<int>(-std::log(1.0 - u * 0.999) * 3.0);
    return static_cast<Tick>(lv < a_.depth_levels ? lv : a_.depth_levels - 1);
  }

  Qty qty() { return static_cast<Qty>(1 + (rng_() % 500)) * (kQtyScale / 100); }

  void emit_add(Command& c, OpKind& k, bool marketable) {
    const Side side = (rng_() & 1) ? Side::Buy : Side::Sell;
    const Qty q = qty();
    if (marketable) {
      const Tick t = side == Side::Buy ? mid_ + 1 + static_cast<Tick>(rng_() % 3)
                                       : mid_ - 1 - static_cast<Tick>(rng_() % 3);
      k = OpKind::AddMarketable;
      c = Command{0, next_id_++, q, t, CmdType::AddLimit, side, Tif::Ioc, 0};
      return;
    }
    const Tick off = offset();
    const Tick t = side == Side::Buy ? mid_ - 1 - off : mid_ + 1 + off;
    const OrderId id = next_id_++;
    live_.push_back(LiveOrder{id, t, q, side});
    k = OpKind::AddRest;
    c = Command{0, id, q, t, CmdType::AddLimit, side, Tif::Gtc, 0};
  }

  void emit_cancel(Command& c, OpKind& k) {
    const std::size_t i = rng_() % live_.size();
    const LiveOrder o = live_[i];
    live_[i] = live_.back();
    live_.pop_back();
    k = OpKind::Cancel;
    c = Command{0, o.id, 0, o.tick, CmdType::Cancel, o.side, Tif::Gtc, 0};
  }

  void emit_modify(Command& c, OpKind& k) {
    const std::size_t i = rng_() % live_.size();
    LiveOrder& o = live_[i];
    k = OpKind::Modify;
    if ((rng_() % 3) != 0) {
      // Size down at the same price: the path that keeps queue position.
      const Qty nq = o.qty > kQtyScale / 100 ? o.qty / 2 : o.qty;
      o.qty = nq;
      c = Command{0, o.id, nq, o.tick, CmdType::Modify, o.side, Tif::Gtc, 0};
    } else {
      // Reprice by a tick: cancel and replace at the back of the new queue.
      const Tick nt = o.tick + ((rng_() & 1) ? 1 : -1);
      const Tick lo = o.side == Side::Buy ? mid_ - a_.depth_levels - 1 : mid_ + 1;
      const Tick hi = o.side == Side::Buy ? mid_ - 1 : mid_ + a_.depth_levels + 1;
      o.tick = nt < lo ? lo : (nt > hi ? hi : nt);
      c = Command{0, o.id, o.qty, o.tick, CmdType::Modify, o.side, Tif::Gtc, 0};
    }
  }

  Args a_;
  std::mt19937_64 rng_;
  Tick mid_;
  OrderId next_id_ = 1;
  std::vector<LiveOrder> live_;
};

double now_s() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration<double>(clock::now() - t0).count();
}

BookConfig bench_book_cfg() {
  BookConfig c;
  c.min_tick = 1;
  c.max_tick = 262144;
  c.max_orders = 1u << 22;
  c.id_map_capacity = 1u << 23;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 1;

  if (a.core >= 0 && !pin_to_core(a.core)) {
    std::fprintf(stderr, "warning: could not pin to core %d\n", a.core);
  }

  const double ghz = measure_tsc_ghz(300);
  std::printf("host: %d cpus, tsc %.4f GHz (measured against CLOCK_MONOTONIC)\n",
              static_cast<int>(std::thread::hardware_concurrency()), ghz);

  // Cost of the rdtscp/lfence pair with nothing between it.
  CycleHist timer_cost;
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t t0 = rdtsc_begin();
    const std::uint64_t t1 = rdtsc_end();
    timer_cost.add(t1 - t0);
  }
  const double overhead = timer_cost.pct_cycles(50);
  std::printf("timer pair cost: p50 %.0f cycles (%.1f ns), subtracted from the latency table\n\n",
              overhead, overhead / ghz);

  const Tick mid = 19160;
  std::vector<Command> block(a.block);
  std::vector<OpKind> kinds(a.block);

  // ---- phase 1: per operation latency -------------------------------------
  {
    CountingSink sink;
    MatchingEngine eng(bench_book_cfg(), &sink);
    FlowGen gen(a, mid);
    CycleHist hist[static_cast<int>(OpKind::Count)];
    CycleHist all;

    // Warm up: build the steady state book and touch the pages before
    // anything is recorded.
    for (int w = 0; w < 6; ++w) {
      gen.fill(block.data(), kinds.data(), a.block);
      for (std::size_t i = 0; i < a.block; ++i) eng.apply(block[i]);
    }

    const double t_start = now_s();
    std::uint64_t ops = 0;
    while (now_s() - t_start < a.seconds) {
      gen.fill(block.data(), kinds.data(), a.block);
      for (std::size_t i = 0; i < a.block; ++i) {
        const std::uint64_t t0 = rdtsc_begin();
        eng.apply(block[i]);
        const std::uint64_t t1 = rdtsc_end();
        const std::uint64_t d = t1 - t0;
        hist[static_cast<int>(kinds[i])].add(d);
        all.add(d);
        ++ops;
      }
    }

    std::printf("per operation latency, matcher pinned%s, %.0f s of timed work\n",
                a.core >= 0 ? "" : " (NOT PINNED)", a.seconds);
    std::printf("%-20s %12s %9s %9s %9s %9s %9s\n", "operation", "count", "p50", "p90",
                "p99", "p99.9", "max");
    auto row = [&](const char* name, const CycleHist& h) {
      if (!h.count()) return;
      std::printf("%-20s %12llu %8.0fns %8.0fns %8.0fns %8.0fns %8.1fus\n", name,
                  static_cast<unsigned long long>(h.count()),
                  to_ns(h.pct_cycles(50), ghz, overhead),
                  to_ns(h.pct_cycles(90), ghz, overhead),
                  to_ns(h.pct_cycles(99), ghz, overhead),
                  to_ns(h.pct_cycles(99.9), ghz, overhead),
                  to_ns(static_cast<double>(h.max_cycles()), ghz, overhead) / 1000.0);
    };
    for (int k = 0; k < static_cast<int>(OpKind::Count); ++k) row(kOpName[k], hist[k]);
    row("all", all);
    std::printf("book at end: %llu resting orders (generator target %llu), best bid %d best ask %d\n",
                static_cast<unsigned long long>(eng.book().live_orders()),
                static_cast<unsigned long long>(a.max_live),
                eng.book().best_bid(), eng.book().best_ask());
    std::printf("events: %llu trades, %llu accepts, %llu cancels, %llu rejects\n\n",
                static_cast<unsigned long long>(sink.trades),
                static_cast<unsigned long long>(sink.accepts),
                static_cast<unsigned long long>(sink.cancels),
                static_cast<unsigned long long>(sink.rejects));
    if (a.csv) {
      std::printf("csv,phase,op,count,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n");
      for (int k = 0; k < static_cast<int>(OpKind::Count); ++k) {
        if (!hist[k].count()) continue;
        std::printf("csv,latency,%s,%llu,%.1f,%.1f,%.1f,%.1f,%.1f\n", kOpName[k],
                    static_cast<unsigned long long>(hist[k].count()),
                    to_ns(hist[k].pct_cycles(50), ghz, overhead),
                    to_ns(hist[k].pct_cycles(90), ghz, overhead),
                    to_ns(hist[k].pct_cycles(99), ghz, overhead),
                    to_ns(hist[k].pct_cycles(99.9), ghz, overhead),
                    to_ns(static_cast<double>(hist[k].max_cycles()), ghz, overhead));
      }
      std::printf("csv,latency,all,%llu,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                  static_cast<unsigned long long>(all.count()),
                  to_ns(all.pct_cycles(50), ghz, overhead),
                  to_ns(all.pct_cycles(90), ghz, overhead),
                  to_ns(all.pct_cycles(99), ghz, overhead),
                  to_ns(all.pct_cycles(99.9), ghz, overhead),
                  to_ns(static_cast<double>(all.max_cycles()), ghz, overhead));
    }
    (void)ops;
  }

  // ---- phase 2: throughput with no timers in the loop ----------------------
  double single_thread_mps = 0;
  {
    CountingSink sink;
    MatchingEngine eng(bench_book_cfg(), &sink);
    FlowGen gen(a, mid);
    for (int w = 0; w < 6; ++w) {
      gen.fill(block.data(), kinds.data(), a.block);
      for (std::size_t i = 0; i < a.block; ++i) eng.apply(block[i]);
    }

    std::uint64_t applied = 0;
    double timed = 0;
    while (timed < a.seconds) {
      // Generation happens outside the clock, so the timed loop is nothing but
      // engine work.
      gen.fill(block.data(), kinds.data(), a.block);
      const double t0 = now_s();
      for (std::size_t i = 0; i < a.block; ++i) eng.apply(block[i]);
      timed += now_s() - t0;
      applied += a.block;
    }
    single_thread_mps = applied / timed / 1e6;
    std::printf("single thread throughput: %.2f M msg/s (%.1f ns/msg), %llu messages in %.1f s\n",
                single_thread_mps, timed / applied * 1e9,
                static_cast<unsigned long long>(applied), timed);
    std::printf("  resting orders at end: %llu, trades: %llu\n\n",
                static_cast<unsigned long long>(eng.book().live_orders()),
                static_cast<unsigned long long>(sink.trades));
  }

  // ---- phase 3: feed thread -> ring -> matcher ------------------------------
  {
    CommandRing ring(1u << 16);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> produced{0};
    CountingSink sink;
    MatchingEngine eng(bench_book_cfg(), &sink);

    const double seconds = a.seconds;
    std::thread feed([&] {
      if (a.feed_core >= 0) pin_to_core(a.feed_core);
      FlowGen gen(a, mid);
      std::vector<Command> b(65536);
      std::vector<OpKind> k(65536);
      std::uint64_t n = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        gen.fill(b.data(), k.data(), b.size());
        for (std::size_t i = 0; i < b.size(); ++i) {
          b[i].ts = static_cast<Ts>(rdtsc_begin());
          while (!ring.push(b[i])) {
            if (stop.load(std::memory_order_relaxed)) { produced.store(n); return; }
          }
          ++n;
        }
      }
      produced.store(n);
    });

    CycleHist transit;
    std::uint64_t consumed = 0;
    const double t0 = now_s();
    Command c{};
    while (now_s() - t0 < seconds) {
      for (int i = 0; i < 4096; ++i) {
        if (ring.pop(c)) {
          const std::uint64_t arrive = rdtsc_end();
          transit.add(arrive - static_cast<std::uint64_t>(c.ts));
          c.ts = 0;
          eng.apply(c);
          ++consumed;
        }
      }
    }
    const double elapsed = now_s() - t0;
    stop.store(true);
    while (ring.pop(c)) {
    }
    feed.join();

    std::printf("feed thread -> spsc ring -> matcher%s\n",
                (a.core >= 0 && a.feed_core >= 0) ? ", both pinned" : " (NOT PINNED)");
    std::printf("  end to end: %.2f M msg/s over %.1f s (%llu messages)\n",
                consumed / elapsed / 1e6, elapsed,
                static_cast<unsigned long long>(consumed));
    std::printf("  ring transit: p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns  max %.1f us\n",
                to_ns(transit.pct_cycles(50), ghz, overhead),
                to_ns(transit.pct_cycles(99), ghz, overhead),
                to_ns(transit.pct_cycles(99.9), ghz, overhead),
                to_ns(static_cast<double>(transit.max_cycles()), ghz, overhead) / 1000.0);
    std::printf("  (transit includes the queue wait, so it tracks how far the feed runs\n"
                "   ahead of the matcher, not just the handoff cost)\n");
    if (a.csv) {
      std::printf("csv,throughput,single_thread_mps,%.4f\n", single_thread_mps);
      std::printf("csv,throughput,pipeline_mps,%.4f\n", consumed / elapsed / 1e6);
    }
  }
  return 0;
}
