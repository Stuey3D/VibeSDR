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
add_library(vibedsp STATIC
    vibedsp/fft.cpp vibedsp/ddc.cpp vibedsp/resampler.cpp vibedsp/stereo.cpp
    vibedsp/rds.cpp vibedsp/pipeline.cpp
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

add_library(vibelocalsdr SHARED
    vibe_localsdr_jni.cpp local_sdr_shim.cpp net_shim.cpp rtl_tcp_server.cpp
    mdns_responder.cpp
    spyserver/spyserver_messages.cpp spyserver/spyserver_client.cpp
    decoders/fsk_decoder.cpp
    decoders/wefax_decoder.cpp
    decoders/ft8_decoder.cpp
    decoders/sstv_decoder.cpp
    decoders/audio_nr.cpp
    decoders/auto_notch.cpp
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

target_link_libraries(vibelocalsdr PRIVATE
    vibedsp
    "${SDR_KIT_ROOT}/${ANDROID_ABI}/lib/librtlsdr.so"
    "${OPUS_A}"
    log)
