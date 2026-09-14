// Pin the calling thread to one CPU. Without this the scheduler moves the
// matcher between cores mid run and the tail of the latency distribution
// becomes a measurement of migration, not of the engine.
#pragma once

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace ltx {

inline bool pin_to_core(int cpu) {
#ifdef __linux__
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

}  // namespace ltx
