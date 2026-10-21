# Cross toolchain: Windows x64 with MinGW-w64 (Ubuntu package mingw-w64, posix thread model).
#   cmake -S . -B build-win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
# Tests run under Wine (wine64); CMAKE_CROSSCOMPILING_EMULATOR lets ctest launch them directly.
# Binaries are linked statically against libstdc++/libgcc/winpthread so no MinGW DLLs are needed.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(_mingw x86_64-w64-mingw32)
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
find_program(_wine NAMES wine64 wine)
if(_wine)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${_wine})
endif()
# Never pick up the build host's pkg-config files (e.g. the host libusb-1.0.pc).
set(ENV{PKG_CONFIG_LIBDIR} "/usr/${_mingw}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
