// On disk format for the queue model dataset.
//
// Only the 32 measured features are stored. The 32 bucket indicators are a pure
// function of them and are rebuilt at load time, which halves the file and means
// a change to the bucketing does not invalidate a dataset that took minutes to
// produce.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ml/features.hpp"

namespace ltx::ml {

inline constexpr char kMagic[8] = {'L', 'T', 'X', 'Q', 'M', '0', '0', '2'};

// Time to clear, bucketed. Experiment 002 asked one question, does the queue in
// front of an order trade away within 30 seconds, and answered it with a
// classifier. That throws away most of what a market maker wants: not whether
// the queue clears by an arbitrary horizon but roughly when, so it can compare
// joining this level against joining another. These buckets turn the same
// observation into a discrete time survival problem, with one hazard fitted per
// bucket on the samples that survived to it.
//
// Edges in milliseconds; kCensored means it had not cleared by the last edge.
inline constexpr std::size_t kBuckets = 6;
inline constexpr std::int64_t kBucketEdges[kBuckets] = {5000, 15000, 30000, 60000,
                                                        150000, 300000};
inline constexpr std::uint8_t kCensored = 255;

inline std::uint8_t bucket_of(std::int64_t dt_ms) {
  for (std::size_t i = 0; i < kBuckets; ++i) {
    if (dt_ms <= kBucketEdges[i]) return static_cast<std::uint8_t>(i);
  }
  return kCensored;
}

struct DatasetHeader {
  char magic[8];
  std::uint32_t base_dim;
  std::uint32_t full_dim;
  std::uint64_t records;
  std::uint32_t horizon_ms;
  std::uint32_t days;
};

struct Record {
  float base[kBaseFeatures];   // 128 bytes
  std::int64_t ts_ms;
  float consumed_eth;          // volume that actually traded at the price, capped
  std::uint8_t label;          // cleared within 30 s, the experiment 002 target
  std::uint8_t side;           // 0 buy, 1 sell
  std::uint8_t offset;
  std::uint8_t day;
  std::uint8_t clear_bucket;   // which bucket it cleared in, or kCensored
  std::uint8_t pad[7];
};
static_assert(sizeof(Record) == 152, "record should stay at 152 bytes");

inline bool write_dataset(const std::string& path, const std::vector<Record>& rows,
                          std::uint32_t horizon_ms, std::uint32_t days) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  DatasetHeader h{};
  std::memcpy(h.magic, kMagic, 8);
  h.base_dim = kBaseFeatures;
  h.full_dim = kDim;
  h.records = rows.size();
  h.horizon_ms = horizon_ms;
  h.days = days;
  std::fwrite(&h, sizeof(h), 1, f);
  if (!rows.empty()) std::fwrite(rows.data(), sizeof(Record), rows.size(), f);
  std::fclose(f);
  return true;
}

inline bool read_dataset(const std::string& path, std::vector<Record>& rows,
                         DatasetHeader& h) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  if (std::fread(&h, sizeof(h), 1, f) != 1 || std::memcmp(h.magic, kMagic, 8) != 0 ||
      h.base_dim != kBaseFeatures) {
    std::fclose(f);
    return false;
  }
  rows.resize(h.records);
  const std::size_t got = h.records ? std::fread(rows.data(), sizeof(Record), h.records, f) : 0;
  std::fclose(f);
  return got == h.records;
}

// Rebuilds the full 64 wide vector from a stored record.
inline void materialise(const Record& r, float* f) {
  std::memcpy(f, r.base, sizeof(r.base));
  expand(f);
}

}  // namespace ltx::ml
