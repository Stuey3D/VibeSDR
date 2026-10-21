# Cross toolchain: Windows x86 (32-bit) with MinGW-w64 (Ubuntu package mingw-w64, posix thread model).
#   cmake -S . -B build-win32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-i686.cmake
# Builds the 32-bit pieces: API\x86\sdrplay_api.dll and sdrplay_apiService.exe (the service is
# x86 on x64 Windows too). No emulator is set (a 64-bit-only Wine has no wine32), so i686 test
# executables are built but not run.
# Binaries are linked statically against libstdc++/libgcc/winpthread so no MinGW DLLs are needed.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(_mingw i686-w64-mingw32)
# Prefer the -posix variants (std::thread / std::mutex need the posix thread model).
find_program(CMAKE_C_COMPILER NAMES ${_mingw}-gcc-posix ${_mingw}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${_mingw}-g++-posix ${_mingw}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${_mingw}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${_mingw})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
# Never pick up the build host's pkg-config files (e.g. the host libusb-1.0.pc).
set(ENV{PKG_CONFIG_LIBDIR} "/usr/${_mingw}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
