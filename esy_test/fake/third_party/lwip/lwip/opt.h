/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_TEST_FAKE_LWIP_OPT_H
#define ESY_TEST_FAKE_LWIP_OPT_H

/** @brief Reusable raw TCP fixture defaults to numeric addresses only.
 * DNS-specific module fixtures may override this header or define LWIP_DNS. */
#ifndef LWIP_DNS
#define LWIP_DNS 0
#endif

#endif /* ESY_TEST_FAKE_LWIP_OPT_H */
