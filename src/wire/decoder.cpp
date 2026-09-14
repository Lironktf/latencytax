#include "wire/decoder.hpp"

#include <cstring>

namespace ltx::wire {
namespace {

// Trims the trailing spaces ITCH pads symbols with.
std::string trim_symbol(const char* s) {
  std::size_t n = kSymbolLen;
  while (n > 0 && s[n - 1] == ' ') --n;
  return std::string(s, n);
}

}  // namespace

std::size_t Decoder::decode_message(const std::uint8_t* m, std::size_t len,
                                    std::vector<Routed>& out) {
  if (len < kItchHeaderLen) {
    ++stats_.bad_message_length;
    return 0;
  }
  const Header h = decode_header(m);
  const std::size_t want = message_length(h.type);
  if (want == 0) {
    ++stats_.unknown_type;
    return 0;
  }
  if (len != want) {
    ++stats_.bad_message_length;
    return 0;
  }

  // Engine timestamps are nanoseconds; the feed's are nanoseconds since
  // midnight, which is what the engine gets. Nothing downstream needs the date.
  const Ts ts = static_cast<Ts>(h.timestamp_ns);
  const std::uint16_t loc = h.stock_locate;

  if (h.type == static_cast<char>(MsgType::SymbolDirectory)) {
    const SymbolDirectoryMsg d = decode_symbol_directory(m);
    if (loc >= specs_.size()) specs_.resize(static_cast<std::size_t>(loc) + 1);
    specs_[loc].symbol = trim_symbol(d.symbol);
    specs_[loc].price_decimals = d.price_decimals;
    specs_[loc].size_decimals = d.size_decimals;
    specs_[loc].known = true;
    return 0;
  }
  if (h.type == static_cast<char>(MsgType::SystemEvent)) {
    if (track_sessions_ && m[11] == static_cast<std::uint8_t>(
                               SystemEventCode::StartOfSystemHours)) {
      session_marks_.push_back(out.size());
    }
    return 0;
  }

  if (loc >= specs_.size() || !specs_[loc].known) {
    ++stats_.unknown_symbol;
    return 0;
  }
  const int pdec = specs_[loc].price_decimals;

  switch (h.type) {
    case 'A':
    case 'F': {
      const AddOrderMsg a = decode_add(m);
      Command c{};
      c.ts = ts;
      c.id = a.order_ref;
      c.qty = wire_to_qty(a.shares);
      c.tick = wire_price_to_tick(a.price, pdec);
      c.type = CmdType::AddLimit;
      c.side = (a.side == 'B') ? Side::Buy : Side::Sell;
      c.tif = Tif::Gtc;
      out.push_back(Routed{loc, c});
      return 1;
    }
    case 'E':
    case 'C': {
      const OrderExecutedMsg e = decode_executed(m);
      Command c{};
      c.ts = ts;
      c.id = e.order_ref;
      c.qty = wire_to_qty(e.shares);
      c.type = CmdType::Execute;
      out.push_back(Routed{loc, c});
      return 1;
    }
    case 'X': {
      const OrderCancelMsg x = decode_cancel(m);
      Command c{};
      c.ts = ts;
      c.id = x.order_ref;
      c.qty = wire_to_qty(x.shares);
      c.type = CmdType::Reduce;
      out.push_back(Routed{loc, c});
      return 1;
    }
    case 'D': {
      const OrderDeleteMsg d = decode_delete(m);
      Command c{};
      c.ts = ts;
      c.id = d.order_ref;
      c.type = CmdType::Cancel;
      out.push_back(Routed{loc, c});
      return 1;
    }
    case 'U': {
      // Replace names both references, so one command carries the whole thing.
      // The engine takes the side from the order being retired, since the
      // message does not carry it.
      const OrderReplaceMsg r = decode_replace(m);
      Command c{};
      c.ts = ts;
      c.id = r.original_ref;
      c.id2 = r.new_ref;
      c.qty = wire_to_qty(r.shares);
      c.tick = wire_price_to_tick(r.price, pdec);
      c.type = CmdType::Replace;
      out.push_back(Routed{loc, c});
      return 1;
    }
    case 'P':
      // A non displayable trade. It never rested, so it changes no book state.
      return 0;
    default:
      ++stats_.unknown_type;
      return 0;
  }
}

bool Decoder::decode_packet(const std::uint8_t* p, std::size_t n, std::vector<Routed>& out) {
  if (n < kMoldHeaderLen) {
    ++stats_.truncated_packets;
    return false;
  }
  const std::uint64_t seq = load_be<std::uint64_t>(p + kSessionLen);
  const std::uint16_t count = load_be<std::uint16_t>(p + kSessionLen + 8);
  ++stats_.packets;
  stats_.bytes += n;

  if (count == kEndOfSession) {
    ++stats_.end_of_session;
    return true;
  }

  if (have_seq_) {
    if (seq > expected_seq_) {
      ++stats_.sequence_gaps;
      stats_.gap_messages += seq - expected_seq_;
    } else if (seq < expected_seq_) {
      ++stats_.out_of_order_packets;
    }
  }

  std::size_t off = kMoldHeaderLen;
  std::uint16_t seen = 0;
  for (; seen < count; ++seen) {
    if (off + 2 > n) {
      ++stats_.truncated_packets;
      break;
    }
    const std::size_t len = load_be<std::uint16_t>(p + off);
    off += 2;
    if (off + len > n) {
      ++stats_.truncated_packets;
      break;
    }
    ++stats_.messages;
    stats_.commands += decode_message(p + off, len, out);
    off += len;
  }
  expected_seq_ = seq + seen;
  have_seq_ = true;
  return true;
}

std::size_t Decoder::decode_stream(const std::uint8_t* p, std::size_t n,
                                   std::vector<Routed>& out) {
  // Packets are stored back to back with a 4 byte big endian length in front,
  // which is how a capture of a datagram feed is usually written to a file.
  std::size_t off = 0, packets = 0;
  while (off + 4 <= n) {
    const std::uint32_t plen = load_be<std::uint32_t>(p + off);
    off += 4;
    if (plen == 0 || off + plen > n) {
      ++stats_.truncated_packets;
      break;
    }
    decode_packet(p + off, plen, out);
    off += plen;
    ++packets;
  }
  return packets;
}

}  // namespace ltx::wire
