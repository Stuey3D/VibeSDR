# VibeServer — standalone daemon (macOS now, Linux/Raspberry Pi next).
#
# Builds the SAME C++ core Android ships, with no JNI and no USB:
#   vibedsp          — clean-room DSP (kissfft), portable C++17
#   net_shim         — clean-room POSIX sockets, portable as-is
#   local_sdr_shim   — the UberSDR-compatible HTTP/WebSocket server + pipeline
#
# The sources deliberately live in the Android tree and are referenced, NOT copied. One core, two
# front-ends: a bug fixed while iterating here is fixed on the phone too. Copying them would create
# the drift this whole exercise exists to avoid.
#
# librtlsdr is NOT linked. `rtl_sdr_stub.h` supplies no-op declarations so the USB code paths
# compile but are never entered — exactly how iOS already builds this shim. IQ arrives over
# rtl_tcp. Adding real USB later (BRIEF-vibeserver-macos.md §2, vendored libusb) is additive.
#
#   cmake -S vibeserver -B vibeserver/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build vibeserver/build -j
#   ./vibeserver/build/vibeserver --tcp 127.0.0.1:1234

cmake_minimum_required(VERSION 3.20)
project(vibeserver CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../android/app/src/main/cpp")
if(NOT EXISTS "${CPP_DIR}/local_sdr_shim.cpp")
  message(FATAL_ERROR "Shared C++ core not found at ${CPP_DIR} — run cmake from the repo checkout.")
endif()

# ── vibedsp: the clean-room DSP engine ────────────────────────────────────────
add_library(vibedsp STATIC
    "${CPP_DIR}/vibedsp/fft.cpp"        "${CPP_DIR}/vibedsp/ddc.cpp"
    "${CPP_DIR}/vibedsp/resampler.cpp"  "${CPP_DIR}/vibedsp/stereo.cpp"
    "${CPP_DIR}/vibedsp/rds.cpp"        "${CPP_DIR}/vibedsp/pipeline.cpp"
    "${CPP_DIR}/vibedsp/third_party/kissfft/kiss_fft.c"
    "${CPP_DIR}/vibedsp/third_party/kissfft/kiss_fftr.c")
target_include_directories(vibedsp PUBLIC "${CPP_DIR}/vibedsp")
target_compile_options(vibedsp PRIVATE -O3 -ffp-contract=fast)

# ── decoders + ft8_lib ────────────────────────────────────────────────────────
# The shim references the decoder suite directly, so a server build needs them even though a
# headless server is not the place anyone decodes FT8 from. Mirrors the Android target's list.
file(GLOB FT8_SRC
    "${CPP_DIR}/ft8_lib/ft8/*.c"
    "${CPP_DIR}/ft8_lib/fft/kiss_fft.c"
    "${CPP_DIR}/ft8_lib/fft/kiss_fftr.c"
    "${CPP_DIR}/ft8_lib/common/monitor.c")

# ── the core, as a STATIC LIB shared by the CLI and the .app ──────────────────
# The menu-bar app and the command-line harness must be the same server, not two servers that
# drift. Both link this.
add_library(vibeserver_core STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vibeserver_api.cpp"
    "${CPP_DIR}/local_sdr_shim.cpp"
    "${CPP_DIR}/net_shim.cpp"
    "${CPP_DIR}/mdns_responder.cpp"
    "${CPP_DIR}/spyserver/spyserver_client.cpp"
    "${CPP_DIR}/spyserver/spyserver_messages.cpp"
    "${CPP_DIR}/decoders/fsk_decoder.cpp"
    "${CPP_DIR}/decoders/wefax_decoder.cpp"
    "${CPP_DIR}/decoders/ft8_decoder.cpp"
    "${CPP_DIR}/decoders/sstv_decoder.cpp"
    "${CPP_DIR}/decoders/audio_nr.cpp"
    "${CPP_DIR}/decoders/auto_notch.cpp"
    ${FT8_SRC})
target_include_directories(vibeserver_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CPP_DIR}" "${CPP_DIR}/vibedsp" "${CPP_DIR}/ft8_lib")
target_link_libraries(vibeserver_core PUBLIC vibedsp)

# ── the CLI harness ───────────────────────────────────────────────────────────
add_executable(vibeserver
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
# ── librtlsdr + libusb: the dongle, driven DIRECTLY ───────────────────────────
# STATIC on purpose. A user drags one .app out of a DMG and it works — no Homebrew, no dylib
# hunting, nothing to install. Prefer the static archives; fall back to whatever is found so a
# dev machine still configures.
find_library(RTLSDR_LIB NAMES rtlsdr librtlsdr.a rtlsdr.a HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_library(USB_LIB    NAMES usb-1.0 libusb-1.0.a HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_path(RTLSDR_INC rtl-sdr.h HINTS /opt/homebrew/include /usr/local/include /usr/include)
if(NOT RTLSDR_LIB OR NOT RTLSDR_INC)
  message(FATAL_ERROR "librtlsdr not found. On macOS: brew install librtlsdr")
endif()
# Prefer the .a next to whatever find_library picked, so we bundle rather than link a dylib.
get_filename_component(_rtl_dir "${RTLSDR_LIB}" DIRECTORY)
if(EXISTS "${_rtl_dir}/librtlsdr.a")
  set(RTLSDR_LIB "${_rtl_dir}/librtlsdr.a")
endif()
if(EXISTS "${_rtl_dir}/libusb-1.0.a")
  set(USB_LIB "${_rtl_dir}/libusb-1.0.a")
endif()
message(STATUS "librtlsdr: ${RTLSDR_LIB}")
message(STATUS "libusb:    ${USB_LIB}")

target_include_directories(vibeserver_core PUBLIC "${RTLSDR_INC}")
# Use the REAL rtl-sdr.h, not the iOS no-op stub the shim falls back to.
target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_LIBRTLSDR)
target_link_libraries(vibeserver_core PUBLIC "${RTLSDR_LIB}" "${USB_LIB}")

# ── libopus: compressed VibeServer audio (replaces IMA-ADPCM) ──────────────────
# STATIC like the others, so the .app ships self-contained. VIBE_HAVE_OPUS gates every Opus code
# path in the shim; where it is undefined (the Android build has no libopus yet — UberSDR decode
# there is ExoPlayer, not libopus) the shim falls back to raw PCM.
find_library(OPUS_LIB NAMES opus libopus.a opus.a HINTS /opt/homebrew/lib /opt/homebrew/opt/opus/lib /usr/local/lib /usr/lib)
find_path(OPUS_INC opus/opus.h HINTS /opt/homebrew/include /opt/homebrew/opt/opus/include /usr/local/include /usr/include)
if(NOT OPUS_LIB OR NOT OPUS_INC)
  message(FATAL_ERROR "libopus not found. On macOS: brew install opus")
endif()
get_filename_component(_opus_dir "${OPUS_LIB}" DIRECTORY)
if(EXISTS "${_opus_dir}/libopus.a")
  set(OPUS_LIB "${_opus_dir}/libopus.a")
endif()
message(STATUS "libopus:   ${OPUS_LIB}")
target_include_directories(vibeserver_core PUBLIC "${OPUS_INC}")
target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_OPUS)
target_link_libraries(vibeserver_core PUBLIC "${OPUS_LIB}")
target_link_libraries(vibeserver PRIVATE vibeserver_core)
if(APPLE)
  # libusb on macOS talks to IOKit; these are system frameworks, nothing to ship.
  target_link_libraries(vibeserver_core PUBLIC
      "-framework IOKit" "-framework CoreFoundation" "-framework Security" objc)
endif()
find_package(Threads REQUIRED)
target_link_libraries(vibeserver_core PUBLIC Threads::Threads)
