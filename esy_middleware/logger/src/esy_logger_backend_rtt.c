/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_logger_backend_rtt_internal.h"
#include "esy_logger_backend_os_internal.h"
esy_err_t esy_logger_backend_rtt_init(esy_logger_backend_t *backend,
    const esy_logger_backend_rtt_config_t *config) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  esy_logger_backend_runtime_t runtime = esy_logger_os_runtime();
  return esy_logger_backend_rtt_init_with_runtime(backend, config, &runtime);
}
