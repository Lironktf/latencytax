// An order entry gateway: SoupBinTCP sessions carrying OUCH, into the engine.
//
// The market data side of this repository broadcasts what the market did. This
// is the other half of exchange connectivity: a client connects over TCP, logs
// in, names its orders with its own tokens, and gets back a sequenced stream of
// what became of them. The part worth building is not the parsing, it is the
// recovery contract, so this runs the whole thing and then deliberately breaks
// it:
//
//   the client sends orders, collects acknowledgements, has its connection
//   killed part way through, reconnects asking to resume from the next sequence
//   number it had not seen, and every message it missed comes back byte for
//   byte in the right order.
//
// That is verified rather than asserted: the client keeps a digest of every
// sequenced payload it ever received, and at the end it is compared against the
// server's own store. Anything missing, duplicated, reordered or altered shows
// up as a mismatch.
//
// Also reported: order entry round trip over TCP, from the client's send to the
// acknowledgement landing, decomposed the same way tick to trade is.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine/engine.hpp"
#include "util/affinity.hpp"
#include "util/timing.hpp"
#include "wire/ouch.hpp"
#include "wire/session.hpp"
#include "wire/soup.hpp"

using namespace ltx;
using namespace ltx::wire;

namespace {

struct Args {
  int port = 47811;
  int orders = 20000;
  int drop_at = 5000;    // kill the connection after this many acks
  int burst = 500;       // orders fired with no reader before the connection dies
  int server_core = 2;
  int client_core = 3;
  bool quiet = false;
};

void usage() {
  std::printf(
      "ouchgw - SoupBinTCP sessions carrying OUCH into the matching engine\n"
      "\n"
      "usage: ouchgw [options]\n"
      "  --port=N           tcp port, default 47811\n"
      "  --orders=N         orders the client sends, default 20000\n"
      "  --drop-at=N        kill the connection after this many acks, 0 to never\n"
      "  --burst=N          orders fired with nobody reading before the drop, default 500\n"
      "  --server-core=N    default 2\n"
      "  --client-core=N    default 3\n"
      "  --quiet\n"
      "  --help\n");
}

std::uint64_t digest(const std::uint8_t* p, std::size_t n) {
  std::uint64_t h = 1469598103934665603ull;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

void set_nodelay(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// --- the exchange side ------------------------------------------------------
// Turns OUCH in into engine calls, and engine events into OUCH out. Every
// outbound message goes through the store, so it is replayable by construction
// rather than by remembering to copy it somewhere.
class Gateway final : public EventSink {
 public:
  Gateway(MatchingEngine& eng, SequencedStore& store) : eng_(eng), store_(store) {}

  void on_order(const std::uint8_t* p, std::size_t n) {
    if (n == 0) return;
    switch (static_cast<OuchIn>(p[0])) {
      case OuchIn::EnterOrder: return enter(p, n);
      case OuchIn::CancelOrder: return cancel(p, n);
      case OuchIn::ReplaceOrder: return replace(p, n);
      default: break;
    }
  }

  // The engine tells us a resting order traded. Whoever owned it gets told.
  void on_trade(const TradeEvent& e) override {
    auto it = id_to_token_.find(e.maker_id);
    if (it == id_to_token_.end()) return;
    Executed m{};
    m.timestamp_ns = static_cast<std::uint64_t>(e.ts);
    std::memcpy(m.token, it->second.data(), kTokenLen);
    m.shares = qty_to_wire(e.qty);
    m.price = tick_to_wire_price(e.tick, 1);
    m.match_number = ++match_;
    std::uint8_t b[64];
    store_.append(b, encode_executed(b, m));
    if (e.maker_remaining == 0) forget(e.maker_id);
  }

  std::uint64_t published() const { return store_.count(); }

 private:
  static std::string tok(const char* t) { return std::string(t, kTokenLen); }

  void remember(OrderId id, const std::string& token) {
    token_to_id_[token] = id;
    id_to_token_[id] = token;
  }
  void forget(OrderId id) {
    auto it = id_to_token_.find(id);
    if (it == id_to_token_.end()) return;
    token_to_id_.erase(it->second);
    id_to_token_.erase(it);
  }

  void reject(const char* token, RejectReason why) {
    Rejected m{};
    m.timestamp_ns = now_++;
    std::memcpy(m.token, token, kTokenLen);
    m.reason = static_cast<char>(why);
    std::uint8_t b[64];
    store_.append(b, encode_rejected(b, m));
  }

  void enter(const std::uint8_t* p, std::size_t n) {
    EnterOrder o{};
    if (!decode_enter(p, n, o)) return;
    const std::string t = tok(o.token);
    if (token_to_id_.count(t)) return reject(o.token, RejectReason::DuplicateToken);
    if (o.shares == 0) return reject(o.token, RejectReason::BadQuantity);
    const Tick tick = wire_price_to_tick(o.price, 1);
    if (tick <= 0) return reject(o.token, RejectReason::BadPrice);
    if (std::memcmp(o.symbol, "ETH     ", kSymbolLen) != 0) {
      return reject(o.token, RejectReason::UnknownSymbol);
    }

    const OrderId id = ++next_id_;
    const Side side = o.side == 'B' ? Side::Buy : Side::Sell;
    const Tif tif = o.tif == 'I' ? Tif::Ioc : Tif::Gtc;
    // Remembered before the call, because a marketable order can trade and emit
    // its execution from inside add_limit, before it ever returns.
    remember(id, t);
    const Reject r = eng_.book().add_limit(now_++, id, side, tick, wire_to_qty(o.shares), tif);
    if (r != Reject::None) {
      forget(id);
      return reject(o.token, RejectReason::BadPrice);
    }

    Accepted a{};
    a.timestamp_ns = now_++;
    std::memcpy(a.token, o.token, kTokenLen);
    a.side = o.side;
    a.shares = qty_to_wire(eng_.book().order_qty(id));
    std::memcpy(a.symbol, o.symbol, kSymbolLen);
    a.price = o.price;
    a.order_reference = id;
    a.state = static_cast<char>(eng_.book().is_live(id) ? OrderState::Live : OrderState::Dead);
    std::uint8_t b[128];
    store_.append(b, encode_accepted(b, a));
    if (!eng_.book().is_live(id)) forget(id);
  }

  void cancel(const std::uint8_t* p, std::size_t n) {
    CancelOrder c{};
    if (!decode_cancel_order(p, n, c)) return;
    const std::string t = tok(c.token);
    auto it = token_to_id_.find(t);
    if (it == token_to_id_.end()) return reject(c.token, RejectReason::UnknownToken);
    const OrderId id = it->second;
    const Qty left = eng_.book().order_qty(id);
    if (eng_.book().cancel(now_++, id) != Reject::None) {
      return reject(c.token, RejectReason::UnknownToken);
    }
    Canceled m{};
    m.timestamp_ns = now_++;
    std::memcpy(m.token, c.token, kTokenLen);
    m.shares = qty_to_wire(left);
    m.reason = static_cast<char>(CancelReasonCode::User);
    std::uint8_t b[64];
    store_.append(b, encode_canceled(b, m));
    forget(id);
  }

  void replace(const std::uint8_t* p, std::size_t n) {
    ReplaceOrder r{};
    if (!decode_replace_order(p, n, r)) return;
    const std::string old_t = tok(r.existing_token);
    const std::string new_t = tok(r.new_token);
    auto it = token_to_id_.find(old_t);
    if (it == token_to_id_.end()) return reject(r.new_token, RejectReason::UnknownToken);
    if (token_to_id_.count(new_t)) return reject(r.new_token, RejectReason::DuplicateToken);
    const OrderId old_id = it->second;
    const OrderId new_id = ++next_id_;
    const Tick tick = wire_price_to_tick(r.price, 1);
    forget(old_id);
    remember(new_id, new_t);
    if (eng_.book().replace(now_++, old_id, new_id, tick, wire_to_qty(r.shares)) !=
        Reject::None) {
      forget(new_id);
      return reject(r.new_token, RejectReason::BadPrice);
    }
    Replaced m{};
    m.timestamp_ns = now_++;
    std::memcpy(m.new_token, r.new_token, kTokenLen);
    std::memcpy(m.previous_token, r.existing_token, kTokenLen);
    m.shares = qty_to_wire(eng_.book().order_qty(new_id));
    m.price = r.price;
    m.order_reference = new_id;
    m.state = static_cast<char>(eng_.book().is_live(new_id) ? OrderState::Live
                                                            : OrderState::Dead);
    std::uint8_t b[128];
    store_.append(b, encode_replaced(b, m));
    if (!eng_.book().is_live(new_id)) forget(new_id);
  }

  MatchingEngine& eng_;
  SequencedStore& store_;
  std::unordered_map<std::string, OrderId> token_to_id_;
  std::unordered_map<OrderId, std::string> id_to_token_;
  OrderId next_id_ = 0;
  std::uint64_t match_ = 0;
  Ts now_ = 1;
};

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
    else if (num("--port=", a.port)) {}
    else if (num("--orders=", a.orders)) {}
    else if (num("--drop-at=", a.drop_at)) {}
    else if (num("--burst=", a.burst)) {}
    else if (num("--server-core=", a.server_core)) {}
    else if (num("--client-core=", a.client_core)) {}
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
  }

  const double ghz = measure_tsc_ghz(200);
  CycleHist timer_cost;
  for (int i = 0; i < 50000; ++i) timer_cost.add(rdtsc_end() - rdtsc_begin());
  const double overhead = timer_cost.pct_cycles(50);

  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in la{};
  la.sin_family = AF_INET;
  la.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  la.sin_port = ::htons(static_cast<std::uint16_t>(a.port));
  if (::bind(listener, reinterpret_cast<sockaddr*>(&la), sizeof(la)) != 0 ||
      ::listen(listener, 4) != 0) {
    std::fprintf(stderr, "cannot listen on %d\n", a.port);
    return 1;
  }

  SequencedStore store;
  BookConfig cfg;
  cfg.min_tick = 1;
  cfg.max_tick = 65536;
  cfg.max_orders = 1u << 17;
  cfg.id_map_capacity = 1u << 18;
  MatchingEngine eng(cfg, nullptr);
  Gateway gw(eng, store);
  eng.book().set_sink(&gw);

  std::atomic<bool> stop{false};
  std::atomic<int> connections{0};

  std::thread server([&] {
    pin_to_core(a.server_core);
    while (!stop.load(std::memory_order_relaxed)) {
      const int fd = ::accept(listener, nullptr, nullptr);
      if (fd < 0) break;
      set_nodelay(fd);
      ++connections;
      ServerSession sess(&store, "LTXSESS01");
      std::vector<std::uint8_t> out;
      std::uint8_t buf[16384];
      while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.clear();
        const bool keep = sess.consume(buf, static_cast<std::size_t>(n), out,
                                       [&](const std::uint8_t* p, std::size_t len) {
                                         gw.on_order(p, len);
                                       });
        // Anything the engine produced while handling those orders is owed to
        // the client now.
        sess.pump(out);
        if (!out.empty()) {
          std::size_t off = 0;
          while (off < out.size()) {
            const ssize_t w = ::send(fd, out.data() + off, out.size() - off, MSG_NOSIGNAL);
            if (w <= 0) break;
            off += static_cast<std::size_t>(w);
          }
        }
        if (!keep) break;
      }
      ::close(fd);
      sess.on_disconnect();
      if (stop.load(std::memory_order_relaxed)) break;
    }
  });

  // --- client ---------------------------------------------------------------
  pin_to_core(a.client_core);
  std::unordered_map<std::uint64_t, std::uint64_t> seen;   // sequence -> digest
  ClientSession cs;
  CycleHist rtt;
  std::uint64_t sent = 0, acks = 0, reconnects = 0;
  std::uint64_t resume_at = 0, replayed = 0, behind = 0;

  auto connect_now = [&](std::uint64_t from) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    set_nodelay(fd);
    sockaddr_in sa = la;
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    cs.reset_connection();
    std::vector<std::uint8_t> out;
    frame_login_request(out, "ltxusr", "ltxpass", "LTXSESS01", from);
    ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
    return fd;
  };

  int fd = connect_now(1);
  std::vector<std::uint8_t> obuf;
  std::uint8_t rbuf[16384];
  std::uint64_t token_no = 0;

  auto drain = [&](bool block_until_progress) {
    const std::uint64_t before = cs.received();
    for (int spin = 0; spin < (block_until_progress ? 2000000 : 1); ++spin) {
      const ssize_t n = ::recv(fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
      if (n > 0) {
        cs.consume(rbuf, static_cast<std::size_t>(n),
                   [&](std::uint64_t seq, const std::uint8_t* p, std::size_t len) {
                     const std::uint64_t d = digest(p, len);
                     auto it = seen.find(seq);
                     if (it == seen.end()) seen.emplace(seq, d);
                     else if (it->second != d) seen[seq] = 0;   // mismatch marker
                     ++acks;
                   });
      }
      if (block_until_progress && cs.received() > before) break;
    }
  };

  const auto t_start = std::chrono::steady_clock::now();
  for (int i = 0; i < a.orders; ++i) {
    // A mix that leaves resting orders on both sides and occasionally crosses,
    // so the acknowledgement stream contains executions as well as accepts.
    EnterOrder eo{};
    set_token(eo.token, ++token_no);
    const bool buy = (i & 1) == 0;
    eo.side = buy ? 'B' : 'S';
    eo.shares = 10000;
    std::memcpy(eo.symbol, "ETH     ", kSymbolLen);
    const Tick tick = buy ? static_cast<Tick>(19100 - 1 - (i / 2) % 12)
                          : static_cast<Tick>(19100 + 1 + (i / 2) % 12);
    eo.price = tick_to_wire_price(tick, 1);
    eo.tif = 'D';
    std::uint8_t body[64];
    obuf.clear();
    frame_unsequenced(obuf, body, encode_enter(body, eo));

    const std::uint64_t t0 = rdtsc_begin();
    ::send(fd, obuf.data(), obuf.size(), MSG_NOSIGNAL);
    ++sent;
    const std::uint64_t before = cs.received();
    for (int spin = 0; spin < 2000000; ++spin) {
      const ssize_t n = ::recv(fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
      if (n > 0) {
        cs.consume(rbuf, static_cast<std::size_t>(n),
                   [&](std::uint64_t seq, const std::uint8_t* p, std::size_t len) {
                     const std::uint64_t d = digest(p, len);
                     auto it = seen.find(seq);
                     if (it == seen.end()) seen.emplace(seq, d);
                     else if (it->second != d) seen[seq] = 0;
                     ++acks;
                   });
      }
      if (cs.received() > before) break;
    }
    if (cs.received() > before) rtt.add(rdtsc_end() - t0);

    // The interesting part: get the client genuinely behind, then pull the
    // connection out from under it. Waiting for every acknowledgement before
    // sending the next order means the client is never behind, and a recovery
    // test where nothing was missed proves nothing. So: fire a burst without
    // reading any of the replies, then close. TCP still delivers what is already
    // in the send buffer, so the exchange processes those orders and publishes
    // their acknowledgements to a client that is no longer there to hear them.
    if (a.drop_at > 0 && static_cast<int>(cs.received()) >= a.drop_at && reconnects == 0) {
      for (int k = 0; k < a.burst; ++k) {
        EnterOrder be{};
        set_token(be.token, ++token_no);
        const bool b2 = (k & 1) == 0;
        be.side = b2 ? 'B' : 'S';
        be.shares = 10000;
        std::memcpy(be.symbol, "ETH     ", kSymbolLen);
        be.price = tick_to_wire_price(
            b2 ? static_cast<Tick>(19100 - 1 - k % 12) : static_cast<Tick>(19100 + 1 + k % 12),
            1);
        be.tif = 'D';
        std::uint8_t bb[64];
        obuf.clear();
        frame_unsequenced(obuf, bb, encode_enter(bb, be));
        ::send(fd, obuf.data(), obuf.size(), MSG_NOSIGNAL);
        ++sent;
      }
      // Give the exchange a moment to process what is already on the wire, then
      // disappear without reading a single reply.
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      ::close(fd);
      ++reconnects;
      resume_at = cs.resume_from();
      behind = store.count() >= resume_at ? store.count() - resume_at + 1 : 0;
      const std::uint64_t had = cs.received();
      fd = connect_now(resume_at);
      for (int t = 0; t < 400 && cs.received() - had < behind; ++t) {
        drain(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      replayed = cs.received() - had;
    }
  }
  drain(true);
  // Let anything still owed arrive.
  for (int i = 0; i < 200 && cs.received() < store.count(); ++i) {
    drain(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

  obuf.clear();
  frame_empty(obuf, SoupType::LogoutRequest);
  ::send(fd, obuf.data(), obuf.size(), MSG_NOSIGNAL);
  ::close(fd);
  stop.store(true);
  ::shutdown(listener, SHUT_RDWR);
  ::close(listener);
  server.join();

  // --- verify ---------------------------------------------------------------
  std::uint64_t missing = 0, wrong = 0;
  for (std::uint64_t s = 1; s <= store.count(); ++s) {
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    store.get(s, p, n);
    auto it = seen.find(s);
    if (it == seen.end()) { ++missing; continue; }
    if (it->second != digest(p, n)) ++wrong;
  }

  std::printf("ouch over soupbintcp, loopback, server on cpu %d and client on cpu %d\n",
              a.server_core, a.client_core);
  std::printf("  orders sent            %llu in %.2f s (%.0f orders/s)\n",
              static_cast<unsigned long long>(sent), elapsed,
              elapsed > 0 ? sent / elapsed : 0.0);
  std::printf("  sequenced messages     %llu published, %llu distinct received\n",
              static_cast<unsigned long long>(store.count()),
              static_cast<unsigned long long>(seen.size()));
  std::printf("  tcp connections        %d\n", connections.load());
  std::printf("  connection killed      after %d acks, with %llu messages already published\n"
              "                         that the client had never seen\n",
              a.drop_at, static_cast<unsigned long long>(behind));
  std::printf("  resumed from           sequence %llu, %llu messages replayed\n",
              static_cast<unsigned long long>(resume_at),
              static_cast<unsigned long long>(replayed));
  std::printf("\n  RECOVERY: %llu missing, %llu altered, of %llu sequenced messages\n",
              static_cast<unsigned long long>(missing), static_cast<unsigned long long>(wrong),
              static_cast<unsigned long long>(store.count()));
  std::printf("\n  order entry round trip, client send to acknowledgement in hand\n");
  std::printf("    p50 %.0f ns   p99 %.0f ns   p99.9 %.0f ns   max %.1f us   (%llu samples)\n",
              to_ns(rtt.pct_cycles(50), ghz, overhead), to_ns(rtt.pct_cycles(99), ghz, overhead),
              to_ns(rtt.pct_cycles(99.9), ghz, overhead),
              to_ns(static_cast<double>(rtt.max_cycles()), ghz, overhead) / 1000.0,
              static_cast<unsigned long long>(rtt.count()));
  (void)acks;
  return (missing == 0 && wrong == 0) ? 0 : 2;
}
