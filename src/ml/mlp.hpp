// A two layer perceptron, forward and backward written out by hand.
//
//   h_pre = W1 x + b1        H by D matrix vector product
//   h     = relu(h_pre)
//   logit = w2 . h + b2
//   p     = sigmoid(logit)
//
// The backward pass for binary cross entropy collapses neatly: the gradient of
// the loss with respect to the logit is exactly p - y, with no sigmoid
// derivative left over, because the sigmoid and the log cancel. From there
//
//   dw2   = (p - y) * h
//   dh    = (p - y) * w2
//   dh_pre= dh * [h_pre > 0]
//   dW1   = dh_pre outer x
//
// Adam is used for the step, because the feature scales here differ by orders
// of magnitude and a single global learning rate spends most of its time either
// diverging on one coordinate or standing still on another.
//
// Everything is checked against finite differences in tests/test_ml.cpp. A
// hand derived backward pass that nobody gradient checked is a hand derived
// backward pass that is wrong.
#pragma once

#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include "ml/kernels.hpp"

namespace ltx::ml {

struct MlpConfig {
  std::size_t hidden = 32;
  float lr = 0.002f;
  float beta1 = 0.9f;
  float beta2 = 0.999f;
  float eps = 1e-8f;
  float l2 = 1e-6f;
  std::uint64_t seed = 20260914;
};

class Mlp {
 public:
  explicit Mlp(const MlpConfig& c = {}, std::size_t dim = kDim)
      : cfg_(c), d_(dim), h_(c.hidden),
        w1_(c.hidden * dim), b1_(c.hidden, 0.0f), w2_(c.hidden), b2_(0.0f),
        m1_(c.hidden * dim, 0.0f), v1_(c.hidden * dim, 0.0f),
        mb1_(c.hidden, 0.0f), vb1_(c.hidden, 0.0f),
        m2_(c.hidden, 0.0f), v2_(c.hidden, 0.0f),
        hpre_(c.hidden), hact_(c.hidden), dh_(c.hidden), grow_(dim), g2_(c.hidden) {
    // He initialisation, which is the right variance for ReLU: without it a
    // 64 wide input either saturates every unit off or blows the first step up.
    std::mt19937_64 rng(c.seed);
    std::normal_distribution<float> g(0.0f, std::sqrt(2.0f / static_cast<float>(dim)));
    for (float& w : w1_) w = g(rng);
    std::normal_distribution<float> g2(0.0f, std::sqrt(2.0f / static_cast<float>(c.hidden)));
    for (float& w : w2_) w = g2(rng);
  }

  std::size_t dim() const { return d_; }
  std::size_t hidden() const { return h_; }
  const MlpConfig& config() const { return cfg_; }

  float predict(const float* x) {
    gemv(w1_.data(), x, hpre_.data(), h_, d_);
    for (std::size_t i = 0; i < h_; ++i) hpre_[i] += b1_[i];
    for (std::size_t i = 0; i < h_; ++i) hact_[i] = hpre_[i];
    relu(hact_.data(), h_);
    return sigmoid(dot(w2_.data(), hact_.data(), h_) + b2_);
  }

  // One Adam step on a single sample. `p` must be what predict() returned.
  void update(const float* x, float p, float y) {
    ++t_;
    const float d = p - y;

    const float lr_t = lr_corrected();

    // Output layer.
    for (std::size_t i = 0; i < h_; ++i) dh_[i] = d * w2_[i];
    scale_add(g2_.data(), hact_.data(), d, w2_.data(), cfg_.l2, h_);
    ::ltx::ml::adam_step(w2_.data(), m2_.data(), v2_.data(), g2_.data(), cfg_.beta1,
                         cfg_.beta2, lr_t, cfg_.eps, h_);
    adam_scalar(b2_, mb2_, vb2_, d);

    // Through the ReLU.
    relu_grad(dh_.data(), hpre_.data(), h_);

    // Hidden layer: dW1 = dh outer x. A unit whose ReLU is off contributes
    // nothing, and on this data roughly half of them are off at any moment, so
    // the skip is worth more than any amount of tuning inside the loop.
    for (std::size_t r = 0; r < h_; ++r) {
      const float g_r = dh_[r];
      if (g_r == 0.0f) continue;
      float* wrow = w1_.data() + r * d_;
      scale_add(grow_.data(), x, g_r, wrow, cfg_.l2, d_);
      ::ltx::ml::adam_step(wrow, m1_.data() + r * d_, v1_.data() + r * d_, grow_.data(),
                           cfg_.beta1, cfg_.beta2, lr_t, cfg_.eps, d_);
      adam_scalar(b1_[r], mb1_[r], vb1_[r], g_r);
    }
  }

  // Exposed so the gradient check can compare against finite differences.
  float loss(const float* x, float y) { return log_loss(predict(x), y); }
  std::vector<float>& w1() { return w1_; }
  std::vector<float>& w2() { return w2_; }
  std::vector<float>& b1() { return b1_; }
  float& b2() { return b2_; }

  // Analytic gradients for one sample, without touching the optimiser state.
  void gradients(const float* x, float y, std::vector<float>& gw1, std::vector<float>& gb1,
                 std::vector<float>& gw2, float& gb2) {
    const float p = predict(x);
    const float d = p - y;
    gw2.assign(h_, 0.0f);
    gb1.assign(h_, 0.0f);
    gw1.assign(h_ * d_, 0.0f);
    for (std::size_t i = 0; i < h_; ++i) gw2[i] = d * hact_[i];
    gb2 = d;
    std::vector<float> dh(h_);
    for (std::size_t i = 0; i < h_; ++i) dh[i] = d * w2_[i];
    relu_grad(dh.data(), hpre_.data(), h_);
    for (std::size_t r = 0; r < h_; ++r) {
      gb1[r] = dh[r];
      for (std::size_t c = 0; c < d_; ++c) gw1[r * d_ + c] = dh[r] * x[c];
    }
  }

 private:
  float lr_corrected() const {
    const float b1t = 1.0f - std::pow(cfg_.beta1, static_cast<float>(t_));
    const float b2t = 1.0f - std::pow(cfg_.beta2, static_cast<float>(t_));
    return cfg_.lr * std::sqrt(b2t) / b1t;
  }

  void adam_scalar(float& w, float& m, float& v, float g) {
    const float lr_t = lr_corrected();
    m = cfg_.beta1 * m + (1.0f - cfg_.beta1) * g;
    v = cfg_.beta2 * v + (1.0f - cfg_.beta2) * g * g;
    w -= lr_t * m / (std::sqrt(v) + cfg_.eps);
  }

  MlpConfig cfg_;
  std::size_t d_, h_;
  std::vector<float> w1_, b1_, w2_;
  float b2_;
  std::vector<float> m1_, v1_, mb1_, vb1_, m2_, v2_;
  float mb2_ = 0.0f, vb2_ = 0.0f;
  std::vector<float> hpre_, hact_, dh_, grow_, g2_;
  std::uint64_t t_ = 0;
};

}  // namespace ltx::ml
