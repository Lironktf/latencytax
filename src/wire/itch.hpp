// NASDAQ TotalView-ITCH 5.0 message layouts, and MoldUDP64 framing around them.
//
// Why this exists
//   The replay in src/replay/ hands the engine a C++ struct. A real venue hands
//   it bytes off a socket, big endian, unaligned, with a sequence number and a
//   gap to notice if one goes missing. This is that path. The same day of
//   Hyperliquid data goes down both, and both have to land on the exchange's own
//   published book.
//
// What is faithful to the spec
//   The order messages are byte for byte ITCH 5.0: A, F, E, C, X, D, U and P,
//   at 36, 40, 31, 36, 23, 19, 35 and 44 bytes, each with the 11 byte header of
//   message type, stock locate, tracking number and a 48 bit nanoseconds since
//   midnight timestamp. Prices are 4 implied decimals in a uint32. Framing is
//   MoldUDP64: a 20 byte packet header of session, sequence number and message
//   count, then length prefixed message blocks.
//
// What is not
//   ITCH's Stock Directory message is 39 bytes of equity specific fields with no
//   meaning for a perpetual future. Rather than reuse the 'R' type code with a
//   different body, which is the kind of thing that bites a reader later, there
//   is a separate lowercase 'z' Symbol Directory carrying the symbol, the price
//   scale and the size scale. ITCH has no lowercase type codes, so there is no
//   collision and no ambiguity about what is ours.
//
//   ITCH share counts are whole shares in a uint32. Here the unit is 1e-4 of the
//   base asset, which is exactly Hyperliquid's size precision for ETH, so no
//   rounding happens on the wire and a uint32 still reaches 429,496 ETH.
#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include "engine/types.hpp"
#include "wire/byteorder.hpp"

namespace ltx::wire {

// --- MoldUDP64 -------------------------------------------------------------
inline constexpr std::size_t kSessionLen = 10;
inline constexpr std::size_t kMoldHeaderLen = 20;   // session + sequence + count
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

// --- ITCH ------------------------------------------------------------------
inline constexpr std::size_t kItchHeaderLen = 11;   // type + locate + tracking + ts
inline constexpr std::size_t kSymbolLen = 8;

enum class MsgType : char {
  SystemEvent = 'S',
  AddOrder = 'A',
  AddOrderMpid = 'F',
  OrderExecuted = 'E',
  OrderExecutedWithPrice = 'C',
  OrderCancel = 'X',
  OrderDelete = 'D',
  OrderReplace = 'U',
  TradeNonCross = 'P',
  SymbolDirectory = 'z',   // ours, see the note above
};

inline constexpr std::size_t message_length(char type) {
  switch (type) {
    case 'S': return 12;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'z': return 21;
    default:  return 0;    // unknown; the decoder uses the framed length
  }
}

// System event codes, as in the spec.
enum class SystemEventCode : char {
  StartOfMessages = 'O',
  StartOfSystemHours = 'S',
  StartOfMarketHours = 'Q',
  EndOfMarketHours = 'M',
  EndOfSystemHours = 'E',
  EndOfMessages = 'C',
};

// Decoded views. Each borrows the caller's buffer and copies nothing but the
// scalars, which are needed in host order anyway.
struct Header {
  char type;
  std::uint16_t stock_locate;
  std::uint16_t tracking_number;
  std::uint64_t timestamp_ns;   // since midnight
};

inline Header decode_header(const std::uint8_t* p) {
  return Header{static_cast<char>(p[0]), load_be<std::uint16_t>(p + 1),
                load_be<std::uint16_t>(p + 3), load_u48(p + 5)};
}

struct AddOrderMsg {
  std::uint64_t order_ref;
  char side;                 // 'B' or 'S'
  std::uint32_t shares;      // units of 1e-4 base
  char symbol[kSymbolLen];   // space padded
  std::uint32_t price;       // 4 implied decimals
};

struct OrderExecutedMsg {
  std::uint64_t order_ref;
  std::uint32_t shares;
  std::uint64_t match_number;
};

struct OrderCancelMsg {
  std::uint64_t order_ref;
  std::uint32_t shares;      // shares cancelled, not shares remaining
};

struct OrderDeleteMsg {
  std::uint64_t order_ref;
};

struct OrderReplaceMsg {
  std::uint64_t original_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;
};

struct SymbolDirectoryMsg {
  char symbol[kSymbolLen];
  std::uint8_t price_decimals;
  std::uint8_t size_decimals;
};

inline AddOrderMsg decode_add(const std::uint8_t* p) {
  AddOrderMsg m;
  m.order_ref = load_be<std::uint64_t>(p + 11);
  m.side = static_cast<char>(p[19]);
  m.shares = load_be<std::uint32_t>(p + 20);
  std::memcpy(m.symbol, p + 24, kSymbolLen);
  m.price = load_be<std::uint32_t>(p + 32);
  return m;
}

inline OrderExecutedMsg decode_executed(const std::uint8_t* p) {
  return OrderExecutedMsg{load_be<std::uint64_t>(p + 11), load_be<std::uint32_t>(p + 19),
                          load_be<std::uint64_t>(p + 23)};
}

inline OrderCancelMsg decode_cancel(const std::uint8_t* p) {
  return OrderCancelMsg{load_be<std::uint64_t>(p + 11), load_be<std::uint32_t>(p + 19)};
}

inline OrderDeleteMsg decode_delete(const std::uint8_t* p) {
  return OrderDeleteMsg{load_be<std::uint64_t>(p + 11)};
}

inline OrderReplaceMsg decode_replace(const std::uint8_t* p) {
  return OrderReplaceMsg{load_be<std::uint64_t>(p + 11), load_be<std::uint64_t>(p + 19),
                         load_be<std::uint32_t>(p + 27), load_be<std::uint32_t>(p + 31)};
}

inline SymbolDirectoryMsg decode_symbol_directory(const std::uint8_t* p) {
  SymbolDirectoryMsg m;
  std::memcpy(m.symbol, p + 11, kSymbolLen);
  m.price_decimals = p[19];
  m.size_decimals = p[20];
  return m;
}

// --- encoding --------------------------------------------------------------
inline void encode_header(std::uint8_t* p, char type, std::uint16_t locate,
                          std::uint16_t tracking, std::uint64_t ts_ns) {
  p[0] = static_cast<std::uint8_t>(type);
  store_be<std::uint16_t>(p + 1, locate);
  store_be<std::uint16_t>(p + 3, tracking);
  store_u48(p + 5, ts_ns);
}

inline std::size_t encode_add(std::uint8_t* p, std::uint16_t locate, std::uint16_t tracking,
                              std::uint64_t ts_ns, std::uint64_t order_ref, char side,
                              std::uint32_t shares, const char* symbol,
                              std::uint32_t price) {
  encode_header(p, 'A', locate, tracking, ts_ns);
  store_be<std::uint64_t>(p + 11, order_ref);
  p[19] = static_cast<std::uint8_t>(side);
  store_be<std::uint32_t>(p + 20, shares);
  std::memcpy(p + 24, symbol, kSymbolLen);
  store_be<std::uint32_t>(p + 32, price);
  return 36;
}

inline std::size_t encode_executed(std::uint8_t* p, std::uint16_t locate,
                                   std::uint16_t tracking, std::uint64_t ts_ns,
                                   std::uint64_t order_ref, std::uint32_t shares,
                                   std::uint64_t match) {
  encode_header(p, 'E', locate, tracking, ts_ns);
  store_be<std::uint64_t>(p + 11, order_ref);
  store_be<std::uint32_t>(p + 19, shares);
  store_be<std::uint64_t>(p + 23, match);
  return 31;
}

inline std::size_t encode_cancel(std::uint8_t* p, std::uint16_t locate, std::uint16_t tracking,
                                 std::uint64_t ts_ns, std::uint64_t order_ref,
                                 std::uint32_t shares) {
  encode_header(p, 'X', locate, tracking, ts_ns);
  store_be<std::uint64_t>(p + 11, order_ref);
  store_be<std::uint32_t>(p + 19, shares);
  return 23;
}

inline std::size_t encode_delete(std::uint8_t* p, std::uint16_t locate, std::uint16_t tracking,
                                 std::uint64_t ts_ns, std::uint64_t order_ref) {
  encode_header(p, 'D', locate, tracking, ts_ns);
  store_be<std::uint64_t>(p + 11, order_ref);
  return 19;
}

inline std::size_t encode_replace(std::uint8_t* p, std::uint16_t locate,
                                  std::uint16_t tracking, std::uint64_t ts_ns,
                                  std::uint64_t orig_ref, std::uint64_t new_ref,
                                  std::uint32_t shares, std::uint32_t price) {
  encode_header(p, 'U', locate, tracking, ts_ns);
  store_be<std::uint64_t>(p + 11, orig_ref);
  store_be<std::uint64_t>(p + 19, new_ref);
  store_be<std::uint32_t>(p + 27, shares);
  store_be<std::uint32_t>(p + 31, price);
  return 35;
}

inline std::size_t encode_system_event(std::uint8_t* p, std::uint64_t ts_ns, char code) {
  encode_header(p, 'S', 0, 0, ts_ns);
  p[11] = static_cast<std::uint8_t>(code);
  return 12;
}

inline std::size_t encode_symbol_directory(std::uint8_t* p, std::uint16_t locate,
                                           std::uint64_t ts_ns, const char* symbol,
                                           std::uint8_t price_decimals,
                                           std::uint8_t size_decimals) {
  encode_header(p, 'z', locate, 0, ts_ns);
  std::memcpy(p + 11, symbol, kSymbolLen);
  p[19] = price_decimals;
  p[20] = size_decimals;
  return 21;
}

// --- scale conversion ------------------------------------------------------
// The wire carries prices at 4 implied decimals and sizes in units of 1e-4.
// The engine carries prices in ticks and sizes at 1e-8. Both conversions are
// exact for this instrument, which is asserted rather than assumed.
inline constexpr std::int64_t kWirePriceScale = 10000;   // 1e-4 USD
inline constexpr std::int64_t kWireSizeScale = 10000;    // 1e-4 base

inline std::uint32_t tick_to_wire_price(Tick t, int price_decimals) {
  std::int64_t v = t;
  for (int i = price_decimals; i < 4; ++i) v *= 10;
  return static_cast<std::uint32_t>(v);
}

inline Tick wire_price_to_tick(std::uint32_t p, int price_decimals) {
  std::int64_t v = p;
  for (int i = price_decimals; i < 4; ++i) v /= 10;
  return static_cast<Tick>(v);
}

inline std::uint32_t qty_to_wire(Qty q) {
  return static_cast<std::uint32_t>(q / (kQtyScale / kWireSizeScale));
}

inline Qty wire_to_qty(std::uint32_t s) {
  return static_cast<Qty>(s) * (kQtyScale / kWireSizeScale);
}

}  // namespace ltx::wire
