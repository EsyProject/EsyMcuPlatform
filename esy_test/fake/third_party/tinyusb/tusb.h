/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_TINYUSB_TUSB_H
#define FAKE_TINYUSB_TUSB_H

#include <stdbool.h>
#include <stdint.h>

#ifndef CFG_TUD_CDC
#define CFG_TUD_CDC 2u
#endif

#define FAKE_TINYUSB_CDC_MAX_IFACES 4u
#define FAKE_TINYUSB_FIFO_BYTES     256u

typedef struct {
  bool connected;
  uint8_t line_state;
  uint32_t available;
  uint32_t read_count;
  uint32_t write_count;
  uint32_t flush_count;
  uint32_t read_flush_count;
  uint32_t write_clear_count;
  uint32_t task_count;
  uint32_t write_available;
  uint32_t read_limit; /**< Maximum actual read; reset selects UINT32_MAX. */
  uint32_t write_limit; /**< Maximum actual write; reset selects UINT32_MAX. */
  bool read_overreport; /**< Report more than requested without copying bytes. */
  bool write_overreport; /**< Report more than requested without copying bytes. */
  uint8_t last_read_itf;
  uint8_t last_write_itf;
  uint8_t last_flush_itf;
  uint8_t read_data[FAKE_TINYUSB_FIFO_BYTES];
  uint8_t written_data[FAKE_TINYUSB_FIFO_BYTES];
} fake_tinyusb_cdc_itf_state_t;

void fake_tinyusb_reset(void);
fake_tinyusb_cdc_itf_state_t *fake_tinyusb_cdc_get_itf(uint8_t itf);
void fake_tinyusb_cdc_set_rx_data(uint8_t itf, const uint8_t *data, uint32_t len);

void tud_task(void);
bool tud_cdc_n_connected(uint8_t itf);
uint8_t tud_cdc_n_get_line_state(uint8_t itf);
uint32_t tud_cdc_n_available(uint8_t itf);
uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t bufsize);
uint32_t tud_cdc_n_write_available(uint8_t itf);
uint32_t tud_cdc_n_write(uint8_t itf, const void *buffer, uint32_t bufsize);
void tud_cdc_n_write_flush(uint8_t itf);
void tud_cdc_n_read_flush(uint8_t itf);
bool tud_cdc_n_write_clear(uint8_t itf);

void tud_cdc_rx_cb(uint8_t itf);
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts);

#endif /* FAKE_TINYUSB_TUSB_H */
