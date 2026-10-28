/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_hal_xio_spi.h"

#include <string.h>

static esy_err_t s_init_result = ESY_OK;

void fake_esy_hal_xio_spi_reset(void) {
  s_init_result = ESY_OK;
}

void fake_esy_hal_xio_spi_set_init_result(esy_err_t ret) {
  s_init_result = ret;
}

void esy_hal_xio_spi_default_cfg(esy_xio_spi_config_t *cfg) {
  if (cfg == NULL) {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->pin_sck = ESY_XIO_PIN_UNUSED;
  cfg->pin_mosi = ESY_XIO_PIN_UNUSED;
  cfg->pin_miso = ESY_XIO_PIN_UNUSED;
  cfg->pin_cs = ESY_XIO_PIN_UNUSED;
  cfg->pin_trigger = ESY_XIO_PIN_UNUSED;
  cfg->instance = 0xFFu;
  cfg->sm_index = 0xFFu;
  cfg->role = ESY_SPI_ROLE_MASTER;
  cfg->mode = ESY_SPI_MODE_0;
  cfg->is_msb_first = true;
  cfg->data_bits = 8u;
  cfg->speed_hz = 1000000u;
  cfg->cs_active_low = true;
}

esy_err_t esy_hal_xio_spi_init(esy_spi_t *spi,
                                    esy_xio_spi_config_t *cfg) {
  (void)spi;
  (void)cfg;

  return s_init_result;
}

void esy_hal_xio_spi_deinit(esy_spi_t *spi) {
  (void)spi;
}

__attribute__((constructor)) static void fake_esy_hal_xio_spi_ctor(void) {
  fake_esy_hal_xio_spi_reset();
}
