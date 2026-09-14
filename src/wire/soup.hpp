// SoupBinTCP: the session layer underneath order entry.
//
// The market data side of this repository is a broadcast feed with a sequence
// number and a gap you notice afterwards. Order entry is the other half, and it
// has a harder job: a TCP connection that can drop at any moment, and a client
// that must be able to come back and find out exactly what happened while it was
// away. SoupBinTCP is how NASDAQ solves that, and the interesting part is not
// the framing, it is the recovery contract:
//
//   - Everything the server sends as Sequenced Data is implicitly numbered. The
//     numbers are not on the wire; both sides count.
//   - On login the client names the sequence number it wants to start from. 1
//     means the beginning of the session, 0 means "whatever is next, I do not
//     care what I missed".
//   - The server answers with the number it will actually start from, and then
//     replays from there. A client that died after message 900 reconnects
//     asking for 901 and gets 901 onward, byte for byte, as if nothing had
//     happened.
//   - Unsequenced Data goes the other way, client to server, and is not
//     replayable. An order you sent but never saw acknowledged is an order whose
//     fate you have to ask about, which is why the acknowledgement stream is the
//     sequenced one.
//
// Framing: a 2 byte big endian length covering the type byte and the payload,
// then the type byte, then the payload.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wire/byteorder.hpp"

namespace ltx::wire {

inline constexpr std::size_t kSoupHeaderLen = 3;   // length(2) + type(1)

enum class SoupType : char {
  // Server to client.
  LoginAccepted = 'A',
  LoginRejected = 'J',
  SequencedData = 'S',
  ServerHeartbeat = 'H',
  EndOfSession = 'Z',
  // Client to server.
  LoginRequest = 'L',
  UnsequencedData = 'U',
  ClientHeartbeat = 'R',
  LogoutRequest = 'O',
  // Either way.
  Debug = '+',
};

enum class LoginReject : char {
  NotAuthorised = 'A',
  SessionNotAvailable = 'S',
};

inline constexpr std::size_t kUsernameLen = 6;
inline constexpr std::size_t kPasswordLen = 10;
inline constexpr std::size_t kSoupSessionLen = 10;
inline constexpr std::size_t kSeqTextLen = 20;

struct LoginRequest {
  char username[kUsernameLen];
  char password[kPasswordLen];
  char session[kSoupSessionLen];       // blank means "any"
  std::uint64_t requested_sequence;    // 0 means "start from the next one"
};

struct LoginAccepted {
  char session[kSoupSessionLen];
  std::uint64_t sequence;              // the first one the server will send
};

// Numeric fields in the login handshake are right justified ASCII, space
// padded, which is a real thing about this protocol and a real source of bugs.
inline void put_numeric(char* dst, std::size_t n, std::uint64_t v) {
  char tmp[32];
  int len = std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(v));
  if (len < 0) len = 0;
  if (static_cast<std::size_t>(len) > n) len = static_cast<int>(n);
  std::memset(dst, ' ', n);
  std::memcpy(dst + n - static_cast<std::size_t>(len), tmp, static_cast<std::size_t>(len));
}

inline std::uint64_t get_numeric(const char* src, std::size_t n) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const char c = src[i];
    if (c == ' ') continue;
    if (c < '0' || c > '9') return v;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return v;
}

inline void put_alpha(char* dst, std::size_t n, const std::string& s) {
  std::memset(dst, ' ', n);
  std::memcpy(dst, s.data(), std::min(n, s.size()));
}

// --- framing ---------------------------------------------------------------
// Appends one packet. Returns the total bytes written.
inline std::size_t frame(std::vector<std::uint8_t>& out, char type, const void* payload,
                         std::size_t len) {
  const std::size_t total = 1 + len;
  const std::size_t at = out.size();
  out.resize(at + 2 + total);
  store_be<std::uint16_t>(out.data() + at, static_cast<std::uint16_t>(total));
  out[at + 2] = static_cast<std::uint8_t>(type);
  if (len) std::memcpy(out.data() + at + 3, payload, len);
  return 2 + total;
}

// A packet seen in a buffer. `payload` points into the caller's bytes.
struct Packet {
  char type = 0;
  const std::uint8_t* payload = nullptr;
  std::size_t len = 0;
  std::size_t consumed = 0;   // bytes of the buffer this packet occupied
};

// Returns false when the buffer does not yet hold a whole packet, which on a
// stream protocol is the normal case rather than an error.
inline bool parse(const std::uint8_t* p, std::size_t n, Packet& out) {
  if (n < 2) return false;
  const std::size_t total = load_be<std::uint16_t>(p);
  if (total == 0) return false;          // a zero length packet is malformed
  if (n < 2 + total) return false;
  out.type = static_cast<char>(p[2]);
  out.payload = p + 3;
  out.len = total - 1;
  out.consumed = 2 + total;
  return true;
}

inline std::size_t frame_login_request(std::vector<std::uint8_t>& out,
                                       const std::string& user, const std::string& pass,
                                       const std::string& session, std::uint64_t seq) {
  char body[kUsernameLen + kPasswordLen + kSoupSessionLen + kSeqTextLen];
  put_alpha(body, kUsernameLen, user);
  put_alpha(body + kUsernameLen, kPasswordLen, pass);
  put_alpha(body + kUsernameLen + kPasswordLen, kSoupSessionLen, session);
  put_numeric(body + kUsernameLen + kPasswordLen + kSoupSessionLen, kSeqTextLen, seq);
  return frame(out, static_cast<char>(SoupType::LoginRequest), body, sizeof(body));
}

inline bool decode_login_request(const std::uint8_t* p, std::size_t n, LoginRequest& out) {
  if (n != kUsernameLen + kPasswordLen + kSoupSessionLen + kSeqTextLen) return false;
  const char* c = reinterpret_cast<const char*>(p);
  std::memcpy(out.username, c, kUsernameLen);
  std::memcpy(out.password, c + kUsernameLen, kPasswordLen);
  std::memcpy(out.session, c + kUsernameLen + kPasswordLen, kSoupSessionLen);
  out.requested_sequence =
      get_numeric(c + kUsernameLen + kPasswordLen + kSoupSessionLen, kSeqTextLen);
  return true;
}

inline std::size_t frame_login_accepted(std::vector<std::uint8_t>& out,
                                        const std::string& session, std::uint64_t seq) {
  char body[kSoupSessionLen + kSeqTextLen];
  put_alpha(body, kSoupSessionLen, session);
  put_numeric(body + kSoupSessionLen, kSeqTextLen, seq);
  return frame(out, static_cast<char>(SoupType::LoginAccepted), body, sizeof(body));
}

inline bool decode_login_accepted(const std::uint8_t* p, std::size_t n, LoginAccepted& out) {
  if (n != kSoupSessionLen + kSeqTextLen) return false;
  const char* c = reinterpret_cast<const char*>(p);
  std::memcpy(out.session, c, kSoupSessionLen);
  out.sequence = get_numeric(c + kSoupSessionLen, kSeqTextLen);
  return true;
}

inline std::size_t frame_login_rejected(std::vector<std::uint8_t>& out, LoginReject why) {
  const char code = static_cast<char>(why);
  return frame(out, static_cast<char>(SoupType::LoginRejected), &code, 1);
}

inline std::size_t frame_empty(std::vector<std::uint8_t>& out, SoupType t) {
  return frame(out, static_cast<char>(t), nullptr, 0);
}

inline std::size_t frame_sequenced(std::vector<std::uint8_t>& out, const void* p,
                                   std::size_t n) {
  return frame(out, static_cast<char>(SoupType::SequencedData), p, n);
}

inline std::size_t frame_unsequenced(std::vector<std::uint8_t>& out, const void* p,
                                     std::size_t n) {
  return frame(out, static_cast<char>(SoupType::UnsequencedData), p, n);
}

}  // namespace ltx::wire
