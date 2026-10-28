/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_CAN_H
#define ESY_HB_CAN_H

#include "esy_common/defs/peripheral/esy_can_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_hb_can.h
 * @brief Native CAN provider boundary; called by HAL, never middleware.
 * All functions obey esy_hal_can.h lifecycle/data semantics and require one
 * serialized non-ISR owner across all CAN handles. The STM32F4 provider reserves
 * the entire CAN1/CAN2 clock/filter domain, allowing one selected controller at
 * a time. The owner configures pins, clock input and transceiver before init.
 * No native CAN driver, interrupt, DMA, OS or dynamic allocation is required.
 */

/** @brief Initialize stopped with filters disabled; retains ctx on cleanup failure.
 * @param can Zero caller handle. @param config Validated complete configuration.
 * @return OK, BUSY, INVALID_ARG, NOT_SUPPORTED, STATE or TIMEOUT. */
esy_err_t esy_hb_can_init(esy_can_t *can, const esy_can_config_t *config);
/** @brief Stop and release resources; failure preserves ctx for retry.
 * @param can Claimed handle or zero handle. @return OK, STATE, INVALID_ARG or TIMEOUT. */
esy_err_t esy_hb_can_deinit(esy_can_t *can);
/** @brief Start stopped hardware within configured observation budget.
 * @param can Claimed stopped handle. @return OK, STATE, INVALID_ARG or TIMEOUT. */
esy_err_t esy_hb_can_start(esy_can_t *can);
/** @brief Abort TX and enter initialization mode before reporting stopped.
 * @param can Claimed handle. @return OK, STATE, INVALID_ARG or TIMEOUT. */
esy_err_t esy_hb_can_stop(esy_can_t *can);
/** @brief Copy one valid frame to a mailbox, with no bus wait.
 * @param can Running handle. @param frame Valid frame. @return OK, BUSY, STATE, INVALID_ARG or NOT_SUPPORTED. */
esy_err_t esy_hb_can_send(esy_can_t *can, const esy_can_frame_t *frame);
/** @brief Copy/release one FIFO entry; leave output untouched on failure.
 * Classical wire DLC 9..15 is normalized to eight payload/request bytes.
 * @param can Claimed handle. @param frame Output value. @return OK, BUSY, STATE or INVALID_ARG. */
esy_err_t esy_hb_can_receive(esy_can_t *can, esy_can_frame_t *frame);
/** @brief Replace all mask filters while stopped; zero rules reject all.
 * @param can Claimed stopped handle. @param filters Rules or NULL for zero count.
 * @param count At most 28. @return OK, STATE or INVALID_ARG. */
esy_err_t esy_hb_can_set_filters(esy_can_t *can,
                                const esy_can_filter_t *filters, size_t count);
/** @brief Sample native state, sticky overruns, error counters and FIFO depth.
 * @param can Claimed handle. @param status Output snapshot. @return OK, STATE or INVALID_ARG. */
esy_err_t esy_hb_can_get_status(esy_can_t *can, esy_can_status_t *status);
/** @brief Begin manual bus-off recovery; do not restart a pending request.
 * @param can Bus-off handle. @return OK, BUSY, STATE, INVALID_ARG or TIMEOUT. */
esy_err_t esy_hb_can_recover(esy_can_t *can);
/** @brief Query the initialized controller's public capabilities.
 * @param can Claimed handle. @param caps Output mask. @return OK, STATE or INVALID_ARG. */
esy_err_t esy_hb_can_get_caps(const esy_can_t *can, uint32_t *caps);

#ifdef __cplusplus
}
#endif
#endif /* ESY_HB_CAN_H */
