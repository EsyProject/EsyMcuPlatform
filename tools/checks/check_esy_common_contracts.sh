#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ESY_COMMON_DIR="${ROOT_DIR}/esy_common"
INCLUDE_DIRS=(
  "${ESY_COMMON_DIR}/defs/include"
  "${ESY_COMMON_DIR}/utils/include"
  "${ESY_COMMON_DIR}/device/include"
)
if [[ -d "${ESY_COMMON_DIR}/ds/include" ]]; then
  INCLUDE_DIRS+=("${ESY_COMMON_DIR}/ds/include")
fi
if [[ -d "${ESY_COMMON_DIR}/protocol_codec/include" ]]; then
  INCLUDE_DIRS+=("${ESY_COMMON_DIR}/protocol_codec/include")
fi
if [[ -d "${ESY_COMMON_DIR}/smf/include" ]]; then
  INCLUDE_DIRS+=("${ESY_COMMON_DIR}/smf/include")
fi
require_tool() {
  local tool="$1"

  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "error: ${tool} is required for esy_common contract checks" >&2
    exit 127
  fi
}

require_tool cc
require_tool c++

echo "[esy_common] checking C public header self-containment"
while IFS= read -r -d '' header; do
  cc -std=c11 -Wall -Wextra -Werror \
    "${INCLUDE_DIRS[@]/#/-I}" \
    -fsyntax-only -x c "${header}"
done < <(find "${INCLUDE_DIRS[@]}" -name '*.h' -type f -print0 | sort -z)

echo "[esy_common] checking C++ public header compatibility"
while IFS= read -r -d '' header; do
  c++ -std=c++17 -Wall -Wextra -Werror \
    "${INCLUDE_DIRS[@]/#/-I}" \
    -fsyntax-only -x c++ "${header}"
done < <(find "${INCLUDE_DIRS[@]}" -name '*.h' -type f -print0 | sort -z)

echo "[esy_common] checking shared display/input/image/diagnostic values without runtime types"
contract_source="$(mktemp)"
trap 'rm -f "${contract_source}"' EXIT
cat > "${contract_source}" <<'EOF'
#include "esy_common/defs/display/esy_lcd_defs.h"
#include "esy_common/defs/display/esy_touch_defs.h"
#include "esy_common/defs/image/esy_image_defs.h"
#include "esy_common/defs/diagnostics/esy_coredump_defs.h"
#include "esy_common/defs/diagnostics/esy_diagnostics_defs.h"

#if defined(ESY_LCD_PRIV_BYTES) || defined(ESY_TOUCH_PRIV_BYTES)
#error "Middleware runtime storage must not be defined by common value headers"
#endif

/* These declarations conflict if common reintroduces middleware runtime names. */
typedef struct { int unused; } esy_lcd_t;
typedef struct { int unused; } esy_touch_t;
typedef struct { int unused; } esy_ota_t;
typedef struct { int unused; } esy_diagnostics_t;
typedef void (*esy_touch_spi_get_write_buffer)(void);

#if defined(__cplusplus)
#define CONTRACT_ASSERT static_assert
#else
#define CONTRACT_ASSERT _Static_assert
#endif
CONTRACT_ASSERT(ESY_COLOR_FORMAT_RGB565 == 0, "RGB565 value changed");
CONTRACT_ASSERT(ESY_COLOR_FORMAT_RGB666 == 1, "RGB666 value changed");
CONTRACT_ASSERT(ESY_LCD_DIR_L2R_U2D == 0, "Default orientation value changed");
CONTRACT_ASSERT(ESY_LCD_DIR_R2L_D2U == 1, "180-degree orientation value changed");
CONTRACT_ASSERT(ESY_LCD_DIR_U2D_R2L == 2, "270-degree orientation value changed");
CONTRACT_ASSERT(ESY_LCD_DIR_D2U_L2R == 3, "90-degree orientation value changed");

CONTRACT_ASSERT(ESY_IMAGE_SHA256_BYTES == 32u, "Image digest size changed");
CONTRACT_ASSERT(ESY_IMAGE_SIGNATURE_BYTES == 64u, "Image signature size changed");
CONTRACT_ASSERT(ESY_IMAGE_IDLE == 0, "Image idle value changed");
CONTRACT_ASSERT(ESY_IMAGE_RECEIVING == 1, "Image receiving value changed");
CONTRACT_ASSERT(ESY_IMAGE_READY == 2, "Image ready value changed");
CONTRACT_ASSERT(ESY_IMAGE_ACTIVATED == 3, "Image activated value changed");
CONTRACT_ASSERT(ESY_IMAGE_ABORTED == 4, "Image aborted value changed");
CONTRACT_ASSERT(ESY_IMAGE_FAILED == 5, "Image failed value changed");
CONTRACT_ASSERT(ESY_COREDUMP_RECORD_ID_BYTES == 16u, "Coredump identity size changed");
CONTRACT_ASSERT(sizeof(esy_coredump_record_id_t) == 16u, "Coredump identity layout changed");
CONTRACT_ASSERT(ESY_DIAGNOSTICS_NAME_MAX_BYTES == 48u, "Diagnostic name size changed");
CONTRACT_ASSERT(ESY_DIAGNOSTICS_DETAIL_MAX_BYTES == 96u, "Diagnostic detail size changed");
CONTRACT_ASSERT(ESY_DIAGNOSTICS_HEALTH_UNKNOWN == 0, "Unknown health value changed");
CONTRACT_ASSERT(ESY_DIAGNOSTICS_HEALTH_OK == 1, "Healthy value changed");
CONTRACT_ASSERT(ESY_DIAGNOSTICS_HEALTH_WARNING == 2, "Warning health value changed");
CONTRACT_ASSERT(ESY_DIAGNOSTICS_HEALTH_ERROR == 3, "Error health value changed");
CONTRACT_ASSERT(ESY_DIAGNOSTICS_CAP_READ == 1u, "Diagnostic read bit changed");
CONTRACT_ASSERT(ESY_DIAGNOSTICS_CAP_RUN == 2u, "Diagnostic run bit changed");

int main(void) {
  esy_touch_data_t sample = {false, 12u, 34u};
  return sample.is_pressed || sample.x != 12u || sample.y != 34u;
}
EOF
cc -std=c11 -Wall -Wextra -Werror "${INCLUDE_DIRS[@]/#/-I}" \
  -fsyntax-only -x c "${contract_source}"
c++ -std=c++17 -Wall -Wextra -Werror "${INCLUDE_DIRS[@]/#/-I}" \
  -fsyntax-only -x c++ "${contract_source}"

echo "[esy_common] contract checks passed"
