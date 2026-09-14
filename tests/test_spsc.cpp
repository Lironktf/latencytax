// Single producer single consumer ring tests.
//
// The threaded cases run long enough to exercise wraparound many times over.
// Build with -DCMAKE_BUILD_TYPE=TSan to run them under ThreadSanitizer.
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "check.hpp"
#include "engine/spsc_ring.hpp"

using namespace ltx;

namespace {

struct Msg {
  std::uint64_t seq;
  std::uint64_t payload;
};

void test_single_thread_basics() {
  SpscRing<Msg> r(8);
  CHECK_EQ(r.capacity(), size_t(7));
  Msg m{};
  CHECK(!r.pop(m));
  for (std::uint64_t i = 0; i < 7; ++i) CHECK(r.push(Msg{i, i * 3}));
  CHECK(!r.push(Msg{99, 99}));  // full at capacity, one slot held back
  CHECK_EQ(r.size_approx(), size_t(7));
  for (std::uint64_t i = 0; i < 7; ++i) {
    CHECK(r.pop(m));
    CHECK_EQ(m.seq, i);
    CHECK_EQ(m.payload, i * 3);
  }
  CHECK(!r.pop(m));
  CHECK(r.empty_approx());
}

void test_wraparound_single_thread() {
  SpscRing<Msg> r(4);
  std::uint64_t next_write = 0, next_read = 0;
  for (int round = 0; round < 10000; ++round) {
    while (r.push(Msg{next_write, ~next_write})) ++next_write;
    Msg m{};
    int drained = 0;
    while (drained < 2 && r.pop(m)) {
      CHECK_EQ(m.seq, next_read);
      CHECK_EQ(m.payload, ~next_read);
      ++next_read;
      ++drained;
    }
  }
  CHECK(next_write > 10000);
}

void test_bulk() {
  SpscRing<Msg> r(1024);
  std::vector<Msg> src(500);
  for (std::size_t i = 0; i < src.size(); ++i) src[i] = Msg{i, i * i};
  CHECK_EQ(r.push_bulk(src.data(), src.size()), size_t(500));
  CHECK_EQ(r.push_bulk(src.data(), src.size()), size_t(500));
  // 1023 usable slots, 1000 taken, so only 23 of the next 500 fit.
  CHECK_EQ(r.push_bulk(src.data(), src.size()), size_t(23));
  std::vector<Msg> dst(2000);
  CHECK_EQ(r.pop_bulk(dst.data(), dst.size()), size_t(1023));
  for (std::size_t i = 0; i < 500; ++i) CHECK_EQ(dst[i].seq, i);
  CHECK(r.empty_approx());
}

// Every message must arrive exactly once and in order, with no torn payloads.
void test_two_threads() {
  constexpr std::uint64_t kN = 4'000'000;
  SpscRing<Msg> r(1024);
  std::atomic<bool> bad{false};
  std::thread consumer([&] {
    Msg m{};
    std::uint64_t expect = 0;
    while (expect < kN) {
      if (r.pop(m)) {
        if (m.seq != expect || m.payload != expect * 2654435761ull) bad.store(true);
        ++expect;
      }
    }
  });
  for (std::uint64_t i = 0; i < kN; ++i) {
    while (!r.push(Msg{i, i * 2654435761ull})) {
    }
  }
  consumer.join();
  CHECK(!bad.load());
  CHECK(r.empty_approx());
}

void test_two_threads_bulk() {
  constexpr std::uint64_t kN = 2'000'000;
  SpscRing<Msg> r(256);
  std::atomic<std::uint64_t> checksum{0};
  std::atomic<bool> bad{false};
  std::thread consumer([&] {
    Msg buf[64];
    std::uint64_t expect = 0, sum = 0;
    while (expect < kN) {
      const std::size_t got = r.pop_bulk(buf, 64);
      for (std::size_t i = 0; i < got; ++i) {
        if (buf[i].seq != expect) bad.store(true);
        sum += buf[i].payload;
        ++expect;
      }
    }
    checksum.store(sum);
  });
  std::vector<Msg> batch(64);
  std::uint64_t sent = 0, want = 0;
  while (sent < kN) {
    const std::size_t n = std::min<std::uint64_t>(64, kN - sent);
    for (std::size_t i = 0; i < n; ++i) {
      batch[i] = Msg{sent + i, (sent + i) ^ 0xABCDEF};
      want += batch[i].payload;
    }
    std::size_t off = 0;
    while (off < n) off += r.push_bulk(batch.data() + off, n - off);
    sent += n;
  }
  consumer.join();
  CHECK(!bad.load());
  CHECK_EQ(checksum.load(), want);
}

}  // namespace

int main() {
  RUN(test_single_thread_basics);
  RUN(test_wraparound_single_thread);
  RUN(test_bulk);
  RUN(test_two_threads);
  RUN(test_two_threads_bulk);
  return ltxtest::summary();
}
