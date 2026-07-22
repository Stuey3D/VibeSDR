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

# ── the server itself ─────────────────────────────────────────────────────────
add_executable(vibeserver
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
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
target_include_directories(vibeserver PRIVATE
    "${CPP_DIR}" "${CPP_DIR}/vibedsp" "${CPP_DIR}/ft8_lib")
target_link_libraries(vibeserver PRIVATE vibedsp)
find_package(Threads REQUIRED)
target_link_libraries(vibeserver PRIVATE Threads::Threads)
