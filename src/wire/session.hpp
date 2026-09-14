// The server half of a SoupBinTCP session, with no sockets in it.
//
// Splitting the state machine from the transport is the whole point. Session
// recovery is the part that is easy to get subtly wrong and nearly impossible to
// test through a socket: you would have to arrange a real disconnection at a
// precise moment and then prove what came back was exactly right. Here the
// interesting behaviour is a pure function of bytes in and bytes out, so the
// tests in tests/test_ouch.cpp kill and resume a session hundreds of times with
// no network at all.
//
// The durable object is the store, not the connection. A sequenced message
// belongs to the session and outlives any particular TCP connection, which is
// exactly the property that lets a client reconnect and be made whole.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wire/soup.hpp"

namespace ltx::wire {

// Everything the server has sent as Sequenced Data, in order. Sequence numbers
// start at 1 and are implicit: they are the index, not a field on the wire.
class SequencedStore {
 public:
  std::uint64_t append(const void* p, std::size_t n) {
    msgs_.emplace_back(static_cast<const std::uint8_t*>(p),
                       static_cast<const std::uint8_t*>(p) + n);
    return msgs_.size();
  }
  std::uint64_t next() const { return msgs_.size() + 1; }
  std::uint64_t count() const { return msgs_.size(); }
  bool get(std::uint64_t seq, const std::uint8_t*& p, std::size_t& n) const {
    if (seq == 0 || seq > msgs_.size()) return false;
    p = msgs_[seq - 1].data();
    n = msgs_[seq - 1].size();
    return true;
  }

 private:
  std::vector<std::vector<std::uint8_t>> msgs_;
};

struct SessionStats {
  std::uint64_t logins = 0;
  std::uint64_t rejects = 0;
  std::uint64_t orders_in = 0;
  std::uint64_t replayed = 0;
  std::uint64_t heartbeats_in = 0;
  std::uint64_t heartbeats_out = 0;
  std::uint64_t malformed = 0;
};

class ServerSession {
 public:
  ServerSession(SequencedStore* store, std::string id) : store_(store), id_(std::move(id)) {}

  bool logged_in() const { return logged_in_; }
  std::uint64_t cursor() const { return cursor_; }
  const SessionStats& stats() const { return stats_; }

  // A new TCP connection on the same session. The store is untouched; only the
  // per connection state resets.
  void on_disconnect() {
    logged_in_ = false;
    cursor_ = 1;
    partial_.clear();
  }

  // Feeds bytes from the socket. Response bytes are appended to `out`.
  // `on_order` is called with each Unsequenced Data payload, which is where the
  // OUCH messages live. Returns false if the session should be torn down.
  template <typename OnOrder>
  bool consume(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out,
               OnOrder&& on_order) {
    partial_.insert(partial_.end(), p, p + n);
    std::size_t off = 0;
    while (true) {
      Packet pk;
      if (!parse(partial_.data() + off, partial_.size() - off, pk)) break;
      const bool keep = handle(pk, out, on_order);
      off += pk.consumed;
      if (!keep) {
        partial_.erase(partial_.begin(), partial_.begin() + static_cast<long>(off));
        return false;
      }
    }
    if (off) partial_.erase(partial_.begin(), partial_.begin() + static_cast<long>(off));
    // A packet header that never completes is a stalled peer, not a valid state
    // to sit in forever.
    if (partial_.size() > (1u << 20)) {
      ++stats_.malformed;
      return false;
    }
    return true;
  }

  // Sends the client everything it is owed from the store. Called after the
  // engine has published, and after a login that asked to resume.
  void pump(std::vector<std::uint8_t>& out) {
    if (!logged_in_) return;
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    while (store_->get(cursor_, p, n)) {
      frame_sequenced(out, p, n);
      ++cursor_;
    }
  }

  void heartbeat(std::vector<std::uint8_t>& out) {
    if (!logged_in_) return;
    frame_empty(out, SoupType::ServerHeartbeat);
    ++stats_.heartbeats_out;
  }

  void end_session(std::vector<std::uint8_t>& out) {
    frame_empty(out, SoupType::EndOfSession);
    logged_in_ = false;
  }

 private:
  template <typename OnOrder>
  bool handle(const Packet& pk, std::vector<std::uint8_t>& out, OnOrder& on_order) {
    switch (static_cast<SoupType>(pk.type)) {
      case SoupType::LoginRequest: {
        LoginRequest lr{};
        if (!decode_login_request(pk.payload, pk.len, lr)) {
          ++stats_.malformed;
          frame_login_rejected(out, LoginReject::NotAuthorised);
          return false;
        }
        // A named session that is not this one cannot be served here.
        std::string want(lr.session, kSoupSessionLen);
        while (!want.empty() && want.back() == ' ') want.pop_back();
        if (!want.empty() && want != id_) {
          ++stats_.rejects;
          frame_login_rejected(out, LoginReject::SessionNotAvailable);
          return false;
        }
        // 0 means "whatever is next, I do not care what I missed". Anything
        // else is a request to resume from that point, clamped to what exists,
        // because a client asking for a sequence past the end is asking for
        // nothing rather than for an error.
        cursor_ = lr.requested_sequence == 0 ? store_->next() : lr.requested_sequence;
        if (cursor_ > store_->next()) cursor_ = store_->next();
        if (cursor_ == 0) cursor_ = 1;
        logged_in_ = true;
        ++stats_.logins;
        if (cursor_ < store_->next()) stats_.replayed += store_->next() - cursor_;
        frame_login_accepted(out, id_, cursor_);
        pump(out);
        return true;
      }
      case SoupType::UnsequencedData:
        if (!logged_in_) {
          ++stats_.malformed;
          return false;
        }
        ++stats_.orders_in;
        on_order(pk.payload, pk.len);
        return true;
      case SoupType::ClientHeartbeat:
        ++stats_.heartbeats_in;
        return true;
      case SoupType::LogoutRequest:
        logged_in_ = false;
        return false;
      case SoupType::Debug:
        return true;
      default:
        ++stats_.malformed;
        return false;
    }
  }

  SequencedStore* store_;
  std::string id_;
  bool logged_in_ = false;
  std::uint64_t cursor_ = 1;
  std::vector<std::uint8_t> partial_;
  SessionStats stats_;
};

// The client half, for the tests and the driver. Tracks how many sequenced
// messages it has seen, which is what it would ask to resume from.
class ClientSession {
 public:
  std::uint64_t received() const { return received_; }
  std::uint64_t login_sequence() const { return login_seq_; }
  bool logged_in() const { return logged_in_; }

  void reset_connection() {
    logged_in_ = false;
    partial_.clear();
  }

  // `on_message` gets each sequenced payload with its sequence number.
  template <typename OnMessage>
  void consume(const std::uint8_t* p, std::size_t n, OnMessage&& on_message) {
    partial_.insert(partial_.end(), p, p + n);
    std::size_t off = 0;
    while (true) {
      Packet pk;
      if (!parse(partial_.data() + off, partial_.size() - off, pk)) break;
      off += pk.consumed;
      switch (static_cast<SoupType>(pk.type)) {
        case SoupType::LoginAccepted: {
          LoginAccepted la{};
          if (decode_login_accepted(pk.payload, pk.len, la)) {
            login_seq_ = la.sequence;
            next_ = la.sequence;
            logged_in_ = true;
          }
          break;
        }
        case SoupType::LoginRejected:
          logged_in_ = false;
          break;
        case SoupType::SequencedData:
          on_message(next_, pk.payload, pk.len);
          ++next_;
          ++received_;
          break;
        default:
          break;
      }
    }
    if (off) partial_.erase(partial_.begin(), partial_.begin() + static_cast<long>(off));
  }

  // The sequence number to ask for on reconnect: the next one not yet seen.
  std::uint64_t resume_from() const { return next_; }

 private:
  bool logged_in_ = false;
  std::uint64_t next_ = 1;
  std::uint64_t received_ = 0;
  std::uint64_t login_seq_ = 0;
  std::vector<std::uint8_t> partial_;
};

}  // namespace ltx::wire
