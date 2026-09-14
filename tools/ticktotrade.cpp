// Tick to trade: a packet arrives on a socket, and an order leaves on one.
//
// Everything else in this repository measures the engine. That is the part I
// wrote, but it is not the number a trading firm quotes, and quoting it alone
// would be misleading. The number that matters is wire to wire: from the moment
// a market data packet is available to the moment the order it provoked is on
// its way out. This measures that over real UDP sockets on loopback, and breaks
// it into stages so the engine's share of it is visible rather than assumed.
//
// Stages, all from one rdtsc timeline:
//   t0  the feed thread reads the clock and calls sendto
//   t1  the engine thread returns from recvfrom with the packet
//   t2  the packet has been decoded into engine commands
//   t3  the commands have been applied to the book
//   t4  the strategy has looked at the book and decided
//   t5  the engine thread returns from sendto with the order away
//   t6  the feed thread has the order back
//
// t5 cannot ride inside the packet it is timing, so the engine keeps its own
// histogram of the outbound syscall and that is merged in at the end. The
// return leg is the residual: the whole path less the inbound leg and less the
// everything the engine did. Reported as a residual and labelled as one, rather
// than quietly folded into a stage that was actually measured.
//
// The honest reading of the result is in the README. Briefly: without kernel
// bypass the two socket traversals are most of the budget, and the engine is a
// small enough fraction of it that making the engine twice as fast would not
// move the total much. That is the usual finding and it is the reason the
// industry buys network cards rather than compilers.
//
// This is loopback, not a network. There is no NIC, no wire, no switch and no
// kernel bypass. It is a floor on what a real path would cost, not an estimate
// of one.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "engine/engine.hpp"
#include "util/affinity.hpp"
#include "util/timing.hpp"
#include "wire/decoder.hpp"
#include "wire/itch.hpp"

using namespace ltx;
using namespace ltx::wire;

namespace {

struct Args {
  int seconds = 20;
  int feed_core = 3;
  int engine_core = 2;
  int pace_us = 50;     // gap between packets, so the queue never backs up
  int port_md = 47801;  // market data, feed to engine
  int port_ord = 47802; // orders, engine back to feed
  bool quiet = false;
};

void usage() {
  std::printf(
      "ticktotrade - packet in, order out, over real UDP sockets on loopback\n"
      "\n"
      "usage: ticktotrade [options]\n"
      "  --seconds=N        measurement window, default 20\n"
      "  --feed-core=N      cpu for the feed thread, default 3\n"
      "  --engine-core=N    cpu for the engine thread, default 2\n"
      "  --pace-us=N        microseconds between packets, default 50\n"
      "  --port=N           first udp port, default 47801\n"
      "  --quiet\n"
      "  --help\n");
}

int make_socket(int port, bool bind_it) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  // Large buffers so a lost packet is a real event rather than a full queue.
  int buf = 1 << 20;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  if (bind_it) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    a.sin_port = ::htons(static_cast<std::uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      ::close(fd);
      return -1;
    }
  }
  return fd;
}

sockaddr_in addr_of(int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  a.sin_port = ::htons(static_cast<std::uint16_t>(port));
  return a;
}

// The order the engine sends back when its rule fires.
struct OrderMsg {
  std::uint64_t t0;      // the feed's clock reading, echoed
  std::uint64_t t1, t2, t3, t4, t5;
  std::uint64_t seq;
};

void report(const char* name, CycleHist& h, double ghz, double overhead) {
  std::printf("  %-26s p50 %8.0f ns   p99 %9.0f ns   p99.9 %9.0f ns   max %7.1f us\n", name,
              to_ns(h.pct_cycles(50), ghz, overhead), to_ns(h.pct_cycles(99), ghz, overhead),
              to_ns(h.pct_cycles(99.9), ghz, overhead),
              to_ns(static_cast<double>(h.max_cycles()), ghz, overhead) / 1000.0);
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto num = [&](const char* k, int& o) {
      if (s.rfind(k, 0) == 0) { o = std::atoi(s.c_str() + std::strlen(k)); return true; }
      return false;
    };
    if (s == "--help" || s == "-h") { usage(); return 0; }
    else if (num("--seconds=", a.seconds)) {}
    else if (num("--feed-core=", a.feed_core)) {}
    else if (num("--engine-core=", a.engine_core)) {}
    else if (num("--pace-us=", a.pace_us)) {}
    else if (num("--port=", a.port_md)) { a.port_ord = a.port_md + 1; }
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
  }

  const int md_rx = make_socket(a.port_md, true);     // engine receives here
  const int ord_rx = make_socket(a.port_ord, true);   // feed receives orders here
  const int md_tx = make_socket(0, false);
  const int ord_tx = make_socket(0, false);
  if (md_rx < 0 || ord_rx < 0 || md_tx < 0 || ord_tx < 0) {
    std::fprintf(stderr, "cannot open sockets on ports %d and %d\n", a.port_md, a.port_ord);
    return 1;
  }

  const double ghz = measure_tsc_ghz(300);
  CycleHist timer_cost;
  for (int i = 0; i < 100000; ++i) {
    const std::uint64_t x = rdtsc_begin();
    const std::uint64_t y = rdtsc_end();
    timer_cost.add(y - x);
  }
  const double overhead = timer_cost.pct_cycles(50);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> engine_seen{0}, engine_sent{0};
  CycleHist h_out;   // written by the engine thread, read after it joins

  // --- engine side ----------------------------------------------------------
  std::thread engine([&] {
    pin_to_core(a.engine_core);
    BookConfig cfg;
    cfg.min_tick = 1;
    cfg.max_tick = 65536;
    cfg.max_orders = 1u << 16;
    cfg.id_map_capacity = 1u << 17;
    NullSink sink;
    MatchingEngine eng(cfg, &sink);
    Decoder dec;
    std::vector<Routed> cmds;
    cmds.reserve(64);
    std::vector<std::uint8_t> buf(2048);
    const sockaddr_in back = addr_of(a.port_ord);

    while (!stop.load(std::memory_order_relaxed)) {
      // Busy poll. A blocking read would add the cost of waking a thread to
      // every measurement, which is a scheduler number rather than an
      // engine one.
      const ssize_t n = ::recvfrom(md_rx, buf.data(), buf.size(), MSG_DONTWAIT, nullptr,
                                   nullptr);
      if (n <= 0) continue;
      const std::uint64_t t1 = rdtsc_end();

      // The feed's clock reading rides in front of the MoldUDP64 packet.
      std::uint64_t t0 = 0, seq = 0;
      std::memcpy(&t0, buf.data(), 8);
      std::memcpy(&seq, buf.data() + 8, 8);

      cmds.clear();
      dec.decode_packet(buf.data() + 16, static_cast<std::size_t>(n) - 16, cmds);
      const std::uint64_t t2 = rdtsc_end();

      for (const Routed& r : cmds) eng.apply(r.cmd);
      const std::uint64_t t3 = rdtsc_end();

      // A strategy decision worth the name: look at the touch, decide whether
      // to act. This is deliberately trivial, and the point of measuring it
      // separately is to show how little of the budget it is.
      const Tick bid = eng.book().best_bid();
      const Tick ask = eng.book().best_ask();
      const bool act = bid != kInvalidTick && ask != kInvalidTick && (ask - bid) >= 1;
      const std::uint64_t t4 = rdtsc_end();

      if (act) {
        OrderMsg om{t0, t1, t2, t3, t4, 0, seq};
        const std::uint64_t t4b = rdtsc_end();
        om.t5 = t4b;
        ::sendto(ord_tx, &om, sizeof(om), 0, reinterpret_cast<const sockaddr*>(&back),
                 sizeof(back));
        h_out.add(rdtsc_end() - t4b);
        engine_sent.fetch_add(1, std::memory_order_relaxed);
      }
      engine_seen.fetch_add(1, std::memory_order_relaxed);
    }
  });

  // --- feed side ------------------------------------------------------------
  pin_to_core(a.feed_core);
  const sockaddr_in to_engine = addr_of(a.port_md);

  // One MoldUDP64 packet of ITCH per tick, rebuilt each time so the engine has
  // real work to do. Each carries a delete of an order sent a while back and an
  // add of a fresh one, which keeps a two sided book of a bounded size resting
  // rather than letting it grow without limit or cross itself empty.
  std::vector<std::uint8_t> pkt(1024);
  constexpr Tick kMid = 19100;
  constexpr std::uint64_t kLive = 64;   // orders kept resting per side

  auto build_packet = [&](std::uint64_t seq) {
    const std::size_t off = 16;   // clock reading and sequence ride in front
    std::memcpy(pkt.data() + 8, &seq, 8);
    std::memcpy(pkt.data() + off, "LTXT2T    ", kSessionLen);
    store_be<std::uint64_t>(pkt.data() + off + kSessionLen, seq);

    std::size_t p = off + kMoldHeaderLen;
    std::uint16_t count = 0;
    std::uint8_t body[64];
    auto put = [&](std::size_t n) {
      store_be<std::uint16_t>(pkt.data() + p, static_cast<std::uint16_t>(n));
      std::memcpy(pkt.data() + p + 2, body, n);
      p += 2 + n;
      ++count;
    };

    const bool buy = (seq & 1) == 0;
    // Bids sit strictly below the mid and asks strictly above it, so the two
    // sides never cross and the touch is always populated.
    const Tick tick = buy ? static_cast<Tick>(kMid - 1 - (seq / 2) % 20)
                          : static_cast<Tick>(kMid + 1 + (seq / 2) % 20);
    if (seq >= 2 * kLive) put(encode_delete(body, 1, 0, seq, seq - 2 * kLive + 1));
    put(encode_add(body, 1, 0, seq, seq + 1, buy ? 'B' : 'S', 10000, "ETH     ",
                   tick_to_wire_price(tick, 1)));

    store_be<std::uint16_t>(pkt.data() + off + kSessionLen + 8, count);
    return p;
  };

  // The symbol directory once, up front, exactly as a real session does it.
  {
    const std::size_t off = 16;
    std::uint64_t zero = 0;
    std::memcpy(pkt.data(), &zero, 8);
    std::memcpy(pkt.data() + 8, &zero, 8);
    std::memcpy(pkt.data() + off, "LTXT2T    ", kSessionLen);
    store_be<std::uint64_t>(pkt.data() + off + kSessionLen, 0);
    store_be<std::uint16_t>(pkt.data() + off + kSessionLen + 8, 1);
    std::uint8_t body[64];
    const std::size_t n = encode_symbol_directory(body, 1, 0, "ETH     ", 1, 4);
    store_be<std::uint16_t>(pkt.data() + off + kMoldHeaderLen, static_cast<std::uint16_t>(n));
    std::memcpy(pkt.data() + off + kMoldHeaderLen + 2, body, n);
    ::sendto(md_tx, pkt.data(), off + kMoldHeaderLen + 2 + n, 0,
             reinterpret_cast<const sockaddr*>(&to_engine), sizeof(to_engine));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  CycleHist h_in, h_decode, h_apply, h_decide, h_rtt, h_engine_total, h_residual;
  std::uint64_t sent = 0, got = 0;
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();

  // Warm up: the first packets pay for page faults, ARP-equivalent state and a
  // cold decoder, and none of that is the thing being measured.
  std::uint64_t seq = 1;
  for (int i = 0; i < 5000; ++i, ++seq) {
    const std::uint64_t t0 = rdtsc_begin();
    std::memcpy(pkt.data(), &t0, 8);
    const std::size_t len = build_packet(seq);
    ::sendto(md_tx, pkt.data(), len, 0, reinterpret_cast<const sockaddr*>(&to_engine),
             sizeof(to_engine));
    OrderMsg om{};
    for (int spin = 0; spin < 50000; ++spin) {
      if (::recvfrom(ord_rx, &om, sizeof(om), MSG_DONTWAIT, nullptr, nullptr) > 0) break;
    }
  }

  while (std::chrono::duration_cast<std::chrono::seconds>(clock::now() - start).count() <
         a.seconds) {
    const std::uint64_t t0 = rdtsc_begin();
    std::memcpy(pkt.data(), &t0, 8);
    const std::size_t len = build_packet(seq);
    ::sendto(md_tx, pkt.data(), len, 0, reinterpret_cast<const sockaddr*>(&to_engine),
             sizeof(to_engine));
    ++sent;

    OrderMsg om{};
    bool have = false;
    for (int spin = 0; spin < 200000; ++spin) {
      if (::recvfrom(ord_rx, &om, sizeof(om), MSG_DONTWAIT, nullptr, nullptr) > 0) {
        have = true;
        break;
      }
    }
    if (have) {
      const std::uint64_t t6 = rdtsc_end();
      ++got;
      h_in.add(om.t1 - om.t0);
      h_decode.add(om.t2 - om.t1);
      h_apply.add(om.t3 - om.t2);
      h_decide.add(om.t4 - om.t3);
      h_engine_total.add(om.t5 - om.t1);
      h_rtt.add(t6 - om.t0);
      h_residual.add(t6 - om.t5);
    }
    ++seq;
    // Pacing, so the socket queue never backs up and the measurement is of the
    // path rather than of a queue.
    const std::uint64_t spin_until = rdtsc_end() +
                                     static_cast<std::uint64_t>(a.pace_us * ghz * 1000.0);
    while (rdtsc_end() < spin_until) {
    }
  }

  stop.store(true);
  engine.join();
  ::close(md_rx);
  ::close(ord_rx);
  ::close(md_tx);
  ::close(ord_tx);

  if (got == 0) {
    std::fprintf(stderr,
                 "no round trips completed: engine saw %llu packets and sent %llu orders\n",
                 static_cast<unsigned long long>(engine_seen.load()),
                 static_cast<unsigned long long>(engine_sent.load()));
    return 1;
  }
  std::printf("tick to trade, udp on loopback, feed pinned to cpu %d and engine to cpu %d\n",
              a.feed_core, a.engine_core);
  std::printf("tsc %.4f GHz, timer pair %.0f cycles subtracted, %llu packets sent,"
              " %llu round trips measured (%.2f%% seen)\n\n",
              ghz, overhead, static_cast<unsigned long long>(sent),
              static_cast<unsigned long long>(got), sent ? 100.0 * got / sent : 0.0);

  std::printf("  stage\n");
  report("kernel in, send to recv", h_in, ghz, overhead);
  report("decode the packet", h_decode, ghz, overhead);
  report("apply to the book", h_apply, ghz, overhead);
  report("look at the book, decide", h_decide, ghz, overhead);
  report("kernel out, sendto", h_out, ghz, overhead);
  report("return leg (incl. sendto)", h_residual, ghz, overhead);
  std::printf("\n");
  report("engine work, t1 to t5", h_engine_total, ghz, overhead);
  report("tick to trade, t0 to t6", h_rtt, ghz, overhead);

  const double eng = to_ns(h_engine_total.pct_cycles(50), ghz, overhead);
  const double rtt = to_ns(h_rtt.pct_cycles(50), ghz, overhead);
  std::printf("\n  at the median, the part this repository wrote is %.1f%% of the whole"
              " path (%.0f ns of %.0f ns)\n", rtt > 0 ? 100.0 * eng / rtt : 0.0, eng, rtt);
  std::printf("  the rest is the kernel network stack, twice. there is no NIC here and no\n"
              "  kernel bypass, so this is a floor on a real path rather than an estimate\n"
              "  of one.\n");
  return 0;
}
