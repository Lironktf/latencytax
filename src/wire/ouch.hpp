// OUCH: order entry, carried inside SoupBinTCP.
//
// ITCH tells you what the whole market did. OUCH is how you tell the exchange
// what you want and how it tells you what became of it. The two halves have
// different shapes and the difference matters:
//
//   - An order is named by a token the client chooses, not by an identifier the
//     exchange hands back. That is deliberate: a client that sends an order and
//     then loses its connection still knows what to ask about, because it named
//     the thing itself before it ever left the building.
//   - Every inbound message has exactly one outbound consequence, and the
//     outbound stream is the sequenced one, so the answer survives a
//     disconnection even though the question does not.
//   - Prices and sizes are integers, big endian, fixed width, and the layout is
//     fixed. No optional fields, no length prefixes inside the message.
//
// On fidelity: the message set, the field lists and the semantics here follow
// OUCH 4.2. I have not had the specification in front of me to certify every
// byte offset the way I could for the ITCH messages in itch.hpp, whose lengths
// are widely published and which this does match. So the layouts below are
// defined here, pinned by the tests, and should be read as OUCH shaped rather
// than as certified OUCH. Saying which is which is worth more than claiming
// both.
#pragma once

#include <cstdint>
#include <cstring>

#include "engine/types.hpp"
#include "wire/byteorder.hpp"
#include "wire/itch.hpp"   // kSymbolLen, and the shared price and size scales

namespace ltx::wire {

inline constexpr std::size_t kTokenLen = 14;   // client chosen, space padded

// Client to exchange.
enum class OuchIn : char {
  EnterOrder = 'O',
  CancelOrder = 'X',
  ReplaceOrder = 'U',
};

// Exchange to client.
enum class OuchOut : char {
  SystemEvent = 'S',
  Accepted = 'A',
  Canceled = 'C',
  Executed = 'E',
  Rejected = 'J',
  Replaced = 'R',
};

enum class OrderState : char { Live = 'L', Dead = 'D' };

enum class RejectReason : char {
  UnknownSymbol = 'S',
  BadPrice = 'P',
  BadQuantity = 'Q',
  DuplicateToken = 'D',
  UnknownToken = 'T',
  Halted = 'H',
};

enum class CancelReasonCode : char { User = 'U', Replaced = 'R', Ioc = 'I' };

struct EnterOrder {
  char token[kTokenLen];
  char side;               // 'B' or 'S'
  std::uint32_t shares;    // units of 1e-4 base, as on the ITCH side
  char symbol[kSymbolLen];
  std::uint32_t price;     // 4 implied decimals
  char tif;                // 'D' day, 'I' immediate or cancel
};
inline constexpr std::size_t kEnterOrderLen = 1 + kTokenLen + 1 + 4 + kSymbolLen + 4 + 1;

struct CancelOrder {
  char token[kTokenLen];
  std::uint32_t shares;    // 0 means cancel the lot
};
inline constexpr std::size_t kCancelOrderLen = 1 + kTokenLen + 4;

struct ReplaceOrder {
  char existing_token[kTokenLen];
  char new_token[kTokenLen];
  std::uint32_t shares;
  std::uint32_t price;
};
inline constexpr std::size_t kReplaceOrderLen = 1 + kTokenLen + kTokenLen + 4 + 4;

struct Accepted {
  std::uint64_t timestamp_ns;
  char token[kTokenLen];
  char side;
  std::uint32_t shares;
  char symbol[kSymbolLen];
  std::uint32_t price;
  std::uint64_t order_reference;
  char state;
};
inline constexpr std::size_t kAcceptedLen =
    1 + 8 + kTokenLen + 1 + 4 + kSymbolLen + 4 + 8 + 1;

struct Executed {
  std::uint64_t timestamp_ns;
  char token[kTokenLen];
  std::uint32_t shares;
  std::uint32_t price;
  std::uint64_t match_number;
};
inline constexpr std::size_t kExecutedLen = 1 + 8 + kTokenLen + 4 + 4 + 8;

struct Canceled {
  std::uint64_t timestamp_ns;
  char token[kTokenLen];
  std::uint32_t shares;    // how many were cancelled
  char reason;
};
inline constexpr std::size_t kCanceledLen = 1 + 8 + kTokenLen + 4 + 1;

struct Rejected {
  std::uint64_t timestamp_ns;
  char token[kTokenLen];
  char reason;
};
inline constexpr std::size_t kRejectedLen = 1 + 8 + kTokenLen + 1;

struct Replaced {
  std::uint64_t timestamp_ns;
  char new_token[kTokenLen];
  char previous_token[kTokenLen];
  std::uint32_t shares;
  std::uint32_t price;
  std::uint64_t order_reference;
  char state;
};
inline constexpr std::size_t kReplacedLen =
    1 + 8 + kTokenLen + kTokenLen + 4 + 4 + 8 + 1;

// --- encode ----------------------------------------------------------------
inline std::size_t encode_enter(std::uint8_t* p, const EnterOrder& m) {
  p[0] = static_cast<std::uint8_t>(OuchIn::EnterOrder);
  std::memcpy(p + 1, m.token, kTokenLen);
  p[15] = static_cast<std::uint8_t>(m.side);
  store_be<std::uint32_t>(p + 16, m.shares);
  std::memcpy(p + 20, m.symbol, kSymbolLen);
  store_be<std::uint32_t>(p + 28, m.price);
  p[32] = static_cast<std::uint8_t>(m.tif);
  return kEnterOrderLen;
}

inline bool decode_enter(const std::uint8_t* p, std::size_t n, EnterOrder& m) {
  if (n != kEnterOrderLen) return false;
  std::memcpy(m.token, p + 1, kTokenLen);
  m.side = static_cast<char>(p[15]);
  m.shares = load_be<std::uint32_t>(p + 16);
  std::memcpy(m.symbol, p + 20, kSymbolLen);
  m.price = load_be<std::uint32_t>(p + 28);
  m.tif = static_cast<char>(p[32]);
  return true;
}

inline std::size_t encode_cancel_order(std::uint8_t* p, const CancelOrder& m) {
  p[0] = static_cast<std::uint8_t>(OuchIn::CancelOrder);
  std::memcpy(p + 1, m.token, kTokenLen);
  store_be<std::uint32_t>(p + 15, m.shares);
  return kCancelOrderLen;
}

inline bool decode_cancel_order(const std::uint8_t* p, std::size_t n, CancelOrder& m) {
  if (n != kCancelOrderLen) return false;
  std::memcpy(m.token, p + 1, kTokenLen);
  m.shares = load_be<std::uint32_t>(p + 15);
  return true;
}

inline std::size_t encode_replace_order(std::uint8_t* p, const ReplaceOrder& m) {
  p[0] = static_cast<std::uint8_t>(OuchIn::ReplaceOrder);
  std::memcpy(p + 1, m.existing_token, kTokenLen);
  std::memcpy(p + 15, m.new_token, kTokenLen);
  store_be<std::uint32_t>(p + 29, m.shares);
  store_be<std::uint32_t>(p + 33, m.price);
  return kReplaceOrderLen;
}

inline bool decode_replace_order(const std::uint8_t* p, std::size_t n, ReplaceOrder& m) {
  if (n != kReplaceOrderLen) return false;
  std::memcpy(m.existing_token, p + 1, kTokenLen);
  std::memcpy(m.new_token, p + 15, kTokenLen);
  m.shares = load_be<std::uint32_t>(p + 29);
  m.price = load_be<std::uint32_t>(p + 33);
  return true;
}

inline std::size_t encode_accepted(std::uint8_t* p, const Accepted& m) {
  p[0] = static_cast<std::uint8_t>(OuchOut::Accepted);
  store_be<std::uint64_t>(p + 1, m.timestamp_ns);
  std::memcpy(p + 9, m.token, kTokenLen);
  p[23] = static_cast<std::uint8_t>(m.side);
  store_be<std::uint32_t>(p + 24, m.shares);
  std::memcpy(p + 28, m.symbol, kSymbolLen);
  store_be<std::uint32_t>(p + 36, m.price);
  store_be<std::uint64_t>(p + 40, m.order_reference);
  p[48] = static_cast<std::uint8_t>(m.state);
  return kAcceptedLen;
}

inline bool decode_accepted(const std::uint8_t* p, std::size_t n, Accepted& m) {
  if (n != kAcceptedLen) return false;
  m.timestamp_ns = load_be<std::uint64_t>(p + 1);
  std::memcpy(m.token, p + 9, kTokenLen);
  m.side = static_cast<char>(p[23]);
  m.shares = load_be<std::uint32_t>(p + 24);
  std::memcpy(m.symbol, p + 28, kSymbolLen);
  m.price = load_be<std::uint32_t>(p + 36);
  m.order_reference = load_be<std::uint64_t>(p + 40);
  m.state = static_cast<char>(p[48]);
  return true;
}

inline std::size_t encode_executed(std::uint8_t* p, const Executed& m) {
  p[0] = static_cast<std::uint8_t>(OuchOut::Executed);
  store_be<std::uint64_t>(p + 1, m.timestamp_ns);
  std::memcpy(p + 9, m.token, kTokenLen);
  store_be<std::uint32_t>(p + 23, m.shares);
  store_be<std::uint32_t>(p + 27, m.price);
  store_be<std::uint64_t>(p + 31, m.match_number);
  return kExecutedLen;
}

inline bool decode_executed(const std::uint8_t* p, std::size_t n, Executed& m) {
  if (n != kExecutedLen) return false;
  m.timestamp_ns = load_be<std::uint64_t>(p + 1);
  std::memcpy(m.token, p + 9, kTokenLen);
  m.shares = load_be<std::uint32_t>(p + 23);
  m.price = load_be<std::uint32_t>(p + 27);
  m.match_number = load_be<std::uint64_t>(p + 31);
  return true;
}

inline std::size_t encode_canceled(std::uint8_t* p, const Canceled& m) {
  p[0] = static_cast<std::uint8_t>(OuchOut::Canceled);
  store_be<std::uint64_t>(p + 1, m.timestamp_ns);
  std::memcpy(p + 9, m.token, kTokenLen);
  store_be<std::uint32_t>(p + 23, m.shares);
  p[27] = static_cast<std::uint8_t>(m.reason);
  return kCanceledLen;
}

inline bool decode_canceled(const std::uint8_t* p, std::size_t n, Canceled& m) {
  if (n != kCanceledLen) return false;
  m.timestamp_ns = load_be<std::uint64_t>(p + 1);
  std::memcpy(m.token, p + 9, kTokenLen);
  m.shares = load_be<std::uint32_t>(p + 23);
  m.reason = static_cast<char>(p[27]);
  return true;
}

inline std::size_t encode_rejected(std::uint8_t* p, const Rejected& m) {
  p[0] = static_cast<std::uint8_t>(OuchOut::Rejected);
  store_be<std::uint64_t>(p + 1, m.timestamp_ns);
  std::memcpy(p + 9, m.token, kTokenLen);
  p[23] = static_cast<std::uint8_t>(m.reason);
  return kRejectedLen;
}

inline bool decode_rejected(const std::uint8_t* p, std::size_t n, Rejected& m) {
  if (n != kRejectedLen) return false;
  m.timestamp_ns = load_be<std::uint64_t>(p + 1);
  std::memcpy(m.token, p + 9, kTokenLen);
  m.reason = static_cast<char>(p[23]);
  return true;
}

inline std::size_t encode_replaced(std::uint8_t* p, const Replaced& m) {
  p[0] = static_cast<std::uint8_t>(OuchOut::Replaced);
  store_be<std::uint64_t>(p + 1, m.timestamp_ns);
  std::memcpy(p + 9, m.new_token, kTokenLen);
  std::memcpy(p + 23, m.previous_token, kTokenLen);
  store_be<std::uint32_t>(p + 37, m.shares);
  store_be<std::uint32_t>(p + 41, m.price);
  store_be<std::uint64_t>(p + 45, m.order_reference);
  p[53] = static_cast<std::uint8_t>(m.state);
  return kReplacedLen;
}

inline bool decode_replaced(const std::uint8_t* p, std::size_t n, Replaced& m) {
  if (n != kReplacedLen) return false;
  m.timestamp_ns = load_be<std::uint64_t>(p + 1);
  std::memcpy(m.new_token, p + 9, kTokenLen);
  std::memcpy(m.previous_token, p + 23, kTokenLen);
  m.shares = load_be<std::uint32_t>(p + 37);
  m.price = load_be<std::uint32_t>(p + 41);
  m.order_reference = load_be<std::uint64_t>(p + 45);
  m.state = static_cast<char>(p[53]);
  return true;
}

inline void set_token(char* dst, std::uint64_t n) {
  char tmp[kTokenLen + 1];
  const int len = std::snprintf(tmp, sizeof(tmp), "T%013llu",
                                static_cast<unsigned long long>(n));
  std::memset(dst, ' ', kTokenLen);
  std::memcpy(dst, tmp, std::min<std::size_t>(kTokenLen, static_cast<std::size_t>(len)));
}

}  // namespace ltx::wire
