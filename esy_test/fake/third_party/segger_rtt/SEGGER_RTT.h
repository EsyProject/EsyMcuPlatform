/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file SEGGER_RTT.h
 * @brief Test fake surface for the SEGGER RTT API used by logger tests.
 */

#ifndef SEGGER_RTT_H
#define SEGGER_RTT_H

#ifdef __cplusplus
extern "C" {
#endif

unsigned SEGGER_RTT_Write(unsigned buffer_index, const void *buffer,
                          unsigned num_bytes);

#ifdef __cplusplus
}
#endif

#endif /* SEGGER_RTT_H */
