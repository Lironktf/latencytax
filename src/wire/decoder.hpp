// MoldUDP64 packet reader and ITCH message decoder.
//
// The decoder never allocates and never copies a payload. It walks a buffer,
// validates each frame against the buffer it actually has, and hands out
// (locate, Command) pairs. Everything it can reject, it rejects by counting
// rather than by throwing, because a feed handler that throws on a malformed
// packet is a feed handler that stops during the one minute it was needed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/engine.hpp"
#include "wire/itch.hpp"

namespace ltx::wire {

struct DecodeStats {
  std::uint64_t packets = 0;
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;
  std::uint64_t commands = 0;
  // A MoldUDP64 sequence number that did not follow the previous one. The count
  // is of events, and gap_messages is how many messages were missed in total.
  std::uint64_t sequence_gaps = 0;
  std::uint64_t gap_messages = 0;
  std::uint64_t out_of_order_packets = 0;
  std::uint64_t truncated_packets = 0;
  std::uint64_t bad_message_length = 0;
  std::uint64_t unknown_type = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t end_of_session = 0;
};

// One decoded unit of work: which book, and what to do to it.
struct Routed {
  std::uint16_t locate;
  Command cmd;
};

class Decoder {
 public:
  // What a directory message told us about a symbol. Named apart from
  // ltx::SymbolSpec, which is what the book side uses, so the two never get
  // confused at a call site.
  struct WireSymbol {
    std::string symbol;
    int price_decimals = 1;
    int size_decimals = 4;
    bool known = false;
  };

  // price_decimals per locate, learned from the directory messages.
  void reset() {
    specs_.clear();
    session_marks_.clear();
    expected_seq_ = 0;
    have_seq_ = false;
  }

  const std::vector<WireSymbol>& specs() const { return specs_; }
  // Index into the output vector at which each trading session began. One
  // session is one day, so this is where a consumer resets anything it tracks
  // per day. Recorded rather than dispatched so the hot path stays a switch.
  const std::vector<std::size_t>& session_marks() const { return session_marks_; }
  const DecodeStats& stats() const { return stats_; }
  DecodeStats& stats() { return stats_; }

  // Decodes one MoldUDP64 packet. Appends to `out`. Returns false if the packet
  // could not be read at all.
  bool decode_packet(const std::uint8_t* p, std::size_t n, std::vector<Routed>& out);
  // Set before decoding so session markers can be recorded against positions in
  // the caller's output vector.
  void track_sessions(bool on) { track_sessions_ = on; }
  // Marks are recorded as positions in the vector the caller passed. A caller
  // that reuses one small vector per packet clears both together and gets
  // per packet positions, which is what the streaming reader does.
  void clear_session_marks() { session_marks_.clear(); }

  // Walks a whole buffer of back to back packets.
  std::size_t decode_stream(const std::uint8_t* p, std::size_t n, std::vector<Routed>& out);

 private:
  // Returns the number of commands appended.
  std::size_t decode_message(const std::uint8_t* m, std::size_t len,
                             std::vector<Routed>& out);

  std::vector<WireSymbol> specs_;
  std::vector<std::size_t> session_marks_;
  bool track_sessions_ = false;
  std::uint64_t expected_seq_ = 0;
  bool have_seq_ = false;
  // ITCH Order Replace carries a new order reference. The engine keeps the id
  // it was given, so a replace becomes a delete of the old reference followed
  // by an add of the new one, which is exactly what the message means.
  DecodeStats stats_;
};

}  // namespace ltx::wire
