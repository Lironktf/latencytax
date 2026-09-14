// An input journal: every packet the engine was given, in the order it was
// given them, with a sequence number.
//
// This is what makes a failure reproducible. A book that is wrong at half past
// two is not much use on its own; the journal turns it into "run this file and
// watch it go wrong", on a different machine, under a debugger, as many times as
// you like. Exchanges keep one for the same reason, and it is also what a
// regulator asks for.
//
// The format is deliberately dull: an 8 byte magic, then records of sequence
// number, nanosecond timestamp, length, bytes. Nothing self describing, nothing
// versioned beyond the magic, nothing that needs a library to read.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ltx::wire {

inline constexpr char kJournalMagic[8] = {'L', 'T', 'X', 'J', 'R', 'N', 'L', '1'};

struct JournalRecord {
  std::uint64_t seq;
  std::uint64_t ts_ns;
  const std::uint8_t* data;
  std::uint32_t len;
};

class JournalWriter {
 public:
  explicit JournalWriter(const std::string& path) {
    f_ = std::fopen(path.c_str(), "wb");
    if (f_) std::fwrite(kJournalMagic, 1, 8, f_);
  }
  ~JournalWriter() { close(); }
  bool ok() const { return f_ != nullptr; }
  std::uint64_t records() const { return seq_; }
  std::uint64_t bytes() const { return bytes_; }

  void append(std::uint64_t ts_ns, const void* p, std::size_t n) {
    if (!f_) return;
    const std::uint64_t seq = ++seq_;
    const std::uint32_t len = static_cast<std::uint32_t>(n);
    std::fwrite(&seq, sizeof(seq), 1, f_);
    std::fwrite(&ts_ns, sizeof(ts_ns), 1, f_);
    std::fwrite(&len, sizeof(len), 1, f_);
    std::fwrite(p, 1, n, f_);
    bytes_ += 20 + n;
  }

  void close() {
    if (f_) { std::fclose(f_); f_ = nullptr; }
  }

 private:
  std::FILE* f_ = nullptr;
  std::uint64_t seq_ = 0, bytes_ = 0;
};

class JournalReader {
 public:
  bool load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 8) { std::fclose(f); return false; }
    buf_.resize(static_cast<std::size_t>(n));
    const std::size_t got = std::fread(buf_.data(), 1, buf_.size(), f);
    std::fclose(f);
    if (got != buf_.size() || std::memcmp(buf_.data(), kJournalMagic, 8) != 0) return false;
    off_ = 8;
    return true;
  }

  // A truncated journal is the normal case after a crash, so running out of
  // bytes part way through a record ends the replay rather than failing it.
  bool next(JournalRecord& out) {
    if (off_ + 20 > buf_.size()) return false;
    std::memcpy(&out.seq, buf_.data() + off_, 8);
    std::memcpy(&out.ts_ns, buf_.data() + off_ + 8, 8);
    std::memcpy(&out.len, buf_.data() + off_ + 16, 4);
    if (off_ + 20 + out.len > buf_.size()) return false;
    out.data = buf_.data() + off_ + 20;
    off_ += 20 + out.len;
    return true;
  }

  void rewind() { off_ = 8; }
  std::size_t size() const { return buf_.size(); }

 private:
  std::vector<std::uint8_t> buf_;
  std::size_t off_ = 0;
};

}  // namespace ltx::wire
