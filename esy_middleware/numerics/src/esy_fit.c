/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/numerics/esy_fit.h"
#include "esy_middleware/numerics/esy_linalg.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

static const float k_tie_epsilon = 1.0e-6f;

/* Exclude one sample for explicit LOOCV; -1 includes the complete dataset. */
static int accumulate_normal_eq(const float *X, const float *Y, int n, int m,
                                int excluded, esy_fit_feature_fn feature_fn,
                                void *user, float lambda, float *A, float *b) {
  for (int r = 0; r < m; ++r) {
    b[r] = 0.0f;
    for (int c = 0; c < m; ++c) A[r*m + c] = r == c ? lambda : 0.0f;
  }
  float phi[ESY_FIT_MAX_M];
  for (int i = 0; i < n; ++i) {
    if (i == excluded) continue;
    feature_fn(X[i], phi, m, user);
    for (int j = 0; j < m; ++j) {
      if (!isfinite(phi[j])) return -1;
    }
    for (int r = 0; r < m; ++r) {
      b[r] += phi[r] * Y[i];
      for (int c = 0; c < m; ++c) A[r*m + c] += phi[r] * phi[c];
    }
  }
  /* The solver validates every accumulated element before using it. */
  return 0;
}

static int finite_samples(const float *X, const float *Y, int n) {
  for (int i = 0; i < n; ++i) {
    if (!isfinite(X[i]) || !isfinite(Y[i])) return 0;
  }
  return 1;
}

static float predict_feature(float x, const float *w, int m,
                             esy_fit_feature_fn feature_fn, void *user) {
  float phi[ESY_FIT_MAX_M];
  feature_fn(x, phi, m, user);
  float y = 0.0f;
  for (int j = 0; j < m; ++j) y += phi[j] * w[j];
  return y;
}

esy_err_t esy_fit_train(const float *X, const float *Y, int n,
                        int m, float lambda,
                        esy_fit_feature_fn feature_fn, void *feature_user,
                        esy_fit_model_t *out_model) {
  if (X == NULL || Y == NULL || out_model == NULL || feature_fn == NULL ||
      n <= 0 || n > ESY_FIT_MAX_N || m <= 0 || m > ESY_FIT_MAX_M || n < m ||
      !isfinite(lambda) || lambda < 0.0f) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!finite_samples(X, Y, n)) return ESY_ERR_INVALID_ARG;

  float A[ESY_FIT_MAX_M * ESY_FIT_MAX_M];
  float b[ESY_FIT_MAX_M];
  float w[ESY_FIT_MAX_M];
  if (accumulate_normal_eq(X, Y, n, m, -1, feature_fn, feature_user,
                           lambda, A, b) != 0 ||
      esy_linalg_solve(A, b, w, m) != 0) {
    return ESY_ERR_FAIL;
  }
  double se = 0.0;
  for (int i = 0; i < n; ++i) {
    const float prediction = predict_feature(X[i], w, m, feature_fn, feature_user);
    const double residual = (double)Y[i] - prediction;
    se += residual * residual;
  }
  const double rmse = sqrt(se / n);
  if (!isfinite(rmse) || rmse > FLT_MAX) return ESY_ERR_FAIL;

  out_model->m = m;
  for (int j = 0; j < m; ++j) out_model->w[j] = w[j];
  out_model->rmse_train = (float)rmse;
  return ESY_OK;
}

int esy_fit_select_m_loocv(const float *X, const float *Y, int n,
                           int m_min, int m_max, float lambda,
                           esy_fit_feature_fn feature_fn, void *feature_user,
                           float *out_cv_best) {
  if (out_cv_best != NULL) *out_cv_best = 0.0f;
  if (X == NULL || Y == NULL || feature_fn == NULL ||
      n <= 1 || n > ESY_FIT_MAX_N || !isfinite(lambda) || lambda < 0.0f ||
      m_min < 1 || m_max > ESY_FIT_MAX_M || m_min > m_max) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!finite_samples(X, Y, n)) return ESY_ERR_INVALID_ARG;

  double best_cv = 0.0;
  int best_m = -1;
  for (int m = m_min; m <= m_max; ++m) {
    /* Retain the training API's n >= m limit; OLS also needs n-1 >= m. */
    if (n < m || (lambda == 0.0f && n <= m)) continue;
    double se = 0.0;
    int valid = 1;
    for (int held = 0; held < n; ++held) {
      float A[ESY_FIT_MAX_M * ESY_FIT_MAX_M];
      float b[ESY_FIT_MAX_M];
      float w[ESY_FIT_MAX_M];
      if (accumulate_normal_eq(X, Y, n, m, held, feature_fn, feature_user,
                               lambda, A, b) != 0 ||
          esy_linalg_solve(A, b, w, m) != 0) {
        valid = 0;
        break;
      }
      const float prediction = predict_feature(X[held], w, m, feature_fn, feature_user);
      const double residual = (double)Y[held] - prediction;
      se += residual * residual;
    }
    if (!valid) continue;
    const double cv = sqrt(se / n);
    if (!isfinite(cv) || cv > FLT_MAX) continue;
    /* Ascending candidates retain the smaller dimension for ties. */
    if (best_m < 0 || cv + k_tie_epsilon < best_cv) {
      best_cv = cv;
      best_m = m;
    }
  }
  if (best_m < 0) return ESY_ERR_FAIL;
  if (out_cv_best != NULL) *out_cv_best = (float)best_cv;
  return best_m;
}
