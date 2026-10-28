/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_hb_spi.h
 * @brief SPI HAL backend interface (handle-based, no internal allocation).
 *
 * Design rules:
 * - The caller provides handle storage; the HAL backend must not allocate from heap.
 * - Physical controller index is provided via @ref esy_spi_config_t.
 * - Unsupported capabilities must return @ref ESY_ERR_NOT_SUPPORTED (or equivalent).
 */

#ifndef ESY_HB_SPI_H
#define ESY_HB_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_spi_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name HAL backend top-level APIs (handle-based, explicit extern) */
/** @{ */

/**
 * @brief Validate a configuration and reserve its physical controller.
 *
 * Task-only, serialized per handle. Call before board configuration. Atomic
 * reservation excludes competing handles across cores; it does not serialize
 * operations on one handle. Handle storage must initially be zero and must not
 * be copied while claimed. Reclaim of the same retired handle/index is allowed
 * for PM resume. Active or foreign handles return STATE without side effects.
 * Native SDK users must not claim, reconfigure or free this controller outside
 * this lifecycle. ESP32 detects an already-created master driver context, but
 * the SDK offers no complete public query for a bus without master devices or
 * pre-existing slave drivers; excluding these owners is a caller obligation. SPI1/flash is unsupported there. STM32F4 accepts only 8 or
 * 16 data bits; other nonzero widths through 16 return NOT_SUPPORTED.
 *
 * @param[in,out] handle Zero-state or already-reserved retired handle.
 * @param[in] config Configuration to validate before any board/native effects.
 * @return OK on reservation; BUSY if another handle owns it; INVALID_ARG for
 * invalid configuration; NOT_SUPPORTED for unsupported frame width/controller;
 * STATE for an active, copied or foreign handle. Failure changes no pins.
 */
esy_err_t esy_hb_spi_claim(esy_spi_t *handle, const esy_spi_config_t *config);

/**
 * @brief Initialize native hardware while keeping its physical reservation.
 * @param[in,out] handle Claimed handle; an unclaimed zero handle is claimed first.
 * @param[in] config Validated configuration, completed by the board hook.
 * @return OK on success; a negative status otherwise. Native partial state and
 * its reservation remain owned until deinit succeeds, followed by release.
 * @note Task-only; serialize with all handle operations. Allocation performed
 * internally by ESP-IDF SPI driver initialization is SDK-owned and retired here.
 * Pico slave mode permits speed_hz=0: a valid SDK divider is derived from the
 * peripheral clock, while the external master determines the actual SCK.
 */
extern esy_err_t esy_hb_spi_init(esy_spi_t *handle,
                               const esy_spi_config_t *config);

/**
 * @brief Retire native hardware and callbacks, retaining context and reservation.
 * @param[in,out] handle Claimed handle, or a zero handle for an OK no-op.
 * @return OK when native state is retired; BUSY while DMA/native work remains;
 * STATE for copied/foreign handles; a negative SDK cleanup status on failure.
 * @note Task-only and serialized. Failure preserves resources and storage for
 * poll/abort or deinit retry. Success permits board pin reset and then release,
 * or native init with the same reservation during PM resume.
 */
extern esy_err_t esy_hb_spi_deinit(esy_spi_t *handle);

/**
 * @brief Release a reservation after native retirement and board pin reset.
 * @param[in,out] handle Retired handle; zero handles return OK.
 * @return OK on release; BUSY if native resources remain; STATE for a copied or
 * foreign handle; INVALID_ARG for NULL.
 * @note Task-only, serialized. This call must follow board teardown so a new
 * owner cannot be routed before the previous owner's final pin reset.
 */
esy_err_t esy_hb_spi_release(esy_spi_t *handle);

/**
 * @brief Blocking write (8-bit).
 *
 * @param[in] handle SPI handle
 * @param[in] tx_buf Transmit buffer
 * @param[in] len    Number of bytes to send
 *
 * @return >=0 number of bytes written; <0 negative error code
 */
extern int32_t esy_hb_spi_write_blocking(esy_spi_t *handle,
                                             const uint8_t *tx_buf,
                                             size_t len);

/**
 * @brief Blocking read (8-bit).
 *
 * Implementations typically generate clock by transmitting dummy bytes.
 *
 * @param[in]  handle SPI handle
 * @param[out] rx_buf Receive buffer
 * @param[in]  len    Number of bytes to read
 *
 * @return >=0 number of bytes read; <0 negative error code
 */
extern int32_t esy_hb_spi_read_blocking(esy_spi_t *handle,
                                            uint8_t *rx_buf,
                                            size_t len);

/**
 * @brief Blocking full-duplex transfer (8-bit).
 *
 * Depending on the backend, passing NULL for one direction may be supported as
 * half-duplex (send-only or receive-only).
 *
 * @param[in]  handle SPI handle
 * @param[in]  tx_buf Transmit buffer (may be NULL if half-duplex is supported)
 * @param[out] rx_buf Receive buffer   (may be NULL if half-duplex is supported)
 * @param[in]  len    Transfer length in bytes
 *
 * @return >=0 number of bytes transferred; <0 negative error code
 */
extern int32_t esy_hb_spi_transfer_blocking(esy_spi_t *handle,
                                                const uint8_t *tx_buf,
                                                uint8_t *rx_buf,
                                                size_t len);

/**
 * @brief Blocking write (16-bit).
 *
 * @param[in] handle SPI handle
 * @param[in] tx_buf Transmit buffer (16-bit words)
 * @param[in] len    Number of half-words to send
 *
 * @return >=0 number of half-words written; <0 negative error code
 */
extern int32_t esy_hb_spi_write16_blocking(esy_spi_t *handle,
                                               const uint16_t *tx_buf,
                                               size_t len);

/**
 * @brief Blocking read (16-bit).
 *
 * Implementations typically generate clock by transmitting dummy half-words.
 *
 * @param[in]  handle SPI handle
 * @param[out] rx_buf Receive buffer (16-bit words)
 * @param[in]  len    Number of half-words to read
 *
 * @return >=0 number of half-words read; <0 negative error code
 */
extern int32_t esy_hb_spi_read16_blocking(esy_spi_t *handle,
                                              uint16_t *rx_buf,
                                              size_t len);

/**
 * @brief Blocking full-duplex transfer (16-bit).
 *
 * Depending on the backend, passing NULL for one direction may be supported as
 * half-duplex (send-only or receive-only).
 *
 * @param[in]  handle SPI handle
 * @param[in]  tx_buf Transmit buffer (may be NULL if half-duplex is supported)
 * @param[out] rx_buf Receive buffer  (may be NULL if half-duplex is supported)
 * @param[in]  len    Transfer length in half-words
 *
 * @return >=0 number of half-words transferred; <0 negative error code
 */
extern int32_t esy_hb_spi_transfer16_blocking(esy_spi_t *handle,
                                                  const uint16_t *tx_buf,
                                                  uint16_t *rx_buf,
                                                  size_t len);

/**
 * @brief Submit an SPI transfer through the backend's DMA-named API.
 *
 * @param[in] handle        SPI handle
 * @param[in] tx_dma_config TX DMA config (may be NULL if backend supports RX-only)
 * @param[in] rx_dma_config RX DMA config (may be NULL if backend supports TX-only)
 * @param[in] complete_cb Terminal transfer-result callback (may be NULL)
 * @param[in] user_data Opaque callback context
 *
 * @retval ESY_OK Success (transfer started)
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 *
 * @note HAL validates caller-provided memory endpoints before board completion.
 *       Its single-direction placeholder has a NULL memory endpoint, with the
 *       same nonzero byte length and element width as the present direction.
 *       Pico/STM32 route the placeholder to a fixed dummy/discard word; ESP32
 *       accepts the placeholder or an absent configuration and uses the SDK's
 *       missing-buffer mode. ESP32 requires equal direction lengths, 8-bit
 *       elements for frames up to 8 bits or 16-bit otherwise, complete aligned
 *       elements, noncircular mode and at most 4094 bytes. RX up to four bytes
 *       uses the SDK's inline word; longer RX requires a multiple of four bytes
 *       (at most 4092), with other lengths returning NOT_SUPPORTED. These RX
 *       bounds also apply to ESP32 blocking APIs. Invalid descriptors reject
 *       before CS/SDK side effects. ESP32 completes inline and reports
 *       zero bytes for an absent memory endpoint; no callback follows rejection.
 *       A successful terminal result requires both present directions to have
 *       consumed their requested transfer counts.
 */
extern esy_err_t esy_hb_spi_transfer_dma(esy_spi_t *handle,
                                             esy_dma_config_t *tx_dma_config,
                                             esy_dma_config_t *rx_dma_config,
                                             esy_spi_dma_complete_cb_t complete_cb,
                                             void *user_data);

/**
 * @brief Query current SPI DMA transfer remaining counts.
 *
 * Remaining counts are reported in DMA transfer units (not bytes). For
 * single-direction DMA, the unused direction output may be NULL.
 *
 * @param[in]  handle             SPI handle
 * @param[out] out_tx_remaining   Remaining TX transfer count (units), may be NULL
 * @param[out] out_rx_remaining   Remaining RX transfer count (units), may be NULL
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_spi_get_dma_trans_count_remaining(const esy_spi_t *handle,
                                                              size_t *out_tx_remaining,
                                                              size_t *out_rx_remaining);

/**
 * @brief Set SPI clock rate at runtime (optional).
 *
 * @param[in] handle   SPI handle
 * @param[in] speed_hz Target clock rate in Hz
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_spi_set_speed(esy_spi_t *handle, uint32_t speed_hz);

/**
 * @brief Abort the current transaction from the serialized owner task.
 *
 * STM32F4 success retires owned DMA streams, callbacks and wire activity before
 * releasing CS; cancellation emits no terminal callback. Failure retains the
 * transaction and borrowed buffers for poll/abort retry. Other ports may return
 * NOT_SUPPORTED. Never call this operation from a DMA callback.
 *
 * @param[in] handle SPI handle
 *
 * @retval ESY_OK Success
 * @retval <0     Error code
 */
extern esy_err_t esy_hb_spi_abort(esy_spi_t *handle);

/**
 * @brief Query public SPI capabilities for an initialized handle.
 *
 * Uses @ref esy_spi_cap_t, with identical bit meanings on every port.
 * Serialize with handle lifecycle. Board resources and configuration still
 * constrain each operation; DMA bits do not promise asynchronous completion.
 * Zero is valid. Ignore unknown bits; output is written only on success.
 *
 * @param[in]  handle   SPI handle
 * @param[out] out_caps Public capability bitmask
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_spi_get_caps(const esy_spi_t *handle, uint32_t *out_caps);

/**
 * @brief Query physical controller index (optional).
 *
 * @param[in]  handle    SPI handle
 * @param[out] out_index Physical controller index
 *
 * @retval ESY_OK Success
 * @retval <0     Error code (returns @ref ESY_ERR_NOT_SUPPORTED if unsupported)
 */
extern esy_err_t esy_hb_spi_get_phys_index(const esy_spi_t *handle, uint8_t *out_index);

/**
 * @brief SPI IRQ dispatch hook (optional).
 *
 * If a backend requires IRQ forwarding, the platform ISR may call this entry to
 * dispatch the interrupt into the HAL backend.
 *
 * @param[in] handle SPI handle
 *
 * @note If not needed, the platform may omit calling this function, and the
 *       backend may choose not to implement it.
 */
extern void esy_hb_spi_handle_irq(esy_spi_t *handle);
/**
 * @brief Advance deferred DMA retirement from the serialized owner task.
 * @param[in,out] handle Initialized SPI handle, borrowed through callback return.
 * @return ESY_OK when progress succeeds (possibly still active), INVALID_ARG for
 * an invalid handle, or BUSY while cleanup retains the transfer and buffers.
 * Never call from ISR or concurrently with I/O/lifecycle. Pico/STM32 require
 * periodic calls after DMA acceptance; terminal callbacks execute here.
 */
extern esy_err_t esy_hb_spi_poll(esy_spi_t *handle);
/**
 * @brief Query pending completion, both DMA directions and wire from the owner.
 * @param[in] handle Initialized SPI handle; NULL/uninitialized returns false.
 * @return true while the transfer still owns storage or hardware; false if idle.
 * Serialize against I/O, poll and lifecycle. This call does not advance cleanup.
 */
extern bool esy_hb_spi_is_busy(esy_spi_t *handle);

/** @} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ESY_HB_SPI_H */
