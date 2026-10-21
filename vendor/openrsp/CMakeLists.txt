# Open RSP API: drop-in replacement for libsdrplay_api.so.3 and sdrplay_apiService.
#
# Targets:
#   sdrplay_platform      static  OS layer (src/platform/<os>/ + common/): shm, IPC objects, threads, clock, log, service host
#   sdrplay_platform_usb  static  libusb-1.0 USB access + OS hotplug (service only; never linked into the client .so)
#   sdrplay_core          static  portable core (src/core/) + the service engine library (src/service/, minus main.cpp)
#   sdrplay_client        object  client implementation (src/client/), shared by the .so and the unit tests
#   sdrplay_api           shared  libsdrplay_api.so.3 (.so.3.15), the API 3.15 export set
#   sdrplay_apiService    exe     the daemon hosting sdrplay_core
#   unit_*                tests   doctest unit tests, registered with ctest (SDRPLAY_BUILD_TESTS)
cmake_minimum_required(VERSION 3.16)
project(sdrplay_open_api VERSION 3.15 LANGUAGES C CXX)

# Reproducible builds: source and build paths are recorded relative to the tree (__FILE__, debug
# info), so the binaries do not depend on where the tree was built.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options("-ffile-prefix-map=${CMAKE_BINARY_DIR}=." "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # no GNU extensions (MSVC / NDK portability)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # static libs end up inside the shared client lib
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(SDRPLAY_WERROR "Treat warnings as errors" OFF)
# The tests are built when the tree has them (default ON then, OFF otherwise).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  option(SDRPLAY_BUILD_TESTS "Build unit tests" ON)
else()
  option(SDRPLAY_BUILD_TESTS "Build unit tests" OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)
include(Warnings)
include(Platform)
include(SdrplayHeaders)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ------------------------------------------------------------------ platform
if(SDRPLAY_PLATFORM STREQUAL "linux")
  set(PLATFORM_SOURCES
    ${SRC}/platform/linux/SharedMemoryLinux.cpp
    ${SRC}/platform/linux/InterProcessSemaphoreLinux.cpp
    ${SRC}/platform/linux/InterProcessMutexLinux.cpp
    ${SRC}/platform/linux/ThreadOsLinux.cpp
    ${SRC}/platform/linux/LogSinkLinux.cpp
    ${SRC}/platform/linux/ServiceHostLinux.cpp)
elseif(SDRPLAY_PLATFORM STREQUAL "android")
  # Bionic layer: in-process named objects, logcat (src/platform/android/Android.cmake).
  set(PLATFORM_SOURCES ${SDRPLAY_ANDROID_PLATFORM_SOURCES})
elseif(SDRPLAY_MACOS_NATIVE)
  # Darwin layer (src/platform/macos, cmake/MacOS.cmake); its own mach clock replaces ClockStd.
  set(PLATFORM_SOURCES ${SDRPLAY_MACOS_PLATFORM_SOURCES})
elseif(SDRPLAY_WINDOWS_NATIVE)
  # Win32 layer (src/platform/windows, cmake/Windows.cmake); its QPC clock replaces ClockStd.
  set(PLATFORM_SOURCES ${SDRPLAY_WINDOWS_PLATFORM_SOURCES})
else()
  # No native layer for this OS (e.g. iOS core builds): the stub layer.
  set(PLATFORM_SOURCES ${SRC}/platform/stub/PlatformStub.cpp)
endif()
if(NOT SDRPLAY_PLATFORM STREQUAL "linux")
  # InterProcessSemaphore stall recovery is Linux/glibc only.
  list(APPEND PLATFORM_SOURCES ${SRC}/platform/common/IpcStallRecoveryNone.cpp)
endif()
set(PLATFORM_CLOCK_SOURCES ${SRC}/platform/common/ClockStd.cpp)
if(SDRPLAY_MACOS_NATIVE OR SDRPLAY_WINDOWS_NATIVE)
  set(PLATFORM_CLOCK_SOURCES)
endif()
add_library(sdrplay_platform STATIC
  ${PLATFORM_SOURCES}
  ${SRC}/platform/common/ThreadStd.cpp
  ${SRC}/platform/common/CpuFeatures.cpp
  ${PLATFORM_CLOCK_SOURCES})
target_include_directories(sdrplay_platform PUBLIC ${SRC})
target_link_libraries(sdrplay_platform PUBLIC Threads::Threads)
if(SDRPLAY_PLATFORM STREQUAL "linux")
  target_link_libraries(sdrplay_platform PUBLIC rt) # shm_open on older glibc
endif()
sdrplay_set_warnings(sdrplay_platform)

# Proves the stub layer still compiles against the interfaces on every host.
add_library(sdrplay_platform_stub_check OBJECT ${SRC}/platform/stub/PlatformStub.cpp ${SRC}/platform/stub/UsbStub.cpp)
target_include_directories(sdrplay_platform_stub_check PRIVATE ${SRC})
sdrplay_set_warnings(sdrplay_platform_stub_check)

if(SDRPLAY_PLATFORM STREQUAL "android")
  # libusb on injected UsbManager fds (no device discovery on Android).
  set(USB_SOURCES ${SDRPLAY_ANDROID_USB_SOURCES})
elseif(SDRPLAY_HAVE_LIBUSB)
  set(USB_SOURCES ${SRC}/platform/common/UsbLibusb.cpp)
  if(SDRPLAY_HAVE_LIBUDEV)
    list(APPEND USB_SOURCES ${SRC}/platform/linux/HotplugUdev.cpp)
  endif()
  if(SDRPLAY_MACOS_NATIVE)
    list(APPEND USB_SOURCES ${SDRPLAY_MACOS_USB_SOURCES})
  endif()
else()
  set(USB_SOURCES ${SRC}/platform/stub/UsbStub.cpp)
endif()
add_library(sdrplay_platform_usb STATIC ${USB_SOURCES})
target_link_libraries(sdrplay_platform_usb PUBLIC sdrplay_platform)
if(SDRPLAY_HAVE_LIBUSB)
  target_link_libraries(sdrplay_platform_usb PRIVATE PkgConfig::LIBUSB)
endif()
if(SDRPLAY_HAVE_LIBUDEV)
  target_compile_definitions(sdrplay_platform_usb PRIVATE SDRPLAY_HAVE_LIBUDEV=1)
  target_link_libraries(sdrplay_platform_usb PRIVATE PkgConfig::LIBUDEV)
endif()
sdrplay_set_warnings(sdrplay_platform_usb)

# ---------------------------------------------------------------------- core
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${SRC}/core/*.cpp)
set(SERVICE_LIB_SOURCES
  ${SRC}/service/ServiceCore.cpp
  ${SRC}/service/GlobalChannel.cpp
  ${SRC}/service/DeviceChannel.cpp
  ${SRC}/service/Enumerator.cpp
  ${SRC}/service/Firmware.cpp
  ${SRC}/service/Heartbeat.cpp
  ${SRC}/service/ServiceDevice.cpp)
# USB stream transport (src/service/stream): talks only to platform::UsbDevice, so it can be
# unit-tested against a mock; libusb itself stays in sdrplay_platform_usb.
file(GLOB SERVICE_STREAM_SOURCES CONFIGURE_DEPENDS ${SRC}/service/stream/*.cpp)
add_library(sdrplay_core STATIC ${CORE_SOURCES} ${SERVICE_LIB_SOURCES} ${SERVICE_STREAM_SOURCES})
target_include_directories(sdrplay_core PUBLIC ${SRC})
target_link_libraries(sdrplay_core PUBLIC sdrplay_api_headers sdrplay_platform)
sdrplay_set_warnings(sdrplay_core)
# Fixed-order DSP/AGC/register arithmetic: never fuse a*b+c into an FMA (clang defaults to
# -ffp-contract=on, arm64 has FMA).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(sdrplay_core PRIVATE -ffp-contract=off)
endif()
include(cmake/DspSimd.cmake)

# ---------------------------------------------------------------- client lib
file(GLOB CLIENT_SOURCES CONFIGURE_DEPENDS ${SRC}/client/*.cpp)
add_library(sdrplay_client OBJECT ${CLIENT_SOURCES})
target_include_directories(sdrplay_client PUBLIC ${SRC})
target_link_libraries(sdrplay_client PUBLIC sdrplay_api_headers sdrplay_platform)
sdrplay_set_warnings(sdrplay_client)

add_library(sdrplay_api SHARED $<TARGET_OBJECTS:sdrplay_client>)
target_link_libraries(sdrplay_api PRIVATE sdrplay_core sdrplay_platform)
if(SDRPLAY_PLATFORM STREQUAL "linux")
  # API 3.15 ABI: soname libsdrplay_api.so.3, file libsdrplay_api.so.3.15, unversioned exports.
  set_target_properties(sdrplay_api PROPERTIES
    VERSION 3.15 SOVERSION 3
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libsdrplay_api.map)
  # Shared-library dependencies, in this order: libpthread, librt, libstdc++, libc (the
  # -l flags below). Since glibc 2.34 libpthread and librt are part of libc, so they are named
  # by soname with -l: and kept with --no-as-needed. On armhf (32-bit ARM) they are omitted.
  set(SDRPLAY_NEEDED_PTHREAD_RT -l:libpthread.so.0 -l:librt.so.1)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(SDRPLAY_NEEDED_PTHREAD_RT)
  endif()
  target_link_options(sdrplay_api PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/libsdrplay_api.map"
    "LINKER:--no-undefined"
    "LINKER:--no-as-needed" ${SDRPLAY_NEEDED_PTHREAD_RT} -lstdc++ -lc "LINKER:--as-needed")
elseif(WIN32)
  # sdrplay_api.dll, the 24 API 3.15 exports with their ordinals (cmake/sdrplay_api.def, cmake/Windows.cmake).
  sdrplay_windows_client_lib(sdrplay_api)
elseif(SDRPLAY_MACOS_NATIVE)
  # libsdrplay_api.so.3.15, install name @rpath/libsdrplay_api.so.3, 101 exports (cmake/MacOS.cmake).
  sdrplay_macos_client_lib(sdrplay_api)
endif()

# ------------------------------------------------------------------- service
# Default = where the packages install the firmware and the service expects it,
# independent of CMAKE_INSTALL_PREFIX.
set(SDRPLAY_FIRMWARE_DIR "/usr/share/sdrplay_api/firmware" CACHE PATH
    "Where the service looks for firmware sets (<dir>/own, ...)")
add_executable(sdrplay_apiService ${SRC}/service/main.cpp)
target_link_libraries(sdrplay_apiService PRIVATE sdrplay_core sdrplay_platform_usb)
target_compile_definitions(sdrplay_apiService PRIVATE SDRPLAY_FIRMWARE_DIR="${SDRPLAY_FIRMWARE_DIR}")
# In the build tree the daemon finds the firmware through <exe dir>/firmware.
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/firmware)
  file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/firmware ${CMAKE_CURRENT_BINARY_DIR}/firmware SYMBOLIC)
endif()
sdrplay_set_warnings(sdrplay_apiService)

# Python runs the firmware image checker (fwcheck.py) and other optional checks.
find_package(Python3 COMPONENTS Interpreter QUIET)

install(TARGETS sdrplay_api LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(TARGETS sdrplay_apiService RUNTIME DESTINATION /opt/sdrplay_api)

# Firmware sets: <SDRPLAY_FIRMWARE_DIR>/<name>/ holds fw_common.bin, fw_rsp1.bin, fw_rspduo.bin and
# their manifest.sha256. The service uploads the set SDRPLAY_FIRMWARE_DEFAULT (default own: our
# bridge firmware); SDRPLAY_FIRMWARE=<name> in its environment selects another installed set.
# The own set: the committed prebuilt images in firmware/own install into
# ${SDRPLAY_FIRMWARE_DIR}/own by default (no sdcc needed). SDRPLAY_OWN_FIRMWARE=ON (and sdcc + make
# found, firmware/src/Makefile present) rebuilds the three images + manifest.sha256 from source into
# <build>/fw_own (never <build>/firmware, a symlink into the source tree) and installs those instead.
# Without the prebuilt images (firmware/own) the firmware is always built from source, and sdcc
# is required.
if(EXISTS ${PROJECT_SOURCE_DIR}/firmware/own/manifest.sha256)
  set(_fw_prebuilt ON)
else()
  set(_fw_prebuilt OFF)
endif()
if(_fw_prebuilt)
  option(SDRPLAY_OWN_FIRMWARE "Build and install our own RSP bridge firmware (firmware/src, needs sdcc)" OFF)
else()
  set(SDRPLAY_OWN_FIRMWARE ON)
endif()
set(SDRPLAY_FIRMWARE_DEFAULT "own" CACHE STRING
    "Compiled-in firmware set the service uploads (a subdirectory of SDRPLAY_FIRMWARE_DIR); SDRPLAY_FIRMWARE overrides it at run time")
if(NOT SDRPLAY_FIRMWARE_DEFAULT MATCHES "^[A-Za-z0-9._-]+$" OR SDRPLAY_FIRMWARE_DEFAULT MATCHES "^\\.\\.?$")
  message(FATAL_ERROR "SDRPLAY_FIRMWARE_DEFAULT must be a firmware set name: letters, digits, '.', '_', '-' "
                      "(got '${SDRPLAY_FIRMWARE_DEFAULT}')")
endif()
# PUBLIC: every user of service/Firmware.h (service, tests, embedded services) sees the same default.
target_compile_definitions(sdrplay_core PUBLIC SDRPLAY_FIRMWARE_DEFAULT_SET="${SDRPLAY_FIRMWARE_DEFAULT}")
if(NOT SDRPLAY_OWN_FIRMWARE)
  # Default: the committed prebuilt release images (make -C firmware/src own; `make check` proves
  # they are this source). No sdcc needed to build or package.
  install(FILES firmware/own/fw_common.bin firmware/own/fw_rsp1.bin firmware/own/fw_rspduo.bin
                firmware/own/manifest.sha256
          DESTINATION ${SDRPLAY_FIRMWARE_DIR}/own)
endif()
if(SDRPLAY_OWN_FIRMWARE)
  set(_fw_src ${PROJECT_SOURCE_DIR}/firmware/src)
  set(_fw_own ${CMAKE_BINARY_DIR}/fw_own)
  find_program(SDRPLAY_SDCC sdcc)
  find_program(SDRPLAY_GNU_MAKE NAMES gmake make)
  if((NOT SDRPLAY_SDCC OR NOT SDRPLAY_GNU_MAKE) AND NOT _fw_prebuilt)
    message(FATAL_ERROR "The firmware is built from firmware/src and needs sdcc and make "
                        "(apt install sdcc make)")
  elseif(NOT SDRPLAY_SDCC OR NOT SDRPLAY_GNU_MAKE)
    message(WARNING "SDRPLAY_OWN_FIRMWARE=ON but sdcc or make was not found: own firmware not built")
  elseif(NOT EXISTS ${_fw_src}/Makefile)
    message(WARNING "SDRPLAY_OWN_FIRMWARE=ON but firmware/src/Makefile does not exist: own firmware not built")
  else()
    # Makefile interface: `make all O=<dir>` -> <dir>/{fw_common,fw_rsp1,fw_rspduo}.bin
    # + <dir>/manifest.sha256; `make check O=<dir>` = the static checks of every image and two
    # clean builds byte-identical. make tracks the source dependencies itself, so the target
    # always runs it.
    add_custom_target(own_firmware ALL
      COMMAND ${SDRPLAY_GNU_MAKE} -C ${_fw_src} all O=${_fw_own}
      BYPRODUCTS ${_fw_own}/fw_common.bin ${_fw_own}/fw_rsp1.bin ${_fw_own}/fw_rspduo.bin ${_fw_own}/manifest.sha256
      COMMENT "Building own RSP bridge firmware into ${_fw_own}"
      VERBATIM USES_TERMINAL)
    install(FILES ${_fw_own}/fw_common.bin ${_fw_own}/fw_rsp1.bin ${_fw_own}/fw_rspduo.bin
                  ${_fw_own}/manifest.sha256
            DESTINATION ${SDRPLAY_FIRMWARE_DIR}/own)
    if(SDRPLAY_BUILD_TESTS)
      enable_testing()
      add_test(NAME fw_static_gates COMMAND ${SDRPLAY_GNU_MAKE} -C ${_fw_src} check O=${_fw_own})
      set_tests_properties(fw_static_gates PROPERTIES LABELS "fw" TIMEOUT 120)
    endif()
  endif()
endif()

# Optional local install rules for further firmware sets, used when the file exists.
include(${PROJECT_SOURCE_DIR}/firmware/local.cmake OPTIONAL)

# ------------------------------------------------------------ system files
# Linux: the systemd unit, udev rules and hwdb entries the packages install (system/linux).
# Opt-in, because they go to fixed paths outside CMAKE_INSTALL_PREFIX. After installing, run
# `udevadm control --reload && systemd-hwdb update` and `systemctl daemon-reload`.
option(SDRPLAY_INSTALL_SYSTEM_FILES "Install the systemd unit, udev rules and hwdb file (Linux)" OFF)
if(SDRPLAY_INSTALL_SYSTEM_FILES AND SDRPLAY_PLATFORM STREQUAL "linux")
  install(FILES system/linux/sdrplay.service DESTINATION /etc/systemd/system)
  install(FILES system/linux/66-sdrplay.rules DESTINATION /etc/udev/rules.d)
  install(FILES system/linux/20-sdrplay.hwdb DESTINATION /etc/udev/hwdb.d)
endif()

# --------------------------------------------------------------------- tests
if(SDRPLAY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# --------------------------------------------------------------------- tools
# rsp_info: standalone read-only RSP lister.
add_subdirectory(src/tools/rsp_info)

if(SDRPLAY_PLATFORM STREQUAL "android")
  sdrplay_android_finalize() # embedded service + JNI in libsdrplay_api.so (src/platform/android/Android.cmake)
endif()
if(SDRPLAY_MACOS_NATIVE)
  sdrplay_macos_finalize() # rpaths, libusb architectures, macOS ABI test (cmake/MacOS.cmake)
endif()
if(SDRPLAY_WINDOWS_NATIVE)
  sdrplay_windows_finalize() # static runtime, advapi32, PE export/ordinal/import checks (cmake/Windows.cmake)
endif()
