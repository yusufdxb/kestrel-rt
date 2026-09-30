# Third-party notices

Kestrel-RT's own code is MIT-licensed (LICENSE). This file lists every external component the repository
contains, fetches, builds against or talks to, and what the repository does with it. Legal reasoning and
open questions are in [docs/LICENSING.md](docs/LICENSING.md).

## Redistributed in this repository

| Component | Upstream, version | License | Files | Modified? |
|---|---|---|---|---|
| Arm CMSIS 5 Core headers | Arm CMSIS 5, as shipped in the Arduino `renesas_uno` 1.6.0 core (`variants/UNOWIFIR4/includes/ra/arm/CMSIS_5`) | Apache-2.0 (SPDX header in each file) | `third_party/cmsis/Include/{core_cm4.h, cmsis_version.h, cmsis_compiler.h, cmsis_gcc.h, mpu_armv7.h}` | No. sha256 in `third_party/sha256sums.txt` |
| R7FA4M1AB device header | SVDConv-generated CMSIS header (Arm Limited notice, same Arduino core tree) | Apache-2.0 (header notice) | `third_party/renesas/Include/R7FA4M1AB.h` | No. sha256 in `third_party/sha256sums.txt` |

Apache-2.0 obligations met: the original copyright and license headers are retained unmodified in every
file, the files are unmodified, and the license text is included as [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt).
No NOTICE file ships with these headers upstream.

## Fetched at build time, never redistributed here

| Component | Pinned version | License | Used by | How it is obtained |
|---|---|---|---|---|
| Arduino UNO R4 WiFi USB bridge (`arduino/uno-r4-wifi-usb-bridge`) | tag 0.5.2, commit `4394ba03c6bd1afb661da2944de68359ec5b415a` | **No license file or license headers in the sketch at that commit** (see docs/LICENSING.md) | Phase 2 ESP32-S3 fork | user-supplied checkout (`KX_BRIDGE_REF`); `esp/build.sh` verifies the sketch hash and applies `esp/patches/KestrelBridge.ino.ed` |
| free-dap (inside the bridge checkout: `freedap.c/.h`) | as in bridge 0.5.2 | BSD-3-Clause (Alex Taradov) | ESP32-S3 CMSIS-DAP | part of the upstream checkout |
| arduino-esp32 (Arduino fork, branch `unor4wifi-2.0.9`, commit `a2b82168`) + ESP-IDF 4.4 | as pinned by the bridge's submodule | LGPL-2.1 (Arduino core) / Apache-2.0 (ESP-IDF) | ESP32-S3 build | upstream submodule |
| Arduino libraries (ArduinoBLE 1.3.6, ArduinoHttpClient 0.6.0, Arduino_DebugUtils 1.4.0, Arduino_ESP32_OTA 0.3.1, BOSSA `ce7b684`) | as pinned by the bridge's submodules | per library (LGPL-2.1 / BSD) | compiled into the ESP32-S3 image only where referenced | upstream submodules |
| Official bridge firmware binary `UNOR4-WIFI-S3-0.5.2.bin` | release 0.5.2, sha256 `a488aacf...cd7ca` | Arduino release asset | stock-restore path | `scripts/fetch_esp_stock.sh` downloads from github.com/arduino |
| FreeRTOS-Kernel | V11.1.0, commit `dbf70559b27d39c1fdb68dfb9a32140b6a6777a0` | MIT | comparison baseline only | `scripts/fetch_baselines.sh` (tag and commit verified) |
| Zephyr RTOS | v4.2.0, commit `413b789deb391d3a37d06b463288a5fe765ee57e` | Apache-2.0 | comparison baseline only | user west workspace (`ZEPHYR_WS`) |

## Tools and local files used but not redistributed

| Component | Version | License | Use |
|---|---|---|---|
| Arm GNU Toolchain | 13.3.Rel1 | GPL-3.0 (tools); libgcc under the GCC Runtime Library Exception | builds the RA4M1 images; `-lgcc` is the only library linked (`-nostdlib`) |
| Arduino UNO R4 bootloader (`dfu_wifi.hex`) | renesas_uno 1.6.0 | Arduino | read-only SWD comparison in `scripts/swd_verify.py`; never written, never redistributed |
| R7FA4M1AB SVD | renesas_uno 1.6.0 | vendor | register lookup in `scripts/svd_query.py`, read from the local install |
| bossac | 1.9.1-arduino5 | BSD-3-Clause | RA4M1 upload through the Arduino bootloader |
| arduino-cli | 1.5.1 | GPL-3.0 | ESP32-S3 build driver |
| esptool | 4.12.0 | GPL-2.0 | ESP32-S3 ROM-mode flashing |
| pyOCD | 0.45.1 | Apache-2.0 | non-halting SWD reads, bootloader-entry helper |
| pySerial, pyelftools, matplotlib, cython-hidapi | 3.5, 0.33, 3.10.8, 0.9.0 | BSD-3-Clause / public domain / PSF-based / BSD or GPL-3 or HIDAPI (licensee choice) | host tools |
| Graphviz | 2.43.0 | EPL-1.0 | renders `docs/figures/*.dot` (outputs are committed; they contain no Graphviz code) |

## Binaries

No firmware binary is committed or attached to a release. Anyone who builds and distributes the ESP32-S3
image (`build/esp/KestrelBridge.bin`) takes on the obligations of everything linked into it (LGPL-2.1
arduino-esp32 core, Apache-2.0 ESP-IDF, BSD free-dap, and the unresolved status of the upstream sketch).
