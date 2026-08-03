# VibeSDR V5 DSP — HOST test harness (dev machine only, not part of the app build).
#   cmake -S . -B build && cmake --build build && ./build/vibedsp_tests
cmake_minimum_required(VERSION 3.18)
project(vibedsp_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DSP ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ★ ONE SOURCE LIST — see vibedsp/SOURCES. Do not list .cpp files here; add them there and every
# build (app, tests, server, iOS) picks them up together.
# ★ Match only lines that LOOK LIKE FILENAMES (no spaces, ending .cpp). Comments and blanks
#   are therefore ignored WHATEVER they contain — a `;` in a comment used to split the line
#   in two (CMake lists are semicolon-separated) and the tail was read as a source file.
file(STRINGS ${DSP}/SOURCES DSP_NAMES REGEX "^[A-Za-z0-9_./-]+\\.cpp$")
set(DSP_SRC "")
foreach(f ${DSP_NAMES})
    list(APPEND DSP_SRC ${DSP}/${f})
endforeach()
list(APPEND DSP_SRC
    ${DSP}/third_party/kissfft/kiss_fft.c
    ${DSP}/third_party/kissfft/kiss_fftr.c)

add_executable(vibedsp_tests test_fft.cpp ${DSP_SRC})
target_include_directories(vibedsp_tests PRIVATE ${DSP})
target_compile_options(vibedsp_tests PRIVATE -O2 -Wall)

# ★★★ Fast convolution (ka9q's architecture): one forward FFT, channels by slice + inverse FFT.
# A wrong slice layout does not fail loudly — it tunes you somewhere else — so the test asserts
# the recovered tone's FREQUENCY and amplitude, not merely its presence.
# Both zoom methods must put the same tone in the same bin — that claim is what the VibeServer
# setup copy tells the operator, so a failure here means the COPY is wrong too.
add_executable(vibedsp_zoomspec_tests test_zoomspec.cpp ${DSP_SRC})
target_include_directories(vibedsp_zoomspec_tests PRIVATE ${DSP})
target_compile_options(vibedsp_zoomspec_tests PRIVATE -O2 -Wall)

add_executable(vibedsp_channelizer_tests test_channelizer.cpp ${DSP_SRC})
target_include_directories(vibedsp_channelizer_tests PRIVATE ${DSP})
target_compile_options(vibedsp_channelizer_tests PRIVATE -O2 -Wall)

add_executable(vibedsp_demod_tests test_demod.cpp ${DSP_SRC})
target_include_directories(vibedsp_demod_tests PRIVATE ${DSP})
target_compile_options(vibedsp_demod_tests PRIVATE -O2 -Wall)

add_executable(vibedsp_cfft_tests test_cfft.cpp ${DSP_SRC})
target_include_directories(vibedsp_cfft_tests PRIVATE ${DSP})
target_compile_options(vibedsp_cfft_tests PRIVATE -O2 -Wall)

add_executable(vibedsp_pipeline_tests test_pipeline.cpp ${DSP_SRC})
target_include_directories(vibedsp_pipeline_tests PRIVATE ${DSP})
target_compile_options(vibedsp_pipeline_tests PRIVATE -O2 -Wall)

add_executable(vibedsp_rds_tests test_rds.cpp ${DSP_SRC})
target_include_directories(vibedsp_rds_tests PRIVATE ${DSP})
target_compile_options(vibedsp_rds_tests PRIVATE -O2 -Wall)

add_executable(vibedsp_rds_dsp_tests test_rds_dsp.cpp ${DSP_SRC})
target_include_directories(vibedsp_rds_dsp_tests PRIVATE ${DSP})
target_compile_options(vibedsp_rds_dsp_tests PRIVATE -O2 -Wall)

# Adjacent-channel / alias rejection. The DDC cascade folds anything it fails to
# attenuate straight into the audio, where no later filter can remove it. Guard it.
add_executable(vibedsp_adjacent_tests test_adjacent.cpp ${DSP_SRC})
target_include_directories(vibedsp_adjacent_tests PRIVATE ${DSP})
target_compile_options(vibedsp_adjacent_tests PRIVATE -O2 -Wall)

# SSB passband: the Weaver sideband spans 0..bw, NOT +/-bw/2. Getting this wrong
# halves the voice bandwidth and nobody notices until they A/B another receiver.
add_executable(vibedsp_passband_tests test_passband.cpp ${DSP_SRC})
target_include_directories(vibedsp_passband_tests PRIVATE ${DSP})
target_compile_options(vibedsp_passband_tests PRIVATE -O2 -Wall)

# WFM stereo CPU bench. The MPX chain is the engine's most expensive path and it
# runs on the phone (local hardware) and on VibeServer, so keep it measurable.
# Fast convolution vs per-client DDC. The multi-listener demo lives or dies on the SLOPE
# (cost per extra listener), so measure it rather than trusting the brief.
add_executable(vibedsp_bench_channelizer bench_channelizer.cpp ${DSP_SRC})
target_include_directories(vibedsp_bench_channelizer PRIVATE ${DSP})
target_compile_options(vibedsp_bench_channelizer PRIVATE -O2 -Wall)

add_executable(vibedsp_bench_wfm bench_wfm.cpp ${DSP_SRC})
target_include_directories(vibedsp_bench_wfm PRIVATE ${DSP})
target_compile_options(vibedsp_bench_wfm PRIVATE -O2 -Wall)

# RDS weak-signal threshold. The other RDS tests use a clean signal and only prove the
# chain works; this one finds where it STOPS working, which is what users actually hit.
add_executable(vibedsp_rds_snr_probe rds_snr_probe.cpp ${DSP_SRC})
target_include_directories(vibedsp_rds_snr_probe PRIVATE ${DSP})
target_compile_options(vibedsp_rds_snr_probe PRIVATE -O2 -Wall)

# Offline IQ probe. The app records decoded AUDIO, which cannot show anything about RDS
# (57 kHz, discarded by the 15 kHz audio filter). This runs the real pipeline over an
# rtl_sdr capture so a real station can be diagnosed repeatedly, off the air.
add_executable(vibedsp_iq_probe iq_probe.cpp ${DSP_SRC})
target_include_directories(vibedsp_iq_probe PRIVATE ${DSP})
target_compile_options(vibedsp_iq_probe PRIVATE -O2 -Wall)
