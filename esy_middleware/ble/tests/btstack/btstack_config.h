/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_BLE_TEST_BTSTACK_CONFIG_H
#define ESY_BLE_TEST_BTSTACK_CONFIG_H

/* Real-header compile fixture; consumer runtime configuration is independent. */
#define ENABLE_BLE
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_SECURE_CONNECTIONS
#define HCI_ACL_PAYLOAD_SIZE 251
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_SERVICES 2
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_SM_LOOKUP_ENTRIES 4
#define NVM_NUM_DEVICE_DB_ENTRIES 4
/* Compile every native provider source with connection-local CCC state. */
#define NVN_NUM_GATT_SERVER_CCC 0

#endif
