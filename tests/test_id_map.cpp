// Order id map tests. The interesting case is the backward shift delete: a
// churn of inserts and erases must not leave the table degraded, and lookups
// must never walk past a hole into a wrong answer.
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "check.hpp"
#include "engine/id_map.hpp"

using namespace ltx;

namespace {

void test_basic() {
  IdMap m(16);
  CHECK_EQ(m.find(1), kNullSlot);
  CHECK(m.insert(1, 10));
  CHECK(m.insert(2, 20));
  CHECK(!m.insert(1, 99));
  CHECK_EQ(m.find(1), Slot(10));
  CHECK_EQ(m.find(2), Slot(20));
  CHECK_EQ(m.size(), size_t(2));
  CHECK_EQ(m.erase(1), Slot(10));
  CHECK_EQ(m.find(1), kNullSlot);
  CHECK_EQ(m.find(2), Slot(20));
  CHECK_EQ(m.erase(1), kNullSlot);
  CHECK_EQ(m.size(), size_t(1));
}

// Sequential ids all landing in one probe run is the case identity hashing
// gets wrong.
void test_colliding_run() {
  IdMap m(1024);
  for (OrderId i = 1; i <= 400; ++i) CHECK(m.insert(i, static_cast<Slot>(i)));
  for (OrderId i = 1; i <= 400; i += 2) CHECK_EQ(m.erase(i), Slot(i));
  for (OrderId i = 2; i <= 400; i += 2) CHECK_EQ(m.find(i), Slot(i));
  for (OrderId i = 1; i <= 400; i += 2) CHECK_EQ(m.find(i), kNullSlot);
}

void test_grow() {
  IdMap m(16);
  for (OrderId i = 1; i <= 5000; ++i) CHECK(m.insert(i * 7, static_cast<Slot>(i)));
  CHECK_EQ(m.size(), size_t(5000));
  for (OrderId i = 1; i <= 5000; ++i) CHECK_EQ(m.find(i * 7), Slot(i));
  CHECK(m.capacity() >= 8192);
}

void test_against_reference() {
  IdMap m(64);
  std::unordered_map<OrderId, Slot> ref;
  std::mt19937_64 rng(12345);
  std::vector<OrderId> live;
  for (int step = 0; step < 300000; ++step) {
    if (live.empty() || (rng() % 100) < 60) {
      const OrderId id = 1 + (rng() % 100000);
      const Slot s = static_cast<Slot>(rng() % 1000000);
      const bool ok = m.insert(id, s);
      const bool want = ref.find(id) == ref.end();
      CHECK_EQ(ok, want);
      if (want) { ref[id] = s; live.push_back(id); }
    } else {
      const std::size_t i = rng() % live.size();
      const OrderId id = live[i];
      live[i] = live.back();
      live.pop_back();
      CHECK_EQ(m.erase(id), ref[id]);
      ref.erase(id);
    }
  }
  CHECK_EQ(m.size(), ref.size());
  std::size_t bad = 0;
  for (const auto& [id, s] : ref) if (m.find(id) != s) ++bad;
  CHECK_EQ(bad, size_t(0));
  // Ids never inserted must miss.
  std::size_t false_hits = 0;
  for (OrderId id = 100001; id < 110000; ++id) if (m.find(id) != kNullSlot) ++false_hits;
  CHECK_EQ(false_hits, size_t(0));
}

}  // namespace

int main() {
  RUN(test_basic);
  RUN(test_colliding_run);
  RUN(test_grow);
  RUN(test_against_reference);
  return ltxtest::summary();
}
