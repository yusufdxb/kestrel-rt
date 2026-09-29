# Memory

## Map (linker/kestrel_ra4m1.ld)
| Range | Content |
|---|---|
| 0x0000_0000-0x0000_3FFF | Arduino bootloader + option-setting memory. Never written; MPU region 1 makes it no-access (NULL trap). |
| 0x0000_4000-... | vector table (48 entries), code, constants, initial values of `.data` and `.app_shared` |
| 0x2000_0000 | `.data`, `.bss` (kernel, privileged only) |
| next 2 KB boundary | `.task_stacks`: each stack power-of-two sized and aligned to its size |
| next 2 KB boundary | `.app_shared` (2 KB, one MPU region, RW for tasks) |
| 0x2000_7800-0x2000_7FFF | main stack (MSP), 32 B no-access guard at its bottom |

`scripts/check_image.py` (run on every link) fails the build if any loadable byte lies below 0x4000, if the
initial MSP would fail the bootloader's `app_valid` check, or if an allocator symbol was linked.

## MPU regions (safety/mpu/mpu.c)
| # | Base | Size | Priv / unpriv | XN | Purpose |
|---|---|---|---|---|---|
| 0 | 0x0 | 256 KB | RO / RO | no | code, constants |
| 1 | 0x0 | 16 KB | - / - | yes | bootloader block, NULL-pointer trap |
| 2 | 0x2000_0000 | 32 KB | RW / - | yes | all SRAM, kernel data |
| 3 | `.app_shared` | 2 KB | RW / RW | yes | IPC shared with tasks |
| 4 | current task stack | stack size | RW / RW | yes | per switch |
| 5 | current stack bottom | 32 B | - / - | yes | stack guard |
| 6 | MSP bottom | 32 B | - / - | yes | main-stack guard |
| 7 | 0x4007_8000 | 256 B | RW / RO | yes | GPT320 read-only for tasks (timestamps without syscalls) |

PRIVDEFENA gives privileged code the default map elsewhere; HFNMIENA = 0 so fault capture in HardFault/NMI can
read any stack. SRAM is never executable.

## Crash log
VBATT backup registers VBTBKR[128..255] hold one CRC-32-protected crash record; [256..263] a boot counter.
The bootloader only uses VBTBKR[0..3]. Writes cost about 9 us per byte on this part (measured through the flight
recorder), so a record takes about 1.1 ms: written synchronously only when the policy resets anyway, deferred
to the comms task for contained faults (the RAM copy is retrievable immediately).

## Stack sizing
Fill pattern 0xA5A5A5A5 + high-water scan, reported per task in every status frame; worst observed values are
in docs/BENCHMARKS.md. GCC `-fstack-usage` output (`build/<app>/**/*.su`) gives static per-function numbers.
