# esy_hal/src/xio/CMakeLists.txt

set(MODULE_SRCS "")

# ---- Feature-gated XIO source assembly (ENABLE_ESY_XXX) ----
if(ENABLE_ESY_XIO_UART_TX OR ENABLE_ESY_XIO_UART_RX)
    list(APPEND MODULE_SRCS esy_hal_xio_uart.c)
    foreach(direction TX RX)
        string(TOLOWER "${direction}" suffix)
        if(ENABLE_ESY_XIO_UART_${direction})
            list(APPEND MODULE_SRCS esy_hal_xio_uart_${suffix}.c)
        else()
            list(APPEND MODULE_SRCS esy_hal_xio_uart_${suffix}_disabled.c)
        endif()
    endforeach()
endif()

if(ENABLE_ESY_XIO_SPI)
    list(APPEND MODULE_SRCS esy_hal_xio_spi.c)
endif()

# ---- Attach selected XIO sources to the top-level esy_hal target ----
target_sources(esy_hal PRIVATE ${MODULE_SRCS})
