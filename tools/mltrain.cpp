// Trains and scores the queue model.
//
// Three things are compared, and the first two exist so the third has to earn
// its place:
//
//   base rate     predict the training set's positive rate for every sample
//   one feature   logistic regression on the single quantity the problem
//                 reduces to, the queue ahead measured in 30 second windows of
//                 consuming volume
//   logistic      FTRL-Proximal over all 64 features, one online pass in time
//                 order
//   mlp           64 to H to 1, ReLU, Adam, hand written forward and backward
//
// Splitting
//   The training file's last day is held out as a validation day and is the only
//   thing used to pick the number of MLP epochs. The test file is a different
//   period entirely and is scored once, at the end.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <algorithm>
#include <string>
#include <vector>

#include "ml/dataset.hpp"
#include "ml/logistic.hpp"
#include "ml/mlp.hpp"

using namespace ltx;
using namespace ltx::ml;

namespace {

struct Args {
  std::string train = "results/queue_train.bin";
  std::string test = "results/queue_test.bin";
  std::string save;
  int val_days = 1;
  int max_epochs = 8;
  int hidden = 32;
  float lr = 0.002f;
  float ftrl_alpha = 0.05f;
  float ftrl_l1 = 0.5f;
  float ftrl_l2 = 1.0f;
  bool quiet = false;
};

void usage() {
  std::printf(
      "mltrain - train and score the queue model\n"
      "\n"
      "usage: mltrain [options]\n"
      "  --train=FILE       training dataset, default results/queue_train.bin\n"
      "  --test=FILE        test dataset, default results/queue_test.bin\n"
      "  --save=FILE        write the chosen model\n"
      "  --val-days=N       last N days of the training file are validation, default 1\n"
      "  --max-epochs=N     most MLP passes to try, default 8\n"
      "  --hidden=N         MLP hidden width, default 32\n"
      "  --lr=X             MLP learning rate, default 0.002\n"
      "  --ftrl-alpha=X     default 0.05\n"
      "  --ftrl-l1=X        default 0.5\n"
      "  --ftrl-l2=X        default 1.0\n"
      "  --quiet\n"
      "  --help\n");
}

struct Metrics {
  double log_loss = 0, brier = 0, auc = 0, base = 0;
  std::size_t n = 0, positives = 0;
};

// Rank based AUC. Ties get the average rank, which matters here because a
// bucketed linear model produces a lot of them.
double auc_score(const std::vector<float>& p, const std::vector<std::uint8_t>& y) {
  const std::size_t n = p.size();
  std::vector<std::uint32_t> idx(n);
  std::iota(idx.begin(), idx.end(), 0u);
  std::sort(idx.begin(), idx.end(), [&](std::uint32_t a, std::uint32_t b) {
    return p[a] < p[b];
  });
  double rank_sum = 0;
  std::size_t pos = 0;
  std::size_t i = 0;
  while (i < n) {
    std::size_t j = i;
    while (j + 1 < n && p[idx[j + 1]] == p[idx[i]]) ++j;
    const double avg_rank = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
    for (std::size_t k = i; k <= j; ++k) {
      if (y[idx[k]]) { rank_sum += avg_rank; ++pos; }
    }
    i = j + 1;
  }
  const std::size_t neg = n - pos;
  if (pos == 0 || neg == 0) return 0.5;
  return (rank_sum - static_cast<double>(pos) * (pos + 1) / 2.0) /
         (static_cast<double>(pos) * static_cast<double>(neg));
}

Metrics score(const std::vector<float>& p, const std::vector<std::uint8_t>& y) {
  Metrics m;
  m.n = p.size();
  for (std::size_t i = 0; i < p.size(); ++i) {
    m.log_loss += log_loss(p[i], static_cast<float>(y[i]));
    const double d = p[i] - y[i];
    m.brier += d * d;
    m.positives += y[i];
  }
  if (m.n) {
    m.log_loss /= m.n;
    m.brier /= m.n;
    m.base = static_cast<double>(m.positives) / m.n;
  }
  m.auc = auc_score(p, y);
  return m;
}

void print_metrics(const char* name, const Metrics& m) {
  std::printf("  %-14s n=%9zu  logloss %.5f  brier %.5f  auc %.4f\n", name, m.n, m.log_loss,
              m.brier, m.auc);
}

void print_calibration(const std::vector<float>& p, const std::vector<std::uint8_t>& y) {
  double sp[10] = {0}, sy[10] = {0};
  std::size_t cnt[10] = {0};
  for (std::size_t i = 0; i < p.size(); ++i) {
    int b = static_cast<int>(p[i] * 10.0f);
    if (b < 0) b = 0;
    if (b > 9) b = 9;
    sp[b] += p[i];
    sy[b] += y[i];
    ++cnt[b];
  }
  std::printf("  calibration, ten equal width bins of predicted probability\n");
  std::printf("    %-12s %10s %10s %10s\n", "bin", "count", "predicted", "actual");
  for (int b = 0; b < 10; ++b) {
    if (!cnt[b]) continue;
    char lab[16];
    std::snprintf(lab, sizeof(lab), "%.1f-%.1f", b / 10.0, (b + 1) / 10.0);
    std::printf("    %-12s %10zu %10.4f %10.4f\n", lab, cnt[b], sp[b] / cnt[b],
                sy[b] / cnt[b]);
  }
}

// Platt scaling: a two parameter logistic fitted on the model's own logit.
// Ranking and calibration are different jobs, and a model can be good at the
// first while being systematically wrong about the second, which is exactly
// what happens here. The scaler is fitted on the validation day only, never on
// the test set.
struct Platt {
  float a = 1.0f, b = 0.0f;

  void fit(const std::vector<float>& p, const std::vector<std::uint8_t>& y, int iters = 200) {
    std::vector<float> z(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
      const float q = std::min(std::max(p[i], 1e-6f), 1.0f - 1e-6f);
      z[i] = std::log(q / (1.0f - q));
    }
    a = 1.0f;
    b = 0.0f;
    // Plain gradient descent on the cross entropy. Two parameters, so this is
    // not the part worth optimising.
    for (int it = 0; it < iters; ++it) {
      double ga = 0, gb = 0;
      for (std::size_t i = 0; i < z.size(); ++i) {
        const float d = sigmoid(a * z[i] + b) - static_cast<float>(y[i]);
        ga += d * z[i];
        gb += d;
      }
      const double n = static_cast<double>(z.size());
      a -= static_cast<float>(0.5 * ga / n);
      b -= static_cast<float>(0.5 * gb / n);
    }
  }

  float apply(float p) const {
    const float q = std::min(std::max(p, 1e-6f), 1.0f - 1e-6f);
    return sigmoid(a * std::log(q / (1.0f - q)) + b);
  }
};

Metrics constant_metrics(double c, const std::vector<std::uint8_t>& y) {
  std::vector<float> p(y.size(), static_cast<float>(c));
  return score(p, y);
}

// Saves the logistic weights with the validation day's Platt scaling folded in,
// so a consumer gets a calibrated probability without having to know about it.
bool save_model(const std::string& path, const FtrlLogistic& lg, float platt_a,
                float platt_b) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const char magic[8] = {'L', 'T', 'X', 'Q', 'M', 'D', 'L', '1'};
  std::fwrite(magic, 1, 8, f);
  const std::uint32_t dim = static_cast<std::uint32_t>(kDim);
  const std::uint32_t hidden = 0;
  std::fwrite(&dim, sizeof(dim), 1, f);
  std::fwrite(&hidden, sizeof(hidden), 1, f);
  std::fwrite(&platt_a, sizeof(platt_a), 1, f);
  std::fwrite(&platt_b, sizeof(platt_b), 1, f);
  std::fwrite(lg.weights().data(), sizeof(float), kDim, f);
  std::fclose(f);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto str = [&](const char* k, std::string& o) {
      if (s.rfind(k, 0) == 0) { o = s.substr(std::strlen(k)); return true; }
      return false;
    };
    auto num = [&](const char* k, double& o) {
      if (s.rfind(k, 0) == 0) { o = std::atof(s.c_str() + std::strlen(k)); return true; }
      return false;
    };
    double d = 0;
    if (s == "--help" || s == "-h") { usage(); return 0; }
    else if (str("--train=", a.train)) {}
    else if (str("--test=", a.test)) {}
    else if (str("--save=", a.save)) {}
    else if (num("--val-days=", d)) a.val_days = static_cast<int>(d);
    else if (num("--max-epochs=", d)) a.max_epochs = static_cast<int>(d);
    else if (num("--hidden=", d)) a.hidden = static_cast<int>(d);
    else if (num("--lr=", d)) a.lr = static_cast<float>(d);
    else if (num("--ftrl-alpha=", d)) a.ftrl_alpha = static_cast<float>(d);
    else if (num("--ftrl-l1=", d)) a.ftrl_l1 = static_cast<float>(d);
    else if (num("--ftrl-l2=", d)) a.ftrl_l2 = static_cast<float>(d);
    else if (s == "--quiet") a.quiet = true;
    else { std::fprintf(stderr, "unknown argument %s\n", s.c_str()); usage(); return 1; }
  }

  std::vector<Record> train_rows, test_rows;
  DatasetHeader th{}, sh{};
  if (!read_dataset(a.train, train_rows, th)) {
    std::fprintf(stderr, "cannot read %s\n", a.train.c_str());
    return 1;
  }
  const bool have_test = read_dataset(a.test, test_rows, sh);

  // The training file's last day, or days, becomes validation.
  std::uint8_t max_day = 0;
  for (const Record& r : train_rows) max_day = std::max(max_day, r.day);
  const int val_from = static_cast<int>(max_day) + 1 - a.val_days;

  std::vector<const Record*> fit, val;
  for (const Record& r : train_rows) {
    (static_cast<int>(r.day) >= val_from ? val : fit).push_back(&r);
  }

  std::printf("train %s: %zu samples over %u days\n", a.train.c_str(), train_rows.size(),
              th.days);
  std::printf("  fit %zu, validation %zu (day index >= %d)\n", fit.size(), val.size(),
              val_from);
  if (have_test) {
    std::printf("test  %s: %zu samples over %u days\n", a.test.c_str(), test_rows.size(),
                sh.days);
  }
  std::printf("  horizon %u ms, %zu features\n\n", th.horizon_ms, kDim);

  // Materialising once costs memory but keeps the expansion out of every epoch.
  auto materialise_all = [](const std::vector<const Record*>& rs, std::vector<float>& X,
                            std::vector<std::uint8_t>& Y) {
    X.resize(rs.size() * kDim);
    Y.resize(rs.size());
    for (std::size_t i = 0; i < rs.size(); ++i) {
      materialise(*rs[i], X.data() + i * kDim);
      Y[i] = rs[i]->label;
    }
  };
  std::vector<float> Xf, Xv, Xt;
  std::vector<std::uint8_t> Yf, Yv, Yt;
  materialise_all(fit, Xf, Yf);
  materialise_all(val, Xv, Yv);
  std::vector<const Record*> test_ptr;
  test_ptr.reserve(test_rows.size());
  for (const Record& r : test_rows) test_ptr.push_back(&r);
  materialise_all(test_ptr, Xt, Yt);

  const double base_rate =
      Yf.empty() ? 0.0
                 : static_cast<double>(std::accumulate(Yf.begin(), Yf.end(), 0ull,
                                                       [](unsigned long long s, std::uint8_t v) {
                                                         return s + v;
                                                       })) /
                       Yf.size();
  std::printf("positive rate: fit %.4f\n\n", base_rate);

  auto eval = [&](auto&& predict, const std::vector<float>& X,
                  const std::vector<std::uint8_t>& Y, std::vector<float>& out) {
    out.resize(Y.size());
    for (std::size_t i = 0; i < Y.size(); ++i) out[i] = predict(X.data() + i * kDim);
    return score(out, Y);
  };

  const double val_rate = Yv.empty() ? 0.0 : constant_metrics(0.5, Yv).base;
  const double test_rate = Yt.empty() ? 0.0 : constant_metrics(0.5, Yt).base;
  std::printf("positive rate: fit %.4f  validation %.4f  test %.4f\n", base_rate, val_rate,
              test_rate);
  std::printf("the rate moves between periods, so a model has to be recalibrated"
              " rather than trusted as is\n\n");

  // --- baselines ------------------------------------------------------------
  std::printf("validation day\n");
  print_metrics("base rate", constant_metrics(base_rate, Yv));

  // One feature: log1p of the queue ahead measured in 30 second windows of
  // consuming volume, which is the quantity the whole question reduces to.
  FtrlLogistic one({a.ftrl_alpha, 1.0f, 0.0f, a.ftrl_l2}, kDim);
  auto one_x = [](const float* full, float* x) {
    for (std::size_t i = 0; i < kDim; ++i) x[i] = 0.0f;
    x[0] = 1.0f;
    x[25] = full[25];
  };
  {
    std::vector<float> x(kDim);
    for (std::size_t i = 0; i < Yf.size(); ++i) {
      one_x(Xf.data() + i * kDim, x.data());
      const float p = one.predict(x.data());
      one.update(x.data(), p, static_cast<float>(Yf[i]));
    }
    one.finalise();
  }
  auto one_predict = [&](const float* full) {
    float x[kDim];
    one_x(full, x);
    return one.predict_fixed(x);
  };
  std::vector<float> one_val;
  print_metrics("one feature", eval(one_predict, Xv, Yv, one_val));

  // --- logistic, hyperparameters chosen on the validation day ---------------
  const float alphas[3] = {0.02f, 0.05f, 0.15f};
  const float l1s[3] = {0.0f, 0.5f, 2.0f};
  const float l2s[2] = {0.5f, 2.0f};
  FtrlConfig best_cfg{a.ftrl_alpha, 1.0f, a.ftrl_l1, a.ftrl_l2};
  double best_lg = 1e18;
  std::printf("\nlogistic: %d configurations searched on the validation day\n", 3 * 3 * 2);
  for (float al : alphas) {
    for (float l1 : l1s) {
      for (float l2 : l2s) {
        FtrlLogistic m({al, 1.0f, l1, l2}, kDim);
        for (std::size_t i = 0; i < Yf.size(); ++i) {
          const float* x = Xf.data() + i * kDim;
          m.update(x, m.predict(x), static_cast<float>(Yf[i]));
        }
        m.finalise();
        std::vector<float> p;
        const Metrics mv = eval([&](const float* x) { return m.predict_fixed(x); }, Xv, Yv, p);
        if (mv.log_loss < best_lg) {
          best_lg = mv.log_loss;
          best_cfg = FtrlConfig{al, 1.0f, l1, l2};
        }
      }
    }
  }
  std::printf("  chosen alpha %.3f  l1 %.2f  l2 %.2f  (validation logloss %.5f)\n",
              best_cfg.alpha, best_cfg.l1, best_cfg.l2, best_lg);

  FtrlLogistic lg(best_cfg, kDim);
  for (std::size_t i = 0; i < Yf.size(); ++i) {
    const float* x = Xf.data() + i * kDim;
    lg.update(x, lg.predict(x), static_cast<float>(Yf[i]));
  }
  lg.finalise();
  std::vector<float> lg_val;
  const Metrics lgv = eval([&](const float* x) { return lg.predict_fixed(x); }, Xv, Yv, lg_val);
  print_metrics("logistic", lgv);

  // --- mlp, width, rate and epochs all chosen on the validation day ---------
  const std::size_t hiddens[2] = {16, 32};
  const float lrs[2] = {0.001f, 0.003f};
  MlpConfig best_mc;
  int best_epoch = 0;
  double best_mlp = 1e18;
  std::printf("\nmlp: %d configurations, epochs stopped on the validation day\n", 2 * 2);
  for (std::size_t h : hiddens) {
    for (float lr : lrs) {
      MlpConfig mc;
      mc.hidden = h;
      mc.lr = lr;
      Mlp m(mc, kDim);
      double prev = 1e18;
      for (int e = 1; e <= a.max_epochs; ++e) {
        for (std::size_t i = 0; i < Yf.size(); ++i) {
          const float* x = Xf.data() + i * kDim;
          m.update(x, m.predict(x), static_cast<float>(Yf[i]));
        }
        std::vector<float> p;
        const Metrics mv = eval([&](const float* x) { return m.predict(x); }, Xv, Yv, p);
        if (mv.log_loss < best_mlp) {
          best_mlp = mv.log_loss;
          best_mc = mc;
          best_epoch = e;
        }
        if (mv.log_loss > prev) break;   // past the minimum for this configuration
        prev = mv.log_loss;
      }
    }
  }
  std::printf("  chosen hidden %zu  lr %.4f  epochs %d  (validation logloss %.5f)\n",
              best_mc.hidden, best_mc.lr, best_epoch, best_mlp);
  Mlp mlp(best_mc, kDim);
  for (int e = 0; e < best_epoch; ++e) {
    for (std::size_t i = 0; i < Yf.size(); ++i) {
      const float* x = Xf.data() + i * kDim;
      mlp.update(x, mlp.predict(x), static_cast<float>(Yf[i]));
    }
  }
  std::vector<float> mlp_val;
  print_metrics("mlp", eval([&](const float* x) { return mlp.predict(x); }, Xv, Yv, mlp_val));

  // --- recalibrate on the validation day ------------------------------------
  Platt cal_one, cal_lg, cal_mlp;
  cal_one.fit(one_val, Yv);
  cal_lg.fit(lg_val, Yv);
  cal_mlp.fit(mlp_val, Yv);
  std::printf("\nPlatt scaling fitted on the validation day:"
              " one feature a=%.3f b=%.3f, logistic a=%.3f b=%.3f, mlp a=%.3f b=%.3f\n",
              cal_one.a, cal_one.b, cal_lg.a, cal_lg.b, cal_mlp.a, cal_mlp.b);

  std::vector<float> pt;
  if (have_test) {
    std::printf("\ntest set, scored once\n");
    print_metrics("base rate", constant_metrics(base_rate, Yt));
    std::vector<float> p1, p2, p3;
    const Metrics m1 = eval([&](const float* x) { return cal_one.apply(one_predict(x)); }, Xt,
                            Yt, p1);
    print_metrics("one feature", m1);
    const Metrics m2 =
        eval([&](const float* x) { return cal_lg.apply(lg.predict_fixed(x)); }, Xt, Yt, p2);
    print_metrics("logistic", m2);
    const Metrics m3 =
        eval([&](const float* x) { return cal_mlp.apply(mlp.predict(x)); }, Xt, Yt, p3);
    print_metrics("mlp", m3);
    std::printf("  logistic keeps %zu of %zu coordinates non zero under L1\n", lg.nonzero(),
                kDim);
    pt = p2;
    std::printf("\n");
    print_calibration(pt, Yt);

    std::printf("\n  per day on the test set (logistic, recalibrated)\n");
    std::printf("    %-6s %10s %10s %10s %8s\n", "day", "n", "positives", "logloss", "auc");
    for (int d = 0; d <= 64; ++d) {
      std::vector<float> dp;
      std::vector<std::uint8_t> dy;
      for (std::size_t i = 0; i < test_rows.size(); ++i) {
        if (static_cast<int>(test_rows[i].day) != d) continue;
        dp.push_back(pt[i]);
        dy.push_back(Yt[i]);
      }
      if (dp.empty()) continue;
      const Metrics m = score(dp, dy);
      std::printf("    %-6d %10zu %10.4f %10.5f %8.4f\n", d, m.n, m.base, m.log_loss, m.auc);
    }
  }

  if (!a.save.empty()) {
    if (!save_model(a.save, lg, cal_lg.a, cal_lg.b)) {
      std::fprintf(stderr, "cannot write %s\n", a.save.c_str());
      return 1;
    }
    std::printf("\nwrote %s\n", a.save.c_str());
  }
  return 0;
}
