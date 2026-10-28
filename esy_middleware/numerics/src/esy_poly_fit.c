/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/numerics/esy_poly_fit.h"

#include <stddef.h>
#include <float.h>
#include <math.h>

/* --------------------------- Internal scaling helpers ---------------------- */

/* Compute min/max scaling: xs = (x - mean) * scale, approximately [-1, 1]. */
static int compute_scale_minmax(const float *x, int n, float *mean, float *scale) {
  float xmin = x[0];
  float xmax = x[0];

  for (int i = 1; i < n; ++i) {
    if (x[i] < xmin) {
      xmin = x[i];
    }
    if (x[i] > xmax) {
      xmax = x[i];
    }
  }

  *mean = (float)(0.5 * ((double)xmax + xmin));
  const double half = 0.5 * ((double)xmax - xmin);
  const double value = half > 0.0 ? 1.0 / half : 1.0;
  if (value > FLT_MAX) return -1;
  *scale = (float)value;
  return 0;
}

/* Compose the scaled polynomial with xs = (x - mean) * scale. */
static int expand_scaled_to_raw(const float *w, int d, float mean, float scale, float *A) {
  double coefficients[ESY_FIT_MAX_DEG + 1] = {0};
  coefficients[0] = w[d];
  const double offset = -(double)mean * scale;
  for (int k = d - 1; k >= 0; --k) {
    for (int j = d - k; j > 0; --j) {
      coefficients[j] = coefficients[j] * offset + coefficients[j - 1] * scale;
    }
    coefficients[0] = coefficients[0] * offset + w[k];
  }
  for (int j = 0; j <= d; ++j) {
    if (!isfinite(coefficients[j]) || fabs(coefficients[j]) > FLT_MAX) return -1;
    A[j] = (float)coefficients[j];
  }
  return 0;
}

/* --------------------------- Polynomial feature callback ------------------- */

typedef struct {
  float mean;
  float scale;
} poly_feature_ctx_t;

/* φ(x) = [1, xs, xs^2, ..., xs^(m-1)] */
static void poly_feature_fn(float x, float *phi, int m, void *user) {
  const poly_feature_ctx_t *ctx = (const poly_feature_ctx_t *)user;
  const float xs = (float)(((double)x - ctx->mean) * ctx->scale);
  float p = 1.0f;

  for (int j = 0; j < m; ++j) {
    phi[j] = p;
    p *= xs;
  }
}

/* --------------------------- Public API ------------------------------------ */

esy_err_t esy_poly_fit(const float *X, const float *Y, int n,
                       int degree, float lambda,
                       float *A_out,
                       esy_poly_model_t *model_out,
                       float *rmse_out)
{
  if (rmse_out != NULL) {
    *rmse_out = 0.0f;
  }
  if ((X == NULL) || (Y == NULL) || (A_out == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((n <= 0) || (n > ESY_FIT_MAX_N)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((degree < 0) || (degree > ESY_FIT_MAX_DEG)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!isfinite(lambda) || lambda < 0.0f) {
    return ESY_ERR_INVALID_ARG;
  }

  const int m = degree + 1;
  if ((m > ESY_FIT_MAX_M) || (n < m)) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Compute scaling parameters. */
  float mean, scale;
  for (int i = 0; i < n; ++i) {
    if (!isfinite(X[i]) || !isfinite(Y[i])) return ESY_ERR_INVALID_ARG;
  }
  if (compute_scale_minmax(X, n, &mean, &scale) != 0) return ESY_ERR_FAIL;

  /* Train in the scaled domain. */
  poly_feature_ctx_t fctx = { mean, scale };
  esy_fit_model_t    fitm = {0};
  esy_err_t rc = esy_fit_train(X, Y, n, m, lambda, poly_feature_fn, &fctx, &fitm);
  if (rc != ESY_OK) {
    return rc;
  }

  /* Expand coefficients into the raw input domain. */
  float coefficients[ESY_FIT_MAX_DEG + 1];
  if (expand_scaled_to_raw(fitm.w, degree, mean, scale, coefficients) != 0) {
    return ESY_ERR_FAIL;
  }
  for (int j = 0; j < m; ++j) A_out[j] = coefficients[j];

  /* Publish model and scaled-domain RMSE. */
  if (model_out) {
    model_out->degree = degree;
    model_out->mean   = mean;
    model_out->scale  = scale;
    for (int j = 0; j < m; ++j) model_out->w[j] = fitm.w[j];
    model_out->rmse_train = fitm.rmse_train; /* Same scaled-domain RMSE as rmse_out. */
  }
  if (rmse_out) *rmse_out = fitm.rmse_train;

  return ESY_OK;
}

int esy_poly_select_degree_loocv(const float *X, const float *Y, int n,
                                 int deg_min, int deg_max, float lambda)
{
  if ((X == NULL) || (Y == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((n <= 1) || (n > ESY_FIT_MAX_N)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!isfinite(lambda) || lambda < 0.0f) {
    return ESY_ERR_INVALID_ARG;
  }

  if (deg_min < 0 || deg_max > ESY_FIT_MAX_DEG || deg_min > deg_max) {
    return ESY_ERR_INVALID_ARG;
  }

  /* Use one degree-independent scale for all LOOCV candidates. */
  float mean, scale;
  for (int i = 0; i < n; ++i) {
    if (!isfinite(X[i]) || !isfinite(Y[i])) return ESY_ERR_INVALID_ARG;
  }
  if (compute_scale_minmax(X, n, &mean, &scale) != 0) return ESY_ERR_FAIL;
  poly_feature_ctx_t fctx = { mean, scale };

  const int m_min = deg_min + 1;
  const int m_max = deg_max + 1;

  int best_m = esy_fit_select_m_loocv(X, Y, n, m_min, m_max, lambda,
                                      poly_feature_fn, &fctx, NULL);
  if (best_m < 0) {
    return best_m;
  }

  return best_m - 1; /* m = degree + 1 */
}
