# Third-party files

Only register-definition headers are vendored. No third-party code is compiled into Kestrel.

| File | Origin | License | sha256 |
|---|---|---|---|
| cmsis/Include/core_cm4.h, cmsis_version.h, cmsis_compiler.h, cmsis_gcc.h, mpu_armv7.h | Arm CMSIS 5 Core (as shipped in Arduino renesas_uno 1.6.0 `variants/UNOWIFIR4/includes/ra/arm/CMSIS_5`) | Apache-2.0 (header notices retained) | see `sha256sums.txt` |
| renesas/Include/R7FA4M1AB.h | SVDConv-generated CMSIS device header for R7FA4M1AB (same source tree) | Apache-2.0 (header notice retained) | see `sha256sums.txt` |

`board/system.h` replaces the FSP `system.h` the device header includes; `board/ra4m1.h` defines
`IRQn_Type` (left to the BSP by the generated header). The vendor SVD used by
`scripts/svd_query.py` is read from the local Arduino installation and is not redistributed.
