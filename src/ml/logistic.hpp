// Logistic regression trained online with FTRL-Proximal.
//
// Why FTRL rather than plain SGD
//   The samples arrive in time order and are heavily correlated within a few
//   seconds of each other, so a single pass with a per coordinate adaptive step
//   is both what the data shape asks for and what keeps the model honest about
//   ordering: nothing is ever fitted on a sample that comes later than the one
//   being predicted. FTRL-Proximal also carries an L1 term that drives useless
//   coordinates to exactly zero, which on a hand built 64 feature vector is a
//   readable statement about which features earned their place.
//
// The update, per coordinate i, on a sample with gradient g_i:
//   sigma = (sqrt(n_i + g_i^2) - sqrt(n_i)) / alpha
//   z_i  += g_i - sigma * w_i
//   n_i  += g_i^2
// and the weight is recovered lazily at prediction time:
//   w_i = 0                                            if |z_i| <= lambda1
//       = -(z_i - sign(z_i) * lambda1) / (beta + sqrt(n_i)) / alpha + lambda2)
//
// Reference: McMahan et al., "Ad Click Prediction: a View from the Trenches",
// KDD 2013, section 3.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "ml/kernels.hpp"

namespace ltx::ml {

struct FtrlConfig {
  float alpha = 0.05f;     // learning rate scale
  float beta = 1.0f;       // learning rate offset
  float l1 = 0.0f;         // L1, drives coordinates to exactly zero
  float l2 = 1.0f;         // L2
};

class FtrlLogistic {
 public:
  explicit FtrlLogistic(const FtrlConfig& c = {}, std::size_t dim = kDim)
      : cfg_(c), dim_(dim), z_(dim, 0.0f), n_(dim, 0.0f), w_(dim, 0.0f) {}

  std::size_t dim() const { return dim_; }
  const std::vector<float>& weights() const { return w_; }
  const FtrlConfig& config() const { return cfg_; }

  // Recovers the weights implied by (z, n) and returns the predicted
  // probability. Must be called before update, which reuses w_.
  float predict(const float* x) {
    for (std::size_t i = 0; i < dim_; ++i) {
      const float zi = z_[i];
      const float s = zi < 0.0f ? -1.0f : 1.0f;
      if (s * zi <= cfg_.l1) {
        w_[i] = 0.0f;
      } else {
        w_[i] = -(zi - s * cfg_.l1) /
                ((cfg_.beta + std::sqrt(n_[i])) / cfg_.alpha + cfg_.l2);
      }
    }
    return sigmoid(dot(w_.data(), x, dim_));
  }

  // Read only prediction, for evaluation after training has stopped.
  float predict_fixed(const float* x) const { return sigmoid(dot(w_.data(), x, dim_)); }

  // One online step. `p` must be the value predict() just returned for `x`.
  void update(const float* x, float p, float y) {
    const float base = p - y;   // d(logloss)/d(logit)
    for (std::size_t i = 0; i < dim_; ++i) {
      const float g = base * x[i];
      const float g2 = g * g;
      const float ni = n_[i];
      const float sigma = (std::sqrt(ni + g2) - std::sqrt(ni)) / cfg_.alpha;
      z_[i] += g - sigma * w_[i];
      n_[i] = ni + g2;
    }
  }

  // Freezes the lazily recovered weights so predict_fixed is meaningful.
  void finalise() {
    std::vector<float> x(dim_, 0.0f);
    predict(x.data());
  }

  std::size_t nonzero() const {
    std::size_t k = 0;
    for (float v : w_) {
      if (v != 0.0f) ++k;
    }
    return k;
  }

 private:
  FtrlConfig cfg_;
  std::size_t dim_;
  std::vector<float> z_, n_, w_;
};

}  // namespace ltx::ml
