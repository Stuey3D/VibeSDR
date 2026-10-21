# Selects src/platform/<os>/ and finds OS-specific dependencies.
#   SDRPLAY_PLATFORM = linux | android | macos | windows
# linux, android, macos and windows have native layers; other targets (iOS) build the
# compiling stub layer.
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(SDRPLAY_PLATFORM android)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(SDRPLAY_PLATFORM linux)
elseif(APPLE)
  set(SDRPLAY_PLATFORM macos)
elseif(WIN32)
  set(SDRPLAY_PLATFORM windows)
else()
  message(FATAL_ERROR "Unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()
message(STATUS "sdrplay platform layer: ${SDRPLAY_PLATFORM}")

# macOS (not iOS, which keeps the stub layer): Darwin platform sources, API 3.15 dylib
# naming/exports, firmware dir (cmake/MacOS.cmake sets SDRPLAY_MACOS_NATIVE).
set(SDRPLAY_MACOS_NATIVE OFF)
if(SDRPLAY_PLATFORM STREQUAL "macos" AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  include(${CMAKE_CURRENT_LIST_DIR}/MacOS.cmake)
endif()

find_package(Threads REQUIRED)
# Cross builds (Android NDK, iOS, MinGW, ...) must not pick up the BUILD host's .pc files (e.g.
# the host libusb-1.0, which then fails to link with -lusb-1.0). Unless the caller (or the
# toolchain file) points PKG_CONFIG_LIBDIR at target libraries, only the sysroot is searched.
if(CMAKE_CROSSCOMPILING AND NOT DEFINED ENV{PKG_CONFIG_LIBDIR})
  if(CMAKE_SYSROOT)
    set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
  else()
    set(ENV{PKG_CONFIG_LIBDIR} "/nonexistent")
  endif()
  set(ENV{PKG_CONFIG_PATH} "")
endif()
find_package(PkgConfig QUIET)

# libusb-1.0 is the only USB API on every OS.
set(SDRPLAY_HAVE_LIBUSB OFF)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
  if(LIBUSB_FOUND)
    set(SDRPLAY_HAVE_LIBUSB ON)
  endif()
endif()

# Android: libusb built from source for the NDK, bionic platform layer (src/platform/android).
if(SDRPLAY_PLATFORM STREQUAL "android")
  include(${CMAKE_CURRENT_LIST_DIR}/../src/platform/android/Android.cmake)
endif()

# Windows (MinGW): native platform layer, libusb from source, API 3.15 DLL name/exports/ordinals
# (cmake/Windows.cmake sets SDRPLAY_WINDOWS_NATIVE).
set(SDRPLAY_WINDOWS_NATIVE OFF)
if(WIN32)
  include(${CMAKE_CURRENT_LIST_DIR}/Windows.cmake)
endif()

set(SDRPLAY_HAVE_LIBUDEV OFF)
if(SDRPLAY_PLATFORM STREQUAL "linux")
  if(NOT SDRPLAY_HAVE_LIBUSB)
    message(FATAL_ERROR "libusb-1.0 development files are required on Linux (apt install libusb-1.0-0-dev)")
  endif()
  pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)
  set(SDRPLAY_HAVE_LIBUDEV ON)
endif()

# Cross-target test runs (Android /data/local/tmp, a Mac/Wine copy, ...): the unit tests read
# test data under SDRPLAY_SOURCE_DIR (compiled in as the source dir). When
# the tests run on another machine, set SDRPLAY_TEST_DATA_DIR to where that subset of the tree
# is copied on the target. Compile options come after the per-target definitions on the
# command line, so -U/-D here replaces the tests' value (no effect on the libraries).
set(SDRPLAY_TEST_DATA_DIR "" CACHE STRING "On-target copy of the source tree used by the unit tests (default: the source dir)")
if(SDRPLAY_TEST_DATA_DIR)
  add_compile_options(-USDRPLAY_SOURCE_DIR "-DSDRPLAY_SOURCE_DIR=\"${SDRPLAY_TEST_DATA_DIR}\"")
endif()
