/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LVGL_TOUCH_H
#define ESY_LVGL_TOUCH_H

#include "esy_middleware/lvgl/esy_lvgl.h"
#include "esy_middleware/touch/esy_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a pointer-input descriptor for one attached touch handle.
 *
 * Link esy_lvgl_touch. The descriptor is copied by esy_lvgl_create(); keep the
 * touch handle, provider and backing storage alive until destroy succeeds.
 * Reads use esy_touch_read_input() on the GUI task and preserve its device
 * reference and failure behavior. This function creates no resource and needs
 * no deinit. Serialize initialization with GUI lifecycle and touch access.
 * Outputs remain unchanged on error. Successful initialization may replace an
 * unused descriptor; never replace or detach the underlying live touch handle.
 *
 * @param[out] input Non-NULL caller-owned descriptor; no initial contents needed.
 * @param[in,out] touch Attached touch handle, borrowed by the resulting descriptor.
 * @retval ESY_OK Descriptor initialized.
 * @retval ESY_ERR_INVALID_ARG NULL argument.
 * @retval ESY_ERR_STATE Touch handle is not attached.
 */
esy_err_t esy_lvgl_touch_input_init(esy_lvgl_pointer_input_t *input,
                                    esy_touch_t *touch);

#ifdef __cplusplus
}
#endif
#endif /* ESY_LVGL_TOUCH_H */
