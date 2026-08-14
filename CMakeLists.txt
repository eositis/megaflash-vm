cmake_minimum_required(VERSION 3.10)
project(megaflash-vm
    VERSION 0.1.0
    DESCRIPTION "MAME Apple //c + Bramble MegaFlash overlay"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -g -Wno-stringop-overflow")

# Sibling Bramble checkout (Pico emulator sources).
set(BRAMBLE_ROOT "${CMAKE_SOURCE_DIR}/../Bramble" CACHE PATH "Path to Bramble tree")
if(NOT EXISTS "${BRAMBLE_ROOT}/src/main.c")
    message(FATAL_ERROR "Bramble not found at ${BRAMBLE_ROOT} (set -DBRAMBLE_ROOT=...)")
endif()

option(ENABLE_FUSE "Enable FUSE filesystem mount support (requires libfuse3)" OFF)

include_directories(
    ${BRAMBLE_ROOT}/include
    ${BRAMBLE_ROOT}/include/rp2040
    ${BRAMBLE_ROOT}/include/rp2350_rv
    ${BRAMBLE_ROOT}/include/rp2350_arm
    ${CMAKE_SOURCE_DIR}/bramble-overlay/include
)

# Emulator sources from Bramble (no a2bus — that lives in the overlay).
set(EMU_SOURCES
    ${BRAMBLE_ROOT}/src/cpu.c
    ${BRAMBLE_ROOT}/src/instructions.c
    ${BRAMBLE_ROOT}/src/thumb32.c
    ${BRAMBLE_ROOT}/src/membus.c
    ${BRAMBLE_ROOT}/src/uf2.c
    ${BRAMBLE_ROOT}/src/elf.c
    ${BRAMBLE_ROOT}/src/gpio.c
    ${BRAMBLE_ROOT}/src/timer.c
    ${BRAMBLE_ROOT}/src/nvic.c
    ${BRAMBLE_ROOT}/src/clocks.c
    ${BRAMBLE_ROOT}/src/adc.c
    ${BRAMBLE_ROOT}/src/rom.c
    ${BRAMBLE_ROOT}/src/uart.c
    ${BRAMBLE_ROOT}/src/spi.c
    ${BRAMBLE_ROOT}/src/i2c.c
    ${BRAMBLE_ROOT}/src/pwm.c
    ${BRAMBLE_ROOT}/src/dma.c
    ${BRAMBLE_ROOT}/src/pio.c
    ${BRAMBLE_ROOT}/src/gdb.c
    ${BRAMBLE_ROOT}/src/usb.c
    ${BRAMBLE_ROOT}/src/rtc.c
    ${BRAMBLE_ROOT}/src/netbridge.c
    ${BRAMBLE_ROOT}/src/wire.c
    ${BRAMBLE_ROOT}/src/storage.c
    ${BRAMBLE_ROOT}/src/sdcard.c
    ${BRAMBLE_ROOT}/src/emmc.c
    ${BRAMBLE_ROOT}/src/fatfs.c
    ${BRAMBLE_ROOT}/src/fuse_mount.c
    ${BRAMBLE_ROOT}/src/w5500.c
    ${BRAMBLE_ROOT}/src/bme280.c
    ${BRAMBLE_ROOT}/src/corepool.c
    ${BRAMBLE_ROOT}/src/cyw43.c
    ${BRAMBLE_ROOT}/src/tapif.c
    ${BRAMBLE_ROOT}/src/devtools.c
    ${BRAMBLE_ROOT}/src/vnet.c
    ${BRAMBLE_ROOT}/src/sdd.c
    ${BRAMBLE_ROOT}/src/sdd_thermo.c
    ${BRAMBLE_ROOT}/src/rp2350_rv/rv_cpu.c
    ${BRAMBLE_ROOT}/src/rp2350_rv/rv_clint.c
    ${BRAMBLE_ROOT}/src/rp2350_rv/rv_membus.c
    ${BRAMBLE_ROOT}/src/rp2350_rv/rv_bootrom.c
    ${BRAMBLE_ROOT}/src/rp2350_rv/rp2350_periph.c
    ${BRAMBLE_ROOT}/src/rp2350_rv/picobin.c
    ${BRAMBLE_ROOT}/src/rp2350_arm/m33_cpu.c
)

# Apple-bus / MegaFlash overlay (strong bramble_ext_* — do not link stubs).
set(OVERLAY_SOURCES
    ${CMAKE_SOURCE_DIR}/bramble-overlay/a2bus.c
    ${CMAKE_SOURCE_DIR}/bramble-overlay/a2bus_bridge.c
    ${CMAKE_SOURCE_DIR}/bramble-overlay/host_uthernet.c
    ${CMAKE_SOURCE_DIR}/bramble-overlay/megaflash_a2bus_hooks.c
    ${CMAKE_SOURCE_DIR}/bramble-overlay/bramble_ext_a2bus.c
)

if(ENABLE_FUSE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED fuse3)
    add_compile_definitions(ENABLE_FUSE)
    include_directories(${FUSE3_INCLUDE_DIRS})
    set(FUSE_LIBS ${FUSE3_LIBRARIES})
else()
    set(FUSE_LIBS "")
endif()

set(COMMON_LIBS m pthread ${FUSE_LIBS})

add_executable(bramble ${BRAMBLE_ROOT}/src/main.c ${EMU_SOURCES} ${OVERLAY_SOURCES})
target_link_libraries(bramble ${COMMON_LIBS})

add_custom_command(TARGET bramble POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:bramble> ${CMAKE_SOURCE_DIR}/bramble
    COMMENT "Copying overlay bramble to megaflash-vm root"
)

message(STATUS "megaflash-vm overlay bramble")
message(STATUS "  BRAMBLE_ROOT=${BRAMBLE_ROOT}")
