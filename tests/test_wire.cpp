// Wire format tests.
//
// The byte layout cases spell out offsets and expected bytes by hand rather
// than round tripping through the same code that wrote them, because a round
// trip test passes just as happily when both halves are wrong together.
//
// The last case feeds the decoder corrupted and random bytes. A feed handler
// that reads one byte past a truncated packet is a feed handler that crashes
// during the exact minute it was needed, so this is run under the address
// sanitizer in the Debug build.
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "check.hpp"
#include "engine/multi_book.hpp"
#include "wire/decoder.hpp"
#include "wire/encoder.hpp"
#include "wire/itch_fixed.hpp"
#include "wire/itch.hpp"

using namespace ltx;
using namespace ltx::wire;

namespace {

constexpr Qty Q(double x) { return static_cast<Qty>(x * kQtyScale + 0.5); }

void test_byteorder() {
  std::uint8_t b[16] = {};
  store_be<std::uint16_t>(b, 0x1234);
  CHECK_EQ(int(b[0]), 0x12);
  CHECK_EQ(int(b[1]), 0x34);
  CHECK_EQ(int(load_be<std::uint16_t>(b)), 0x1234);

  store_be<std::uint32_t>(b, 0x01020304u);
  CHECK_EQ(int(b[0]), 0x01);
  CHECK_EQ(int(b[3]), 0x04);
  CHECK_EQ(load_be<std::uint32_t>(b), 0x01020304u);

  store_be<std::uint64_t>(b, 0x0102030405060708ull);
  CHECK_EQ(int(b[0]), 0x01);
  CHECK_EQ(int(b[7]), 0x08);
  CHECK(load_be<std::uint64_t>(b) == 0x0102030405060708ull);

  // 48 bit timestamps.
  store_u48(b, 0x0000AABBCCDDEEFFull & 0xFFFFFFFFFFFFull);
  CHECK(load_u48(b) == (0xAABBCCDDEEFFull));
  store_u48(b, 0);
  CHECK(load_u48(b) == 0u);
  store_u48(b, 281474976710655ull);   // 2^48 - 1
  CHECK(load_u48(b) == 281474976710655ull);

  // Unaligned access must work, since that is the normal case on the wire.
  std::uint8_t big[32] = {};
  store_be<std::uint64_t>(big + 3, 0xDEADBEEFCAFEBABEull);
  CHECK(load_be<std::uint64_t>(big + 3) == 0xDEADBEEFCAFEBABEull);
}

// The lengths in the ITCH 5.0 spec, written out so a change to the header size
// or a field width fails here first.
void test_message_lengths() {
  CHECK_EQ(message_length('S'), size_t(12));
  CHECK_EQ(message_length('A'), size_t(36));
  CHECK_EQ(message_length('F'), size_t(40));
  CHECK_EQ(message_length('E'), size_t(31));
  CHECK_EQ(message_length('C'), size_t(36));
  CHECK_EQ(message_length('X'), size_t(23));
  CHECK_EQ(message_length('D'), size_t(19));
  CHECK_EQ(message_length('U'), size_t(35));
  CHECK_EQ(message_length('P'), size_t(44));
  CHECK_EQ(message_length('z'), size_t(21));
  CHECK_EQ(message_length('Q'), size_t(0));   // not handled
  CHECK_EQ(kItchHeaderLen, size_t(11));
  CHECK_EQ(kMoldHeaderLen, size_t(20));
}

// Field offsets for Add Order, checked against the bytes rather than against
// the decoder.
void test_add_order_layout() {
  std::uint8_t b[64] = {};
  const std::size_t n = encode_add(b, 0x0102, 0x0304, 0x0000AABBCCDDull, 0x1122334455667788ull,
                                   'B', 0x00ABCDEFu, "ETH     ", 0x0012D450u);
  CHECK_EQ(n, size_t(36));
  CHECK_EQ(int(b[0]), int('A'));
  CHECK_EQ(int(b[1]), 0x01);          // stock locate, big endian
  CHECK_EQ(int(b[2]), 0x02);
  CHECK_EQ(int(b[3]), 0x03);          // tracking number
  CHECK_EQ(int(b[4]), 0x04);
  CHECK(load_u48(b + 5) == 0xAABBCCDDull);
  CHECK(load_be<std::uint64_t>(b + 11) == 0x1122334455667788ull);
  CHECK_EQ(int(b[19]), int('B'));
  CHECK_EQ(load_be<std::uint32_t>(b + 20), 0x00ABCDEFu);
  CHECK_EQ(std::memcmp(b + 24, "ETH     ", 8), 0);
  CHECK_EQ(load_be<std::uint32_t>(b + 32), 0x0012D450u);

  const Header h = decode_header(b);
  CHECK_EQ(int(h.type), int('A'));
  CHECK_EQ(int(h.stock_locate), 0x0102);
  CHECK_EQ(int(h.tracking_number), 0x0304);
  const AddOrderMsg m = decode_add(b);
  CHECK(m.order_ref == 0x1122334455667788ull);
  CHECK_EQ(int(m.side), int('B'));
  CHECK_EQ(m.shares, 0x00ABCDEFu);
  CHECK_EQ(m.price, 0x0012D450u);
}

void test_other_layouts() {
  std::uint8_t b[64] = {};
  CHECK_EQ(encode_executed(b, 1, 0, 7, 99, 500, 4242), size_t(31));
  CHECK_EQ(int(b[0]), int('E'));
  {
    const OrderExecutedMsg m = decode_executed(b);
    CHECK(m.order_ref == 99u);
    CHECK_EQ(m.shares, 500u);
    CHECK(m.match_number == 4242u);
  }
  CHECK_EQ(encode_cancel(b, 1, 0, 7, 99, 25), size_t(23));
  CHECK_EQ(int(b[0]), int('X'));
  {
    const OrderCancelMsg m = decode_cancel(b);
    CHECK(m.order_ref == 99u);
    CHECK_EQ(m.shares, 25u);
  }
  CHECK_EQ(encode_delete(b, 1, 0, 7, 99), size_t(19));
  CHECK_EQ(int(b[0]), int('D'));
  CHECK(decode_delete(b).order_ref == 99u);

  CHECK_EQ(encode_replace(b, 1, 0, 7, 99, 100, 300, 12345), size_t(35));
  CHECK_EQ(int(b[0]), int('U'));
  {
    const OrderReplaceMsg m = decode_replace(b);
    CHECK(m.original_ref == 99u);
    CHECK(m.new_ref == 100u);
    CHECK_EQ(m.shares, 300u);
    CHECK_EQ(m.price, 12345u);
  }
  CHECK_EQ(encode_symbol_directory(b, 3, 0, "ETH     ", 1, 4), size_t(21));
  CHECK_EQ(int(b[0]), int('z'));
  {
    const SymbolDirectoryMsg m = decode_symbol_directory(b);
    CHECK_EQ(std::memcmp(m.symbol, "ETH     ", 8), 0);
    CHECK_EQ(int(m.price_decimals), 1);
    CHECK_EQ(int(m.size_decimals), 4);
  }
}

// Prices and sizes have to survive the trip in both directions exactly, or the
// book built off the wire is not the book the exchange published.
void test_scale_conversion() {
  // 1916.3 with one decimal is tick 19163, and 4 implied decimals on the wire.
  CHECK_EQ(tick_to_wire_price(19163, 1), 19163000u);
  CHECK_EQ(wire_price_to_tick(19163000u, 1), Tick(19163));
  CHECK_EQ(tick_to_wire_price(1, 1), 1000u);
  CHECK_EQ(wire_price_to_tick(1000u, 1), Tick(1));

  // 260.9149 ETH is 26091490000 at 1e-8, and 2609149 at the wire's 1e-4.
  CHECK_EQ(qty_to_wire(Q(260.9149)), 2609149u);
  CHECK_EQ(wire_to_qty(2609149u), Q(260.9149));
  CHECK_EQ(qty_to_wire(Q(0.0001)), 1u);
  CHECK_EQ(wire_to_qty(1u), Q(0.0001));

  // Round trip over the range the data actually contains.
  std::size_t bad = 0;
  for (Tick t = 1; t <= 262144; t += 7) {
    if (wire_price_to_tick(tick_to_wire_price(t, 1), 1) != t) ++bad;
  }
  CHECK_EQ(bad, size_t(0));
  bad = 0;
  for (std::int64_t u = 1; u <= 4000000; u += 9973) {
    const Qty q = u * (kQtyScale / kWireSizeScale);
    if (wire_to_qty(qty_to_wire(q)) != q) ++bad;
  }
  CHECK_EQ(bad, size_t(0));
}

// Builds a feed in memory so the decoder can be driven without a file.
class MemWriter {
 public:
  std::vector<std::uint8_t> bytes;
  explicit MemWriter(std::size_t mtu = 256) : mtu_(mtu) { reset(); }

  void write(const std::uint8_t* m, std::size_t len) {
    if (off_ + 2 + len > mtu_) flush();
    store_be<std::uint16_t>(buf_ + off_, static_cast<std::uint16_t>(len));
    std::memcpy(buf_ + off_ + 2, m, len);
    off_ += 2 + len;
    ++count_;
  }
  void flush() {
    if (count_ == 0) return;
    std::memcpy(buf_, "LTXTEST   ", kSessionLen);
    store_be<std::uint64_t>(buf_ + kSessionLen, seq_);
    store_be<std::uint16_t>(buf_ + kSessionLen + 8, count_);
    std::uint8_t len[4];
    store_be<std::uint32_t>(len, static_cast<std::uint32_t>(off_));
    bytes.insert(bytes.end(), len, len + 4);
    bytes.insert(bytes.end(), buf_, buf_ + off_);
    seq_ += count_;
    reset();
  }
  // Seals the current packet and throws it away, which is what a dropped
  // datagram looks like to a reader.
  void drop() {
    if (count_ == 0) return;
    seq_ += count_;
    reset();
  }

 private:
  void reset() { off_ = kMoldHeaderLen; count_ = 0; }
  std::uint8_t buf_[2048];
  std::size_t mtu_, off_ = 0;
  std::uint16_t count_ = 0;
  std::uint64_t seq_ = 1;
};

void directory(MemWriter& w, std::uint16_t locate, const char* sym) {
  std::uint8_t b[64];
  w.write(b, encode_symbol_directory(b, locate, 0, sym, 1, 4));
}

void test_framing_and_decode() {
  MemWriter w(128);
  directory(w, 1, "ETH     ");
  std::uint8_t b[64];
  w.write(b, encode_add(b, 1, 0, 1000, 10, 'B', qty_to_wire(Q(5)), "ETH     ",
                        tick_to_wire_price(19160, 1)));
  w.write(b, encode_add(b, 1, 0, 1001, 11, 'S', qty_to_wire(Q(3)), "ETH     ",
                        tick_to_wire_price(19161, 1)));
  w.flush();

  Decoder d;
  std::vector<Routed> out;
  const std::size_t packets = d.decode_stream(w.bytes.data(), w.bytes.size(), out);
  CHECK(packets >= 1);
  CHECK_EQ(out.size(), size_t(2));
  CHECK_EQ(d.stats().sequence_gaps, std::uint64_t(0));
  CHECK_EQ(d.stats().truncated_packets, std::uint64_t(0));
  CHECK_EQ(int(out[0].locate), 1);
  CHECK(out[0].cmd.type == CmdType::AddLimit);
  CHECK(out[0].cmd.side == Side::Buy);
  CHECK_EQ(out[0].cmd.tick, Tick(19160));
  CHECK_EQ(out[0].cmd.qty, Q(5));
  CHECK(out[1].cmd.side == Side::Sell);
  CHECK_EQ(out[1].cmd.tick, Tick(19161));
  CHECK_EQ(d.specs().size(), size_t(2));
  CHECK(d.specs()[1].symbol == "ETH");
}

// A whole packet going missing has to be noticed and counted, not silently
// absorbed, and the reader has to keep going afterwards.
void test_sequence_gap() {
  MemWriter w(64);
  directory(w, 1, "ETH     ");
  w.flush();
  std::uint8_t b[64];
  for (int i = 0; i < 3; ++i) {
    w.write(b, encode_delete(b, 1, 0, 1, 100 + i));
    w.flush();
  }
  // Two messages that never make it onto the stream.
  w.write(b, encode_delete(b, 1, 0, 1, 200));
  w.write(b, encode_delete(b, 1, 0, 1, 201));
  w.drop();
  w.write(b, encode_delete(b, 1, 0, 1, 300));
  w.flush();

  Decoder d;
  std::vector<Routed> out;
  d.decode_stream(w.bytes.data(), w.bytes.size(), out);
  CHECK_EQ(d.stats().sequence_gaps, std::uint64_t(1));
  CHECK_EQ(d.stats().gap_messages, std::uint64_t(2));
  CHECK_EQ(out.size(), size_t(4));   // three before the hole, one after
  CHECK(out.back().cmd.id == 300u);
}

void test_unknown_symbol_and_type() {
  MemWriter w(256);
  std::uint8_t b[64];
  // An order before any directory message: the reader has no scale for it.
  w.write(b, encode_add(b, 7, 0, 1, 1, 'B', 100, "NOPE    ", 1000));
  // A type the reader does not handle.
  encode_header(b, 'Q', 1, 0, 1);
  w.write(b, 11);
  w.flush();

  Decoder d;
  std::vector<Routed> out;
  d.decode_stream(w.bytes.data(), w.bytes.size(), out);
  CHECK_EQ(out.size(), size_t(0));
  CHECK_EQ(d.stats().unknown_symbol, std::uint64_t(1));
  CHECK_EQ(d.stats().unknown_type, std::uint64_t(1));
}

// The whole point: bytes in, the exchange's book out.
void test_decode_into_book() {
  MemWriter w(512);
  directory(w, 1, "ETH     ");
  std::uint8_t b[64];
  const char* sym = "ETH     ";
  w.write(b, encode_add(b, 1, 0, 1, 10, 'B', qty_to_wire(Q(5)), sym, tick_to_wire_price(100, 1)));
  w.write(b, encode_add(b, 1, 0, 2, 11, 'B', qty_to_wire(Q(7)), sym, tick_to_wire_price(100, 1)));
  w.write(b, encode_add(b, 1, 0, 3, 12, 'S', qty_to_wire(Q(4)), sym, tick_to_wire_price(101, 1)));
  // The first bid gives up 2 where it stands, keeping its place in the queue.
  w.write(b, encode_cancel(b, 1, 0, 4, 10, qty_to_wire(Q(2))));
  // Then trades 1.
  w.write(b, encode_executed(b, 1, 0, 5, 10, qty_to_wire(Q(1)), 1));
  // The offer is replaced lower and larger, which sends it to the back of the
  // new queue under a new reference.
  w.write(b, encode_replace(b, 1, 0, 6, 12, 13, qty_to_wire(Q(9)),
                            tick_to_wire_price(102, 1)));
  w.flush();

  Decoder d;
  std::vector<Routed> out;
  d.decode_stream(w.bytes.data(), w.bytes.size(), out);
  CHECK_EQ(out.size(), size_t(6));

  BookConfig cfg;
  cfg.min_tick = 1;
  cfg.max_tick = 4096;
  cfg.max_orders = 1024;
  cfg.id_map_capacity = 2048;
  NullSink sink;
  MultiBook books(cfg, &sink);
  for (const Routed& r : out) {
    books.ensure(r.locate, SymbolSpec{"ETH", 1, 4});
    books.apply(r.locate, r.cmd);
  }
  const OrderBook& bk = books.engine(1).book();
  CHECK(bk.check_invariants());
  CHECK_EQ(bk.best_bid(), Tick(100));
  CHECK_EQ(bk.qty_at(100), Q(2) + Q(7));     // 5 - 2 cancelled - 1 traded, plus 7
  CHECK_EQ(bk.orders_at(100), 2u);
  CHECK_EQ(bk.queue_ahead(11), Q(2));        // order 10 kept its place
  CHECK(!bk.is_live(12));                    // retired by the replace
  CHECK(bk.is_live(13));
  CHECK_EQ(bk.best_ask(), Tick(102));
  CHECK_EQ(bk.qty_at(102), Q(9));
}

// Random and corrupted input must be survivable. Nothing here checks a result;
// the test is that it returns at all, and under the address sanitizer, that it
// never reads outside the buffer it was handed.
void test_corruption_fuzz() {
  MemWriter w(256);
  directory(w, 1, "ETH     ");
  std::uint8_t b[64];
  for (int i = 0; i < 200; ++i) {
    w.write(b, encode_add(b, 1, 0, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(i),
                          (i & 1) ? 'B' : 'S', 1000 + i, "ETH     ", 19160 + i));
  }
  w.flush();
  const std::vector<std::uint8_t> good = w.bytes;

  std::mt19937_64 rng(4242);
  std::uint64_t decoded = 0;
  for (int trial = 0; trial < 3000; ++trial) {
    std::vector<std::uint8_t> bad = good;
    const int kind = static_cast<int>(rng() % 4);
    if (kind == 0 && !bad.empty()) {
      bad.resize(rng() % bad.size());                       // truncate
    } else if (kind == 1) {
      for (int k = 0; k < 8 && !bad.empty(); ++k) {
        bad[rng() % bad.size()] = static_cast<std::uint8_t>(rng());   // flip bytes
      }
    } else if (kind == 2) {
      bad.assign(rng() % 4096, 0);                          // all zeroes
      for (auto& x : bad) x = static_cast<std::uint8_t>(rng());       // random noise
    } else if (!bad.empty()) {
      // A plausible header with an implausible length.
      const std::size_t at = rng() % bad.size();
      if (at + 4 < bad.size()) store_be<std::uint32_t>(bad.data() + at, 0xFFFFFFFFu);
    }
    Decoder d;
    std::vector<Routed> out;
    d.decode_stream(bad.data(), bad.size(), out);
    decoded += out.size();
  }
  CHECK(decoded > 0);   // the undamaged trials still produced work
}

// The fixed latency decoder exists to have the same output as the obvious one,
// not merely a similar one. If the two ever disagree the benchmark comparing
// them is meaningless, so every message type is checked field for field, and
// then a few thousand random ones on top.
void test_fixed_decoder_matches_the_switch() {
  std::uint8_t pad[128];
  std::mt19937_64 rng(31337);
  std::size_t mismatches = 0;

  auto compare = [&](std::size_t len) {
    Command a{}, b{};
    const unsigned ra = decode_switch(pad, len, 1, a);
    const unsigned rb = decode_fixed(pad, len, 1, b);
    if (ra != rb) { ++mismatches; return; }
    if (!ra) return;
    if (a.ts != b.ts || a.id != b.id || a.id2 != b.id2 || a.qty != b.qty ||
        a.tick != b.tick || a.type != b.type || a.side != b.side || a.tif != b.tif) {
      ++mismatches;
    }
  };

  for (int i = 0; i < 4000; ++i) {
    std::memset(pad, 0, sizeof(pad));
    const int pick = i % 6;
    std::size_t n = 0;
    const std::uint64_t seq = rng();
    const Tick tick = static_cast<Tick>(1 + rng() % 200000);
    const std::uint32_t sh = static_cast<std::uint32_t>(rng() % 4000000);
    switch (pick) {
      case 0:
        n = encode_add(pad, 1, 0, rng() & 0xFFFFFFFFFFFFull, seq, (rng() & 1) ? 'B' : 'S', sh,
                       "ETH     ", tick_to_wire_price(tick, 1));
        break;
      case 1: n = encode_executed(pad, 1, 0, rng() & 0xFFFFFF, seq, sh, rng()); break;
      case 2: n = encode_cancel(pad, 1, 0, rng() & 0xFFFFFF, seq, sh); break;
      case 3: n = encode_delete(pad, 1, 0, rng() & 0xFFFFFF, seq); break;
      case 4:
        n = encode_replace(pad, 1, 0, rng() & 0xFFFFFF, seq, seq + 1, sh,
                           tick_to_wire_price(tick, 1));
        break;
      default:
        // A type neither handles, and a length that disagrees with the type.
        n = encode_delete(pad, 1, 0, rng() & 0xFFFFFF, seq);
        if (rng() & 1) pad[0] = static_cast<std::uint8_t>('Q');
        else n += 1;
        break;
    }
    compare(n);
  }
  CHECK_EQ(mismatches, size_t(0));

  // A length that disagrees with the type is refused by both, not guessed at.
  std::memset(pad, 0, sizeof(pad));
  encode_add(pad, 1, 0, 1, 1, 'B', 10, "ETH     ", 1000);
  Command a{}, b{};
  CHECK_EQ(decode_switch(pad, 35, 1, a), 0u);
  CHECK_EQ(decode_fixed(pad, 35, 1, b), 0u);
  CHECK_EQ(decode_switch(pad, 36, 1, a), 1u);
  CHECK_EQ(decode_fixed(pad, 36, 1, b), 1u);
}

}  // namespace

int main() {
  RUN(test_byteorder);
  RUN(test_message_lengths);
  RUN(test_add_order_layout);
  RUN(test_other_layouts);
  RUN(test_scale_conversion);
  RUN(test_framing_and_decode);
  RUN(test_sequence_gap);
  RUN(test_unknown_symbol_and_type);
  RUN(test_decode_into_book);
  RUN(test_fixed_decoder_matches_the_switch);
  RUN(test_corruption_fuzz);
  return ltxtest::summary();
}
