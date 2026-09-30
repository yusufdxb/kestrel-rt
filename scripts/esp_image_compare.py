#!/usr/bin/env python3
"""Compare two ESP32-S3 app images, ignoring the fields a fresh compile always changes:
the app descriptor's ELF SHA-256 (image offset 0xb0-0xcf; the ELF includes debug info) and the trailing
checksum byte + appended SHA-256 digest (last 33 bytes). Everything else must match byte for byte.
    python3 scripts/esp_image_compare.py VALIDATED.bin REBUILT.bin"""
import sys

a, b = (open(p, "rb").read() for p in sys.argv[1:3])
if len(a) != len(b):
    sys.exit(f"length differs: {len(a)} vs {len(b)}")
skip = set(range(0xB0, 0xD0)) | set(range(len(a) - 33, len(a)))
diff = [i for i in range(len(a)) if a[i] != b[i] and i not in skip]
ign = sum(1 for i in skip if a[i] != b[i])
print(f"{len(a)} bytes, {len(diff)} differing outside the ELF-hash/digest fields ({ign} inside)")
sys.exit(1 if diff else 0)
