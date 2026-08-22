# VibeSDR V5 — local-SDR shim, NATIVE-ONLY (GPL-free).
#
# V5 rips out SDR++ Brown / FFTW / VOLK entirely. The on-device DSP is now the
# clean-room `vibedsp` engine (RxPipeline) and the localhost HTTP/WebSocket +
# RTL-TCP networking uses the clean-room `net_shim`. The ONLY remaining native
# dependency is librtlsdr (prebuilt in the android-sdr-kit), needed to open the
# USB RTL-SDR and drive its hardware controls — that is itself GPL but is the
# device driver, linked as a shared lib, not bundled DSP.
#
# Prebuilt librtlsdr (+ rtl-sdr.h) comes from the android-sdr-kit, extracted to
# ./sdr-kit/<ABI>/{include,lib}.

cmake_minimum_required(VERSION 3.22)
project(vibelocalsdr)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Locations ---------------------------------------------------------------
set(SDR_KIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdr-kit" CACHE PATH "Prebuilt SDR deps root")
if (NOT EXISTS "${SDR_KIT_ROOT}/${ANDROID_ABI}/lib/librtlsdr.so")
    message(FATAL_ERROR "sdr-kit librtlsdr missing for ABI '${ANDROID_ABI}' at ${SDR_KIT_ROOT}")
endif()

# --- vibedsp: V5 clean-room GPL-free DSP -------------------------------------
# Static lib linked into the shim. Its vendored KissFFT symbols are PREFIXED
# (vibe_*) to avoid colliding with ft8_lib's own KissFFT, also linked into the
# shim. Plain C++17; no SDR++/GPL dependency.
# ★ ONE SOURCE LIST — see vibedsp/SOURCES. Do not list .cpp files here. ★★ The Android build was
# MISSING channelizer.cpp and zoomspec.cpp on 2026-08-02 and would have failed to link next; it
# reads the same list as the tests, the server and the iOS archive now.
# ★ Match only lines that LOOK LIKE FILENAMES (no spaces, ending .cpp). Comments and blanks
#   are therefore ignored WHATEVER they contain — a `;` in a comment used to split the line
#   in two (CMake lists are semicolon-separated) and the tail was read as a source file.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/vibedsp/SOURCES" VIBEDSP_NAMES REGEX "^[A-Za-z0-9_./-]+\\.cpp$")
set(VIBEDSP_SRC "")
foreach(f ${VIBEDSP_NAMES})
    list(APPEND VIBEDSP_SRC vibedsp/${f})
endforeach()
add_library(vibedsp STATIC
    ${VIBEDSP_SRC}
    vibedsp/third_party/kissfft/kiss_fft.c
    vibedsp/third_party/kissfft/kiss_fftr.c)
set_target_properties(vibedsp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# -O3 for the DSP hot loops (the RelWithDebInfo default is only -O2). AArch64 has
# mandatory NEON; the engine also uses explicit NEON intrinsics (simd_internal.h).
target_compile_options(vibedsp PRIVATE -O3 -ffp-contract=fast)
target_include_directories(vibedsp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vibedsp")
target_compile_definitions(vibedsp PRIVATE
    kiss_fft_alloc=vibe_kiss_fft_alloc kiss_fft=vibe_kiss_fft
    kiss_fft_stride=vibe_kiss_fft_stride kiss_fft_cleanup=vibe_kiss_fft_cleanup
    kiss_fft_next_fast_size=vibe_kiss_fft_next_fast_size
    kiss_fftr_alloc=vibe_kiss_fftr_alloc kiss_fftr=vibe_kiss_fftr kiss_fftri=vibe_kiss_fftri)

# --- VibeSDR local-SDR shim --------------------------------------------------
# Opens an RTL-SDR from a USB fd via librtlsdr (rtlsdr_open_sys_dev), runs the
# vibedsp engine, and serves the UberSDR protocol over net_shim. ft8_lib (kgoba,
# MIT) provides the FT8/FT4 core with its own self-contained KISS FFT.
file(GLOB FT8_LIB_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/ft8_lib/ft8/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ft8_lib/fft/kiss_fft.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ft8_lib/fft/kiss_fftr.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ft8_lib/common/monitor.c")

# ★★ libairspyhf, VENDORED AND PATCHED. Upstream (BSD-3) has no file-descriptor entry point,
# and Android's UsbManager hands you an fd and forbids enumeration — so a stock build cannot
# open the radio at all. cpp/libairspyhf/ adds airspyhf_open_fd() on libusb_wrap_sys_device(),
# exactly as librtlsdr's rtlsdr_open_sys_dev() does for dongles. See the note in airspyhf.c.
# ★ Compiled here rather than prebuilt: it is two small C files, and vendoring the source keeps
# the patch visible in the tree instead of buried in a binary nobody can inspect.
set(AIRSPYHF_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/libairspyhf/airspyhf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libairspyhf/iqbalancer.c")

add_library(vibelocalsdr SHARED
    vibe_localsdr_jni.cpp local_sdr_shim.cpp net_shim.cpp rtl_tcp_server.cpp
    mdns_responder.cpp
    # ★★★ COUNTRY AND NETWORK LOOKUP — THE SAME FILES THE DAEMON USES, not a copy. The admin page
    #     said "No country data yet" on Android for ever, and the Network column read "unknown",
    #     because these two were only ever compiled into the Linux/macOS daemon (Stuart,
    #     2026-08-22: "it should be built the same way as the Linux version which works").
    # ★★ Their DOWNLOAD half cannot work here — both shell out to curl, and asndb to gunzip — so
    #    the app fetches and calls geoip::ingest / asndb::ingest instead. That is the same division
    #    of labour as the EiBi schedule, which the app has always downloaded on this platform.
    # ★ Referenced in place rather than copied: one parser, one band of behaviour, and no second
    #   file to drift.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../vibeserver/geoip.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../vibeserver/asndb.cpp
    # ★ RadioDNS — the broadcaster's own station logo by PI code. Shared with the daemon; its curl
    #   transport is replaced here by VibeHttp (Android has no curl). See radiodns.h setFetcher.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../vibeserver/radiodns.cpp
    # ★ SCM_RIGHTS hand-off — how the front-door process gives an accepted socket to the radio.
    #   Full mode does not link without it, and nothing else references it, so its absence is
    #   invisible until the day Full mode is switched on.
    fd_passing.cpp
    # ★ BOTH radio sources, even though Android has no SDRplay. They are written as
    # "real implementation #ifdef <SDK> #else no-op stubs", and the shim references their
    # methods unconditionally — a runtime branch still emits a link-time symbol, so leaving
    # one out does not save anything, it just fails to link (found on iOS, 2026-07-27).
    sdrplay_source.cpp airspyhf_source.cpp
    ${AIRSPYHF_SRC}
    spyserver/spyserver_messages.cpp spyserver/spyserver_client.cpp
    decoders/fsk_decoder.cpp
    decoders/wefax_decoder.cpp
    decoders/ft8_decoder.cpp
    decoders/sstv_decoder.cpp
    decoders/audio_nr.cpp
    decoders/auto_notch.cpp
    decoders/time_decoder.cpp
    ${FT8_LIB_SRC})
set_target_properties(vibelocalsdr PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(vibelocalsdr PRIVATE -std=c++17 -O3 -ffp-contract=fast)
target_include_directories(vibelocalsdr PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/ft8_lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/vibedsp"
    "${SDR_KIT_ROOT}/${ANDROID_ABI}/include")

# ── libopus: compressed VibeServer audio (Opus), at parity with the macOS core ────────────────
# Vendored per-ABI static lib built by tools/build_opus_android.sh. VIBE_HAVE_OPUS gates the Opus
# path in local_sdr_shim.cpp; without the lib the shim falls back to raw PCM. Fail loudly if the
# ABI's lib is missing so a stale checkout can't silently ship the PCM fallback.
set(OPUS_A "${CMAKE_CURRENT_SOURCE_DIR}/opus/lib/${ANDROID_ABI}/libopus.a")
if (NOT EXISTS "${OPUS_A}")
    message(FATAL_ERROR "libopus missing for ABI '${ANDROID_ABI}' at ${OPUS_A} — run cpp/tools/build_opus_android.sh")
endif()
target_include_directories(vibelocalsdr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/opus/include")
target_compile_definitions(vibelocalsdr PRIVATE VIBE_HAVE_OPUS)

# ★ VIBE_AIRSPYHF_HAS_FD says this libairspyhf is the patched one. AirspyHfSource::openFd()
# refuses cleanly without it rather than pretending — a desktop linking Homebrew's stock build
# genuinely has no such entry point.
target_include_directories(vibelocalsdr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libairspyhf")
target_compile_definitions(vibelocalsdr PRIVATE VIBE_HAVE_AIRSPYHF VIBE_AIRSPYHF_HAS_FD)

target_link_libraries(vibelocalsdr PRIVATE
    vibedsp
    "${SDR_KIT_ROOT}/${ANDROID_ABI}/lib/librtlsdr.so"
    # libairspyhf needs libusb directly — the same one librtlsdr already uses, so the two
    # drivers share a single libusb rather than each dragging in a copy.
    "${SDR_KIT_ROOT}/${ANDROID_ABI}/lib/libusb1.0.so"
    "${OPUS_A}"
    log)
