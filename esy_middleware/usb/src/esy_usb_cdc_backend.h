/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_USB_CDC_BACKEND_H
#define ESY_USB_CDC_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file esy_usb_cdc_backend.h
 * @brief Internal USB CDC backend interface used by the public CDC adapter.
 *
 * This is a module-private boundary. Production code outside
 * `esy_middleware/usb` must use the public `esy_usb_cdc.h` API instead.
 */

/**
 * @brief Return the number of CDC interfaces supported by the linked backend.
 *
 * The public adapter also caps the value by its fixed registry size.
 *
 * @return Backend CDC interface count.
 */
uint8_t esy_usb_cdc_backend_get_interface_count(void);

/**
 * @brief Run the linked USB device stack task pump.
 */
void esy_usb_cdc_backend_task(void);

/**
 * @brief Query whether a CDC interface is connected.
 *
 * @param[in] itf CDC interface index.
 * @retval true Interface is connected.
 * @retval false Interface is not connected.
 */
bool esy_usb_cdc_backend_connected(uint8_t itf);

/**
 * @brief Read backend line-state bits for a CDC interface.
 *
 * Bit 0 is DTR for the TinyUSB backend.
 *
 * @param[in] itf CDC interface index.
 * @return Backend line-state bit field.
 */
uint8_t esy_usb_cdc_backend_get_line_state(uint8_t itf);

/**
 * @brief Return bytes available in the backend RX FIFO.
 *
 * @param[in] itf CDC interface index.
 * @return Available byte count.
 */
uint32_t esy_usb_cdc_backend_available(uint8_t itf);

/**
 * @brief Read bytes from the backend RX FIFO.
 *
 * @param[in]  itf CDC interface index.
 * @param[out] buf Destination buffer.
 * @param[in]  len Destination buffer length in bytes.
 * @return Actual number of bytes read.
 */
uint32_t esy_usb_cdc_backend_read(uint8_t itf, void *buf, uint32_t len);

/**
 * @brief Return bytes currently available in the backend TX FIFO.
 *
 * @param[in] itf CDC interface index.
 * @return Writable byte count.
 */
uint32_t esy_usb_cdc_backend_write_available(uint8_t itf);

/**
 * @brief Write bytes into the backend TX FIFO.
 *
 * @param[in] itf CDC interface index.
 * @param[in] buf Source buffer.
 * @param[in] len Source length in bytes.
 * @return Actual number of bytes accepted by the backend.
 */
uint32_t esy_usb_cdc_backend_write(uint8_t itf, const void *buf, uint32_t len);

/**
 * @brief Flush the backend TX FIFO for a CDC interface.
 *
 * @param[in] itf CDC interface index.
 */
void esy_usb_cdc_backend_flush(uint8_t itf);

/** @brief Discard software RX/TX FIFO contents without cancelling endpoint I/O.
 * @param[in] itf CDC interface index. */
void esy_usb_cdc_backend_discard_buffers(uint8_t itf);

#endif /* ESY_USB_CDC_BACKEND_H */
