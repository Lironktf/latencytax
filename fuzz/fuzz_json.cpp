// The raw file parsers. These read bytes off disk that came off a socket, and
// they are hand written scanners rather than a library, which is exactly the
// kind of code that walks off the end of a buffer on a truncated line.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "replay/loader.hpp"

using namespace ltx;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (1u << 18)) return 0;
  const std::string_view line(reinterpret_cast<const char*>(data), size);

  Snapshot s;
  parse_snapshot(line, 1, s);

  RawTrade t;
  parse_trade(line, 1, t);

  std::int64_t v = 0;
  parse_fixed(line, 1, v);
  parse_fixed(line, 8, v);
  return 0;
}
