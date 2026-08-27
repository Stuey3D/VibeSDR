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
# rtl_tcp. Adding real USB later (briefs/BRIEF-vibeserver-macos.md §2, vendored libusb) is additive.
#
#   cmake -S vibeserver -B vibeserver/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build vibeserver/build -j
#   ./vibeserver/build/vibeserver --tcp 127.0.0.1:1234

cmake_minimum_required(VERSION 3.20)
# ★★★ 3.1.0 — the weak-signal release (2026-08-15). One version drives BOTH products: the Debian
#     package takes its upstream version from here, and the macOS app's plist is bumped alongside
#     it. They must not drift: a Pi reporting 3.1.0 while the Mac says 3.0.2 makes every "which
#     version are you on?" answer ambiguous, which is the question every fault report starts with.
project(vibeserver VERSION 4.1.44 LANGUAGES CXX C)
# ★ The single source of truth for the version. macOS shipped as VibeServer 1.0.0 (notarised),
#   so Linux starts level with it rather than inventing a separate number for the same server.
set(VIBESERVER_VERSION "${PROJECT_VERSION}")

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
# ★ ONE SOURCE LIST — see vibedsp/SOURCES. Do not list .cpp files here.
# ★ Match only lines that LOOK LIKE FILENAMES (no spaces, ending .cpp). Comments and blanks
#   are therefore ignored WHATEVER they contain — a `;` in a comment used to split the line
#   in two (CMake lists are semicolon-separated) and the tail was read as a source file.
file(STRINGS "${CPP_DIR}/vibedsp/SOURCES" VIBEDSP_NAMES REGEX "^[A-Za-z0-9_./-]+\\.cpp$")
set(VIBEDSP_SRC "")
foreach(f ${VIBEDSP_NAMES})
    list(APPEND VIBEDSP_SRC "${CPP_DIR}/vibedsp/${f}")
endforeach()
add_library(vibedsp STATIC
    ${VIBEDSP_SRC}
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
    # ★ The DAEMON owns config persistence, not the shared core — local_sdr_shim.cpp is compiled
    #   into the Android and iOS apps too, and a phone has no /etc to write. See vibeserver_config.h.
    "${CMAKE_CURRENT_SOURCE_DIR}/vibeserver_config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/eibi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/solar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/geoip.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/asndb.cpp"
    # ★ Guarantees a radio process cannot outlive the front door. A no-op on Linux (the kernel
    #   does it via PR_SET_PDEATHSIG); the real work is macOS, where nothing survives exec.
    "${CMAKE_CURRENT_SOURCE_DIR}/parent_watch.cpp"
    # ★★ RADIO DETECTION BELONGS TO THE CORE, NOT THE CLI. It lived in the executable only, which
    #    was fine while nothing else asked what is attached — but the Mac .app links the core and
    #    Full mode's radio list (vs_radio_*) needs exactly this. Leaving it in the CLI made
    #    vibeserver_core reference symbols it did not contain: the CLI still linked (it supplied
    #    them itself) and the .APP would have failed, i.e. the break would have surfaced on the
    #    platform being ported to, not on the one being built.
    "${CMAKE_CURRENT_SOURCE_DIR}/radios.cpp"
    # ★ Station logos from RadioDNS, looked up by PI code. Server-side because a browser can make
    #   neither the DNS SRV query nor the cross-origin fetch. Credit: radiodns.org
    "${CMAKE_CURRENT_SOURCE_DIR}/radiodns.cpp"
    # ★ Listing this machine in the public directory at vibeserver.vibesdr.net. In the CORE, not
    #   the CLI, for the same reason radios.cpp is: the .app needs it too.
    "${CMAKE_CURRENT_SOURCE_DIR}/directory.cpp"
    "${CPP_DIR}/local_sdr_shim.cpp"
    "${CPP_DIR}/net_shim.cpp"
    # ★ Hands a live connection to another process, so several radios can share one
    #   forwarded port without anything being proxied. See fd_passing.h.
    "${CPP_DIR}/fd_passing.cpp"
    "${CPP_DIR}/mdns_responder.cpp"
    "${CPP_DIR}/sdrplay_source.cpp"
    "${CPP_DIR}/airspyhf_source.cpp"
    "${CPP_DIR}/hackrf_source.cpp"
    "${CPP_DIR}/spyserver/spyserver_client.cpp"
    "${CPP_DIR}/spyserver/spyserver_messages.cpp"
    "${CPP_DIR}/decoders/fsk_decoder.cpp"
    "${CPP_DIR}/decoders/wefax_decoder.cpp"
    "${CPP_DIR}/decoders/ft8_decoder.cpp"
    "${CPP_DIR}/decoders/sstv_decoder.cpp"
    "${CPP_DIR}/decoders/audio_nr.cpp"
    "${CPP_DIR}/decoders/auto_notch.cpp"
    "${CPP_DIR}/decoders/time_decoder.cpp"
    ${FT8_SRC})
target_include_directories(vibeserver_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CPP_DIR}" "${CPP_DIR}/vibedsp" "${CPP_DIR}/ft8_lib")
target_link_libraries(vibeserver_core PUBLIC vibedsp)

# ── the CLI harness ───────────────────────────────────────────────────────────
# ★ The TUI is part of the SAME binary, not a second one: Stuart's requirement is "SSH in, type
# vibeserver, and it pops up". A separate `vibeserver-tui` would be one more name to remember.
# See tui.cpp for how one binary is both — no arguments AND a terminal means a human typed it.
if(UNIX AND NOT APPLE)
  # ★★★ THE WIDE BUILD, OR UTF-8 CANNOT WORK AT ALL. Plain libncurses is byte-oriented: it has no
  #     idea a three-byte sequence is one character, so it escapes each byte and an em-dash renders
  #     as "?~@~T" mid-sentence — which reads as corrupted memory, not an encoding problem.
  #     setlocale() alone does NOT fix it; the library has to be ncursesw.
  set(CURSES_NEED_NCURSES TRUE)
  set(CURSES_NEED_WIDE TRUE)
  find_package(Curses)
endif()
add_executable(vibeserver
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rtl_eeprom.cpp"
    $<$<BOOL:${CURSES_FOUND}>:${CMAKE_CURRENT_SOURCE_DIR}/tui.cpp>)
if(CURSES_FOUND)
  target_compile_definitions(vibeserver PRIVATE VIBE_HAVE_TUI)
  target_include_directories(vibeserver PRIVATE ${CURSES_INCLUDE_DIRS})
  target_link_libraries(vibeserver PRIVATE ${CURSES_LIBRARIES})
  message(STATUS "TUI: enabled (ncurses)")
elseif(VIBESERVER_STRICT_RADIOS AND NOT APPLE)
  # ★ NOT ON macOS. The wizard is the LINUX front door — the Mac ships a GUI app (VibeServerApp)
  #   and has never had ncurses, so failing a strict Mac build here would only block the strict
  #   flag from being switched on for the thing it IS needed for: the radio list.
  # ★★★ A RELEASE WITHOUT THE WIZARD IS NOT A RELEASE. Losing ncurses does not fail the build, it
  #     quietly removes the ONE command the setup instructions tell a new owner to type: `vibeserver`
  #     then falls through to daemon mode, prints a wall of server startup lines, and fights the
  #     running service for the radio. The owner sees "could not open the Airspy HF+ (is another
  #     program using it?)" and concludes their hardware or their install is broken.
  #
  # ★★ SHIPPED EXACTLY THAT as 3.0.0-2 (2026-08-08). The build root had been rebuilt from scratch
  #    that evening and libncursesw-dev was not among the packages put back — the only trace was a
  #    cmake STATUS line in a build nobody was reading. Same failure as the SDRplay headers, which
  #    is what this flag was added for; it simply did not cover the TUI.
  #
  # ★ So the strict flag now means "a release build contains everything a release contains", not
  #   just the radios. Anything else that can be silently omitted belongs under it too.
  message(FATAL_ERROR
          "ncurses (wide) not found, and VIBESERVER_STRICT_RADIOS is on.\n"
          "  The settings wizard would be silently omitted and `vibeserver` would start a daemon\n"
          "  instead of the setup screen. In the build root:\n"
          "    apt-get install libncursesw5-dev")
else()
  message(STATUS "TUI: not built (ncurses not found)")
endif()
# ── librtlsdr + libusb: the dongle, driven DIRECTLY ───────────────────────────
# STATIC on purpose. A user drags one .app out of a DMG and it works — no Homebrew, no dylib
# hunting, nothing to install. Prefer the static archives; fall back to whatever is found so a
# dev machine still configures.
# ★ STATIC ONLY. Restrict find_library to .a archives so it CANNOT pick a Homebrew dylib — a notarised
# hardened-runtime app that links one crashes on launch for everyone (absent without Homebrew; rejected
# for a Team-ID mismatch even on the dev box). The old NAMES-based find preferred the .dylib and the
# swap-to-.a fallback was fragile; this is bulletproof.
# ★★★ STATIC IS A macOS RULE, NOT A UNIVERSAL ONE. Everything above is about shipping ONE .app out
# of a DMG with nothing to install — a notarised hardened-runtime binary that links a Homebrew dylib
# crashes on launch for everyone. NONE of that applies on Linux, where the package manager owns the
# libraries and the .deb simply declares Depends:.
# ★★ And forcing static there actively BREAKS: Debian's libusb-1.0.a needs libudev symbols that are
# not pulled in, so the link fails with a wall of undefined `udev_*` references (Raspberry Pi,
# 2026-07-31). Shared linking is both idiomatic and the only thing that works.
set(_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
if(APPLE)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()
# ★★★ PREFER OUR OWN librtlsdr ON LINUX, when the build image has one. Debian's is 2.0.2 and does
#     not know the RTL-SDR Blog V4L, so hardware support would otherwise arrive on the
#     distribution's release cycle rather than ours — and the package would be at the mercy of
#     whatever each user's distro happened to ship. Android vendors its .so and macOS links
#     statically; this makes Linux the same, and everything except SDRplay (whose API is theirs and
#     is dlopen'd) self-contained on all three.
# ★ Falls back to the system library when the prefix is absent, so a plain `cmake` on a dev box
#   still configures exactly as before.
set(VIBE_RTLSDR_PREFIX "/opt/vibe-rtlsdr")
if(UNIX AND NOT APPLE AND EXISTS "${VIBE_RTLSDR_PREFIX}/lib/librtlsdr.so")
  set(VIBE_BUNDLED_RTLSDR ON)
  message(STATUS "librtlsdr: BUNDLED from ${VIBE_RTLSDR_PREFIX}")
endif()

find_library(RTLSDR_LIB NAMES rtlsdr HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_library(USB_LIB    NAMES usb-1.0 HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${_saved_suffixes})
find_path(RTLSDR_INC rtl-sdr.h HINTS /opt/homebrew/include /usr/local/include /usr/include)
if(VIBE_BUNDLED_RTLSDR)
  # ★ Ours wins over anything found above — headers AND library, so the two cannot disagree about
  #   the ABI. A mismatched pair is the sort of fault that only shows on the one dongle you do not
  #   own.
  set(RTLSDR_LIB "${VIBE_RTLSDR_PREFIX}/lib/librtlsdr.so")
  set(RTLSDR_INC "${VIBE_RTLSDR_PREFIX}/include")
endif()
if(NOT RTLSDR_LIB OR NOT USB_LIB OR NOT RTLSDR_INC)
  message(FATAL_ERROR "static librtlsdr/libusb not found. On macOS: brew install librtlsdr libusb")
endif()
message(STATUS "librtlsdr: ${RTLSDR_LIB}")
message(STATUS "libusb:    ${USB_LIB}")

target_include_directories(vibeserver_core PUBLIC "${RTLSDR_INC}")
# Use the REAL rtl-sdr.h, not the iOS no-op stub the shim falls back to.
target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_LIBRTLSDR)
# ★★ THE VERSION, WHERE THE CODE CAN SAY IT. It lived only in CMake, so the server could not tell a
#    client what it was — and the app's About panel had nothing to show but the client's own name.
#    Defined from VIBESERVER_VERSION so it cannot drift from the package the user installed.
target_compile_definitions(vibeserver_core PUBLIC VIBESERVER_VERSION_STR="${VIBESERVER_VERSION}")
target_link_libraries(vibeserver_core PUBLIC "${RTLSDR_LIB}" "${USB_LIB}")

# ── SDRplay (RSP): HEADERS ONLY, deliberately never linked ────────────────────
# ★★ The API is a separate install living in /usr/local/lib, and it ships as a DYLIB with no
# static archive. Linking it would repeat exactly the failure the librtlsdr block above warns
# about: a notarised hardened-runtime app that hard-links a dylib from /usr/local CRASHES ON
# LAUNCH for everyone who does not have that dylib — which is almost every user, since almost
# nobody owns an RSP. Breaking the app for the majority to serve the minority is the wrong
# trade, and it is avoidable: sdrplay_source.cpp dlopen()s the library on first use.
# So we take the HEADERS (for the structs and typedefs) and link NOTHING. A machine without
# the API simply reports zero SDRplay devices.
find_path(SDRPLAY_INC sdrplay_api.h HINTS /usr/local/include /Library/SDRplayAPI/3.15.1/include)
if(SDRPLAY_INC)
  message(STATUS "SDRplay headers: ${SDRPLAY_INC} (runtime dlopen, not linked)")
  target_include_directories(vibeserver_core PUBLIC "${SDRPLAY_INC}")
  target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_SDRPLAY)
elseif(VIBESERVER_STRICT_RADIOS)
  # ★★★ A RELEASE MUST NEVER BE QUIETLY MISSING A RADIO. This branch used to just print a note
  #     and carry on, and the note scrolled past in a build log nobody reads. Moving packaging
  #     into a clean bookworm chroot — which has no SDRplay headers, because the API is a
  #     separate closed-source install — therefore SHIPPED two releases with RSP support silently
  #     compiled out. Every RSP owner who installed them got "no radio found" from a receiver
  #     that had, on the previous version, worked perfectly.
  # ★★ The headers are just structs and typedefs and nothing is linked, so the fix is only to put
  #    them in the build root. But the guard is the point: packaging fails loudly instead.
  message(FATAL_ERROR
          "SDRplay headers not found, and VIBESERVER_STRICT_RADIOS is on.\n"
          "A package built now would report 'no radio' to every RSP owner.\n"
          "Copy sdrplay_api*.h into the build root's /usr/local/include and try again.")
else()
  message(STATUS "SDRplay headers not found — building without RSP support")
endif()

# ── libairspyhf: Airspy HF+ Discovery / Dual Port ─────────────────────────────
# ★★ STATIC, AND THAT IS THE WHOLE POINT — the opposite of the SDRplay block above. libairspyhf
# is BSD-3-Clause and ships a real .a, so it links straight in: the radio is plug-and-play with
# nothing for the user to install, and there is no dylib to be missing at launch. The RSP needs
# dlopen only because its API is closed-source and separately installed; that objection simply
# does not apply here.
# ★ NOT libairspy. An Airspy R2 or Mini speaks a different API entirely (see airspyhf_source.h),
# which is also GPL-2.0 and deprecated upstream after a relicence. Different decision, not a
# flag on this one.
# ★★★ BUILD OUR VENDORED COPY, NOT HOMEBREW'S. Both platforms must carry the same libairspyhf
# or a fix lands on one and not the other — which is exactly what happened: upstream 1.6.8's
# airspyhf_close() frees USB transfers AFTER destroying the context that owns them, a
# use-after-free that is fatal and reproducible on Android and merely lucky on a desktop. The
# Android build was patched and the Mac kept linking the unpatched Homebrew archive
# (Stuart, 2026-07-27: "all these fixes if needed need to be in the mac version").
# ★ Same source, same patches, one place to fix anything found next. Homebrew's remains the
# fallback so a checkout without the vendored tree still builds.
set(AHF_VENDOR "${CPP_DIR}/libairspyhf")
if(EXISTS "${AHF_VENDOR}/airspyhf.c")
  message(STATUS "libairspyhf: vendored (patched) at ${AHF_VENDOR}")
  target_sources(vibeserver_core PRIVATE
    "${AHF_VENDOR}/airspyhf.c" "${AHF_VENDOR}/iqbalancer.c")
  target_include_directories(vibeserver_core PUBLIC "${AHF_VENDOR}")
  # ★ libairspyhf.c includes <libusb.h> unqualified; Homebrew puts it in libusb-1.0/.
  find_path(LIBUSB_INC libusb.h PATH_SUFFIXES libusb-1.0
            HINTS /opt/homebrew/include /usr/local/include /usr/include)
  if(LIBUSB_INC)
    target_include_directories(vibeserver_core PRIVATE "${LIBUSB_INC}")
  endif()
  target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_AIRSPYHF VIBE_AIRSPYHF_HAS_FD)
  set(AIRSPYHF_VENDORED TRUE)
endif()

# ── libhackrf: HackRF One (EXPERIMENTAL — this is the DESKTOP half) ───────────
# ★★★ NOBODY HERE OWNS ONE. The driver is written against libhackrf's API and has never been run
#     against a radio — exactly one listener can test it. That is why it is Experimental, why it is
#     mentioned only in the README and release notes, and why no VibeAGC is wired to it.
# ★★★ AND WHY IT IS STRICT ANYWAY. Normally an experimental extra would be "build it if the library
#     happens to be there" — but the person who would notice it missing is the ONE person who can
#     test it, and they would be testing a binary without the feature while reporting on it. Worse,
#     that is precisely how 3.0.0-2 shipped with the setup wizard compiled out: a rebuilt build root
#     lost a -dev package and the only trace was a cmake STATUS line nobody read. So on a release
#     build its absence is an error, not a shrug. See the ncurses block above for the original.
# ★★★ ANDROID DOES NOT COME THROUGH HERE. It builds its own copy from
#     cpp/libhackrf/ — a VENDORED, PATCHED libhackrf with an fd entry point, because UsbManager
#     hands the app an already-open descriptor and forbids enumeration. See
#     android/app/src/main/cpp/CMakeLists.txt. This block is the desktop's system/Homebrew
#     libhackrf, which is stock and has NO hackrf_open_fd — hence VIBE_HACKRF_HAS_FD, which is
#     set there and deliberately not here.
find_library(HACKRF_LIB NAMES hackrf HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_path(HACKRF_INC libhackrf/hackrf.h HINTS /opt/homebrew/include /usr/local/include /usr/include)
# ★★★ PREFER THE STATIC ARCHIVE ON macOS, exactly as the librtlsdr and libairspyhf blocks do — a
#     cached dylib path is how a notarised build ships broken. Homebrew's libhackrf.a needs only
#     libusb, which this target already links, so nothing new has to be bundled or re-signed.
if(APPLE AND HACKRF_LIB)
  get_filename_component(_hrf_dir "${HACKRF_LIB}" DIRECTORY)
  if(EXISTS "${_hrf_dir}/libhackrf.a")
    set(HACKRF_LIB "${_hrf_dir}/libhackrf.a")
  endif()
endif()
if(HACKRF_LIB AND HACKRF_INC)
  message(STATUS "libhackrf: ${HACKRF_LIB} (HackRF support: EXPERIMENTAL)")
  target_include_directories(vibeserver_core PUBLIC "${HACKRF_INC}")
  target_link_libraries(vibeserver_core PUBLIC "${HACKRF_LIB}")
  target_compile_definitions(vibeserver_core PUBLIC VIBE_HAS_HACKRF)
elseif(VIBESERVER_STRICT_RADIOS AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
  message(FATAL_ERROR
          "libhackrf not found, and VIBESERVER_STRICT_RADIOS is on.\n"
          "  A release build must contain every radio a release claims to contain — and the one\n"
          "  person who can test this radio cannot tell a missing driver from a broken one.\n"
          "  Linux: install libhackrf-dev (it is in vibeserver/linux/Dockerfile.build).\n"
          "  macOS: brew install hackrf.\n"
          "  Or build with -DVIBESERVER_STRICT_RADIOS=OFF for a local build that does not need it.")
else()
  message(STATUS "libhackrf: not found — building without HackRF support")
endif()

find_library(AIRSPYHF_LIB NAMES airspyhf libairspyhf.a HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_path(AIRSPYHF_INC libairspyhf/airspyhf.h HINTS /opt/homebrew/include /usr/local/include /usr/include)
if(AIRSPYHF_VENDORED)
  # already handled above
elseif(AIRSPYHF_LIB AND AIRSPYHF_INC)
  # Prefer the static archive next to whatever find_library cached, exactly as the librtlsdr and
  # libopus blocks do — a cached dylib path is how a notarised build ships broken.
  get_filename_component(_ahf_dir "${AIRSPYHF_LIB}" DIRECTORY)
  if(APPLE AND EXISTS "${_ahf_dir}/libairspyhf.a")   # ★ macOS only
    set(AIRSPYHF_LIB "${_ahf_dir}/libairspyhf.a")
  endif()
  message(STATUS "libairspyhf: ${AIRSPYHF_LIB}")
  target_include_directories(vibeserver_core PUBLIC "${AIRSPYHF_INC}")
  target_link_libraries(vibeserver_core PUBLIC "${AIRSPYHF_LIB}")
  target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_AIRSPYHF)
else()
  message(STATUS "libairspyhf not found — building without Airspy HF+ support")
endif()

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
if(APPLE AND EXISTS "${_opus_dir}/libopus.a")   # ★ macOS only — see the suffix block above
  set(OPUS_LIB "${_opus_dir}/libopus.a")
endif()
message(STATUS "libopus:   ${OPUS_LIB}")
target_include_directories(vibeserver_core PUBLIC "${OPUS_INC}")
target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_OPUS)
target_link_libraries(vibeserver_core PUBLIC "${OPUS_LIB}")
target_link_libraries(vibeserver PRIVATE vibeserver_core)

# ★★ THE RADIO-LIST API, UNDER TEST. Unlike the tests in scripts/run-tests.sh — which compile a
#    single .cpp with a named handful of deps — this one needs the WHOLE core linked (it probes
#    real drivers), so it has to be a CMake target rather than a standalone g++ line.
#    ★ What it is really guarding is the out-of-range answers: a GUI asks about radio N while the
#      list is being rescanned underneath it, and a wrong answer there is a serial pointed at the
#      wrong hardware, not a crash.
add_executable(test-radio-api "${CMAKE_CURRENT_SOURCE_DIR}/test-radio-api.cpp")
target_link_libraries(test-radio-api PRIVATE vibeserver_core)
if(APPLE)
  # libusb on macOS talks to IOKit; these are system frameworks, nothing to ship.
  target_link_libraries(vibeserver_core PUBLIC
      "-framework IOKit" "-framework CoreFoundation" "-framework Security" objc)
endif()
find_package(Threads REQUIRED)
target_link_libraries(vibeserver_core PUBLIC Threads::Threads)

# ── Linux install + .deb packaging ────────────────────────────────────────────
# ★★ A RELEASE IS NOT A BINARY. A working install is FOUR files — the binary, the systemd unit,
# the udev rules and a default config — plus a user in `plugdev` and a udev reload. Every one of
# those was done BY HAND on the Pi, which is fine for us and hopeless for anyone else: the
# audience here is people leaving a Pi in a loft or on a solar panel, not people who want to
# debug a group membership. Package it or it is not released.
# ★ macOS ships as a notarised .app (see mac/build-app.sh) and wants none of this.
if(UNIX AND NOT APPLE)
  include(GNUInstallDirs)
  install(TARGETS vibeserver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(VIBE_BUNDLED_RTLSDR)
    # ★★ SHIPPED BESIDE US, NOT OVER THE SYSTEM'S. /usr/lib/vibeserver is ours; Debian's
    #    librtlsdr0 stays exactly where it is for everything else on the machine, and `apt purge`
    #    takes only our copy.
    # ★ Follow the symlink chain: the build installs librtlsdr.so -> .so.0 -> .so.0.x.y, and the
    #   loader wants the SONAME (.so.0). Installing the real file plus that one link is enough.
    file(GLOB _vibe_rtlsdr_files "${VIBE_RTLSDR_PREFIX}/lib/librtlsdr.so*")
    install(FILES ${_vibe_rtlsdr_files} DESTINATION lib/vibeserver)
    # ★★★ $ORIGIN, NOT AN ABSOLUTE PATH. The binary installs to /usr/bin, so ../lib/vibeserver
    #     resolves to /usr/lib/vibeserver wherever the package is unpacked — and it is searched
    #     BEFORE the default paths, so our copy wins over the distribution's without replacing it.
    set_target_properties(vibeserver PROPERTIES
      INSTALL_RPATH "$ORIGIN/../lib/vibeserver"
      BUILD_WITH_INSTALL_RPATH FALSE)
  endif()
  install(FILES linux/vibeserver.service DESTINATION lib/systemd/system)
# ★ The per-radio template and the reconciler that starts the second and later radios.
install(FILES linux/vibeserver@.service linux/vibeserver-radios.service
        DESTINATION lib/systemd/system)
  # ★ /lib/udev/rules.d is the PACKAGE path; /etc/udev/rules.d is for the ADMIN's own overrides.
  #   Shipping into /etc would fight the sysadmin and survive `apt purge`.
  install(FILES linux/99-vibeserver-sdr.rules DESTINATION lib/udev/rules.d)
  # ★★ THE MAINTENANCE BUTTONS' PRIVILEGE. The service runs unprivileged AND with
  #    NoNewPrivileges=yes, so it cannot elevate at all — sudo is impossible by design here. The
  #    daemon writes a request file instead; the .path unit notices it and runs the helper as
  #    root. See linux/vibeserver-maintenance for the permitted actions.
  install(PROGRAMS linux/vibeserver-maintenance DESTINATION lib/vibeserver)
  install(FILES linux/vibeserver-maintenance.path
                linux/vibeserver-maintenance.service
          DESTINATION lib/systemd/system)
  # ★ Run as root from the unit's ExecStartPre — the daemon is unprivileged and cannot write
  #   sysfs. See linux/vibeserver-governor for why a wrong governor costs 25% of the machine.
  install(PROGRAMS linux/vibeserver-governor linux/vibeserver-radios DESTINATION lib/vibeserver)

  # ★★★ SHIP cloudflared, DO NOT ASK FOR IT. The public listing offers to make an address for an
  #     owner who has no port forward — the only route that works behind CGNAT — and that offer is
  #     empty if it depends on a binary Debian does not carry. "Install cloudflared first" is a
  #     dead end on a Pi in a loft (Stuart, 2026-08-23: "I thought we were bundling cloudflare").
  #  ★★ It cannot be a Depends: line for exactly that reason — there is no such package to depend
  #     ON — so it travels inside ours, in lib/vibeserver beside the other helpers.
  #  ★★ APACHE-2.0, so redistribution is permitted WITH THE LICENCE ALONGSIDE. The licence is
  #     installed with it; a bundled binary without one is how a permitted act becomes an
  #     infringing one.
  #  ★ Absent = a package WITHOUT the tunnel rather than a broken build: someone building from a
  #    fresh clone has not run fetch.sh, and their server should still compile and run.
  set(_vibe_cf_src "${CMAKE_CURRENT_SOURCE_DIR}/../tools/cloudflared-desktop/bin")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_vibe_cf "${_vibe_cf_src}/cloudflared-linux-arm64")
  else()
    set(_vibe_cf "${_vibe_cf_src}/cloudflared-linux-amd64")
  endif()
  if(EXISTS "${_vibe_cf}")
    install(PROGRAMS "${_vibe_cf}" DESTINATION lib/vibeserver RENAME cloudflared)
    install(FILES "${_vibe_cf_src}/LICENSE" DESTINATION share/doc/vibeserver
            RENAME LICENSE.cloudflared)
    message(STATUS "bundling cloudflared: ${_vibe_cf}")
  else()
    message(WARNING "cloudflared not bundled (run tools/cloudflared-desktop/fetch.sh) — "
                    "the public listing will need one on PATH")
  endif()
  # ★★ NOT a plain install() — the config is the one file the user edits, and apt must never
  #   clobber it on upgrade. Staged elsewhere and copied by postinst ONLY if absent.
  install(FILES linux/vibeserver.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/vibeserver)

  set(CPACK_GENERATOR "DEB")
  set(CPACK_PACKAGE_NAME "vibeserver")
  set(CPACK_PACKAGE_VENDOR "Stuart Carr")
  set(CPACK_PACKAGE_CONTACT "https://github.com/Stuey3D/VibeSDR/issues")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "VibeServer — turn an SDR into a network receiver for VibeSDR and any browser")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://vibesdr.net")
  set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  # ★ Read from the same place as everything else rather than repeating the number here.
  # ★★★ PLUS A DEBIAN REVISION. `apt install` is a SILENT NO-OP when the installed version already
  #   matches — it says "already the newest version" and exits 0. A rebuilt package with an
  #   unchanged version therefore does not install, and on an APT REPOSITORY that means nobody ever
  #   receives the update, with nothing anywhere reporting it. scripts/publish-apt.sh derives the
  #   next revision from what is already in the pool and passes it here, so every publish is a
  #   version apt will actually act on. Local builds default to -1 and need not care.
  set(VIBESERVER_DEB_REV "1" CACHE STRING "Debian package revision (publish-apt.sh sets this)")
  set(CPACK_PACKAGE_VERSION "${VIBESERVER_VERSION}-${VIBESERVER_DEB_REV}")
  # ★★ ASK dpkg WHAT WE LINKED rather than hand-listing packages: shlibdeps reads the ELF and
  #   gets the versions right for the distro doing the build. A hand-written list rots the first
  #   time a dependency is added and produces a package that installs and then will not start.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  if(VIBE_BUNDLED_RTLSDR)
    # ★★★ TELL shlibdeps WHERE OUR OWN LIBRARY LIVES, or it resolves the ELF's NEEDED entry for
    #     librtlsdr.so.0 against the SYSTEM package and adds `librtlsdr0` as a dependency — which
    #     would drag in the very 2.0.2 we are bundling around, and make the package refuse to
    #     install anywhere that lacks it. With the private dir declared, dpkg-shlibdeps recognises
    #     the library as one we ship and says nothing about it.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${CMAKE_INSTALL_PREFIX}/lib/vibeserver")
  endif()
  # ★★★ curl IS A REAL DEPENDENCY AND shlibdeps CANNOT SEE IT. The EiBi schedule is fetched by
  #   shelling out to curl (see vibeserver/eibi.cpp), and shlibdeps only reads the ELF — it knows
  #   nothing about programs we exec. On a Raspberry Pi OS Lite image with no curl, the package
  #   would install, the server would run, and the station schedule would silently never arrive:
  #   a failure that looks like a missing feature rather than a missing package.
  #   ★ The general trap: adding a shell-out adds an undeclared dependency, and NOTHING in the
  #     build will tell you. Anything exec()d belongs in this line.
  #   ★★ gzip JOINED THE LIST FOR 2.0, and it is the exact trap the paragraph above describes:
  #      the IP-to-network data (asndb.cpp) is fetched with `curl ... | gunzip -c`. gzip is
  #      Priority: required on Debian so it is present in practice on every real image — but
  #      "present in practice" is how an undeclared dependency hides until the one image that
  #      lacks it, and the failure mode is silent (no country/network data, which reads as a
  #      missing feature).
  #   ★ NOT declared, deliberately: systemctl, shutdown and apt-get. Those are exec'd only by the
  #     admin maintenance buttons, only when running as root, and they belong to systemd and apt —
  #     which this package already requires by shipping a .service unit. Declaring `systemd` would
  #     also wrongly refuse to install on a container image where the binary is still useful.
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "adduser, curl, gzip")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${CMAKE_CURRENT_SOURCE_DIR}/linux/debian/postinst"
      "${CMAKE_CURRENT_SOURCE_DIR}/linux/debian/prerm"
      "${CMAKE_CURRENT_SOURCE_DIR}/linux/debian/postrm")
  include(CPack)
endif()
