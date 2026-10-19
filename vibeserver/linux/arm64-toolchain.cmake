# ★★★ arm64 CROSS-COMPILE, from an amd64 bookworm container on the Lenovo (2026-10-18).
#     Stuart: "the docker build can we do that remotely on the lenovo going forward that has loads of
#     space". The Mac's Docker VM is gone; the arm64 package used to be built NATIVELY in an arm64
#     bookworm container on Apple Silicon. On the Lenovo (x86-64) an arm64 container would be qemu —
#     the emulation that segfaulted the compiler on a different file every run when it was amd64 on
#     the Mac — so this is a cross-compiler instead: native speed, and still bookworm, so the libc6
#     floor in Depends comes out of the same glibc it always did.
# ★★ NO -march / -mcpu, ON PURPOSE. The native build never set one, so it got Debian's aarch64 default,
#    plain armv8-a — the Cortex-A53 in a Pi 3 and the Zero 2 W runs it. The cross gcc is the same
#    gcc-12 with the same default, so the baseline is unchanged. Do not "tune" this for the Pi 5: a
#    -mcpu=cortex-a76 binary is SIGILL on every Pi 3 still serving.
# ★ The shape mirrors armhf-toolchain.cmake — only the triplet and the package architecture differ.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /opt/vibe-deps)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig)
# ★★★ Without this CPack asks `dpkg --print-architecture` — amd64, in this container — and stamps an
#     aarch64 binary into a package apt would offer to x86 machines.
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
