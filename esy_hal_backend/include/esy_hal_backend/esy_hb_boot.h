/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_HB_BOOT_H
#define ESY_HB_BOOT_H

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/image/esy_boot_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Query native/direct ownership and the running Flash image address.
 * @param[out] runtime Caller-owned output; unchanged on failure.
 * @return ESY_OK, INVALID_ARG, NOT_SUPPORTED or a provider error.
 * @note Task/startup context; SDK ready, all boot and Flash operations serialized.
 */
esy_err_t esy_hb_boot_get_runtime(esy_boot_runtime_t *runtime);

/**
 * @brief Validate board regions against a managed provider's partition table.
 * @param[in] layout Borrowed board layout, retained only during the call.
 * @return ESY_OK, INVALID_ARG, NOT_SUPPORTED on direct ports, or provider failure.
 * @note No writes; task/startup context, serialized with layout and boot changes.
 */
esy_err_t esy_hb_boot_validate_layout(const esy_bootloader_layout_t *layout);

/**
 * @brief Validate and durably select an inactive native image without reset.
 * @param[in] address Image base in HAL Flash coordinates.
 * @param[in] image_size Actual image length, greater than zero.
 * @return ESY_OK, INVALID_ARG, STATE, NOT_SUPPORTED or native I/O/validation error.
 * @note Task context, image and metadata already verified/published, externally
 * serialized. Failure can follow persistent writes. Direct ports are unsupported.
 */
esy_err_t esy_hb_boot_set_pending_image(uint32_t address, uint32_t image_size);

/**
 * @brief Confirm only the currently running managed image after health checks.
 * @return ESY_OK, STATE, NOT_SUPPORTED or native persistent-write failure.
 * @note Task context, externally serialized. Already valid is idempotent; a
 * different next-boot selection fails. Direct ports return NOT_SUPPORTED.
 */
esy_err_t esy_hb_boot_confirm_running_image(void);

/**
 * @brief Jump to an application entry point.
 *
 * This function typically:
 * - Sets the stack pointer
 * - Updates vector-table, memory-map, or boot target state as required
 * - Jumps to the application's reset handler
 *
 * @param[in] app_address Application image/vector base address.
 *
 * @warning Before calling this function, interrupts should be disabled,
 *          unnecessary peripherals stopped, and caches/memory remapping
 * handled.
 * @note STM32F4 requires privileged Thread mode using MSP, with all asynchronous
 * users stopped. An ISR, unprivileged caller or PSP-selected caller returns
 * without changing state. The caller supplies a readable, correctly aligned
 * vector table containing a valid eight-byte-aligned initial MSP in writable
 * application RAM and a Thumb Reset_Handler address. Image/vector validation
 * belongs to the caller. The port disables IRQs, suspends the HAL tick, clears
 * external IRQ enables/pending bits and installs VTOR before a stack-free
 * assembly handoff loads MSP and enters Reset_Handler. IRQs stay disabled;
 * application startup must restore its interrupt policy after initialization.
 * CPU/peripheral state is not reset. Reset_Handler must not return; an unexpected
 * return traps without returning to the abandoned caller stack.
 * @note Direct ports use a CPU handoff sequence. ESP32 accepts a native OTA
 * partition's physical Flash base and restarts via ESP-IDF; an already selected
 * target preserves its trial state. SDK startup and serialized boot/Flash access
 * are required. A return indicates that no handoff occurred.
 */
extern void esy_hb_boot_jump_to_app(uint32_t app_address);

#ifdef __cplusplus
}
#endif

#endif /* ESY_HB_BOOT_H */
