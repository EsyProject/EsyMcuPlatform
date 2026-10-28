/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LINALG_H
#define ESY_LINALG_H

#include <stddef.h>
#include <stdint.h>

#include "esy_middleware/numerics/esy_numerics_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_linalg.h
 * @brief Lightweight linear algebra utilities for small dense systems.
 *
 * This header provides minimal linear algebra routines intended for embedded
 * and constrained environments:
 * - Solving small symmetric positive definite systems (A x = b)
 * - Inverting small square matrices with partial pivoting
 *
 * Design goals:
 * - No dynamic memory allocation
 * - Fixed maximum dimension known at compile time
 * - Simple API suitable for numerical kernels (e.g. regression, fitting)
 *
 * Calls are synchronous and task-context only. All buffers are caller-owned and
 * must not overlap. Separate buffers may be processed concurrently; shared
 * mutable buffers require external serialization. No pointer or resource is
 * retained. All inputs must be finite; nonfinite intermediate/output values
 * cause failure. Output buffers need no initialization and may be reused.
 */

/**
 * @brief Solve a symmetric positive definite linear system A · x = b.
 *
 * The function uses Cholesky factorization and therefore requires @p A to be
 * symmetric positive definite. It is intended for normal-equation systems such
 * as regression kernels. Symmetry is a caller precondition; the lower
 * triangle is used and nonpositive Cholesky pivots are rejected.
 *
 * @note
 * - The input matrix @p A is modified during computation; @p b is only read.
 * - The caller must provide writable buffers; A and x may be partially
 *   overwritten on failure and must not be used as a successful result.
 * - The dimension @p m must not exceed @ref ESY_LINALG_MAX_M.
 * - Passing NULL pointers or an invalid dimension returns failure.
 *
 * @param[in,out] A  Coefficient matrix A (size m×m), modified in-place.
 * @param[in]     b  Right-hand side vector b (size m), not modified.
 * @param[out]    x  Output solution vector x (size m).
 * @param[in]     m  Dimension of the system (1..ESY_LINALG_MAX_M).
 *
 * @retval 0     Success.
 * @retval nonzero Failure (e.g. matrix is singular or ill-conditioned).
 */
int esy_linalg_solve(float *A, float *b, float *x, int m);

/**
 * @brief Compute the inverse of a square matrix.
 *
 * Calculates:
 *   A_inv = inv(A)
 *
 * @note
 * - The input matrix @p A is not modified; failure preserves A_inv.
 * - The output buffer @p A_inv must have space for m×m elements.
 * - The matrix must be invertible; otherwise the function fails.
 * - Passing NULL pointers or an invalid dimension returns failure.
 *
 * @param[in]  A      Input matrix A (size m×m).
 * @param[out] A_inv  Output inverse matrix (size m×m).
 * @param[in]  m      Matrix dimension (1..ESY_LINALG_MAX_M).
 *
 * @retval 0     Success.
 * @retval nonzero Failure (e.g. matrix is not invertible).
 */
int esy_linalg_inverse(const float *A, float *A_inv, int m);

#ifdef __cplusplus
}
#endif

#endif /* ESY_LINALG_H */
