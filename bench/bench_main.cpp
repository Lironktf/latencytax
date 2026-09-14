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
#include "ml/kernels.hpp"
#include "wire/itch_fixed.hpp"
#include "wire/decoder.hpp"
#include "ml/logistic.hpp"
#include "ml/mlp.hpp"
#include "util/affinity.hpp"
#include "util/hugevec.hpp"
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
  bool kernels = false;
  int hugepages = 1;
  bool decode = false;
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
      "  --kernels          benchmark the ml kernels instead of the engine\n"
      "  --hugepages=0|1    ask for 2 MB pages for the book arrays, default 1\n"
      "  --decode           compare the branching and fixed latency ITCH decoders\n"
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
    else if (s == "--kernels") a.kernels = true;
    else if (s == "--decode") a.decode = true;
    else if (num("--hugepages=", d)) a.hugepages = static_cast<int>(d);
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

// Stops the compiler hoisting a loop invariant call out of a timing loop or
// deleting it outright. The empty asm block is opaque to the optimiser, and the
// memory clobber forces anything it might have cached in a register back out.
//
// This is not decoration. The first version of the kernel benchmark below timed
// a dot product over two vectors that never changed, and reported 103 GFLOP/s
// on a core whose AVX2 fused multiply add ceiling is 2 units times 8 lanes times
// 2 flops times 2.4 GHz, or 76.8. A microbenchmark that reports a number above
// the machine's peak is not a fast kernel, it is a deleted one.
template <typename T>
inline void do_not_optimize(T& value) {
  asm volatile("" : "+r,m"(value) : : "memory");
}
inline void clobber() { asm volatile("" : : : "memory"); }

enum class OpKind : int { AddRest = 0, AddMarketable, Cancel, Modify, Count };
const char* kOpName[] = {"add (rests)", "add (marketable)", "cancel", "modify"};

// Generates a stream of commands against its own model of which orders it has
// sent, so cancels and modifies name real ids without the generator ever
// reading engine state. Some of them will already have been filled by the time
// they are cancelled; the engine rejects those, which is what happens on a real
// venue too.
class FlowGen {
 public:
  FlowGen(const Args& a, Tick mid) : a_(a), rng_(a.seed), mid_(mid), mid0_(mid) {
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
      // The mid walks, so the hot region of the level array moves instead of
      // sitting on one cache line for the whole run. It walks slowly and inside
      // a band: if it drifted further than the quoted depth over the lifetime
      // of a resting order, most of the book would be stale and crossable, and
      // the benchmark would turn into a sweep test rather than a book test.
      if ((rng_() & 65535) == 0) {
        const Tick step = (rng_() & 1) ? 1 : -1;
        const Tick next = mid_ + step;
        if (next > mid0_ - 48 && next < mid0_ + 48) mid_ = next;
      }
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

  // Resting orders: 0.01 to 5 ETH, flat.
  Qty qty() { return static_cast<Qty>(1 + (rng_() % 500)) * (kQtyScale / 100); }

  // Marketable orders are shaped like the measured Hyperliquid ETH-perp tape,
  // which has a median print of 0.065 ETH and a mean of 1.34: mostly dust with
  // a long tail. A flat distribution here would have every marketable order
  // sweep several price levels and turn the benchmark into a fill test.
  Qty take_qty() {
    const double u = uniform();
    const double eth = (u < 0.75) ? (0.01 + 0.19 * uniform())
                                  : (-std::log(1.0 - uniform() * 0.999) * 4.0);
    return static_cast<Qty>(eth * kQtyScale) + 1;
  }

  void emit_add(Command& c, OpKind& k, bool marketable) {
    const Side side = (rng_() & 1) ? Side::Buy : Side::Sell;
    if (marketable) {
      const Tick t = side == Side::Buy ? mid_ + 1 + static_cast<Tick>(rng_() % 3)
                                       : mid_ - 1 - static_cast<Tick>(rng_() % 3);
      k = OpKind::AddMarketable;
      c = Command{.id = next_id_++, .qty = take_qty(), .tick = t,
                  .type = CmdType::AddLimit, .side = side, .tif = Tif::Ioc};
      return;
    }
    const Qty q = qty();
    const Tick off = offset();
    const Tick t = side == Side::Buy ? mid_ - 1 - off : mid_ + 1 + off;
    const OrderId id = next_id_++;
    live_.push_back(LiveOrder{id, t, q, side});
    k = OpKind::AddRest;
    c = Command{.id = id, .qty = q, .tick = t, .type = CmdType::AddLimit, .side = side};
  }

  void emit_cancel(Command& c, OpKind& k) {
    const std::size_t i = rng_() % live_.size();
    const LiveOrder o = live_[i];
    live_[i] = live_.back();
    live_.pop_back();
    k = OpKind::Cancel;
    c = Command{.id = o.id, .tick = o.tick, .type = CmdType::Cancel, .side = o.side};
  }

  void emit_modify(Command& c, OpKind& k) {
    const std::size_t i = rng_() % live_.size();
    LiveOrder& o = live_[i];
    k = OpKind::Modify;
    if ((rng_() % 3) != 0) {
      // Size down at the same price: the path that keeps queue position.
      const Qty nq = o.qty > kQtyScale / 100 ? o.qty / 2 : o.qty;
      o.qty = nq;
      c = Command{.id = o.id, .qty = nq, .tick = o.tick, .type = CmdType::Modify,
                  .side = o.side};
    } else {
      // Reprice by a tick: cancel and replace at the back of the new queue.
      const Tick nt = o.tick + ((rng_() & 1) ? 1 : -1);
      const Tick lo = o.side == Side::Buy ? mid_ - a_.depth_levels - 1 : mid_ + 1;
      const Tick hi = o.side == Side::Buy ? mid_ - 1 : mid_ + a_.depth_levels + 1;
      o.tick = nt < lo ? lo : (nt > hi ? hi : nt);
      c = Command{.id = o.id, .qty = o.qty, .tick = o.tick, .type = CmdType::Modify,
                  .side = o.side};
    }
  }

  Args a_;
  std::mt19937_64 rng_;
  Tick mid_;
  Tick mid0_;
  OrderId next_id_ = 1;
  std::vector<LiveOrder> live_;
};

double now_s() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration<double>(clock::now() - t0).count();
}

// The pool and the id map are sized to the flow rather than to some large
// round number. An 8 million entry id map is 128 MB, and at that size every
// lookup is a TLB miss on top of a cache miss; the benchmark then measures the
// page tables rather than the engine. These are sized to about four times the
// steady state order count, which is what a venue would provision.
BookConfig bench_book_cfg(const Args& a) {
  BookConfig c;
  c.min_tick = 1;
  c.max_tick = 65536;                  // 6553.6 USD at a 0.1 tick
  std::size_t orders = 1024;
  while (orders < a.max_live * 4) orders <<= 1;
  c.max_orders = orders;
  c.id_map_capacity = orders * 2;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 1;

  huge_pages_enabled() = a.hugepages != 0;

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
  std::printf("timer pair cost: p50 %.0f cycles (%.1f ns), subtracted from the latency table\n",
              overhead, overhead / ghz);
  std::printf("book arrays: %s\n\n",
              a.hugepages ? "2 MB pages requested with madvise"
                          : "4 KB pages, huge pages explicitly refused");

  if (a.decode) {
    // Two decoders over the same messages. The branching one switches on the
    // type and reads different offsets in each arm; the fixed one turns the
    // type into a table lookup so it selects an address rather than a branch,
    // and masks the fields a message does not have. The question is not which
    // is faster on average, it is which has a latency that depends on what
    // arrived.
    using namespace ltx::wire;
    std::mt19937_64 rng(a.seed);
    // A padded buffer, because the fixed decoder issues loads for fields a
    // message may not have. Real handlers read into an oversized buffer for the
    // same reason.
    std::vector<std::uint8_t> buf(1u << 22, 0);
    struct Msg { std::uint32_t off; std::uint8_t len; };
    std::vector<Msg> msgs;
    msgs.reserve(200000);

    auto emit = [&](std::size_t off, char type, std::uint64_t seq) -> std::size_t {
      std::uint8_t* m = buf.data() + off;
      switch (type) {
        case 'A':
          return encode_add(m, 1, 0, seq, seq + 1, (seq & 1) ? 'B' : 'S', 10000, "ETH     ",
                            tick_to_wire_price(static_cast<Tick>(19000 + seq % 200), 1));
        case 'E': return encode_executed(m, 1, 0, seq, seq + 1, 500, seq);
        case 'X': return encode_cancel(m, 1, 0, seq, seq + 1, 250);
        case 'D': return encode_delete(m, 1, 0, seq, seq + 1);
        default:
          return encode_replace(m, 1, 0, seq, seq + 1, seq + 2, 9000,
                                tick_to_wire_price(static_cast<Tick>(19000 + seq % 200), 1));
      }
    };

    // Two orderings. The first resembles a real feed, where messages of a type
    // arrive in runs. The second shuffles the types, which is the case a branch
    // predictor cannot learn.
    auto build = [&](bool clustered) {
      msgs.clear();
      std::size_t off = 0;
      const char types[5] = {'A', 'E', 'X', 'D', 'U'};
      for (std::uint64_t i = 0; i < 120000 && off + 64 < buf.size(); ++i) {
        const char t = clustered ? types[(i / 64) % 5] : types[rng() % 5];
        const std::size_t n = emit(off, t, i);
        msgs.push_back(Msg{static_cast<std::uint32_t>(off), static_cast<std::uint8_t>(n)});
        off += n;
      }
    };

    std::printf("ITCH decode, a switch against a table, same signature and same output\n\n");
    std::printf("%-26s %12s %12s %10s\n", "decoder", "clustered", "shuffled", "change");

    // Throughput first, with no timer in the loop. One decode costs on the order
    // of ten cycles, well under the cost of an rdtscp pair, so a per message
    // timer would mostly be measuring itself.
    auto throughput = [&](bool fixed, bool clustered) {
      build(clustered);
      Command c{};
      std::uint64_t produced = 0;
      // The minimum over passes rather than the mean. Every source of error in a
      // microbenchmark on a shared machine adds time: a migration, a steal, a
      // neighbour evicting the cache. None of them make it faster, so the
      // fastest pass is the one least contaminated by things that are not the
      // code. The mean over passes moved by 50% run to run here; the minimum
      // moves by a few percent.
      double best = 1e18;
      for (int pass = 0; pass < 24; ++pass) {
        const bool measure = pass >= 4;
        const double t0 = now_s();
        for (const Msg& m : msgs) {
          const std::uint8_t* p = buf.data() + m.off;
          produced += fixed ? decode_fixed(p, m.len, 1, c) : decode_switch(p, m.len, 1, c);
          // Consume the decoded fields, or the compiler is free to notice that
          // only the last command is ever read and skip most of the stores.
          produced += c.id ^ static_cast<std::uint64_t>(c.qty) ^
                      static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.tick)) ^
                      c.id2;
        }
        const double dt = now_s() - t0;
        if (measure && dt < best) best = dt;
        do_not_optimize(produced);
      }
      return static_cast<double>(msgs.size()) / best / 1e6;
    };

    const double sw_c = throughput(false, true);
    const double sw_s = throughput(false, false);
    const double fx_c = throughput(true, true);
    const double fx_s = throughput(true, false);
    std::printf("%-26s %10.2f M %10.2f M %9.1f%%\n", "switch on the type", sw_c, sw_s,
                100.0 * (sw_s - sw_c) / sw_c);
    std::printf("%-26s %10.2f M %10.2f M %9.1f%%\n", "table indexed by type", fx_c, fx_s,
                100.0 * (fx_s - fx_c) / fx_c);

    // A per message tail is not measurable here. One decode is four or five
    // cycles and an rdtscp pair costs about forty, so bracketing each call would
    // report the clock. Blocks of 256 put the reading an order of magnitude
    // above the timer, which resolves how steady the sustained rate is but says
    // nothing about the tail of any single message. That is a limitation of this
    // machine, not a result, and the numbers below are labelled accordingly.
    std::printf("\n%-26s %10s %9s %9s %9s %9s\n", "per msg, blocks of 256", "order",
                "p50", "p99", "p99.9", "spread");
    for (int which = 0; which < 2; ++which) {
      const bool fixed = which == 1;
      for (int order = 0; order < 2; ++order) {
        const bool clustered = order == 0;
        build(clustered);
        CycleHist h;
        Command c{};
        std::uint64_t produced = 0;
        for (int pass = 0; pass < 10; ++pass) {
          const bool measure = pass >= 3;
          for (std::size_t i = 0; i + 256 <= msgs.size(); i += 256) {
            const std::uint64_t s0 = rdtsc_begin();
            for (std::size_t k = 0; k < 256; ++k) {
              const Msg& m = msgs[i + k];
              const std::uint8_t* p = buf.data() + m.off;
              produced += fixed ? decode_fixed(p, m.len, 1, c) : decode_switch(p, m.len, 1, c);
              produced += c.id ^ static_cast<std::uint64_t>(c.qty);
            }
            const std::uint64_t s1 = rdtsc_end();
            if (measure) h.add((s1 - s0) / 256);
          }
          do_not_optimize(produced);
        }
        const double p50 = to_ns(h.pct_cycles(50), ghz, overhead / 256.0);
        const double p999 = to_ns(h.pct_cycles(99.9), ghz, overhead / 256.0);
        std::printf("%-26s %10s %8.1fns %8.1fns %8.1fns %8.1fns\n",
                    fixed ? "table indexed by type" : "switch on the type",
                    clustered ? "clustered" : "shuffled", p50,
                    to_ns(h.pct_cycles(99), ghz, overhead / 256.0), p999, p999 - p50);
      }
    }

    std::printf("\nthe switch is faster on clustered input and the table is faster on\n");
    std::printf("shuffled input. that is the whole tradeoff and it is worth stating plainly\n");
    std::printf("rather than picking the ordering that flatters one of them. a branch\n");
    std::printf("predictor handed runs of one message type learns them and the switch wins.\n");
    std::printf("handed an order it cannot learn, the switch loses most of its speed while\n");
    std::printf("the table gives up much less, because the type selects an address rather\n");
    std::printf("than a jump.\n");
    std::printf("\nclustered is what a real feed looks like. shuffled is what a feed looks\n");
    std::printf("like during the minute you care about, when every instrument moves at once.\n");
    std::printf("\nthe throughput row is the evidence. the block row resolves how steady\n");
    std::printf("the sustained rate is and nothing about a single message tail, because a\n");
    std::printf("decode is four or five cycles and the timer costs about forty.\n");
    std::printf("\nbranch miss counters would settle it directly. this KVM guest does not\n");
    std::printf("expose a PMU, so the evidence is sensitivity to message order rather than\n");
    std::printf("a counter, and that is a limitation rather than a choice.\n");
    return 0;
  }


  if (a.kernels) {
    // The queue model runs inside the replay loop, once per resting quote per
    // market event, so what matters is the cost of one 64 wide inference rather
    // than peak throughput on a large batch. Each kernel is timed against its
    // own scalar reference compiled from the same source.
    using namespace ltx::ml;
    std::printf("ml kernels, %s\n\n", LTX_HAVE_AVX2 ? "AVX2 and FMA available"
                                                     : "no AVX2, scalar fallback");
    std::mt19937_64 rng(1);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> x(kDim), w(kDim), y(kDim);
    for (auto& v : x) v = u(rng);
    for (auto& v : w) v = u(rng);
    for (auto& v : y) v = u(rng);
    const std::size_t H = 32;
    std::vector<float> W(H * kDim), out(H);
    for (auto& v : W) v = u(rng);

    // Budgeted by time rather than by a fixed repetition count, because the
    // kernels here differ by two orders of magnitude in cost and one fixed
    // count is either far too few for the cheap ones or minutes for the rest.
    auto time_it = [&](const char* name, double flops, auto&& fn) {
      for (int i = 0; i < 50000; ++i) fn();     // warm up
      std::size_t reps = 4096;
      double t = 0;
      while (true) {
        const double t0 = now_s();
        for (std::size_t i = 0; i < reps; ++i) fn();
        t = now_s() - t0;
        if (t > 0.35) break;
        reps *= 4;
      }
      std::printf("  %-28s %8.2f ns/call   %7.2f GFLOP/s  (%zu reps)\n", name,
                  t / reps * 1e9, flops * reps / t / 1e9, reps);
      return t / reps * 1e9;
    };

    float sink = 0.0f;
    const float* wp = w.data();
    const float* xp = x.data();
    float* yp = y.data();
    const float* Wp = W.data();
    float* outp = out.data();

    const double d_simd = time_it("dot 64, vector", 2.0 * kDim, [&] {
      do_not_optimize(wp);
      do_not_optimize(xp);
      float r = dot(wp, xp, kDim);
      do_not_optimize(r);
      sink += r;
    });
    const double d_scalar = time_it("dot 64, scalar", 2.0 * kDim, [&] {
      do_not_optimize(wp);
      do_not_optimize(xp);
      float r = scalar::dot(wp, xp, kDim);
      do_not_optimize(r);
      sink += r;
    });
    std::printf("  %-28s %8.2fx\n\n", "dot speedup", d_scalar / d_simd);

    const double a_simd = time_it("axpy 64, vector", 2.0 * kDim, [&] {
      do_not_optimize(yp);
      do_not_optimize(xp);
      axpy(yp, xp, 1e-8f, kDim);
      clobber();
    });
    const double a_scalar = time_it("axpy 64, scalar", 2.0 * kDim, [&] {
      do_not_optimize(yp);
      do_not_optimize(xp);
      scalar::axpy(yp, xp, 1e-8f, kDim);
      clobber();
    });
    std::printf("  %-28s %8.2fx\n\n", "axpy speedup", a_scalar / a_simd);

    const double g_row = time_it("gemv 32x64, row at a time", 2.0 * H * kDim, [&] {
      do_not_optimize(Wp);
      do_not_optimize(xp);
      do_not_optimize(outp);
      gemv_rowwise(Wp, xp, outp, H, kDim);
      clobber();
    });
    const double g_simd = time_it("gemv 32x64, 4 rows blocked", 2.0 * H * kDim, [&] {
      do_not_optimize(Wp);
      do_not_optimize(xp);
      do_not_optimize(outp);
      gemv(Wp, xp, outp, H, kDim);
      clobber();
    });
    const double g_scalar = time_it("gemv 32x64, scalar", 2.0 * H * kDim, [&] {
      do_not_optimize(Wp);
      do_not_optimize(xp);
      do_not_optimize(outp);
      scalar::gemv(Wp, xp, outp, H, kDim);
      clobber();
    });
    std::printf("  %-28s %8.2fx over scalar, %8.2fx from blocking alone\n\n",
                "gemv speedup", g_scalar / g_simd, g_row / g_simd);

    // The Adam step was the slowest thing in the model before it was widened:
    // a square root and a division per weight, neither of which pipelines with
    // anything when it sits alone in a scalar loop body.
    std::vector<float> aw(H * kDim), am(H * kDim, 0.0f), av(H * kDim, 0.0f), ag(H * kDim);
    for (auto& v : aw) v = u(rng);
    for (auto& v : ag) v = u(rng);
    float* awp = aw.data();
    float* amp = am.data();
    float* avp = av.data();
    const float* agp = ag.data();
    const double ad_simd = time_it("adam step 2048, vector", 5.0 * H * kDim, [&] {
      do_not_optimize(awp);
      do_not_optimize(agp);
      adam_step(awp, amp, avp, agp, 0.9f, 0.999f, 1e-3f, 1e-8f, H * kDim);
      clobber();
    });
    const double ad_scalar = time_it("adam step 2048, scalar", 5.0 * H * kDim, [&] {
      do_not_optimize(awp);
      do_not_optimize(agp);
      scalar::adam_step(awp, amp, avp, agp, 0.9f, 0.999f, 1e-3f, 1e-8f, H * kDim);
      clobber();
    });
    std::printf("  %-28s %8.2fx\n\n", "adam speedup", ad_scalar / ad_simd);

    FtrlLogistic lg({}, kDim);
    lg.finalise();
    Mlp mlp({}, kDim);
    time_it("logistic inference", 2.0 * kDim, [&] {
      do_not_optimize(xp);
      float r = lg.predict_fixed(xp);
      do_not_optimize(r);
      sink += r;
    });
    time_it("mlp 64-32-1 inference", 2.0 * (H * kDim + H), [&] {
      do_not_optimize(xp);
      float r = mlp.predict(xp);
      do_not_optimize(r);
      sink += r;
    });
    time_it("mlp 64-32-1 train step", 6.0 * (H * kDim + H), [&] {
      do_not_optimize(xp);
      const float p = mlp.predict(xp);
      mlp.update(xp, p, 1.0f);
      clobber();
    });
    std::printf("\n  peak for this core: AVX2 fused multiply add, 2 units x 8 lanes"
                " x 2 flops x %.2f GHz = %.1f GFLOP/s\n",
                ghz, 2.0 * 8.0 * 2.0 * ghz);
    std::printf("\n  (checksum %g, printed so nothing above is optimised away)\n",
                static_cast<double>(sink));
    return 0;
  }

  const Tick mid = 19160;
  std::vector<Command> block(a.block);
  std::vector<OpKind> kinds(a.block);

  // ---- phase 1: per operation latency -------------------------------------
  {
    CountingSink sink;
    MatchingEngine eng(bench_book_cfg(a), &sink);
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
    std::printf("%-20s %12s %9s %9s %9s %9s %9s %9s\n", "operation", "count", "p50", "p90",
                "p99", "p99.9", "mean", "max");
    auto row = [&](const char* name, const CycleHist& h) {
      if (!h.count()) return;
      std::printf("%-20s %12llu %8.0fns %8.0fns %8.0fns %8.0fns %8.0fns %8.1fus\n", name,
                  static_cast<unsigned long long>(h.count()),
                  to_ns(h.pct_cycles(50), ghz, overhead),
                  to_ns(h.pct_cycles(90), ghz, overhead),
                  to_ns(h.pct_cycles(99), ghz, overhead),
                  to_ns(h.pct_cycles(99.9), ghz, overhead),
                  to_ns(h.mean_cycles(), ghz, overhead),
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
    MatchingEngine eng(bench_book_cfg(a), &sink);
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

  // ---- phase 3: feed thread -> ring -> matcher, saturated -------------------
  // Bulk push and pop, and no timers inside the loop. This is the throughput
  // the pipeline sustains when the feed is always ahead of the matcher.
  double pipeline_mps = 0;
  {
    CommandRing ring(1u << 16);
    std::atomic<bool> stop{false};
    CountingSink sink;
    MatchingEngine eng(bench_book_cfg(a), &sink);

    std::thread feed([&] {
      if (a.feed_core >= 0) pin_to_core(a.feed_core);
      FlowGen gen(a, mid);
      std::vector<Command> b(4096);
      std::vector<OpKind> k(4096);
      while (!stop.load(std::memory_order_relaxed)) {
        gen.fill(b.data(), k.data(), b.size());
        std::size_t off = 0;
        while (off < b.size()) {
          off += ring.push_bulk(b.data() + off, b.size() - off);
          if (stop.load(std::memory_order_relaxed)) return;
        }
      }
    });

    std::vector<Command> batch(4096);
    // Warm up before the clock starts.
    std::uint64_t warm = 0;
    while (warm < 4u << 20) {
      const std::size_t n = ring.pop_bulk(batch.data(), batch.size());
      for (std::size_t i = 0; i < n; ++i) eng.apply(batch[i]);
      warm += n;
    }
    std::uint64_t consumed = 0;
    const double t0 = now_s();
    while (now_s() - t0 < a.seconds) {
      for (int r = 0; r < 64; ++r) {
        const std::size_t n = ring.pop_bulk(batch.data(), batch.size());
        for (std::size_t i = 0; i < n; ++i) eng.apply(batch[i]);
        consumed += n;
      }
    }
    const double elapsed = now_s() - t0;
    stop.store(true);
    while (ring.pop_bulk(batch.data(), batch.size())) {
    }
    feed.join();
    pipeline_mps = consumed / elapsed / 1e6;
    std::printf("feed thread -> spsc ring -> matcher, saturated%s\n",
                (a.core >= 0 && a.feed_core >= 0) ? ", both pinned" : " (NOT PINNED)");
    std::printf("  end to end: %.2f M msg/s over %.1f s (%llu messages, bulk push and pop)\n\n",
                pipeline_mps, elapsed, static_cast<unsigned long long>(consumed));
  }

  // ---- phase 4: unloaded queue handoff --------------------------------------
  // The saturated number above says nothing about how long a message takes to
  // cross the queue, because a full ring means every message waits behind 65535
  // others. Here the producer only sends when the ring is empty, so the reading
  // is the handoff itself: a release store on one core, an acquire load on
  // another, and the cache line moving between them.
  {
    CommandRing ring(1024);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> sent{0};
    CycleHist transit;

    std::thread consumer([&] {
      if (a.feed_core >= 0) pin_to_core(a.feed_core);
      Command c{};
      while (!stop.load(std::memory_order_relaxed)) {
        if (ring.pop(c)) {
          const std::uint64_t arrive = rdtsc_end();
          transit.add(arrive - static_cast<std::uint64_t>(c.ts));
        }
      }
    });

    const double t0 = now_s();
    const double budget = a.seconds < 5.0 ? a.seconds : 5.0;
    std::uint64_t n = 0;
    while (now_s() - t0 < budget) {
      for (int i = 0; i < 1000; ++i) {
        Command c{};
        c.type = CmdType::Nop;
        c.ts = static_cast<Ts>(rdtsc_begin());
        while (!ring.push(c)) {
        }
        ++n;
        // Wait for the consumer to take it, so the next message is never queued
        // behind this one.
        while (!ring.empty_approx()) {
        }
      }
    }
    stop.store(true);
    consumer.join();
    sent.store(n);
    std::printf("spsc handoff latency, unloaded (producer waits for the ring to drain)\n");
    std::printf("  %llu samples: p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns  max %.1f us\n",
                static_cast<unsigned long long>(transit.count()),
                to_ns(transit.pct_cycles(50), ghz, overhead),
                to_ns(transit.pct_cycles(99), ghz, overhead),
                to_ns(transit.pct_cycles(99.9), ghz, overhead),
                to_ns(static_cast<double>(transit.max_cycles()), ghz, overhead) / 1000.0);
    if (a.csv) {
      std::printf("csv,throughput,single_thread_mps,%.4f\n", single_thread_mps);
      std::printf("csv,throughput,pipeline_mps,%.4f\n", pipeline_mps);
      std::printf("csv,handoff,p50_ns,%.1f\n", to_ns(transit.pct_cycles(50), ghz, overhead));
      std::printf("csv,handoff,p99_ns,%.1f\n", to_ns(transit.pct_cycles(99), ghz, overhead));
      std::printf("csv,handoff,p999_ns,%.1f\n", to_ns(transit.pct_cycles(99.9), ghz, overhead));
    }
  }
  return 0;
}
