/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_NUMERICS_CONFIG_H
#define ESY_NUMERICS_CONFIG_H

/**
 * @file esy_numerics_config.h
 * @brief Shared numerical limits and model ABI configuration.
 *
 * CMake consumers inherit the generated configuration from esy_numerics. Set the
 * ESY_* limit variables in the consumer feature file before adding EsyMcuPlatform,
 * then rebuild the library and every consumer. Never override limits per file.
 * Direct source builds without the generated configuration use these defaults.
 */
#ifdef ESY_NUMERICS_CONFIGURED_BUILD
#include "esy_middleware/numerics/esy_numerics_build_config.h"
#else
#define ESY_NUMERICS_BUILD_LINALG_MAX_M 8
#define ESY_NUMERICS_BUILD_FIT_MAX_M 8
#define ESY_NUMERICS_BUILD_FIT_MAX_N 32
#define ESY_NUMERICS_BUILD_FIT_MAX_DEG 6
#endif

#if defined(ESY_LINALG_MAX_M) && ESY_LINALG_MAX_M != ESY_NUMERICS_BUILD_LINALG_MAX_M
#error "ESY_LINALG_MAX_M differs from the esy_numerics build configuration"
#endif
#ifndef ESY_LINALG_MAX_M
/** @brief Maximum matrix dimension; shared by library and consumers. */
#define ESY_LINALG_MAX_M ESY_NUMERICS_BUILD_LINALG_MAX_M
#endif

#if defined(ESY_FIT_MAX_M) && ESY_FIT_MAX_M != ESY_NUMERICS_BUILD_FIT_MAX_M
#error "ESY_FIT_MAX_M differs from the esy_numerics build configuration"
#endif
#ifndef ESY_FIT_MAX_M
/** @brief Maximum feature count and fitted-model weight capacity. */
#define ESY_FIT_MAX_M ESY_NUMERICS_BUILD_FIT_MAX_M
#endif

#if defined(ESY_FIT_MAX_N) && ESY_FIT_MAX_N != ESY_NUMERICS_BUILD_FIT_MAX_N
#error "ESY_FIT_MAX_N differs from the esy_numerics build configuration"
#endif
#ifndef ESY_FIT_MAX_N
/** @brief Maximum sample count; bounds work rather than stack array capacity. */
#define ESY_FIT_MAX_N ESY_NUMERICS_BUILD_FIT_MAX_N
#endif

#if defined(ESY_FIT_MAX_DEG) && ESY_FIT_MAX_DEG != ESY_NUMERICS_BUILD_FIT_MAX_DEG
#error "ESY_FIT_MAX_DEG differs from the esy_numerics build configuration"
#endif
#ifndef ESY_FIT_MAX_DEG
/** @brief Maximum polynomial degree; model capacity is this value plus one. */
#define ESY_FIT_MAX_DEG ESY_NUMERICS_BUILD_FIT_MAX_DEG
#endif

#if ESY_FIT_MAX_M < 1 || ESY_LINALG_MAX_M < ESY_FIT_MAX_M || ESY_LINALG_MAX_M > 46340
#error "Numerical dimensions must satisfy 1 <= FIT_MAX_M <= LINALG_MAX_M <= 46340"
#endif
#if ESY_FIT_MAX_N < 1 || ESY_FIT_MAX_N > 2147483647
#error "ESY_FIT_MAX_N must be a positive int"
#endif
#if ESY_FIT_MAX_DEG < 0 || ESY_FIT_MAX_DEG >= ESY_FIT_MAX_M
#error "ESY_FIT_MAX_DEG must be in [0, ESY_FIT_MAX_M)"
#endif

#endif /* ESY_NUMERICS_CONFIG_H */
