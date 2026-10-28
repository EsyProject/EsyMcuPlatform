/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_FIT_H
#define ESY_FIT_H

#include <stdint.h>

#include "esy_middleware/numerics/esy_numerics_config.h"

#include "esy_common/defs/base/esy_common_defs.h"  /* esy_err_t and ESY_* error codes */

/**
 * @file esy_fit.h
 * @brief Generic linear regression core (least squares / ridge) and LOOCV model selection.
 *
 * ## Overview
 * This module provides a small, dependency-light regression kernel for fitting a
 * linear model in an arbitrary feature space:
 *
 *   y ≈ w^T φ(x)
 *
 * where the feature vector φ(x) is supplied by a user callback. This makes the module
 * agnostic to the feature design and suitable for:
 * - polynomial bases
 * - orthogonal polynomials
 * - splines
 * - radial basis functions (RBF)
 * - any other user-defined features
 *
 * ## Constraints and memory model
 * - The module uses static-sized arrays controlled by compile-time limits.
 * - No dynamic allocation is required.
 * - The maximum feature dimension and sample count are controlled by:
 *   - @ref ESY_FIT_MAX_M (default 8)
 *   - @ref ESY_FIT_MAX_N (default 32)
 *
 * ## Numerical notes
 * Training uses the normal equation with optional L2 regularization (ridge):
 *
 *   (ΦᵀΦ + λI) w = Φᵀ y
 *
 * Ridge regularization (λ > 0) improves conditioning and helps avoid overfitting.
 *
 * LOOCV explicitly fits each held-out dataset. Candidates with a failing fold
 * are skipped; no leverage denominator clipping or inverse approximation is used.
 * All inputs and regularization coefficients must be finite.
 *
 * ## Execution and ownership
 * Calls are synchronous, task-context only, and use caller-owned arrays and stack
 * workspaces. No pointer is retained. Separate buffers and callback contexts may
 * be used concurrently; callers must serialize access to shared mutable buffers
 * or contexts. Input arrays must remain unchanged until return; input, output,
 * and callback-owned writable storage must not overlap. There are no init/deinit
 * resources. Output models need no initialization, may be reused, and remain
 * unchanged on failure. Only w[0..m-1] is valid after success.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Types and callbacks                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Feature mapping callback: map scalar x into a feature vector φ(x) of length m.
 *
 * The implementation must write exactly m floats into `phi[0..m-1]`.
 * The callback must be deterministic for the same `(x, m, user)` tuple and
 * must not keep the @p phi pointer after returning. It executes synchronously
 * in the calling task and must write finite values. The caller retains ownership
 * of @p user and keeps it alive until the API returns. Callback blocking extends
 * the entire call's execution time; there is no internal scheduling or timeout.
 *
 * @param[in]  x     Input scalar value.
 * @param[out] phi   Output feature buffer with capacity at least m.
 * @param[in]  m     Feature dimension.
 * @param[in]  user  User-provided context pointer (may be NULL).
 */
typedef void (*esy_fit_feature_fn)(float x, float *phi, int m, void *user);

/**
 * @brief Trained linear model parameters.
 *
 * This struct only stores fitted parameters and a training-set RMSE for convenience.
 * The meaning of w[] depends on the feature mapping φ(x).
 */
typedef struct {
  int   m;                /**< Feature/parameter dimension. */
  float w[ESY_FIT_MAX_M]; /**< Weights (w[0]..w[m-1]). */
  float rmse_train;       /**< Training RMSE computed on the training set. */
} esy_fit_model_t;

/* -------------------------------------------------------------------------- */
/* API                                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Train a linear model with optional ridge regularization.
 *
 * Solves the normal equation:
 *   (Φ^TΦ + λI) w = Φ^T y
 *
 * Notes:
 * - n >= m is required, including for positive ridge regularization.
 * - λ > 0 (ridge) helps conditioning and can reduce overfitting.
 * - λ == 0 corresponds to ordinary least squares (OLS).
 * - This function uses normal equations and a Cholesky solve; singular or
 *   non-positive-definite systems return ESY_ERR_FAIL.
 *
 * @param[in]  X            Input samples x, length n.
 * @param[in]  Y            Target samples y, length n.
 * @param[in]  n            Number of samples (1..ESY_FIT_MAX_N).
 * @param[in]  m            Feature dimension (1..ESY_FIT_MAX_M).
 * @param[in]  lambda       Ridge coefficient (>= 0).
 * @param[in]  feature_fn   Feature callback (must not be NULL).
 * @param[in]  feature_user User context passed to feature_fn (may be NULL).
 * @param[out] out_model    Output model (must not be NULL).
 * @return esy_err_t        ESY_OK on success; ESY_ERR_INVALID_ARG for invalid
 *                          inputs (including nonfinite samples/lambda);
 *                          ESY_ERR_FAIL for nonfinite features or numerical failure.
 */
esy_err_t esy_fit_train(const float *X, const float *Y, int n,
                        int m, float lambda,
                        esy_fit_feature_fn feature_fn, void *feature_user,
                        esy_fit_model_t *out_model);

/**
 * @brief Select the best feature dimension m in [m_min, m_max] using LOOCV.
 *
 * This function explicitly refits each leave-one-out dataset using the same
 * feature callback. It skips candidates with n < m, OLS candidates with n <= m,
 * and candidates with any failed or nonfinite fold. Positive ridge can solve a
 * fold with fewer samples than parameters. Returns ESY_ERR_FAIL if no candidate
 * remains. Runtime is O(n * (n*m*m + m*m*m)) per candidate, with O(m*m) workspace.
 *
 * Ties (|Δ| <= 1e-6) are resolved in favor of the smaller m.
 * If @p out_cv_best is not NULL, it is set to 0.0f before validation and to
 * the selected LOOCV RMSE on success.
 *
 * @param[in]  X            Input samples x, length n.
 * @param[in]  Y            Target samples y, length n.
 * @param[in]  n            Number of samples (2..ESY_FIT_MAX_N).
 * @param[in]  m_min        Minimum candidate feature dimension (>= 1).
 * @param[in]  m_max        Maximum candidate feature dimension (<= ESY_FIT_MAX_M and >= m_min).
 * @param[in]  lambda       Ridge coefficient (>= 0).
 * @param[in]  feature_fn   Feature callback (must not be NULL).
 * @param[in]  feature_user User context passed to feature_fn (may be NULL).
 * @param[out] out_cv_best  Optional output: LOOCV RMSE for the selected m (may be NULL).
 * @return int              Best m on success; ESY_ERR_INVALID_ARG for invalid
 *                          bounds, nonfinite inputs or negative lambda;
 *                          ESY_ERR_FAIL if all candidates fail.
 */
int esy_fit_select_m_loocv(const float *X, const float *Y, int n,
                           int m_min, int m_max, float lambda,
                           esy_fit_feature_fn feature_fn, void *feature_user,
                           float *out_cv_best);

#ifdef __cplusplus
}
#endif

#endif /* ESY_FIT_H */
