/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_adf4372.h
 * @brief ADF4372 microwave frequency synthesizer driver.
 *
 * The driver binds an ADF4372 to an @ref esy_freq_synth_t handle using
 * caller-owned SPI and GPIO resources. It uses no dynamic memory.
 */
#ifndef ESY_ADF4372_H
#define ESY_ADF4372_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_gpio_defs.h"
#include "esy_middleware/freq_synth/esy_freq_synth.h"
#include "esy_hal/esy_hal_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ADF4372 output channel. */
typedef enum {
  ESY_ADF4372_CHANNEL_RF8 = 0, /**< RF8x, 62.5 MHz to 8 GHz. */
  ESY_ADF4372_CHANNEL_AUX_RF8, /**< RFAUX8x, 62.5 MHz to 8 GHz. */
  ESY_ADF4372_CHANNEL_RF16,    /**< RF16x, 8 GHz to 16 GHz. */
} esy_adf4372_channel_t;

/** @brief ADF4372 VCC_VCO supply selection. */
typedef enum {
  ESY_ADF4372_VCO_SUPPLY_3V3 = 0, /**< VCC_VCO is powered from 3.3 V. */
  ESY_ADF4372_VCO_SUPPLY_5V,      /**< VCC_VCO is powered from 5 V. */
} esy_adf4372_vco_supply_t;

/**
 * @brief VCO calibration values captured after automatic calibration.
 *
 * The values are usable only for a nearby frequency in the same VCO core and
 * band. The caller must verify lock and calibration validity for the actual
 * reference, supply, temperature and destination frequency.
 */
typedef struct {
  uint64_t frequency_hz;         /**< Frequency used for capture. */
  esy_adf4372_channel_t channel; /**< Channel used for capture. */
  uint8_t vco_core;              /**< One-hot VCO core code: 1, 2, 4, or 8. */
  uint8_t vco_band;              /**< VCO band code. */
  uint8_t vco_bias;              /**< VCO bias code. */
} esy_adf4372_vco_calibration_t;

/**
 * @brief ADF4372 initialization configuration.
 *
 * `spi` and GPIO resources must already be initialized and remain caller-owned
 * until deinitialization. CS is required and active low. CE is optional.
 */
typedef struct {
  esy_spi_t *spi;        /**< Caller-owned initialized SPI handle. */
  uint32_t spi_speed_hz; /**< Best-effort speed request; zero preserves current
                            speed. The driver ignores the speed-setting result;
                            configure and validate a safe SPI bus speed
                            independently. Failure does not promise rollback. */
  esy_gpio_t *cs_gpio;   /**< Required active-low CS resource. */
  esy_gpio_t *ce_gpio;   /**< Optional CE resource, or NULL. */
  uint32_t
      cs_high_delay_us; /**< Optional CS high hold after each transaction. */
  uint32_t ref_clk_hz;  /**< Reference: 10 to 500 MHz single-ended, or 600 MHz differential. */
  uint8_t r_counter;    /**< R divider, 1 to 32; ref_clk_hz / R must not exceed 125 MHz. */
  esy_adf4372_vco_supply_t vco_supply; /**< Physical VCC_VCO supply. */
  uint64_t initial_freq_hz;            /**< Initial MAIN frequency in Hz. */
  esy_adf4372_channel_t main_channel;  /**< MAIN channel: RF8x or RF16x. */
  bool initial_output_enabled; /**< Enables MAIN after initialization. */
  bool auxiliary_uses_vco;     /**< Routes RFAUX8x directly to the VCO. */
  bool differential_ref_clk;   /**< Enables differential reference input. */
  bool mute_till_lock;         /**< Mutes outputs until lock. */
  bool spi_three_wire;         /**< Uses 3-wire SDIO mode. */
  uint8_t charge_pump_current; /**< CP_CURRENT field, 0 to 15. */
  uint16_t
      charge_pump_current_ua; /**< Charge-pump current for bleed calculation. */
  uint8_t muxout_mode;        /**< MUXOUT mode field, 0 to 15. */
  bool muxout_enabled;        /**< Enables MUXOUT; must be false for 4-wire SDO readback. */
  bool muxout_3v3;            /**< Selects 3.3 V MUXOUT level. */
  uint8_t rf8_power_level;    /**< RF8x power setting, 0 to 3. */
} esy_adf4372_config_t;

/**
 * @brief Fill a conservative default configuration.
 * @param[out] config Destination configuration. NULL is ignored.
 */
void esy_adf4372_default_config(esy_adf4372_config_t *config);

/**
 * @brief Initialize ADF4372 and bind it to a synthesizer handle.
 *
 * The first use requires `esy_freq_synth_t synth = {0}`. A live or faulted
 * handle returns ESY_ERR_STATE without modification. Invalid configuration
 * leaves the handle unchanged. I/O failure attempts cleanup; if cleanup also
 * fails, the handle remains attached and faulted until deinit succeeds.
 * These operations are task-only and require external serialization.

 * @param[in,out] synth Frequency synthesizer handle.
 * @param[in] config Immutable configuration.
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer or configuration.
 * @retval ESY_ERR_IO SPI transfer failure or invalid device revision readback.
 * @retval ESY_ERR_FAIL GPIO setup failure.
 */
esy_err_t esy_adf4372_init(esy_freq_synth_t *synth,
                           const esy_adf4372_config_t *config);

/**
 * @brief Disable outputs and detach only after successful cleanup.
 *
 * Task context only; externally serialize with every operation on this handle
 * and the shared SPI bus. Unregister the logical device first. Caller-owned
 * SPI/GPIO resources must remain valid until this function succeeds.
 * On failure, the handle remains attached and faulted; only unregister and
 * matching backend deinit may be retried. Do not clear or reuse its storage.
 * Success leaves a zeroed handle; repeated deinit of that handle succeeds.
 * @param[in,out] synth Zero-initialized or initialized backend handle.
 * @retval ESY_OK Cleanup complete, including an already empty handle.
 * @retval ESY_ERR_INVALID_ARG NULL handle.
 * @retval ESY_ERR_BUSY Logical device still registered or cleanup pending.
 * @retval ESY_ERR_STATE Handle belongs to a different backend.
 * @return SPI/GPIO error when cleanup cannot establish the safe state.
 */
esy_err_t esy_adf4372_deinit(esy_freq_synth_t *synth);

/**
 * @brief Read VCO calibration values after the device has locked.
 * @param[in] synth Initialized ADF4372 synthesizer handle.
 * Holds the logical device reference for the complete operation. The output
 * object is unchanged on failure. Failure to restore the FSM faults the handle.
 * @param[out] calibration Destination calibration object.
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid pointer.
 * @retval ESY_ERR_STATE Handle is not an ADF4372 backend.
 * @retval ESY_ERR_IO SPI transfer failure.
 */
esy_err_t esy_adf4372_read_vco_calibration(
    esy_freq_synth_t *synth, esy_adf4372_vco_calibration_t *calibration);

/**
 * @brief Set MAIN frequency while bypassing automatic VCO calibration.
 *
 * The supplied values must have been captured for the same MAIN channel and a
 * nearby frequency in the same VCO calibration region.
 *
 * @param[in,out] synth Initialized ADF4372 synthesizer handle.
 * @param[in] frequency_hz Requested MAIN frequency in Hz.
 * @param[in] calibration Captured calibration values.
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid frequency, calibration, or channel.
 * @retval ESY_ERR_STATE Handle is not an ADF4372 backend.
 * @retval ESY_ERR_IO SPI write failure.
 */
esy_err_t esy_adf4372_set_frequency_fast(
    esy_freq_synth_t *synth, uint64_t frequency_hz,
    const esy_adf4372_vco_calibration_t *calibration);

#ifdef __cplusplus
}
#endif
#endif /* ESY_ADF4372_H */
