/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_ADC_DEFS_H
#define ESY_ADC_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_dma_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_adc_defs.h
 * @brief Common ADC type definitions.
 *
 * This file defines the platform-independent ADC abstraction, including:
 * - the unified ADC handle type,
 * - configuration structures,
 * - trigger-related enumerations,
 * - and the backend function table (vtable).
 *
 * These definitions are used to decouple upper layers from
 * platform-specific ADC implementations.
 */

/* -------------------------------------------------------------------------- */
/* Forward Declarations and Handle Types                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Forward declaration of the ADC handle structure.
 *
 * This declaration is shared with HAL backend implementations and
 * headers (e.g. esy_hb_adc.h), enabling the
 * handle to be referenced in callbacks and vtables.
 */
struct esy_adc_s;

/**
 * @brief ADC handle type.
 */
typedef struct esy_adc_s esy_adc_t;

/**
 * @brief Public capability bits returned by the initialized handle's get_caps.
 *
 * Bits describe implemented operations for the current handle/role, subject to
 * board resources, configuration limits and normal runtime errors. A set bit
 * neither reserves a resource nor guarantees every parameter combination.
 * DMA-named bits describe the corresponding API, not native DMA or asynchronous
 * completion. Unknown bits must be ignored; zero is a valid capability mask.
 */
typedef enum {
  ESY_ADC_CAP_DMA = 1u << 0, /**< Single-channel DMA transfer entry point. */
  ESY_ADC_CAP_SCAN = 1u << 1, /**< Multi-channel DMA scan entry point. */
  ESY_ADC_CAP_EXTERNAL_TRIGGER = 1u << 2, /**< External conversion trigger selection. */
  ESY_ADC_CAP_CIRCULAR_DMA = 1u << 3, /**< Continuous circular DMA entry point. */
} esy_adc_cap_t;

/* -------------------------------------------------------------------------- */
/* Enumerations                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief ADC sampling time configuration.
 *
 * Represents different ADC sampling cycle lengths.
 * The actual duration of each cycle is platform-dependent.
 */
typedef enum {
  ESY_ADC_SAMPLETIME_3CYCLES = 0, /**< 3 sampling cycles */
  ESY_ADC_SAMPLETIME_15CYCLES,    /**< 15 sampling cycles */
  ESY_ADC_SAMPLETIME_28CYCLES,    /**< 28 sampling cycles */
  ESY_ADC_SAMPLETIME_56CYCLES,    /**< 56 sampling cycles */
  ESY_ADC_SAMPLETIME_84CYCLES,    /**< 84 sampling cycles */
  ESY_ADC_SAMPLETIME_112CYCLES,   /**< 112 sampling cycles */
  ESY_ADC_SAMPLETIME_144CYCLES,   /**< 144 sampling cycles */
  ESY_ADC_SAMPLETIME_480CYCLES    /**< 480 sampling cycles */
} esy_adc_sample_time_t;

/**
 * @brief ADC external trigger edge selection.
 */
typedef enum {
  ESY_ADC_TRIGGER_EDGE_NONE = 0, /**< No external trigger */
  ESY_ADC_TRIGGER_EDGE_RISING,   /**< Rising edge trigger */
  ESY_ADC_TRIGGER_EDGE_FALLING,  /**< Falling edge trigger */
  ESY_ADC_TRIGGER_EDGE_BOTH      /**< Both edges trigger */
} esy_adc_trigger_edge_t;

/**
 * @brief ADC external trigger source.
 *
 * The exact meaning and mapping of each enumeration value
 * is platform-specific and defined by the backend implementation.
 */
typedef enum {
  ESY_ADC_TRIGGER_SRC_SOFTWARE = 0, /**< Software trigger */
  ESY_ADC_TRIGGER_SRC_TIM2_TRGO,    /**< TIM2 TRGO */
  ESY_ADC_TRIGGER_SRC_TIM3_TRGO,    /**< TIM3 TRGO */
  ESY_ADC_TRIGGER_SRC_TIM4_TRGO,    /**< TIM4 TRGO */
  ESY_ADC_TRIGGER_SRC_TIM8_TRGO,    /**< TIM8 TRGO */
  ESY_ADC_TRIGGER_SRC_EXTI11        /**< EXTI11 trigger */
  /* Extendable */
} esy_adc_trigger_source_t;

/* -------------------------------------------------------------------------- */
/* Configuration Structures                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Internal ADC controller configuration.
 *
 * This structure follows the same pattern as other peripherals
 * (e.g. I2C): the physical controller index is stored in the
 * top-level configuration, while this structure contains only
 * controller-internal parameters.
 */
typedef struct {
  uint8_t channel; /**< ADC channel index (platform-specific numbering) */
} esy_adc_inner_config_t;

/**
 * @brief Top-level ADC configuration.
 *
 * @note
 * - @ref phys_index selects the physical ADC controller instance.
 * - @ref inner.channel specifies the ADC channel number.
 */
typedef struct {
  uint8_t phys_index;                /**< Physical ADC controller index */
  esy_adc_sample_time_t sample_time; /**< Sampling time configuration */
  esy_adc_inner_config_t inner;      /**< Internal controller configuration */
} esy_adc_config_t;

/* -------------------------------------------------------------------------- */
/* Vtable Definition                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief ADC backend function table (vtable).
 *
 * All functions take @ref esy_adc_t* as the first argument.
 * Unsupported operations must return @ref ESY_ERR_NOT_SUPPORTED.
 */
typedef struct {
  /**
   * @brief Perform a single blocking ADC conversion.
   *
   * @param[in] adc ADC handle.
   * @return Conversion result on success, or a negative error code on failure.
   */
  int32_t (*sample_once)(esy_adc_t *adc);

  /**
   * @brief Start a single-channel DMA transfer.
   *
   * @param[in] adc     ADC handle.
   * @param[in] dma_cfg DMA configuration.
   * @return Operation result.
   */
  esy_err_t (*transfer_dma)(esy_adc_t *adc, esy_dma_config_t *dma_cfg);

  /**
   * @brief Query whether the DMA engine is busy.
   *
   * @param[in] adc ADC handle.
   * @retval true  DMA transfer in progress.
   * @retval false DMA idle.
   */
  bool (*is_dma_busy)(const esy_adc_t *adc);

  /**
   * @brief Start a multi-channel DMA transfer.
   *
   * @param[in] adc         ADC handle.
   * @param[in] dma_cfg     DMA configuration.
   * @param[in] index_list  List of channel indices.
   * @param[in] num_indexes Number of channels.
   * @return Operation result.
   */
  esy_err_t (*transfer_dma_multi_channel)(esy_adc_t *adc,
                                          esy_dma_config_t *dma_cfg,
                                          const uint8_t *index_list,
                                          uint8_t num_indexes);

  /**
   * @brief Start continuous DMA sampling.
   *
   * @param[in] adc     ADC handle.
   * @param[in] dma_cfg DMA configuration.
   * @return Operation result.
   */
  esy_err_t (*start_continuous_dma)(esy_adc_t *adc, esy_dma_config_t *dma_cfg);

  /**
   * @brief Retire ADC DMA from task context; failure retains borrowed storage.
   * @param[in] adc ADC handle.
   * @return ESY_OK after hardware/callback retirement, or a negative error.
   */
  esy_err_t (*abort_dma)(esy_adc_t *adc);

  /**
   * @brief Configure the ADC trigger source.
   *
   * @param[in] adc    ADC handle.
   * @param[in] edge   Trigger edge (see @ref esy_adc_trigger_edge_t).
   * @param[in] source Trigger source (see @ref esy_adc_trigger_source_t).
   * @return Operation result.
   */
  esy_err_t (*set_trigger_source)(esy_adc_t *adc, esy_adc_trigger_edge_t edge,
                                  esy_adc_trigger_source_t source);

  /**
   * @brief Get hardware capability flags.
   *
   * @param[in]  adc      ADC handle.
   * @param[out] out_caps Capability bitmask.
   * @return Operation result.
   */
  esy_err_t (*get_caps)(const esy_adc_t *adc, uint32_t *out_caps);

  /**
   * @brief Get physical controller index.
   *
   * @param[in]  adc       ADC handle.
   * @param[out] out_index Physical controller index.
   * @return Operation result.
   */
  esy_err_t (*get_phys_index)(const esy_adc_t *adc, uint8_t *out_index);
} esy_adc_vtable_t;

/* -------------------------------------------------------------------------- */
/* Handle Definition                                                          */
/* -------------------------------------------------------------------------- */

/** @brief Fixed caller-owned HAL runtime capacity; contents are private. */
#define ESY_ADC_HAL_STORAGE_BYTES \
  (sizeof(void *) + \
   ((sizeof(esy_adc_trigger_edge_t) + sizeof(esy_adc_trigger_source_t) + \
     sizeof(void *) - 1u) / sizeof(void *)) * sizeof(void *))

/**
 * @brief ADC handle.
 *
 * Initialize the entire handle with {0} before first use. The HAL binds a
 * shared immutable operations table; custom implementations may bind their own
 * immutable table with a lifetime covering the handle. Neither callers nor
 * backends may access or modify HAL-private storage. The backend owns @ref ctx.
 */
struct esy_adc_s {
  const esy_adc_vtable_t *ops; /**< Borrowed immutable operations table. */
  void *ctx;                 /**< Backend-private context. */
  union {
    void *alignment; /**< HAL runtime alignment. */
    uint8_t bytes[ESY_ADC_HAL_STORAGE_BYTES]; /**< HAL-private runtime storage. */
  } hal; /**< HAL-owned storage; preserve until successful deinit. */
};

#ifdef __cplusplus
}
#endif

#endif /* ESY_ADC_DEFS_H */
