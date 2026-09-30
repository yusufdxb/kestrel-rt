#!/usr/bin/env python3
"""Reboot the UNO R4 WiFi's ESP32-S3 into ROM download mode through the bridge's HID interface
(feature report 0xAA -> usb_persist_restart(RESTART_BOOTLOADER); same as Arduino unor4wifi-reboot).
Read-only in itself. Note: leaving ROM mode reboots the bridge, and the stock bridge's boot code
pulses the RA4M1 reset line (observed 2026-09-30)."""
import sys
import time

import hid

devs = hid.enumerate(0x2341, 0x1002)
if not devs:
    sys.exit("bridge HID (2341:1002) not found")
h = hid.device()
h.open_path(devs[0]["path"])
b = [0] * 65
b[1] = 0xAA
try:
    h.send_feature_report(b)
except OSError:
    pass  # the device may disconnect before the transfer completes
time.sleep(4)
print("sent; expect USB 303a:1001")
