// Order entry tests: SoupBinTCP framing, the session state machine, and OUCH
// message layouts.
//
// Two of these matter more than the rest.
//
//   Fragmentation. TCP hands you arbitrary byte boundaries, and a parser that
//   only works when a read happens to contain whole packets is a parser that
//   works in testing and fails in production. One case feeds the session a byte
//   at a time and requires the result to be identical to one big read.
//
//   Recovery. A client that dies part way through must be able to reconnect and
//   be made whole, exactly, in order, with nothing missing and nothing repeated.
//   The state machine has no sockets in it, so that can be fuzzed: kill and
//   resume a session hundreds of times at random points and check every message
//   against what the server stored.
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "wire/ouch.hpp"
#include "wire/session.hpp"
#include "wire/soup.hpp"

using namespace ltx;
using namespace ltx::wire;

namespace {

std::uint64_t digest(const std::uint8_t* p, std::size_t n) {
  std::uint64_t h = 1469598103934665603ull;
  for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

void test_framing() {
  std::vector<std::uint8_t> buf;
  const char body[] = "hello";
  const std::size_t n = frame(buf, 'S', body, 5);
  CHECK_EQ(n, size_t(8));               // 2 length + 1 type + 5 payload
  CHECK_EQ(buf.size(), size_t(8));
  CHECK_EQ(int(buf[0]), 0);             // length is big endian and covers type
  CHECK_EQ(int(buf[1]), 6);
  CHECK_EQ(int(buf[2]), int('S'));

  Packet pk;
  CHECK(parse(buf.data(), buf.size(), pk));
  CHECK_EQ(int(pk.type), int('S'));
  CHECK_EQ(pk.len, size_t(5));
  CHECK_EQ(pk.consumed, size_t(8));
  CHECK_EQ(std::memcmp(pk.payload, "hello", 5), 0);

  // One byte short is not an error, it is the normal state of a stream.
  CHECK(!parse(buf.data(), buf.size() - 1, pk));
  CHECK(!parse(buf.data(), 1, pk));
  CHECK(!parse(buf.data(), 0, pk));

  // Two packets back to back.
  frame(buf, 'H', nullptr, 0);
  CHECK(parse(buf.data(), buf.size(), pk));
  CHECK_EQ(pk.consumed, size_t(8));
  Packet pk2;
  CHECK(parse(buf.data() + pk.consumed, buf.size() - pk.consumed, pk2));
  CHECK_EQ(int(pk2.type), int('H'));
  CHECK_EQ(pk2.len, size_t(0));
}

// The login handshake carries numbers as right justified ASCII, space padded,
// which is a real property of this protocol and a real source of bugs.
void test_numeric_fields() {
  char f[20];
  put_numeric(f, 20, 0);
  CHECK_EQ(f[19], '0');
  CHECK_EQ(f[0], ' ');
  CHECK_EQ(get_numeric(f, 20), std::uint64_t(0));

  put_numeric(f, 20, 1234567890123ull);
  CHECK_EQ(get_numeric(f, 20), std::uint64_t(1234567890123ull));
  put_numeric(f, 20, 1);
  CHECK_EQ(get_numeric(f, 20), std::uint64_t(1));
  CHECK_EQ(f[19], '1');
  CHECK_EQ(f[18], ' ');

  std::vector<std::uint8_t> buf;
  frame_login_request(buf, "ltxusr", "secret", "SESS01", 4242);
  Packet pk;
  CHECK(parse(buf.data(), buf.size(), pk));
  LoginRequest lr{};
  CHECK(decode_login_request(pk.payload, pk.len, lr));
  CHECK_EQ(std::memcmp(lr.username, "ltxusr", 6), 0);
  CHECK_EQ(lr.requested_sequence, std::uint64_t(4242));

  buf.clear();
  frame_login_accepted(buf, "SESS01", 99);
  CHECK(parse(buf.data(), buf.size(), pk));
  LoginAccepted la{};
  CHECK(decode_login_accepted(pk.payload, pk.len, la));
  CHECK_EQ(la.sequence, std::uint64_t(99));
  CHECK_EQ(std::memcmp(la.session, "SESS01    ", 10), 0);
}

void test_ouch_layouts() {
  std::uint8_t b[128];
  EnterOrder e{};
  set_token(e.token, 7);
  e.side = 'B';
  e.shares = 123456;
  std::memcpy(e.symbol, "ETH     ", kSymbolLen);
  e.price = 19160000;
  e.tif = 'D';
  CHECK_EQ(encode_enter(b, e), kEnterOrderLen);
  CHECK_EQ(int(b[0]), int('O'));
  CHECK_EQ(int(b[15]), int('B'));
  CHECK_EQ(load_be<std::uint32_t>(b + 16), 123456u);
  CHECK_EQ(std::memcmp(b + 20, "ETH     ", 8), 0);
  CHECK_EQ(load_be<std::uint32_t>(b + 28), 19160000u);
  EnterOrder e2{};
  CHECK(decode_enter(b, kEnterOrderLen, e2));
  CHECK_EQ(std::memcmp(e2.token, e.token, kTokenLen), 0);
  CHECK_EQ(e2.shares, e.shares);
  CHECK_EQ(e2.price, e.price);
  CHECK(!decode_enter(b, kEnterOrderLen - 1, e2));

  CancelOrder c{};
  set_token(c.token, 9);
  c.shares = 500;
  CHECK_EQ(encode_cancel_order(b, c), kCancelOrderLen);
  CancelOrder c2{};
  CHECK(decode_cancel_order(b, kCancelOrderLen, c2));
  CHECK_EQ(c2.shares, 500u);

  ReplaceOrder r{};
  set_token(r.existing_token, 1);
  set_token(r.new_token, 2);
  r.shares = 77;
  r.price = 19150000;
  CHECK_EQ(encode_replace_order(b, r), kReplaceOrderLen);
  ReplaceOrder r2{};
  CHECK(decode_replace_order(b, kReplaceOrderLen, r2));
  CHECK_EQ(std::memcmp(r2.new_token, r.new_token, kTokenLen), 0);
  CHECK_EQ(r2.price, 19150000u);

  Accepted a{};
  a.timestamp_ns = 0x0102030405060708ull;
  set_token(a.token, 3);
  a.side = 'S';
  a.shares = 10;
  std::memcpy(a.symbol, "ETH     ", kSymbolLen);
  a.price = 1;
  a.order_reference = 424242;
  a.state = 'L';
  CHECK_EQ(encode_accepted(b, a), kAcceptedLen);
  CHECK_EQ(int(b[0]), int('A'));
  Accepted a2{};
  CHECK(decode_accepted(b, kAcceptedLen, a2));
  CHECK(a2.timestamp_ns == a.timestamp_ns);
  CHECK(a2.order_reference == a.order_reference);
  CHECK_EQ(a2.state, 'L');

  Executed x{};
  x.timestamp_ns = 5;
  set_token(x.token, 4);
  x.shares = 3;
  x.price = 7;
  x.match_number = 11;
  CHECK_EQ(encode_executed(b, x), kExecutedLen);
  Executed x2{};
  CHECK(decode_executed(b, kExecutedLen, x2));
  CHECK(x2.match_number == 11u);

  Canceled k{};
  k.timestamp_ns = 1;
  set_token(k.token, 5);
  k.shares = 2;
  k.reason = 'U';
  CHECK_EQ(encode_canceled(b, k), kCanceledLen);
  Canceled k2{};
  CHECK(decode_canceled(b, kCanceledLen, k2));
  CHECK_EQ(k2.reason, 'U');

  Rejected j{};
  j.timestamp_ns = 1;
  set_token(j.token, 6);
  j.reason = 'P';
  CHECK_EQ(encode_rejected(b, j), kRejectedLen);
  Rejected j2{};
  CHECK(decode_rejected(b, kRejectedLen, j2));
  CHECK_EQ(j2.reason, 'P');

  Replaced v{};
  v.timestamp_ns = 1;
  set_token(v.new_token, 8);
  set_token(v.previous_token, 7);
  v.shares = 4;
  v.price = 9;
  v.order_reference = 5;
  v.state = 'D';
  CHECK_EQ(encode_replaced(b, v), kReplacedLen);
  Replaced v2{};
  CHECK(decode_replaced(b, kReplacedLen, v2));
  CHECK_EQ(std::memcmp(v2.previous_token, v.previous_token, kTokenLen), 0);
  CHECK_EQ(v2.state, 'D');
}

// Fills a store with recognisable messages.
void fill(SequencedStore& st, std::uint64_t n) {
  for (std::uint64_t i = 1; i <= n; ++i) {
    std::uint8_t b[64];
    Accepted a{};
    a.timestamp_ns = i;
    set_token(a.token, i);
    a.side = (i & 1) ? 'B' : 'S';
    a.shares = static_cast<std::uint32_t>(i * 10);
    std::memcpy(a.symbol, "ETH     ", kSymbolLen);
    a.price = static_cast<std::uint32_t>(19000000 + i);
    a.order_reference = i;
    a.state = 'L';
    st.append(b, encode_accepted(b, a));
  }
}

void test_login_and_replay_positions() {
  SequencedStore st;
  fill(st, 10);

  // Requesting 0 means "whatever is next": no replay.
  {
    ServerSession s(&st, "SESS01");
    std::vector<std::uint8_t> in, out;
    frame_login_request(in, "u", "p", "", 0);
    CHECK(s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {}));
    CHECK(s.logged_in());
    CHECK_EQ(s.cursor(), std::uint64_t(11));
    ClientSession c;
    c.consume(out.data(), out.size(), [](std::uint64_t, const std::uint8_t*, std::size_t) {});
    CHECK_EQ(c.received(), std::uint64_t(0));
    CHECK_EQ(c.login_sequence(), std::uint64_t(11));
  }
  // Requesting 1 replays everything.
  {
    ServerSession s(&st, "SESS01");
    std::vector<std::uint8_t> in, out;
    frame_login_request(in, "u", "p", "", 1);
    s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {});
    ClientSession c;
    std::uint64_t seen = 0;
    c.consume(out.data(), out.size(),
              [&](std::uint64_t seq, const std::uint8_t*, std::size_t) {
                ++seen;
                CHECK_EQ(seq, seen);
              });
    CHECK_EQ(seen, std::uint64_t(10));
  }
  // Requesting the middle replays the tail.
  {
    ServerSession s(&st, "SESS01");
    std::vector<std::uint8_t> in, out;
    frame_login_request(in, "u", "p", "", 7);
    s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {});
    ClientSession c;
    std::uint64_t first = 0, seen = 0;
    c.consume(out.data(), out.size(),
              [&](std::uint64_t seq, const std::uint8_t*, std::size_t) {
                if (!seen) first = seq;
                ++seen;
              });
    CHECK_EQ(first, std::uint64_t(7));
    CHECK_EQ(seen, std::uint64_t(4));
  }
  // Requesting past the end asks for nothing, and is not an error.
  {
    ServerSession s(&st, "SESS01");
    std::vector<std::uint8_t> in, out;
    frame_login_request(in, "u", "p", "", 999);
    CHECK(s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {}));
    CHECK(s.logged_in());
    CHECK_EQ(s.cursor(), std::uint64_t(11));
  }
  // A different session name is refused.
  {
    ServerSession s(&st, "SESS01");
    std::vector<std::uint8_t> in, out;
    frame_login_request(in, "u", "p", "OTHER", 1);
    CHECK(!s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {}));
    CHECK(!s.logged_in());
    Packet pk;
    CHECK(parse(out.data(), out.size(), pk));
    CHECK_EQ(int(pk.type), int('J'));
  }
}

void test_order_before_login_is_refused() {
  SequencedStore st;
  ServerSession s(&st, "SESS01");
  std::vector<std::uint8_t> in, out;
  std::uint8_t b[64];
  EnterOrder e{};
  set_token(e.token, 1);
  e.side = 'B';
  e.shares = 1;
  std::memcpy(e.symbol, "ETH     ", kSymbolLen);
  e.price = 1;
  e.tif = 'D';
  frame_unsequenced(in, b, encode_enter(b, e));
  std::uint64_t seen = 0;
  CHECK(!s.consume(in.data(), in.size(), out, [&](const std::uint8_t*, std::size_t) {
    ++seen;
  }));
  CHECK_EQ(seen, std::uint64_t(0));
}

void test_logout_and_heartbeats() {
  SequencedStore st;
  ServerSession s(&st, "SESS01");
  std::vector<std::uint8_t> in, out;
  frame_login_request(in, "u", "p", "", 0);
  s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {});
  in.clear();
  frame_empty(in, SoupType::ClientHeartbeat);
  CHECK(s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {}));
  CHECK_EQ(s.stats().heartbeats_in, std::uint64_t(1));
  out.clear();
  s.heartbeat(out);
  Packet pk;
  CHECK(parse(out.data(), out.size(), pk));
  CHECK_EQ(int(pk.type), int('H'));
  in.clear();
  frame_empty(in, SoupType::LogoutRequest);
  CHECK(!s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {}));
  CHECK(!s.logged_in());
}

// TCP does not respect message boundaries. Feeding the session one byte at a
// time has to produce exactly the same result as one big read, or the parser
// only works when the network is being kind.
void test_byte_at_a_time_is_identical() {
  SequencedStore st;
  fill(st, 25);

  auto run = [&](std::size_t chunk) {
    ServerSession s(&st, "SESS01");
    std::vector<std::uint8_t> in, out;
    frame_login_request(in, "u", "p", "", 1);
    std::uint8_t b[64];
    for (int i = 0; i < 5; ++i) {
      EnterOrder e{};
      set_token(e.token, static_cast<std::uint64_t>(100 + i));
      e.side = 'B';
      e.shares = 1;
      std::memcpy(e.symbol, "ETH     ", kSymbolLen);
      e.price = 1;
      e.tif = 'D';
      frame_unsequenced(in, b, encode_enter(b, e));
    }
    frame_empty(in, SoupType::ClientHeartbeat);
    std::uint64_t orders = 0;
    for (std::size_t off = 0; off < in.size(); off += chunk) {
      const std::size_t n = std::min(chunk, in.size() - off);
      s.consume(in.data() + off, n, out,
                [&](const std::uint8_t*, std::size_t) { ++orders; });
    }
    return std::pair<std::uint64_t, std::vector<std::uint8_t>>(orders, out);
  };

  const auto whole = run(1 << 20);
  for (std::size_t chunk : {std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(7),
                            std::size_t(13), std::size_t(64)}) {
    const auto split = run(chunk);
    CHECK_EQ(split.first, whole.first);
    CHECK_EQ(split.second.size(), whole.second.size());
    CHECK_EQ(std::memcmp(split.second.data(), whole.second.data(), whole.second.size()), 0);
  }
  CHECK_EQ(whole.first, std::uint64_t(5));
}

// The contract: kill a session at any point and the client can come back and be
// made whole, exactly once per message, in order, byte for byte.
void test_recovery_fuzz() {
  std::mt19937_64 rng(90210);
  std::size_t failures = 0;

  for (int trial = 0; trial < 300; ++trial) {
    SequencedStore st;
    ClientSession c;
    std::vector<std::uint64_t> got;          // sequence numbers, in arrival order
    std::vector<std::uint64_t> got_digest;

    const std::uint64_t total = 20 + rng() % 200;
    std::uint64_t published = 0;
    std::uint64_t resume = 1;

    while (published < total) {
      // A fresh connection, resuming where the client believes it left off.
      ServerSession s(&st, "SESS01");
      std::vector<std::uint8_t> in, out;
      frame_login_request(in, "u", "p", "", resume);
      c.reset_connection();
      s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {});

      // The exchange publishes some more while this connection is up.
      const std::uint64_t chunk = 1 + rng() % 30;
      for (std::uint64_t k = 0; k < chunk && published < total; ++k, ++published) {
        std::uint8_t b[64];
        Canceled m{};
        m.timestamp_ns = published + 1;
        set_token(m.token, published + 1);
        m.shares = static_cast<std::uint32_t>(published + 1);
        m.reason = 'U';
        st.append(b, encode_canceled(b, m));
      }
      s.pump(out);

      // The client reads only part of what arrived before the wire dies. This
      // is the whole point: it must not assume it saw everything that was sent.
      const std::size_t keep = out.empty() ? 0 : (rng() % (out.size() + 1));
      c.consume(out.data(), keep,
                [&](std::uint64_t seq, const std::uint8_t* p, std::size_t n) {
                  got.push_back(seq);
                  got_digest.push_back(digest(p, n));
                });
      resume = c.resume_from();
    }

    // One last clean connection to collect whatever is still owed.
    {
      ServerSession s(&st, "SESS01");
      std::vector<std::uint8_t> in, out;
      frame_login_request(in, "u", "p", "", resume);
      c.reset_connection();
      s.consume(in.data(), in.size(), out, [](const std::uint8_t*, std::size_t) {});
      c.consume(out.data(), out.size(),
                [&](std::uint64_t seq, const std::uint8_t* p, std::size_t n) {
                  got.push_back(seq);
                  got_digest.push_back(digest(p, n));
                });
    }

    // Every message exactly once, in order, unaltered.
    if (got.size() != total) { ++failures; continue; }
    for (std::uint64_t i = 0; i < total; ++i) {
      if (got[i] != i + 1) { ++failures; break; }
      const std::uint8_t* p = nullptr;
      std::size_t n = 0;
      st.get(i + 1, p, n);
      if (got_digest[i] != digest(p, n)) { ++failures; break; }
    }
  }
  CHECK_EQ(failures, size_t(0));
}

// Garbage on the wire must be survivable, not fatal to the process.
void test_garbage_is_survivable() {
  std::mt19937_64 rng(5150);
  SequencedStore st;
  fill(st, 5);
  std::size_t handled = 0;
  for (int trial = 0; trial < 2000; ++trial) {
    ServerSession s(&st, "SESS01");
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> junk(rng() % 512);
    for (auto& b : junk) b = static_cast<std::uint8_t>(rng());
    s.consume(junk.data(), junk.size(), out,
              [&](const std::uint8_t*, std::size_t) { ++handled; });
  }
  CHECK(handled < 100000);   // the test is that it returned at all
}

}  // namespace

int main() {
  RUN(test_framing);
  RUN(test_numeric_fields);
  RUN(test_ouch_layouts);
  RUN(test_login_and_replay_positions);
  RUN(test_order_before_login_is_refused);
  RUN(test_logout_and_heartbeats);
  RUN(test_byte_at_a_time_is_identical);
  RUN(test_recovery_fuzz);
  RUN(test_garbage_is_survivable);
  return ltxtest::summary();
}
