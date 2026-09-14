// Feeds arbitrary bytes to the order entry session, in arbitrary fragments.
//
// The fragmentation is part of the input rather than fixed, because TCP chooses
// the boundaries and a session that only survives whole packet reads has not
// been tested. The first byte of the input decides the chunk size.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "wire/session.hpp"

using namespace ltx::wire;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2 || size > (1u << 20)) return 0;
  const std::size_t chunk = 1 + (data[0] % 97);
  ++data;
  --size;

  SequencedStore store;
  for (int i = 0; i < 8; ++i) {
    const std::uint8_t body[] = {'A', static_cast<std::uint8_t>(i)};
    store.append(body, sizeof(body));
  }

  ServerSession s(&store, "SESS01");
  std::vector<std::uint8_t> out;
  std::uint64_t orders = 0;
  for (std::size_t off = 0; off < size; off += chunk) {
    const std::size_t n = (chunk < size - off) ? chunk : (size - off);
    if (!s.consume(data + off, n, out, [&](const std::uint8_t*, std::size_t) { ++orders; })) {
      break;
    }
    // A session that never stops appending to its output is a memory leak with
    // extra steps.
    if (out.size() > (1u << 22)) __builtin_trap();
    out.clear();
  }

  ClientSession c;
  c.consume(out.data(), out.size(), [](std::uint64_t, const std::uint8_t*, std::size_t) {});
  return 0;
}
