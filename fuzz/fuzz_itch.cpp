// Feeds arbitrary bytes to the market data path: MoldUDP64 framing, ITCH
// decoding, and the book that results.
//
// This is the code that touches the network first and trusts it least. The
// property being fuzzed is not that it produces anything sensible from garbage,
// it is that it never reads outside the buffer it was handed, never loops
// forever, and never leaves the book in a state its own invariants reject.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/multi_book.hpp"
#include "wire/decoder.hpp"

using namespace ltx;
using namespace ltx::wire;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (1u << 20)) return 0;

  Decoder dec;
  dec.track_sessions(true);
  std::vector<Routed> cmds;
  dec.decode_stream(data, size, cmds);

  BookConfig cfg;
  cfg.min_tick = 1;
  cfg.max_tick = 4096;
  cfg.max_orders = 4096;
  cfg.id_map_capacity = 8192;
  NullSink sink;
  MultiBook books(cfg, &sink);

  std::size_t applied = 0;
  for (const Routed& r : cmds) {
    // Locates are a 16 bit field, so a hostile feed can name 65,535 symbols.
    // Cap what gets instantiated so the fuzzer explores the decoder rather than
    // the allocator.
    if (r.locate > 8) continue;
    if (!books.has(r.locate)) books.ensure(r.locate, SymbolSpec{"FUZZ", 1, 4});
    books.apply(r.locate, r.cmd);
    if ((++applied & 127) == 0) {
      for (std::size_t i = 0; i < books.size(); ++i) {
        const std::uint16_t loc = static_cast<std::uint16_t>(i);
        if (books.has(loc) && !books.engine(loc).book().check_invariants()) __builtin_trap();
      }
    }
  }
  for (std::size_t i = 0; i < books.size(); ++i) {
    const std::uint16_t loc = static_cast<std::uint16_t>(i);
    if (books.has(loc) && !books.engine(loc).book().check_invariants()) __builtin_trap();
  }
  return 0;
}
