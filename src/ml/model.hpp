// Loads a trained queue model and scores one query.
//
// The file holds the recovered logistic weights and nothing else: 64 floats.
// The Platt scaling fitted on the validation day is folded in at save time, so
// a caller gets a calibrated probability without needing to know that Platt
// scaling happened.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ml/features.hpp"
#include "ml/kernels.hpp"

namespace ltx::ml {

class QueueModel {
 public:
  bool load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[8];
    std::uint32_t dim = 0, hidden = 0;
    float a = 1.0f, b = 0.0f;
    bool ok = std::fread(magic, 1, 8, f) == 8 &&
              std::memcmp(magic, "LTXQMDL1", 8) == 0 &&
              std::fread(&dim, sizeof(dim), 1, f) == 1 &&
              std::fread(&hidden, sizeof(hidden), 1, f) == 1 &&
              std::fread(&a, sizeof(a), 1, f) == 1 &&
              std::fread(&b, sizeof(b), 1, f) == 1 && dim == kDim;
    if (ok) {
      w_.resize(kDim);
      ok = std::fread(w_.data(), sizeof(float), kDim, f) == kDim;
    }
    std::fclose(f);
    if (!ok) return false;
    platt_a_ = a;
    platt_b_ = b;
    loaded_ = true;
    return true;
  }

  bool loaded() const { return loaded_; }
  const std::vector<float>& weights() const { return w_; }

  // Probability that `q_eth` in front of an order at this level trades away
  // inside the model's horizon.
  float predict(const MarketState& st, const Query& q, std::int64_t ts_ms) const {
    float f[kDim];
    build(st, q, ts_ms, f);
    const float z = dot(w_.data(), f, kDim);
    return sigmoid(platt_a_ * z + platt_b_);
  }

 private:
  std::vector<float> w_;
  float platt_a_ = 1.0f, platt_b_ = 0.0f;
  bool loaded_ = false;
};

}  // namespace ltx::ml
