# ARMv7 baseline: ONE build intended to run on every ARMv7 SoC, rather than a
# build per core variant.
#
# armv7-a + VFPv3-D16 + Thumb-2, hard float, is the Debian/Ubuntu armhf
# baseline and the lowest common denominator across ARMv7 application cores.
# NEON is deliberately NOT enabled: it is optional on ARMv7 (notably on some
# Cortex-A9 configurations), so requiring it would silently exclude parts of
# the hardware this is meant to cover. Anything wanting per-core tuning
# belongs in a per-CPU prebuild, not here.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(_armv7_baseline "-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -mthumb")
set(CMAKE_C_FLAGS_INIT   "${_armv7_baseline}")
set(CMAKE_CXX_FLAGS_INIT "${_armv7_baseline}")

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
# Host tools (python3 for the Multiface ROM blanking) must still be found on
# the host, so only libraries and headers are restricted to the sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Lets ctest run the target binaries, and lets gtest_discover_tests enumerate
# them at build time, instead of the tests being built and never executed.
set(CMAKE_CROSSCOMPILING_EMULATOR /usr/bin/qemu-arm-static -L /usr/arm-linux-gnueabihf)
