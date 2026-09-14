// Kernel and model tests.
//
// The two that matter:
//
//   the vector kernels are compared against a scalar reference at every length
//   around the block boundaries, because a hand written kernel with four
//   accumulators and two unrolled tails is exactly the kind of code that is
//   correct for multiples of 32 and wrong for 33;
//
//   the backward pass is compared against finite differences, coordinate by
//   coordinate. A hand derived gradient that nobody checked numerically is a
//   hand derived gradient that is wrong, and it fails silently: the model still
//   trains, just to the wrong place.
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "check.hpp"
#include "ml/features.hpp"
#include "ml/kernels.hpp"
#include "ml/logistic.hpp"
#include "ml/mlp.hpp"

using namespace ltx;
using namespace ltx::ml;

namespace {

bool close(float a, float b, float tol) {
  const float d = std::fabs(a - b);
  const float m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
  return d <= tol * (m > 1.0f ? m : 1.0f);
}

void test_dot_against_scalar() {
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<float> u(-2.0f, 2.0f);
  std::size_t bad = 0;
  // Every length up to 80 covers the 32 wide block, the 8 wide block and both
  // scalar tails, plus a few large ones.
  for (std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(7), std::size_t(8),
                        std::size_t(9), std::size_t(31), std::size_t(32), std::size_t(33),
                        std::size_t(39), std::size_t(40), std::size_t(63), std::size_t(64),
                        std::size_t(65), std::size_t(127), std::size_t(1000)}) {
    std::vector<float> a(n), b(n);
    for (std::size_t i = 0; i < n; ++i) { a[i] = u(rng); b[i] = u(rng); }
    const float got = dot(a.data(), b.data(), n);
    const float want = scalar::dot(a.data(), b.data(), n);
    // Four accumulators reassociate the sum, so this is a tolerance and not an
    // equality, which is the honest thing to assert about float addition.
    if (!close(got, want, 1e-5f)) ++bad;
  }
  CHECK_EQ(bad, size_t(0));
}

void test_axpy_against_scalar() {
  std::mt19937_64 rng(11);
  std::uniform_real_distribution<float> u(-2.0f, 2.0f);
  std::size_t bad = 0;
  for (std::size_t n = 0; n <= 80; ++n) {
    std::vector<float> x(n), y1(n), y2(n);
    for (std::size_t i = 0; i < n; ++i) { x[i] = u(rng); y1[i] = u(rng); y2[i] = y1[i]; }
    const float alpha = u(rng);
    axpy(y1.data(), x.data(), alpha, n);
    scalar::axpy(y2.data(), x.data(), alpha, n);
    for (std::size_t i = 0; i < n; ++i) {
      if (!close(y1[i], y2[i], 1e-6f)) ++bad;
    }
  }
  CHECK_EQ(bad, size_t(0));
}

void test_dot_known_values() {
  const float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const float b[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  CHECK(close(dot(a, b, 8), 36.0f, 1e-6f));
  CHECK(close(dot(a, a, 8), 204.0f, 1e-6f));   // 1+4+9+16+25+36+49+64
  const float z[4] = {0, 0, 0, 0};
  CHECK(close(dot(a, z, 4), 0.0f, 1e-6f));
}

void test_gemv_and_transpose() {
  // W is 3 x 4, x is 4, so y is 3.
  const float W[12] = {1, 2, 3, 4,
                       0, 1, 0, 1,
                       2, 0, 2, 0};
  const float x[4] = {1, 1, 2, 3};
  float y[3];
  gemv(W, x, y, 3, 4);
  CHECK(close(y[0], 1 + 2 + 6 + 12, 1e-6f));
  CHECK(close(y[1], 0 + 1 + 0 + 3, 1e-6f));
  CHECK(close(y[2], 2 + 0 + 4 + 0, 1e-6f));

  // W^T g for g = (1, 2, 3) is the column sums weighted by g.
  const float g[3] = {1, 2, 3};
  float out[4];
  gemv_t(W, g, out, 3, 4);
  CHECK(close(out[0], 1 * 1 + 2 * 0 + 3 * 2, 1e-6f));
  CHECK(close(out[1], 1 * 2 + 2 * 1 + 3 * 0, 1e-6f));
  CHECK(close(out[2], 1 * 3 + 2 * 0 + 3 * 2, 1e-6f));
  CHECK(close(out[3], 1 * 4 + 2 * 1 + 3 * 0, 1e-6f));

  // ger adds alpha * g outer x.
  float Wc[12];
  for (int i = 0; i < 12; ++i) Wc[i] = W[i];
  ger(Wc, g, x, 0.5f, 3, 4);
  CHECK(close(Wc[0], W[0] + 0.5f * g[0] * x[0], 1e-6f));
  CHECK(close(Wc[7], W[7] + 0.5f * g[1] * x[3], 1e-6f));
  CHECK(close(Wc[10], W[10] + 0.5f * g[2] * x[2], 1e-6f));
}

void test_relu_and_sigmoid() {
  std::vector<float> v{-3.0f, -0.001f, 0.0f, 0.001f, 5.0f, -1e9f, 1e9f};
  std::vector<float> w = v;
  relu(w.data(), w.size());
  for (std::size_t i = 0; i < v.size(); ++i) {
    CHECK(close(w[i], v[i] > 0 ? v[i] : 0.0f, 1e-7f));
  }
  std::vector<float> g(v.size(), 1.0f);
  relu_grad(g.data(), v.data(), v.size());
  for (std::size_t i = 0; i < v.size(); ++i) CHECK_EQ(g[i], v[i] > 0 ? 1.0f : 0.0f);

  CHECK(close(sigmoid(0.0f), 0.5f, 1e-6f));
  // The two branches have to agree at the seam and neither may overflow.
  CHECK(close(sigmoid(20.0f), 1.0f, 1e-6f));
  CHECK(close(sigmoid(-20.0f), 0.0f, 1e-6f));
  CHECK(std::isfinite(sigmoid(1000.0f)));
  CHECK(std::isfinite(sigmoid(-1000.0f)));
  CHECK(sigmoid(-1000.0f) >= 0.0f);
  CHECK(sigmoid(1000.0f) <= 1.0f);
  CHECK(close(sigmoid(2.0f) + sigmoid(-2.0f), 1.0f, 1e-6f));

  CHECK(std::isfinite(log_loss(0.0f, 1.0f)));
  CHECK(std::isfinite(log_loss(1.0f, 0.0f)));
  CHECK(close(log_loss(0.5f, 1.0f), std::log(2.0f), 1e-5f));
}

// The whole hand derived backward pass, against central differences.
void test_mlp_gradient_check() {
  MlpConfig c;
  c.hidden = 5;
  c.l2 = 0.0f;   // the analytic gradients below exclude the decay term
  c.seed = 99;
  const std::size_t D = 16;
  Mlp m(c, D);

  std::mt19937_64 rng(3);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> x(D);
  for (float& v : x) v = u(rng);
  const float y = 1.0f;

  std::vector<float> gw1, gb1, gw2;
  float gb2 = 0;
  m.gradients(x.data(), y, gw1, gb1, gw2, gb2);

  const float h = 1e-3f;
  std::size_t bad = 0;
  auto central = [&](float& param) {
    const float save = param;
    param = save + h;
    const float lp = m.loss(x.data(), y);
    param = save - h;
    const float lm = m.loss(x.data(), y);
    param = save;
    return (lp - lm) / (2.0f * h);
  };

  for (std::size_t i = 0; i < m.w1().size(); ++i) {
    const float num = central(m.w1()[i]);
    if (!close(num, gw1[i], 2e-2f) && std::fabs(num - gw1[i]) > 2e-4f) ++bad;
  }
  for (std::size_t i = 0; i < m.b1().size(); ++i) {
    const float num = central(m.b1()[i]);
    if (!close(num, gb1[i], 2e-2f) && std::fabs(num - gb1[i]) > 2e-4f) ++bad;
  }
  for (std::size_t i = 0; i < m.w2().size(); ++i) {
    const float num = central(m.w2()[i]);
    if (!close(num, gw2[i], 2e-2f) && std::fabs(num - gw2[i]) > 2e-4f) ++bad;
  }
  {
    const float num = central(m.b2());
    if (!close(num, gb2, 2e-2f) && std::fabs(num - gb2) > 2e-4f) ++bad;
  }
  CHECK_EQ(bad, size_t(0));
}

// A problem the model can actually solve, so a failure to learn shows up.
void test_mlp_learns_xor_like() {
  MlpConfig c;
  c.hidden = 16;
  c.lr = 0.02f;
  c.seed = 5;
  const std::size_t D = 4;
  Mlp m(c, D);
  std::mt19937_64 rng(17);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  // y = 1 when the first two coordinates have opposite signs. A linear model
  // cannot do this; the hidden layer has to.
  double loss = 0;
  for (int i = 0; i < 40000; ++i) {
    float x[D];
    for (std::size_t k = 0; k < D; ++k) x[k] = u(rng);
    x[3] = 1.0f;
    const float y = (x[0] * x[1] < 0.0f) ? 1.0f : 0.0f;
    const float p = m.predict(x);
    m.update(x, p, y);
    if (i >= 39000) loss += log_loss(p, y);
  }
  loss /= 1000.0;
  CHECK(loss < 0.35);   // well under the 0.693 of predicting one half
}

void test_ftrl_learns_and_sparsifies() {
  // Ten informative coordinates and 53 that are pure noise. Labels are drawn
  // from the true probability rather than thresholded, so the problem is not
  // separable and the weights have somewhere to converge to.
  const std::size_t D = 64;
  FtrlLogistic m({0.1f, 1.0f, 2.0f, 1.0f}, D);
  std::mt19937_64 rng(23);
  std::normal_distribution<float> g(0.0f, 1.0f);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  std::vector<float> truth(D, 0.0f);
  for (std::size_t i = 1; i <= 10; ++i) truth[i] = (i % 2) ? 1.5f : -1.5f;

  double loss = 0, bayes = 0;
  for (int it = 0; it < 200000; ++it) {
    std::vector<float> x(D);
    x[0] = 1.0f;
    for (std::size_t k = 1; k < D; ++k) x[k] = g(rng);
    const float z = scalar::dot(truth.data(), x.data(), D);
    const float pt = sigmoid(z);
    const float y = u(rng) < pt ? 1.0f : 0.0f;
    const float p = m.predict(x.data());
    m.update(x.data(), p, y);
    if (it >= 199000) {
      loss += log_loss(p, y);
      bayes += log_loss(pt, y);
    }
  }
  loss /= 1000.0;
  bayes /= 1000.0;
  // Within a little of the loss the true model itself would take.
  CHECK(loss < bayes + 0.05);
  m.finalise();

  // Every informative coordinate survives L1, and the noise that survives is an
  // order of magnitude smaller.
  std::size_t kept_signal = 0;
  double sig = 0, noise = 0;
  for (std::size_t k = 1; k <= 10; ++k) {
    if (m.weights()[k] != 0.0f) ++kept_signal;
    sig += std::fabs(m.weights()[k]);
  }
  for (std::size_t k = 11; k < D; ++k) noise += std::fabs(m.weights()[k]);
  CHECK_EQ(kept_signal, size_t(10));
  CHECK((noise / 53.0) < 0.1 * (sig / 10.0));
}

// Every feature has to be finite for every input the raw data can produce,
// including the degenerate ones, because one NaN poisons every weight it
// touches and every prediction after that.
void test_features_are_finite() {
  MarketState st;
  Snapshot s{};
  s.time_ms = 1786579191396;
  s.n_bids = 20;
  s.n_asks = 20;
  for (int i = 0; i < 20; ++i) {
    s.bids[i] = RawLevel{static_cast<Tick>(19160 - i), static_cast<Qty>(1 + i) * kQtyScale, 1u};
    s.asks[i] = RawLevel{static_cast<Tick>(19161 + i), static_cast<Qty>(1 + i) * kQtyScale, 1u};
  }
  // Enough snapshots for the state to be ready, with the mid moving both ways
  // so a signed drift goes negative, which is what produced NaN the first time.
  for (int k = 0; k < 20; ++k) {
    Snapshot t = s;
    t.time_ms = s.time_ms + k * 5000;
    const Tick shift = static_cast<Tick>((k % 4) - 2);
    for (int i = 0; i < 20; ++i) {
      t.bids[i].tick = static_cast<Tick>(s.bids[i].tick + shift);
      t.asks[i].tick = static_cast<Tick>(s.asks[i].tick + shift);
    }
    st.on_snapshot(t);
    RawTrade tr{};
    tr.time_ms = t.time_ms + 1;
    tr.tick = t.bids[0].tick;
    tr.qty = kQtyScale;
    tr.aggressor = (k % 2) ? Side::Buy : Side::Sell;
    st.on_trade(tr);
  }
  CHECK(st.ready());

  std::size_t bad = 0;
  float f[kDim];
  for (int soff = 0; soff < 2; ++soff) {
    for (int off = 0; off < 3; ++off) {
      for (double frac : {0.0, 0.25, 1.0, 4.0}) {
        Query q;
        q.side = soff == 0 ? Side::Buy : Side::Sell;
        q.offset_ticks = off;
        q.tick = static_cast<Tick>(19160 - off);
        q.level_eth = off == 2 ? 0.0 : 1.0 + off;   // a level with nothing on it
        q.orders = off == 2 ? 0u : 3u;
        q.q_eth = frac * (q.level_eth > 0 ? q.level_eth : 1.0);
        build(st, q, st.snapshot().time_ms, f);
        for (std::size_t i = 0; i < kDim; ++i) {
          if (!std::isfinite(f[i])) ++bad;
        }
      }
    }
  }
  CHECK_EQ(bad, size_t(0));

  // The expansion must set exactly one indicator in each of its three groups.
  int g1 = 0, g2 = 0, g3 = 0;
  for (std::size_t i = 32; i < 48; ++i) g1 += f[i] != 0.0f;
  for (std::size_t i = 48; i < 54; ++i) g2 += f[i] != 0.0f;
  for (std::size_t i = 56; i < 64; ++i) g3 += f[i] != 0.0f;
  CHECK_EQ(g1, 1);
  CHECK_EQ(g2, 1);
  CHECK_EQ(g3, 1);
}

}  // namespace

int main() {
  RUN(test_dot_against_scalar);
  RUN(test_axpy_against_scalar);
  RUN(test_dot_known_values);
  RUN(test_gemv_and_transpose);
  RUN(test_relu_and_sigmoid);
  RUN(test_mlp_gradient_check);
  RUN(test_mlp_learns_xor_like);
  RUN(test_ftrl_learns_and_sparsifies);
  RUN(test_features_are_finite);
  return ltxtest::summary();
}
