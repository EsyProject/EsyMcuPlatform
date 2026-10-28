# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

set(MODULE_SRCS "")

# ---- Module source list -----------------------------------------------------
list(APPEND MODULE_SRCS esy_linalg.c)
list(APPEND MODULE_SRCS esy_fit.c)
list(APPEND MODULE_SRCS esy_poly_fit.c)

if(ENABLE_ESY_NUMERICS_QUANTIZE)
    list(APPEND MODULE_SRCS esy_numerics_quantize.c)
endif()

target_sources(esy_numerics
    PRIVATE
        ${MODULE_SRCS}
)
