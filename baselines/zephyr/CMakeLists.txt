# Baseline A2: Kestrel benchmark workload on Zephyr v4.2.0 (board arduino_uno_r4@wifi).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kestrel_zephyr_baseline)
set(K ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# Shared register-level drivers + measurement shim (same code as the other baselines).
set(SHARED ${K}/baselines/common/bench.c ${K}/drivers/uart/sci_uart.c ${K}/drivers/icu/icu.c
    ${K}/drivers/gpio/gpio.c ${K}/drivers/adc/adc.c ${K}/drivers/timer/gpt.c ${K}/trace/frame.c ${K}/trace/crc16.c
    ${K}/apps/common/workload.c)
target_sources(app PRIVATE src/main.c ${SHARED})
target_include_directories(app BEFORE PRIVATE ${K} ${K}/board ${K}/third_party/renesas/Include)
target_compile_definitions(app PRIVATE KESTREL_SHARED= KESTREL_BUILD_ID="zephyr")
