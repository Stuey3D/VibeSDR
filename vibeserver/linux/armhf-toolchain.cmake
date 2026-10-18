# ★★★ armhf CROSS-COMPILE, from the Mac's NATIVE arm64 bookworm container (2026-09-22).
#     Apple Silicon cannot execute AArch32, so an arm/v7 container would be qemu — the emulation
#     that segfaulted amd64 on a different file every run. A cross-compiler is native speed and
#     the build root is still bookworm, so Depends: stays at the glibc 2.36 baseline.
# ★★ armv7l, NOT armv6: CMakeLists keys -mfpu=neon-vfpv4 off this, which is what the Pi 2's own
#    build has always had. Stuart: "as long as it doesnt effect the amazing performance we got out
#    of the Pi2". ARMv6 (Pi Zero W / Pi 1) cannot run a NEON binary and is NOT served by this.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf /opt/vibe-deps)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE armhf)
# ★★★ ARM MODE, NOT THUMB-2 — measured on the Pi 2, 2026-09-22. Debian's armhf compiler defaults to
#     Thumb-2; the Raspbian gcc every previous Pi 2 build used defaults to armv6, i.e. ARM mode. Same
#     source, same NEON flags, one listener on 96.6 WFM: native 167% of a core, this toolchain
#     without -marm 181%. The DSP loops are what pay for Thumb's denser encoding.
set(CMAKE_C_FLAGS_INIT   "-marm")
set(CMAKE_CXX_FLAGS_INIT "-marm")
