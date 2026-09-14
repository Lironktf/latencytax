// A test harness small enough to read in one sitting. Each test binary calls
// RUN(name) for its cases and returns non zero if any check failed.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace ltxtest {

inline int g_failures = 0;
inline int g_checks = 0;
inline const char* g_current = "";

inline void fail(const char* file, int line, const char* expr, const std::string& extra) {
  ++g_failures;
  std::fprintf(stderr, "FAIL %s at %s:%d: %s%s%s\n", g_current, file, line, expr,
               extra.empty() ? "" : "  ", extra.c_str());
}

inline int summary() {
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "ok", g_checks,
              g_failures);
  return g_failures ? 1 : 0;
}

}  // namespace ltxtest

#define CHECK(expr)                                                     \
  do {                                                                  \
    ++::ltxtest::g_checks;                                              \
    if (!(expr)) ::ltxtest::fail(__FILE__, __LINE__, #expr, "");        \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    ++::ltxtest::g_checks;                                                   \
    const auto _a = (a);                                                     \
    const auto _b = (b);                                                     \
    if (!(_a == _b)) {                                                       \
      ::ltxtest::fail(__FILE__, __LINE__, #a " == " #b,                      \
                      "got " + std::to_string(_a) + " want " + std::to_string(_b)); \
    }                                                                        \
  } while (0)

#define RUN(fn)                     \
  do {                              \
    ::ltxtest::g_current = #fn;     \
    fn();                           \
  } while (0)
