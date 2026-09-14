// Builds MoldUDP64 packets of ITCH messages.
//
// Messages are accumulated until the next one would not fit inside the target
// packet size, then the packet is sealed with its session, sequence number and
// message count and written out with a 4 byte big endian length in front. The
// length prefix is what makes a stream of datagrams readable back from a file;
// on a socket the datagram boundary does that job.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wire/itch.hpp"

namespace ltx::wire {

class PacketWriter {
 public:
  // 1400 keeps a packet inside a normal Ethernet MTU with room for UDP and IP
  // headers, which is where a real feed would sit.
  explicit PacketWriter(std::FILE* out, const std::string& session = "LTXSESS01",
                        std::size_t mtu = 1400)
      : out_(out), mtu_(mtu) {
    std::memset(session_, ' ', kSessionLen);
    std::memcpy(session_, session.data(), std::min(session.size(), kSessionLen));
    buf_.resize(mtu_ + 64);
    reset_packet();
  }

  std::uint64_t packets() const { return packets_; }
  std::uint64_t messages() const { return messages_; }
  std::uint64_t bytes() const { return bytes_; }

  // `len` bytes at `msg` become one message block.
  void write(const std::uint8_t* msg, std::size_t len) {
    if (off_ + 2 + len > mtu_ || count_ == 0xFFFE) flush();
    store_be<std::uint16_t>(buf_.data() + off_, static_cast<std::uint16_t>(len));
    std::memcpy(buf_.data() + off_ + 2, msg, len);
    off_ += 2 + len;
    ++count_;
    ++messages_;
  }

  void flush() {
    if (count_ == 0) return;
    std::memcpy(buf_.data(), session_, kSessionLen);
    store_be<std::uint64_t>(buf_.data() + kSessionLen, next_seq_);
    store_be<std::uint16_t>(buf_.data() + kSessionLen + 8, count_);
    std::uint8_t len_prefix[4];
    store_be<std::uint32_t>(len_prefix, static_cast<std::uint32_t>(off_));
    std::fwrite(len_prefix, 1, 4, out_);
    std::fwrite(buf_.data(), 1, off_, out_);
    bytes_ += 4 + off_;
    next_seq_ += count_;
    ++packets_;
    reset_packet();
  }

  // An empty packet with a message count of 0xFFFF, which is how MoldUDP64 says
  // the session is over.
  void end_of_session() {
    flush();
    std::memcpy(buf_.data(), session_, kSessionLen);
    store_be<std::uint64_t>(buf_.data() + kSessionLen, next_seq_);
    store_be<std::uint16_t>(buf_.data() + kSessionLen + 8, kEndOfSession);
    std::uint8_t len_prefix[4];
    store_be<std::uint32_t>(len_prefix, static_cast<std::uint32_t>(kMoldHeaderLen));
    std::fwrite(len_prefix, 1, 4, out_);
    std::fwrite(buf_.data(), 1, kMoldHeaderLen, out_);
    bytes_ += 4 + kMoldHeaderLen;
    ++packets_;
  }

 private:
  void reset_packet() {
    off_ = kMoldHeaderLen;
    count_ = 0;
  }

  std::FILE* out_;
  std::size_t mtu_;
  char session_[kSessionLen];
  std::vector<std::uint8_t> buf_;
  std::size_t off_ = 0;
  std::uint16_t count_ = 0;
  std::uint64_t next_seq_ = 1;
  std::uint64_t packets_ = 0, messages_ = 0, bytes_ = 0;
};

}  // namespace ltx::wire
