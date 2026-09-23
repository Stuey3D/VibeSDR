# ★★★ ARMv6 SCALAR (Pi Zero / Zero W / Pi 1), 2026-09-22 — built against a RASPBIAN sysroot.
#     Debian's arm-linux-gnueabihf toolchain cannot do this whatever the flags: its crt*.o,
#     libc_nonshared.a and libgcc.a are ARMv7 Thumb-2, and an ARMv6 core dies on the first of them.
#     clang generates the code (-march=armv6 ARM mode, VFPv2) and takes EVERY runtime object from
#     Raspbian's own ARMv6 packages via --sysroot/--gcc-toolchain.
#  ★ CMAKE_SYSTEM_PROCESSOR armv6l is what makes CMakeLists add -marm -march=armv6 -mfpu=vfp and
#    leave the NEON kernels out (scalar path — measured working on a Pi 3 in scalar mode).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv6l)
set(RPI_SYSROOT /opt/rpi-sysroot)
set(CMAKE_SYSROOT ${RPI_SYSROOT})
set(_t "--target=arm-linux-gnueabihf -march=armv6 -mfpu=vfp -mfloat-abi=hard -marm --gcc-toolchain=${RPI_SYSROOT}/usr")
set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_FLAGS_INIT   "${_t}")
set(CMAKE_CXX_FLAGS_INIT "${_t}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_t} -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_t} -fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH ${RPI_SYSROOT} /opt/vibe-rtlsdr-v6)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${RPI_SYSROOT})
set(ENV{PKG_CONFIG_LIBDIR} ${RPI_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${RPI_SYSROOT}/usr/share/pkgconfig)
