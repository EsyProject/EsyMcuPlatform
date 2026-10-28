/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LOGGER_DEFS_H
#define ESY_LOGGER_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log severity levels shared by projects and logger implementations.
 */
typedef enum {
  ESY_LOG_LEVEL_ERROR = 0,  /**< Error conditions. */
  ESY_LOG_LEVEL_WARN,       /**< Warning conditions. */
  ESY_LOG_LEVEL_INFO,       /**< Informational messages. */
  ESY_LOG_LEVEL_DEBUG,      /**< Debug-level messages. */
  ESY_LOG_LEVEL_VERBOSE,    /**< Highly verbose tracing messages. */
} esy_log_level_t;

#ifdef __cplusplus
}
#endif

#endif /* ESY_LOGGER_DEFS_H */
