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

inline constexpr char kMagic[8] = {'L', 'T', 'X', 'Q', 'M', '0', '0', '1'};

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
  std::uint8_t label;
  std::uint8_t side;           // 0 buy, 1 sell
  std::uint8_t offset;
  std::uint8_t day;
};
static_assert(sizeof(Record) == 144, "record should stay at 144 bytes");

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
