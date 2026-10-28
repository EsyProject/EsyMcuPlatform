# USB CDC HIL Smoke Checklist

## Purpose

This checklist records board-level USB CDC smoke coverage for consumer projects
that integrate `esy_middleware/usb`. Host unit tests cover adapter behavior with
a fake backend; this checklist covers the product-owned TinyUSB descriptors,
board bring-up, host enumeration, and scheduling path.

## Setup

- Board:
- Firmware commit:
- EsyMcuPlatform commit:
- Host OS:
- USB cable / hub:
- CDC interface count:
- Terminal or test tool:

## Test Procedures

1. Flash firmware with `ENABLE_ESY_USB=ON` and `ENABLE_ESY_USB_CDC=ON`.
2. Confirm the device enumerates as the expected USB device and exposes the
   configured CDC interface count.
3. Open the CDC port from the host and confirm `esy_usb_cdc_connected()` becomes
   true only after DTR is asserted.
4. Close the host terminal and confirm the connection state returns false.
5. Send at least 256 bytes host-to-device and confirm the firmware receives the
   exact byte stream without blocking inside the RX callback.
6. Send at least 256 bytes device-to-host and confirm the host receives the exact
   byte stream, allowing documented short writes and retry policy above this
   module.
7. Toggle RTS/DTR if the host tool supports it and confirm the line-state
   callback receives the expected values.
8. Unplug and replug USB while the firmware is running; confirm no hard fault,
   stale callback, or wedged task loop is observed.
9. If multiple CDC interfaces are enabled, bind two handles and confirm traffic
   on one interface does not appear on the other.
10. If stdio-over-CDC is enabled elsewhere, confirm that no interface is owned by
    both stdio and `esy_usb_cdc`.

## Result

- Pass / Fail:
- Observed issues:
- Follow-up ticket:
