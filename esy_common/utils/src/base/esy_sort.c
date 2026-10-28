/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/utils/base/esy_sort.h"

static void sort_u16_range(uint16_t arr[], int32_t low, int32_t high) {
  if ((arr == NULL) || (low < 0) || (high < 0) || (low >= high)) {
    return;
  }

  for (int32_t i = low + 1; i <= high; ++i) {
    uint16_t value = arr[i];
    int32_t j = i - 1;

    while ((j >= low) && (arr[j] > value)) {
      arr[j + 1] = arr[j];
      --j;
    }

    arr[j + 1] = value;
  }
}

void esy_sort_u16(uint16_t *arr, size_t count) {
  if ((arr == NULL) || (count < 2u)) {
    return;
  }

  sort_u16_range(arr, 0, (int32_t)(count - 1u));
}
