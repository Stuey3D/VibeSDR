# Linux armhf (ARMv7-A, VFPv3-D16, hard float; Debian's armhf baseline) with the Debian/Ubuntu
# cross compiler (apt install g++-arm-linux-gnueabihf), e.g. on an arm64 Raspberry Pi 5 or x86-64.
#
#   cmake -S . -B build-armhf -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-armhf.cmake \
#         -DSDRPLAY_ARMHF_DEPS=<dir> [-DSDRPLAY_API_HEADER_DIR=<headers>]
#
# SDRPLAY_ARMHF_DEPS: optional directory into which the armhf .debs of libusb-1.0-0(-dev),
# libudev1, libudev-dev and libcap2 were unpacked with `dpkg -x` (apt-get download <pkg>:armhf),
# so no armhf multiarch packages have to be installed system-wide (installing libc6:armhf /
# libudev1:armhf pulls in lockstep upgrades of the host's libc6 and systemd). Without it,
# pkg-config uses the system multiarch dir /usr/lib/arm-linux-gnueabihf.
#
# Running the tests: natively on an AArch64 kernel with 32-bit EL0
# support through the cross libc's loader:
#   /usr/arm-linux-gnueabihf/lib/ld-linux-armhf.so.3 \
#     --library-path /usr/arm-linux-gnueabihf/lib:<deps>/usr/lib/arm-linux-gnueabihf <test>
# or with qemu-arm -L /usr/arm-linux-gnueabihf.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(SDRPLAY_ARMHF_DEPS "" CACHE PATH "dpkg -x tree of the armhf libusb/libudev packages (optional)")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SDRPLAY_ARMHF_DEPS)
if(SDRPLAY_ARMHF_DEPS)
  list(APPEND CMAKE_FIND_ROOT_PATH "${SDRPLAY_ARMHF_DEPS}")
  set(ENV{PKG_CONFIG_LIBDIR} "${SDRPLAY_ARMHF_DEPS}/usr/lib/arm-linux-gnueabihf/pkgconfig")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SDRPLAY_ARMHF_DEPS}")
  # libusb-1.0.so.0 needs libudev.so.1 (-> libcap.so.2) at link time.
  set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-rpath-link,${SDRPLAY_ARMHF_DEPS}/usr/lib/arm-linux-gnueabihf")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,-rpath-link,${SDRPLAY_ARMHF_DEPS}/usr/lib/arm-linux-gnueabihf")
else()
  set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig")
endif()
set(ENV{PKG_CONFIG_PATH} "")
