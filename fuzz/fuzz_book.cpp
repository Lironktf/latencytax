// Drives the book directly from arbitrary bytes, reading each 12 byte group as
// an operation. Every operation is followed by an invariant walk, so a
// bookkeeping error is caught on the operation that caused it rather than
// thousands of operations later.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/engine.hpp"

using namespace ltx;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 65536) return 0;

  BookConfig cfg;
  cfg.min_tick = 1;
  cfg.max_tick = 512;
  cfg.max_orders = 512;
  cfg.id_map_capacity = 1024;
  CountingSink sink;
  MatchingEngine eng(cfg, &sink);

  std::size_t off = 0;
  int ops = 0;
  while (off + 12 <= size && ops < 4000) {
    const std::uint8_t op = data[off] % 7;
    const OrderId id = 1 + (data[off + 1] % 64);
    std::uint16_t rawtick = 0, rawqty = 0;
    std::memcpy(&rawtick, data + off + 2, 2);
    std::memcpy(&rawqty, data + off + 4, 2);
    const Tick tick = 1 + static_cast<Tick>(rawtick % 600);   // some out of range
    const Qty qty = static_cast<Qty>(rawqty) * 1000;
    const Side side = (data[off + 6] & 1) ? Side::Buy : Side::Sell;
    const Tif tif = static_cast<Tif>(data[off + 7] % 3);
    const OrderId id2 = 1 + (data[off + 8] % 64);
    const Ts ts = static_cast<Ts>(ops);
    off += 12;
    ++ops;

    switch (op) {
      case 0: eng.book().add_limit(ts, id, side, tick, qty, tif); break;
      case 1: eng.book().add_market(ts, id, side, qty); break;
      case 2: eng.book().cancel(ts, id); break;
      case 3: eng.book().modify(ts, id, tick, qty); break;
      case 4: eng.book().reduce(ts, id, qty); break;
      case 5: eng.book().execute(ts, id, qty); break;
      default: eng.book().replace(ts, id, id2, tick, qty); break;
    }
    if (!eng.book().check_invariants()) __builtin_trap();
  }
  return 0;
}
