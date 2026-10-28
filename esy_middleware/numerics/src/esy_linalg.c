/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/numerics/esy_linalg.h"

#include <math.h>
#include <float.h>
#include <stddef.h>

static const float k_min_pivot = 1.0e-12f;

/* Small helpers for matrix copy and identity initialization. */
static void esy_mat_copy(float *dst, const float *src, int n) {
  for (int i = 0; i < n * n; ++i) {
    dst[i] = src[i];
  }
}

static void esy_mat_eye(float *A, int n) {
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      A[i * n + j] = (i == j) ? 1.0f : 0.0f;
    }
  }
}

/* Cholesky solve. A must be SPD; the lower-triangular factor is stored in A. */
int esy_linalg_solve(float *A, float *b, float *x, int m) {
  if ((A == NULL) || (b == NULL) || (x == NULL)) {
    return -1;
  }
  if ((m <= 0) || (m > ESY_LINALG_MAX_M)) {
    return -1;
  }

  for (int i = 0; i < m * m; ++i) {
    if (!isfinite(A[i])) return -1;
  }
  for (int i = 0; i < m; ++i) {
    if (!isfinite(b[i])) return -1;
  }

  /* Factor A = L L^T. */
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j <= i; ++j) {
      float sum = A[i * m + j];
      for (int k = 0; k < j; ++k) {
        sum -= A[i * m + k] * A[j * m + k];
      }
      if (i == j) {
        if (!isfinite(sum) || sum <= fmaxf(k_min_pivot,
            FLT_EPSILON * (float)m * fabsf(A[i * m + i]))) {
          return -1;
        }
        A[i * m + j] = sqrtf(sum);
      } else {
        A[i * m + j] = sum / A[j * m + j];
        if (!isfinite(A[i * m + j])) return -1;
      }
    }
    /* Clear the upper triangle so A only stores L. */
    for (int j = i + 1; j < m; ++j) {
      A[i * m + j] = 0.0f;
    }
  }

  /* Solve L y = b. */
  float y[ESY_LINALG_MAX_M];
  for (int i = 0; i < m; ++i) {
    float sum = b[i];
    for (int k = 0; k < i; ++k) {
      sum -= A[i * m + k] * y[k];
    }
    y[i] = sum / A[i * m + i];
    if (!isfinite(y[i])) return -1;
  }

  /* Solve L^T x = y. */
  for (int i = m - 1; i >= 0; --i) {
    float sum = y[i];
    for (int k = i + 1; k < m; ++k) {
      sum -= A[k * m + i] * x[k]; /* Use the transpose of L. */
    }
    x[i] = sum / A[i * m + i];
    if (!isfinite(x[i])) return -1;
  }
  return 0;
}

/* Gauss-Jordan inverse for small matrices. */
int esy_linalg_inverse(const float *A, float *A_inv, int m) {
  if ((A == NULL) || (A_inv == NULL)) {
    return -1;
  }
  if ((m <= 0) || (m > ESY_LINALG_MAX_M)) {
    return -1;
  }

  for (int i = 0; i < m * m; ++i) {
    if (!isfinite(A[i])) return -1;
  }
  float L[ESY_LINALG_MAX_M * ESY_LINALG_MAX_M];
  float R[ESY_LINALG_MAX_M * ESY_LINALG_MAX_M];
  esy_mat_copy(L, A, m);
  esy_mat_eye(R, m);

  for (int k = 0; k < m; ++k) {
    int piv = k;
    float maxv = fabsf(L[k * m + k]);
    for (int i = k + 1; i < m; ++i) {
      float v = fabsf(L[i * m + k]);
      if (v > maxv) {
        maxv = v;
        piv = i;
      }
    }
    if (!isfinite(maxv) || maxv < k_min_pivot) {
      return -1;
    }

    if (piv != k) {
      for (int j = 0; j < m; ++j) {
        float t = L[k * m + j];
        float u = R[k * m + j];
        L[k * m + j] = L[piv * m + j];
        R[k * m + j] = R[piv * m + j];
        L[piv * m + j] = t;
        R[piv * m + j] = u;
      }
    }

    float diag = L[k * m + k];
    for (int j = 0; j < m; ++j) {
      L[k * m + j] /= diag;
      R[k * m + j] /= diag;
    }

    for (int i = 0; i < m; ++i) {
      if (i == k) {
        continue;
      }
      float f = L[i * m + k];
      if (f != 0.0f) {
        for (int j = 0; j < m; ++j) {
          L[i * m + j] -= f * L[k * m + j];
          R[i * m + j] -= f * R[k * m + j];
        }
      }
    }
  }

  for (int i = 0; i < m * m; ++i) {
    if (!isfinite(L[i]) || !isfinite(R[i])) return -1;
  }
  esy_mat_copy(A_inv, R, m);
  return 0;
}
