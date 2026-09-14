// Tick to trade: a packet arrives, and the order it provoked leaves.
//
// Everything else in this repository measures the engine. That is the part I
// wrote, but it is not the number a trading firm quotes, and quoting it alone
// would be misleading. This measures wire to wire, and breaks it into stages so
// the engine's share of it is visible rather than assumed.
//
// Stages, all from one rdtsc timeline:
//   t0  the feed reads the clock and sends
//   t1  the engine has the packet
//   t2  the packet is decoded into engine commands
//   t3  the commands are applied to the book
//   t4  the strategy has looked at the book and decided
//   t5  the engine is about to send the order
//   t6  the feed has the order back
//
// t5 cannot ride inside the packet it is timing, so the engine keeps its own
// histogram of the outbound call and that is merged in at the end.
//
// Four transports, so "kernel bypass would help" stops being a claim and becomes
// a measured bound:
//
//   udp        AF_INET datagrams on loopback. sendto and recvfrom. The baseline,
//              and the one that resembles a real feed most closely.
//   udp-tuned  the same, with both sockets connected so each call skips the
//              address handling and the route lookup, SO_BUSY_POLL asked for,
//              and recvmmsg on the receive side.
//   unix       AF_UNIX datagrams in the abstract namespace. The same syscalls
//              and the same scheduling, with none of the IP and UDP processing,
//              so the gap between this and udp is what the protocol stack costs.
//   ring       the lock free SPSC ring from src/engine, with no kernel in the
//              path at all. This is not a bypass NIC. It is the floor a bypass
//              NIC is trying to approach, and having it measured turns the
//              usual hand wave about kernel bypass into a number.
//
// None of this is a network. There is no NIC, no wire, no switch and no bypass
// stack. Every figure here is a floor on what a real path would cost.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
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
#include "engine/spsc_ring.hpp"
#include "util/affinity.hpp"
#include "util/timing.hpp"
#include "wire/decoder.hpp"
#include "wire/itch.hpp"

using namespace ltx;
using namespace ltx::wire;

namespace {

enum class Transport { Udp, UdpTuned, Unix, Ring };

const char* transport_name(Transport t) {
  switch (t) {
    case Transport::Udp: return "udp on loopback";
    case Transport::UdpTuned: return "udp on loopback, connected sockets, recvmmsg";
    case Transport::Unix: return "af_unix datagrams";
    case Transport::Ring: return "spsc ring, no kernel in the path";
  }
  return "?";
}

struct Args {
  Transport transport = Transport::Udp;
  bool all = false;
  int seconds = 20;
  int feed_core = 3;
  int engine_core = 2;
  int pace_us = 50;
  int port = 47801;
  bool quiet = false;
};

void usage() {
  std::printf(
      "ticktotrade - packet in, order out, timed stage by stage\n"
      "\n"
      "usage: ticktotrade [options]\n"
      "  --transport=T      udp, udp-tuned, unix or ring. default udp\n"
      "  --all              run every transport in turn and print the ladder\n"
      "  --seconds=N        measurement window per transport, default 20\n"
      "  --feed-core=N      cpu for the feed thread, default 3\n"
      "  --engine-core=N    cpu for the engine thread, default 2\n"
      "  --pace-us=N        microseconds between packets, default 50\n"
      "  --port=N           first udp port, default 47801\n"
      "  --quiet\n"
      "  --help\n");
}

// --- links ------------------------------------------------------------------
// One direction of one transport. send and recv are the only things the
// measurement loop needs, and recv never blocks: a blocking read would put the
// cost of waking a thread into every sample, which is a scheduler number.

struct SockLink {
  int rx = -1, tx = -1;
  bool use_recvmmsg = false;
  sockaddr_storage peer{};
  socklen_t peer_len = 0;
  bool connected = false;

  bool send(const void* p, std::size_t n) const {
    if (connected) return ::send(tx, p, n, 0) > 0;
    return ::sendto(tx, p, n, 0, reinterpret_cast<const sockaddr*>(&peer), peer_len) > 0;
  }

  ssize_t recv(void* p, std::size_t cap) const {
    if (use_recvmmsg) {
      // Batched receive. At this pacing there is usually one message waiting, so
      // this does not help here; it is in the tuned variant because it is what
      // a real handler does when the feed bursts, and leaving it out would make
      // the comparison look better than it is.
      iovec iov{p, cap};
      mmsghdr msgs[1]{};
      msgs[0].msg_hdr.msg_iov = &iov;
      msgs[0].msg_hdr.msg_iovlen = 1;
      const int got = ::recvmmsg(rx, msgs, 1, MSG_DONTWAIT, nullptr);
      return got > 0 ? static_cast<ssize_t>(msgs[0].msg_len) : -1;
    }
    return ::recvfrom(rx, p, cap, MSG_DONTWAIT, nullptr, nullptr);
  }

  void close() {
    if (rx >= 0) ::close(rx);
    if (tx >= 0) ::close(tx);
    rx = tx = -1;
  }
};

// A fixed frame so the ring holds plain data and copies no pointers.
struct Frame {
  std::uint32_t len;
  std::uint8_t data[252];
};
static_assert(sizeof(Frame) == 256, "frame should stay at 256 bytes");

struct RingLink {
  SpscRing<Frame>* q = nullptr;

  bool send(const void* p, std::size_t n) const {
    if (n > sizeof(Frame::data)) return false;
    Frame f;
    f.len = static_cast<std::uint32_t>(n);
    std::memcpy(f.data, p, n);
    return q->push(f);
  }
  ssize_t recv(void* p, std::size_t cap) const {
    Frame f;
    if (!q->pop(f)) return -1;
    const std::size_t n = std::min<std::size_t>(f.len, cap);
    std::memcpy(p, f.data, n);
    return static_cast<ssize_t>(n);
  }
};

int udp_socket(int port, bool bind_it) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
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

// Abstract namespace, so nothing is left in the filesystem if this dies.
int unix_socket(const char* name, bool bind_it) {
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  int buf = 1 << 20;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  if (bind_it) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    a.sun_path[0] = '\0';
    std::strncpy(a.sun_path + 1, name, sizeof(a.sun_path) - 2);
    const socklen_t len =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), len) != 0) {
      ::close(fd);
      return -1;
    }
  }
  return fd;
}

void fill_unix_addr(sockaddr_storage& ss, socklen_t& len, const char* name) {
  auto* a = reinterpret_cast<sockaddr_un*>(&ss);
  std::memset(a, 0, sizeof(*a));
  a->sun_family = AF_UNIX;
  a->sun_path[0] = '\0';
  std::strncpy(a->sun_path + 1, name, sizeof(a->sun_path) - 2);
  len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name));
}

void fill_udp_addr(sockaddr_storage& ss, socklen_t& len, int port) {
  auto* a = reinterpret_cast<sockaddr_in*>(&ss);
  std::memset(a, 0, sizeof(*a));
  a->sin_family = AF_INET;
  a->sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  a->sin_port = ::htons(static_cast<std::uint16_t>(port));
  len = sizeof(sockaddr_in);
}

struct OrderMsg {
  std::uint64_t t0, t1, t2, t3, t4, t5, seq;
};

struct Result {
  Transport transport;
  double p50_total = 0, p99_total = 0, p999_total = 0;
  double p50_in = 0, p50_decode = 0, p50_apply = 0, p50_decide = 0, p50_out = 0;
  double p50_engine = 0;
  std::uint64_t sent = 0, got = 0;
  bool busy_poll_set = false;
};

void report_line(const char* name, CycleHist& h, double ghz, double overhead) {
  std::printf("  %-28s p50 %8.0f ns   p99 %9.0f ns   p99.9 %9.0f ns   max %7.1f us\n", name,
              to_ns(h.pct_cycles(50), ghz, overhead), to_ns(h.pct_cycles(99), ghz, overhead),
              to_ns(h.pct_cycles(99.9), ghz, overhead),
              to_ns(static_cast<double>(h.max_cycles()), ghz, overhead) / 1000.0);
}

// --- the measurement --------------------------------------------------------
template <typename MdLink, typename OrdLink>
Result measure(const Args& a, Transport transport, const MdLink& md_feed,
               const MdLink& md_engine, const OrdLink& ord_engine, const OrdLink& ord_feed,
               double ghz, double overhead) {
  Result res;
  res.transport = transport;

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> engine_seen{0};
  CycleHist h_out;

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

    while (!stop.load(std::memory_order_relaxed)) {
      const ssize_t n = md_engine.recv(buf.data(), buf.size());
      if (n <= 0) continue;
      const std::uint64_t t1 = rdtsc_end();

      std::uint64_t t0 = 0, seq = 0;
      std::memcpy(&t0, buf.data(), 8);
      std::memcpy(&seq, buf.data() + 8, 8);

      cmds.clear();
      dec.decode_packet(buf.data() + 16, static_cast<std::size_t>(n) - 16, cmds);
      const std::uint64_t t2 = rdtsc_end();

      for (const Routed& r : cmds) eng.apply(r.cmd);
      const std::uint64_t t3 = rdtsc_end();

      const Tick bid = eng.book().best_bid();
      const Tick ask = eng.book().best_ask();
      const bool act = bid != kInvalidTick && ask != kInvalidTick && (ask - bid) >= 1;
      const std::uint64_t t4 = rdtsc_end();

      if (act) {
        OrderMsg om{t0, t1, t2, t3, t4, 0, seq};
        const std::uint64_t t5 = rdtsc_end();
        om.t5 = t5;
        ord_engine.send(&om, sizeof(om));
        h_out.add(rdtsc_end() - t5);
      }
      engine_seen.fetch_add(1, std::memory_order_relaxed);
    }
  });

  pin_to_core(a.feed_core);

  std::vector<std::uint8_t> pkt(512);
  constexpr Tick kMid = 19100;
  constexpr std::uint64_t kLive = 64;

  auto build_packet = [&](std::uint64_t seq) {
    const std::size_t off = 16;
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
    const Tick tick = buy ? static_cast<Tick>(kMid - 1 - (seq / 2) % 20)
                          : static_cast<Tick>(kMid + 1 + (seq / 2) % 20);
    if (seq >= 2 * kLive) put(encode_delete(body, 1, 0, seq, seq - 2 * kLive + 1));
    put(encode_add(body, 1, 0, seq, seq + 1, buy ? 'B' : 'S', 10000, "ETH     ",
                   tick_to_wire_price(tick, 1)));
    store_be<std::uint16_t>(pkt.data() + off + kSessionLen + 8, count);
    return p;
  };

  // The symbol directory once, as a real session does it.
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
    md_feed.send(pkt.data(), off + kMoldHeaderLen + 2 + n);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  CycleHist h_in, h_decode, h_apply, h_decide, h_rtt, h_engine_total;
  using clock = std::chrono::steady_clock;

  std::uint64_t seq = 1;
  for (int i = 0; i < 5000; ++i, ++seq) {
    const std::uint64_t t0 = rdtsc_begin();
    std::memcpy(pkt.data(), &t0, 8);
    md_feed.send(pkt.data(), build_packet(seq));
    OrderMsg om{};
    for (int spin = 0; spin < 50000; ++spin) {
      if (ord_feed.recv(&om, sizeof(om)) > 0 && om.seq == seq) break;
    }
  }

  const auto start = clock::now();
  while (std::chrono::duration_cast<std::chrono::seconds>(clock::now() - start).count() <
         a.seconds) {
    const std::uint64_t t0 = rdtsc_begin();
    std::memcpy(pkt.data(), &t0, 8);
    md_feed.send(pkt.data(), build_packet(seq));
    ++res.sent;

    // Match the reply to the request that caused it. Reading whatever happens
    // to be waiting looks fine on a transport that drops when it is full and is
    // badly wrong on one that does not: a single missed reply leaves a permanent
    // backlog, and every later sample then times a request from thousands of
    // iterations ago. That is what the ring row did before this, reporting 1.7
    // ms on a path whose own stages added up to under 700 ns.
    OrderMsg om{};
    bool have = false;
    for (int spin = 0; spin < 200000; ++spin) {
      if (ord_feed.recv(&om, sizeof(om)) > 0 && om.seq == seq) { have = true; break; }
    }
    if (have) {
      const std::uint64_t t6 = rdtsc_end();
      ++res.got;
      h_in.add(om.t1 - om.t0);
      h_decode.add(om.t2 - om.t1);
      h_apply.add(om.t3 - om.t2);
      h_decide.add(om.t4 - om.t3);
      h_engine_total.add(om.t5 - om.t1);
      h_rtt.add(t6 - om.t0);
    }
    ++seq;
    const std::uint64_t until = rdtsc_end() +
                                static_cast<std::uint64_t>(a.pace_us * ghz * 1000.0);
    while (rdtsc_end() < until) {
    }
  }

  stop.store(true);
  engine.join();

  if (!a.quiet) {
    std::printf("\n%s\n", transport_name(transport));
    std::printf("  %llu sent, %llu round trips (%.2f%%)\n",
                static_cast<unsigned long long>(res.sent),
                static_cast<unsigned long long>(res.got),
                res.sent ? 100.0 * res.got / res.sent : 0.0);
    report_line("in, send to recv", h_in, ghz, overhead);
    report_line("decode the packet", h_decode, ghz, overhead);
    report_line("apply to the book", h_apply, ghz, overhead);
    report_line("look at the book, decide", h_decide, ghz, overhead);
    report_line("out, the send call", h_out, ghz, overhead);
    report_line("engine work, t1 to t5", h_engine_total, ghz, overhead);
    report_line("TICK TO TRADE, t0 to t6", h_rtt, ghz, overhead);
  }

  res.p50_total = to_ns(h_rtt.pct_cycles(50), ghz, overhead);
  res.p99_total = to_ns(h_rtt.pct_cycles(99), ghz, overhead);
  res.p999_total = to_ns(h_rtt.pct_cycles(99.9), ghz, overhead);
  res.p50_in = to_ns(h_in.pct_cycles(50), ghz, overhead);
  res.p50_decode = to_ns(h_decode.pct_cycles(50), ghz, overhead);
  res.p50_apply = to_ns(h_apply.pct_cycles(50), ghz, overhead);
  res.p50_decide = to_ns(h_decide.pct_cycles(50), ghz, overhead);
  res.p50_out = to_ns(h_out.pct_cycles(50), ghz, overhead);
  res.p50_engine = to_ns(h_engine_total.pct_cycles(50), ghz, overhead);
  return res;
}

Result run_transport(const Args& a, Transport t, double ghz, double overhead) {
  if (t == Transport::Ring) {
    SpscRing<Frame> md(4096), ord(4096);
    RingLink md_feed{&md}, md_engine{&md}, ord_engine{&ord}, ord_feed{&ord};
    return measure(a, t, md_feed, md_engine, ord_engine, ord_feed, ghz, overhead);
  }

  SockLink md_feed, md_engine, ord_engine, ord_feed;
  bool busy_ok = false;
  if (t == Transport::Unix) {
    static int gen = 0;
    ++gen;
    char n1[64], n2[64];
    std::snprintf(n1, sizeof(n1), "ltx-md-%d-%d", ::getpid(), gen);
    std::snprintf(n2, sizeof(n2), "ltx-ord-%d-%d", ::getpid(), gen);
    md_engine.rx = unix_socket(n1, true);
    ord_feed.rx = unix_socket(n2, true);
    md_feed.tx = unix_socket(nullptr, false);
    ord_engine.tx = unix_socket(nullptr, false);
    fill_unix_addr(md_feed.peer, md_feed.peer_len, n1);
    fill_unix_addr(ord_engine.peer, ord_engine.peer_len, n2);
  } else {
    const int p1 = a.port, p2 = a.port + 1;
    md_engine.rx = udp_socket(p1, true);
    ord_feed.rx = udp_socket(p2, true);
    md_feed.tx = udp_socket(0, false);
    ord_engine.tx = udp_socket(0, false);
    fill_udp_addr(md_feed.peer, md_feed.peer_len, p1);
    fill_udp_addr(ord_engine.peer, ord_engine.peer_len, p2);
    if (t == Transport::UdpTuned) {
      // Connecting a datagram socket pins the route and lets every later call
      // skip the address copy and the lookup.
      ::connect(md_feed.tx, reinterpret_cast<sockaddr*>(&md_feed.peer), md_feed.peer_len);
      ::connect(ord_engine.tx, reinterpret_cast<sockaddr*>(&ord_engine.peer),
                ord_engine.peer_len);
      md_feed.connected = true;
      ord_engine.connected = true;
      md_engine.use_recvmmsg = true;
      ord_feed.use_recvmmsg = true;
#ifdef SO_BUSY_POLL
      int us = 50;
      busy_ok = ::setsockopt(md_engine.rx, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us)) == 0 &&
                ::setsockopt(ord_feed.rx, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us)) == 0;
#endif
    }
  }
  if (md_engine.rx < 0 || ord_feed.rx < 0 || md_feed.tx < 0 || ord_engine.tx < 0) {
    std::fprintf(stderr, "cannot open sockets for %s\n", transport_name(t));
    Result r;
    r.transport = t;
    return r;
  }
  Result r = measure(a, t, md_feed, md_engine, ord_engine, ord_feed, ghz, overhead);
  r.busy_poll_set = busy_ok;
  md_feed.close();
  md_engine.close();
  ord_engine.close();
  ord_feed.close();
  return r;
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
    else if (s == "--all") a.all = true;
    else if (s == "--quiet") a.quiet = true;
    else if (s.rfind("--transport=", 0) == 0) {
      const std::string t = s.substr(12);
      if (t == "udp") a.transport = Transport::Udp;
      else if (t == "udp-tuned") a.transport = Transport::UdpTuned;
      else if (t == "unix") a.transport = Transport::Unix;
      else if (t == "ring") a.transport = Transport::Ring;
      else { std::fprintf(stderr, "unknown transport %s\n", t.c_str()); return 1; }
    }
    else if (num("--seconds=", a.seconds)) {}
    else if (num("--feed-core=", a.feed_core)) {}
    else if (num("--engine-core=", a.engine_core)) {}
    else if (num("--pace-us=", a.pace_us)) {}
    else if (num("--port=", a.port)) {}
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
  }

  const double ghz = measure_tsc_ghz(300);
  CycleHist timer_cost;
  for (int i = 0; i < 100000; ++i) {
    const std::uint64_t x = rdtsc_begin();
    const std::uint64_t y = rdtsc_end();
    timer_cost.add(y - x);
  }
  const double overhead = timer_cost.pct_cycles(50);

  std::printf("tick to trade, feed on cpu %d and engine on cpu %d, tsc %.4f GHz,"
              " timer pair %.0f cycles subtracted\n",
              a.feed_core, a.engine_core, ghz, overhead);

  std::vector<Result> results;
  if (a.all) {
    for (Transport t : {Transport::Udp, Transport::UdpTuned, Transport::Unix,
                        Transport::Ring}) {
      results.push_back(run_transport(a, t, ghz, overhead));
    }
  } else {
    results.push_back(run_transport(a, a.transport, ghz, overhead));
  }

  std::printf("\n\nthe ladder, medians in nanoseconds\n");
  std::printf("%-44s %9s %9s %9s %9s %9s %9s %9s\n", "transport", "in", "decode", "book",
              "decide", "out", "engine", "TOTAL");
  for (const Result& r : results) {
    std::printf("%-44s %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f\n",
                transport_name(r.transport), r.p50_in, r.p50_decode, r.p50_apply,
                r.p50_decide, r.p50_out, r.p50_engine, r.p50_total);
  }
  std::printf("\n%-44s %9s %9s %9s\n", "", "p50", "p99", "p99.9");
  for (const Result& r : results) {
    std::printf("%-44s %9.0f %9.0f %9.0f   engine is %.1f%% of it\n",
                transport_name(r.transport), r.p50_total, r.p99_total, r.p999_total,
                r.p50_total > 0 ? 100.0 * r.p50_engine / r.p50_total : 0.0);
  }
  for (const Result& r : results) {
    if (r.transport == Transport::UdpTuned && !r.busy_poll_set) {
      std::printf("\nnote: SO_BUSY_POLL was refused, so the tuned row is connected sockets\n"
                  "      and recvmmsg only. Busy polling needs a NAPI device and loopback\n"
                  "      is not one, so it would have done nothing here regardless.\n");
    }
  }
  std::printf("\nnone of this is a network. no NIC, no wire, no bypass stack. the ring row\n"
              "is not a bypass NIC either: it is the floor one is trying to reach.\n");
  return 0;
}
