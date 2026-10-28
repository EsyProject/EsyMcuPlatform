# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

target_sources(esy_logger_core PRIVATE esy_logger.c)
foreach(backend IN ITEMS uart printf rtt)
    if(TARGET esy_logger_backend_${backend}_core)
        target_sources(esy_logger_backend_${backend}_core PRIVATE esy_logger_backend_${backend}_core.c)
    endif()
    if(TARGET esy_logger_backend_${backend})
        target_sources(esy_logger_backend_${backend} PRIVATE esy_logger_backend_${backend}.c)
    endif()
endforeach()
