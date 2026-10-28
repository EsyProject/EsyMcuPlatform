/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_PORTMACRO_H
#define TEST_PORTMACRO_H
#include "task.h"
typedef struct { unsigned lock; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define taskENTER_CRITICAL(mux) ((void)native_enter(0, mux))
#define taskEXIT_CRITICAL(mux) native_exit(0, mux, 0)
#define taskENTER_CRITICAL_ISR(mux) ((void)native_enter(1, mux))
#define taskEXIT_CRITICAL_ISR(mux) native_exit(1, mux, 0)
#endif
