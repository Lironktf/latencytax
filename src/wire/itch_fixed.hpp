// A fixed latency ITCH decoder: same work for every message, no branch on
// content.
//
// Why bother
//   The decoder in decoder.cpp switches on the message type and then reads
//   different offsets in each arm. That is the obvious way to write it and it is
//   fast on average, but its cost depends on what arrived: the branch predictor
//   does well on a run of Add Orders and badly at every transition, so the
//   latency distribution has a tail that is a property of the data rather than
//   of the work. An exchange feed handler built in hardware does not have that
//   problem, because it does the same thing to every message.
//
// How this does it
//   Every field offset becomes a table lookup indexed by the type byte, so the
//   type selects an *address* rather than a *branch*. All fields are then loaded
//   unconditionally and masked to zero when the message does not have them, so
//   the output is identical to the branching decoder rather than merely similar.
//   Whether a message produces a command becomes an increment of the output
//   index by 0 or 1, not an if.
//
//   The one thing it needs in return: 8 bytes of readable slack past the end of
//   the message, because a load can be issued for a field the message does not
//   have. Real handlers read into an oversized buffer for exactly this reason,
//   and the requirement is in the signature.
//
// What it is not
//   It is not faster on average. The point is the shape of the distribution, and
//   bench/bench_decode.cpp measures both so the claim can be checked.
#pragma once

#include <cstdint>
#include <cstring>

#include "engine/engine.hpp"
#include "wire/itch.hpp"

namespace ltx::wire {

// Per type constants. Offsets are into the message, and every one of them is a
// position a message of that type is long enough to contain.
struct FixedDesc {
  std::uint8_t len;        // expected message length, 0 for types we do not handle
  std::uint8_t id_off;     // primary order reference
  std::uint8_t id2_off;    // replacement reference, for Order Replace
  std::uint8_t qty_off;    // shares
  std::uint8_t price_off;  // price
  std::uint8_t side_off;   // buy or sell character
  std::uint64_t id2_mask;  // zero when the message has no second reference
  std::uint32_t qty_mask;
  std::uint32_t price_mask;
  std::uint8_t side_mask;
  std::uint8_t emits;      // 1 if this produces an engine command
  CmdType cmd;
};

// Built once at compile time. Unused fields point at offset 0, which every
// message is long enough to contain, and are masked to zero on the way out.
inline constexpr FixedDesc make_desc(char t) {
  FixedDesc d{};
  d.cmd = CmdType::Nop;
  switch (t) {
    case 'A':
      d = FixedDesc{36, 11, 0, 20, 32, 19, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF, 1,
                    CmdType::AddLimit};
      break;
    case 'F':
      d = FixedDesc{40, 11, 0, 20, 32, 19, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF, 1,
                    CmdType::AddLimit};
      break;
    case 'E':
      d = FixedDesc{31, 11, 0, 19, 0, 0, 0, 0xFFFFFFFFu, 0, 0, 1, CmdType::Execute};
      break;
    case 'C':
      d = FixedDesc{36, 11, 0, 19, 0, 0, 0, 0xFFFFFFFFu, 0, 0, 1, CmdType::Execute};
      break;
    case 'X':
      d = FixedDesc{23, 11, 0, 19, 0, 0, 0, 0xFFFFFFFFu, 0, 0, 1, CmdType::Reduce};
      break;
    case 'D':
      d = FixedDesc{19, 11, 0, 0, 0, 0, 0, 0, 0, 0, 1, CmdType::Cancel};
      break;
    case 'U':
      d = FixedDesc{35, 11, 19, 27, 31, 0, ~0ull, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 1,
                    CmdType::Replace};
      break;
    case 'P':
      d = FixedDesc{44, 11, 0, 20, 32, 19, 0, 0, 0, 0, 0, CmdType::Nop};
      break;
    default:
      break;
  }
  return d;
}

struct FixedTable {
  FixedDesc d[256];
  constexpr FixedTable() : d() {
    for (int i = 0; i < 256; ++i) d[i] = make_desc(static_cast<char>(i));
  }
};
inline constexpr FixedTable kFixed{};

// Decodes one message into `out`. Returns 1 if a command was produced, 0 if not.
//
// `m` must have at least 8 readable bytes past `m + len`. The decoder issues
// loads for fields the message may not have, which is the entire point.
inline unsigned decode_fixed(const std::uint8_t* m, std::size_t len, int price_decimals,
                             Command& out) {
  const FixedDesc& d = kFixed.d[m[0]];
  // The only branch that survives, and it is on the framed length rather than on
  // the content: a message whose framing disagrees with its type is rejected
  // before anything is trusted.
  if (d.len == 0 || len != d.len) return 0;

  const std::uint64_t id = load_be<std::uint64_t>(m + d.id_off);
  const std::uint64_t id2 = load_be<std::uint64_t>(m + d.id2_off) & d.id2_mask;
  const std::uint32_t shares = load_be<std::uint32_t>(m + d.qty_off) & d.qty_mask;
  const std::uint32_t price = load_be<std::uint32_t>(m + d.price_off) & d.price_mask;
  const std::uint8_t side_ch = m[d.side_off] & d.side_mask;

  out.ts = static_cast<Ts>(load_u48(m + 5));
  out.id = id;
  out.id2 = id2;
  out.qty = wire_to_qty(shares);
  // A masked price of zero converts to tick zero, which is what the branching
  // decoder leaves in a command that has no price.
  out.tick = price ? wire_price_to_tick(price, price_decimals) : 0;
  // 'B' is buy, anything else is sell, and Side::Buy is zero, so this is a
  // comparison and a move rather than a jump.
  out.side = static_cast<Side>(side_ch != 'B');
  out.tif = Tif::Gtc;
  out.type = d.cmd;
  out.pad = 0;
  return d.emits;
}

// The same job written the obvious way, for comparison. This is what the
// decoder in decoder.cpp does per message once the framing is stripped away:
// switch on the type, then read the offsets that type happens to use. Keeping
// it here, with an identical signature, is what makes the benchmark honest;
// comparing a whole packet path against a single message decode would not be.
inline unsigned decode_switch(const std::uint8_t* m, std::size_t len, int price_decimals,
                              Command& out) {
  const char type = static_cast<char>(m[0]);
  const std::size_t want = message_length(type);
  if (want == 0 || len != want) return 0;

  out.ts = static_cast<Ts>(load_u48(m + 5));
  out.id2 = 0;
  out.qty = 0;
  out.tick = 0;
  out.side = Side::Buy;
  out.tif = Tif::Gtc;
  out.pad = 0;

  switch (type) {
    case 'A':
    case 'F':
      out.id = load_be<std::uint64_t>(m + 11);
      out.side = m[19] == 'B' ? Side::Buy : Side::Sell;
      out.qty = wire_to_qty(load_be<std::uint32_t>(m + 20));
      out.tick = wire_price_to_tick(load_be<std::uint32_t>(m + 32), price_decimals);
      out.type = CmdType::AddLimit;
      return 1;
    case 'E':
    case 'C':
      out.id = load_be<std::uint64_t>(m + 11);
      out.qty = wire_to_qty(load_be<std::uint32_t>(m + 19));
      out.type = CmdType::Execute;
      return 1;
    case 'X':
      out.id = load_be<std::uint64_t>(m + 11);
      out.qty = wire_to_qty(load_be<std::uint32_t>(m + 19));
      out.type = CmdType::Reduce;
      return 1;
    case 'D':
      out.id = load_be<std::uint64_t>(m + 11);
      out.type = CmdType::Cancel;
      return 1;
    case 'U':
      out.id = load_be<std::uint64_t>(m + 11);
      out.id2 = load_be<std::uint64_t>(m + 19);
      out.qty = wire_to_qty(load_be<std::uint32_t>(m + 27));
      out.tick = wire_price_to_tick(load_be<std::uint32_t>(m + 31), price_decimals);
      out.type = CmdType::Replace;
      return 1;
    default:
      out.id = 0;
      out.type = CmdType::Nop;
      return 0;
  }
}

}  // namespace ltx::wire
