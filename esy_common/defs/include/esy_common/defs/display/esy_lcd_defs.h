/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_LCD_DEFS_H
#define ESY_LCD_DEFS_H

/**
 * @file esy_lcd_defs.h
 * @brief Shared pixel format and display orientation values.
 */

/**
 * @brief Supported pixel color formats.
 */
typedef enum {
  ESY_COLOR_FORMAT_RGB565, /**< RGB565, 2 bytes per pixel, most significant byte first. */
  ESY_COLOR_FORMAT_RGB666  /**< RGB666, 3 bytes per pixel. */
} esy_color_format_t;

/**
 * @brief LCD display orientation.
 */
typedef enum {
  ESY_LCD_DIR_L2R_U2D = 0, /**< Left-to-right, up-to-down (0 deg). */
  ESY_LCD_DIR_R2L_D2U,     /**< Right-to-left, down-to-up (180 deg). */
  ESY_LCD_DIR_U2D_R2L,     /**< Up-to-down, right-to-left (270 deg). */
  ESY_LCD_DIR_D2U_L2R      /**< Down-to-up, left-to-right (90 deg). */
} esy_lcd_dir_t;

#endif /* ESY_LCD_DEFS_H */
