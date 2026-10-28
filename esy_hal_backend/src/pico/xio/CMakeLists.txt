# esy_hal_backend/src/pico/xio/CMakeLists.txt

# Attach XIO sources to the top-level esy_hal_backend target.

set(MODULE_SRCS "")

if(ENABLE_ESY_XIO_SPI)
    list(APPEND MODULE_SRCS
        spi/esy_hb_xio_spi.c
    )
endif()

if(ENABLE_ESY_XIO_UART_RX)
    list(APPEND MODULE_SRCS
        uart_rx/esy_hb_xio_uart_rx.c
    )
endif()

if(ENABLE_ESY_XIO_UART_TX)
    list(APPEND MODULE_SRCS
        uart_tx/esy_hb_xio_uart_tx.c
    )
endif()

target_sources(esy_hal_backend PRIVATE ${MODULE_SRCS})

target_include_directories(esy_hal_backend PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

if(ENABLE_ESY_XIO_SPI)
    pico_generate_pio_header(esy_hal_backend
        ${CMAKE_CURRENT_LIST_DIR}/spi/esy_hb_xio_spi.pio
    )
endif()

if(ENABLE_ESY_XIO_UART_RX)
    pico_generate_pio_header(esy_hal_backend
        ${CMAKE_CURRENT_LIST_DIR}/uart_rx/esy_hb_xio_uart_rx.pio
    )
endif()

if(ENABLE_ESY_XIO_UART_TX)
    pico_generate_pio_header(esy_hal_backend
        ${CMAKE_CURRENT_LIST_DIR}/uart_tx/esy_hb_xio_uart_tx.pio
    )
endif()
