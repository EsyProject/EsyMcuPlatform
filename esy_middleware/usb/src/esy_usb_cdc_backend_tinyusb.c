/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_usb_cdc_backend.h"

#include <tusb.h>

uint8_t esy_usb_cdc_backend_get_interface_count(void) {
  return (uint8_t)CFG_TUD_CDC;
}

void esy_usb_cdc_backend_task(void) { tud_task(); }

bool esy_usb_cdc_backend_connected(uint8_t itf) {
  return tud_cdc_n_connected(itf);
}

uint8_t esy_usb_cdc_backend_get_line_state(uint8_t itf) {
  return tud_cdc_n_get_line_state(itf);
}

uint32_t esy_usb_cdc_backend_available(uint8_t itf) {
  return tud_cdc_n_available(itf);
}

uint32_t esy_usb_cdc_backend_read(uint8_t itf, void *buf, uint32_t len) {
  return tud_cdc_n_read(itf, buf, len);
}

uint32_t esy_usb_cdc_backend_write_available(uint8_t itf) {
  return tud_cdc_n_write_available(itf);
}

uint32_t esy_usb_cdc_backend_write(uint8_t itf, const void *buf, uint32_t len) {
  return tud_cdc_n_write(itf, buf, len);
}

void esy_usb_cdc_backend_flush(uint8_t itf) {
  tud_cdc_n_write_flush(itf);
}

void esy_usb_cdc_backend_discard_buffers(uint8_t itf) {
  tud_cdc_n_read_flush(itf);
  (void)tud_cdc_n_write_clear(itf);
}
