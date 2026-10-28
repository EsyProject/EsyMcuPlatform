/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HAL_BOOT_H
#define ESY_HAL_BOOT_H

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/image/esy_boot_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Query the board-owned image and metadata regions through HAL.
 *
 * Task or single-threaded startup context only. Externally serialize this query
 * with board configuration changes. No resources are retained or writes made.
 * Managed ports also query SDK state and check native partition ownership.
 * The bootloader validates the layout against its image format and geometry.
 *
 * @param[out] layout Caller-owned output; unchanged on failure.
 * @retval ESY_OK Layout copied successfully.
 * @retval ESY_ERR_INVALID_ARG NULL output.
 * @retval ESY_ERR_NOT_SUPPORTED The board provides no boot layout.
 */
esy_err_t esy_hal_boot_get_layout(esy_bootloader_layout_t *layout);

/**
 * @brief Query boot ownership and the running image in HAL Flash coordinates.
 *
 * Direct ports return an execution address inside the image; managed ports
 * return its native partition base. This is separate from the diagnostic PC.
 * update_allowed is false until a managed trial image has been confirmed.
 * Task/startup context only; serialize all boot, layout and Flash operations.
 * The SDK must be ready. No resources are retained; output is unchanged on error.
 *
 * @param[out] runtime Caller-owned output.
 * @retval ESY_OK State copied.
 * @retval ESY_ERR_INVALID_ARG NULL output.
 * @retval ESY_ERR_NOT_SUPPORTED Unsupported SDK boot/encryption configuration.
 * @retval <0 Provider query failure.
 */
esy_err_t esy_hal_boot_get_runtime(esy_boot_runtime_t *runtime);

/**
 * @brief Commit a verified inactive image for the next managed boot, without reset.
 *
 * Call only after complete image verification and durable metadata publication.
 * The image must start at a native OTA partition base and fit its capacity.
 * The provider checks native image validity and refuses updates while a trial
 * image needs rollback protection. Failure may follow a durable SDK write;
 * this call is not atomic with the caller's metadata. Never call on ordinary
 * reset or startup, since doing so can rearm trial-boot state.
 * Task context only, externally serialized with Flash and boot operations.
 *
 * @param[in] address Image address in HAL Flash coordinates.
 * @param[in] image_size Actual image length, greater than zero.
 * @retval ESY_OK Native next-boot selection committed.
 * @retval ESY_ERR_INVALID_ARG Invalid image range.
 * @retval ESY_ERR_STATE Active trial or running target prevents selection.
 * @retval ESY_ERR_NOT_SUPPORTED Direct-boot port or unsupported SDK configuration.
 * @retval <0 Native validation or persistent-write failure.
 */
esy_err_t esy_hal_boot_set_pending_image(uint32_t address, uint32_t image_size);

/**
 * @brief Confirm the running managed image after caller-owned health checks.
 *
 * Idempotent for an already valid running image. Refuses confirmation when a
 * different image has already been selected for next boot. Native confirmation
 * may update durable rollback/security state and cannot be atomically combined
 * with Esy metadata. Task context only; externally serialize all boot/Flash I/O.
 *
 * @retval ESY_OK Native image confirmed, or already valid.
 * @retval ESY_ERR_STATE A different image is selected or native state is invalid.
 * @retval ESY_ERR_NOT_SUPPORTED Direct-boot port or unsupported SDK configuration.
 * @retval <0 Native confirmation failure.
 */
esy_err_t esy_hal_boot_confirm_running_image(void);

/**
 * @brief Transfer control to an application image through the hardware backend.
 *
 * Startup context only. The caller must stop asynchronous users and establish
 * the backend's IRQ, multicore and peripheral handoff preconditions. Successful
 * handoff does not return; any return means the backend could not transfer.
 *
 * Managed ESP32 handoff selects an OTA partition and restarts through ESP-IDF;
 * an already selected target restarts without rewriting trial state. Direct
 * ports enter the image using their documented CPU handoff sequence.
 * STM32F4 requires privileged Thread mode using MSP and a caller-validated,
 * correctly aligned vector table with a valid initial MSP and Thumb reset entry.
 * Incompatible CPU context returns without changing state. It installs VTOR and
 * transfers through a stack-free native entry; interrupts remain disabled until
 * application startup restores its policy. This is not a CPU/peripheral reset,
 * and the application's reset entry must not return.
 *
 * @param[in] app_address Application image/vector base in HAL Flash coordinates.
 */
void esy_hal_boot_jump_to_app(uint32_t app_address);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HAL_BOOT_H */
