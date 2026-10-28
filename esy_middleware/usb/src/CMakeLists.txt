set(MODULE_SRCS "")

if(ENABLE_ESY_USB_BACKEND_TINYUSB AND NOT ENABLE_ESY_USB_CDC)
    message(FATAL_ERROR
        "ENABLE_ESY_USB_BACKEND_TINYUSB is a CDC backend and requires ENABLE_ESY_USB_CDC."
    )
endif()

if(ENABLE_ESY_USB_CDC)
    if(NOT ENABLE_ESY_USB_BACKEND_TINYUSB)
        message(FATAL_ERROR
            "ENABLE_ESY_USB_CDC requires a CDC backend. "
            "Enable ENABLE_ESY_USB_BACKEND_TINYUSB or add another backend."
        )
    endif()

    list(APPEND MODULE_SRCS esy_usb_cdc.c)
endif()

if(ENABLE_ESY_USB_BACKEND_TINYUSB)
    list(APPEND MODULE_SRCS esy_usb_cdc_backend_tinyusb.c)
endif()

target_sources(esy_usb PRIVATE ${MODULE_SRCS})
