# Тулчейн для PocketBook InkPad 3 (PB740, Allwinner B288, прошивка 6.5).
# GCC 6.3 из SDK_6.3.0 (ветка 6.5), sysroot с glibc 2.23, ABI softfp — как у libinkview.so.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7-a)

if(DEFINED ENV{PB_SDK})
    set(PB_SDK "$ENV{PB_SDK}")
else()
    set(PB_SDK /opt/pb-sdk/SDK-B288)
endif()
set(PB_SYSROOT "${PB_SDK}/usr/arm-obreey-linux-gnueabi/sysroot")

set(CMAKE_C_COMPILER "${PB_SDK}/usr/bin/arm-obreey-linux-gnueabi-gcc")
set(CMAKE_CXX_COMPILER "${PB_SDK}/usr/bin/arm-obreey-linux-gnueabi-g++")

set(CMAKE_FIND_ROOT_PATH "${PB_SYSROOT}" "${PB_SYSROOT}/usr/local")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Обёртка Buildroot в SDK всегда подставляет перед нашими флагами
# -mcpu=cortex-a8 -march=armv7-a -mfpu=neon -mfloat-abi=softfp.
# GCC берёт последнее значение каждого флага, поэтому -march и -mcpu задаются согласованной парой:
# с одним -mcpu=cortex-a7 остаётся armv7-a и пропадает аппаратное деление (sdiv).
# -mfpu и -mfloat-abi — ровно как в arm_conf.cmake из SDK.
set(PB_ARCH_FLAGS "-march=armv7ve -mcpu=cortex-a7 -mfpu=neon -mfloat-abi=softfp")
set(CMAKE_C_FLAGS_INIT "${PB_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PB_ARCH_FLAGS}")

set(PB740 ON)
