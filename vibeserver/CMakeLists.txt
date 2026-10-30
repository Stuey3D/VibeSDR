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
project(vibeserver VERSION 12.0.0 LANGUAGES CXX C)
# ★★★ V11 (2026-09-28): the app, Jr, every VibeServer host, Lite and the Mac app all move to major 11
#     together (docs/BRIEF-v11-compatibility.md). A PRE-RELEASE is named on its OWN line, read by every
#     reader of the line above (android CMake, lite gradle, mac/build-app.sh): the package becomes
#     11.0.0~b1, which dpkg sorts ABOVE 5.6.x and BELOW 11.0.0, so both upgrades are clean.
#     ★ Empty it for the release. Never publish a ~ version AFTER its release — it would sort below it.
set(VIBESERVER_PRERELEASE "beta47")

# ── ★★★ SANITISER BUILDS FOR THE TESTS AND THE OFFLINE REPLAY ────────────────────────────────
#
#   cmake -S vibeserver -B vibeserver/build-san -DVIBE_SANITIZE=ON && cmake --build ... && ctest
#   scripts/sanitize-tests.sh   does both, plus the vibedsp suites
#
# ★★★ THE TESTS AND `dab-offline` ONLY — NEVER THE SERVER. ASan doubles the memory and costs
#     2-3x; a Pi already runs its DSP near real time, so a sanitised server would drop buffers and
#     "find" faults that are its own slowness. The offline replay is where this pays: the SAME
#     decoder over a captured air file (VIBE_IQ_DUMP), no clock to keep, so a read one byte past
#     an FIB or a shift by 32 is caught AT THE INSTRUCTION rather than three frames later as a
#     mysterious burst of errors — which is exactly how the five DSP bugs of 2026-09-07 presented.
# ★★ UBSan does NOT recover: the first signed overflow or misaligned load fails the test then and
#    there, with a stack. A recovered warning in a thousand-line log is a warning nobody reads.
# ★ Not on by default. It is a deliberate run, and a release build must never carry these.
option(VIBE_SANITIZE "Build the tests and dab-offline with AddressSanitizer + UBSan" OFF)
set(VIBE_SAN_OPTS "")
if(VIBE_SANITIZE)
  set(VIBE_SAN_OPTS -fsanitize=address,undefined -fno-sanitize-recover=undefined
                    -fno-omit-frame-pointer -g)
  message(STATUS "VIBE_SANITIZE=ON — tests and dab-offline carry ASan + UBSan (the server does NOT)")
endif()
function(vibe_sanitize tgt)
  if(VIBE_SANITIZE)
    target_compile_options(${tgt} PRIVATE ${VIBE_SAN_OPTS})
    target_link_options(${tgt} PRIVATE ${VIBE_SAN_OPTS})
  endif()
endfunction()

# ★ The single source of truth for the version. macOS shipped as VibeServer 1.0.0 (notarised),
#   so Linux starts level with it rather than inventing a separate number for the same server.
set(VIBESERVER_VERSION "${PROJECT_VERSION}")
if(VIBESERVER_PRERELEASE)
  set(VIBESERVER_VERSION "${PROJECT_VERSION}~${VIBESERVER_PRERELEASE}")
endif()

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
# ★ Re-read it when it changes. file(STRINGS) is read once at configure time, so a NEW source added
#   to SOURCES (mpxmeasure.cpp, 2026-09-29) left an existing build dir linking without it — undefined
#   MpxMeasure symbols — until someone re-ran cmake by hand. build-app.sh reuses its build dir.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CPP_DIR}/vibedsp/SOURCES")
set(VIBEDSP_SRC "")
foreach(f ${VIBEDSP_NAMES})
    list(APPEND VIBEDSP_SRC "${CPP_DIR}/vibedsp/${f}")
endforeach()
# ── Linux: NOTHING FROM THE SYSTEM BUT glibc (Stuart, 2026-10-18) ────────────────────────────
# ★★★ THE RULE. The .deb needs nothing from the distribution for SDR support or libraries: no
#     libusb, librtlsdr, libhackrf, libopus, ncurses, libstdc++ or libgcc. glibc (libc6) necessarily
#     stays; AAC is FAAD2 + libxaac, linked in (V12 — no FFmpeg, bundled or installed); the SDRplay API stays the owner's own install,
#     dlopen'd. Everything else is OURS, built from pinned source (linux/build-deps.sh, the same
#     versions as mac/build-deps.sh) or compiled from the vendored tree, and linked IN.
# ★★ WHY. dpkg-shlibdeps writes the BUILD image's library versions into Depends, not the versions
#    our code needs — `libhackrf0 (>= 2022.09.1)` alone refused Ubuntu 22.04 though every call we make
#    is years older; `libstdc++6 (>= 12)` and friends did the same quietly. A receiver's drivers should
#    arrive on our release cycle, and install on whatever the owner's distribution happens to be.
# ★★ VIBE_LINUX_DEPS is the prefix build-deps.sh fills (/opt/vibe-deps in the build images,
#    /opt/vibe-deps-v6 for the ARMv6 binary). Present = the ONLY source for those libraries, every
#    path forced to it below, exactly like VIBE_MAC_DEPS on the Mac. Absent on a release build
#    (VIBESERVER_STRICT_RADIOS) = a configure error. Absent on a dev box = the system libraries, as
#    before, so a plain `cmake` on any Linux still builds.
# ★ CPack refuses the package afterwards if Depends names anything but libc6 and the three tools —
#   see linux/check-deb-depends.cmake. That guard is what keeps this from regressing silently.
set(VIBE_LINUX_DEPS "/opt/vibe-deps" CACHE PATH "Linux: prefix from linux/build-deps.sh (release builds)")
if(UNIX AND NOT APPLE)
  if(EXISTS "${VIBE_LINUX_DEPS}/lib/libusb-1.0.a")
    set(VIBE_LINUX_SELFCONTAINED ON)
    message(STATUS "Linux deps: ${VIBE_LINUX_DEPS} (self-contained: static libusb/libopus/librtlsdr, C++ runtime)")
    # ★★ THE C++ RUNTIME TOO. libstdc++ and libgcc_s are the other two lines shlibdeps stamped with the
    #    build image's GCC. Static, they are simply part of the binary. The SDRplay API is a C interface
    #    dlopen'd RTLD_LOCAL, and an executable exports none of its static libstdc++ symbols (nothing
    #    is linked against it at build time), so the API's own libstdc++ cannot meet ours.
    add_link_options(-static-libstdc++ -static-libgcc)
    # ★★★ …WITHOUT RAISING THE glibc FLOOR. Static libstdc++ 12 was built against bookworm's glibc
    #     2.36, and its std::random_device calls arc4random@GLIBC_2.36 — which alone made the package
    #     `libc6 (>= 2.36)` and refused Ubuntu 22.04. linux/glibc_floor.c DEFINES arc4random from
    #     getentropy() (glibc 2.25) — not --wrap, which the ARMv6 build's lld ignored. The object goes
    #     into EVERY executable (link_libraries, before any target exists): an archive member would be
    #     searched too early on the link line to satisfy a reference from libstdc++.a at its end.
    add_library(vibe_glibc_floor OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/linux/glibc_floor.c")
    link_libraries(vibe_glibc_floor)
  elseif(VIBESERVER_STRICT_RADIOS)
    message(FATAL_ERROR
            "VIBE_LINUX_DEPS=${VIBE_LINUX_DEPS} has no lib/libusb-1.0.a, and VIBESERVER_STRICT_RADIOS is on.\n"
            "  A release links our own libusb/libopus/librtlsdr, never the distribution's — the system\n"
            "  ones would put library packages straight back into Depends. Build in the images\n"
            "  (linux/Dockerfile.build, Dockerfile.cross-armhf), or run linux/build-deps.sh <prefix>\n"
            "  and pass -DVIBE_LINUX_DEPS=<prefix>.")
  endif()
endif()
# ── 32-bit ARM (2026-09-16) ────────────────────────────────────────────────────
# ★ armv7l (a 32-bit OS on a Pi 2/3/4, or an armeabi-v7a-class board) has NEON — every kernel in
#   vibedsp and the DAB headers now builds for it (see vibedsp/neon_compat.h) but only if the
#   compiler is TOLD; Debian's armhf default is VFPv3-D16 with no NEON.
# ★ armv6l (Pi Zero W, Pi 1) has no NEON at all: the scalar path, ARM mode (Thumb-1 cannot do the
#   hard-float ABI), and libatomic for the 64-bit atomics the pipeline's counters use.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv7")
  add_compile_options(-mfpu=neon-vfpv4)
  # ★ And libatomic here too (2026-09-19): 8-byte atomics — std::atomic<double> in the DAB receiver
  #   and service — are library calls (__atomic_load_8) on 32-bit ARM with GCC. The server happened
  #   to get libatomic from elsewhere; the DAB test executables did not, and `make all` for the
  #   first armhf .deb stopped at test-dab-receiver's link.
  # ★★ libatomic.a, NOT libatomic.so, on a self-contained build: the shared one is libatomic1 in Depends.
  #    `-l:` names the archive file itself; GCC's and clang's own library directories are searched.
  if(VIBE_LINUX_SELFCONTAINED)
    link_libraries(-l:libatomic.a)
  else()
    link_libraries(atomic)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv6")
  add_compile_options(-marm -march=armv6 -mfpu=vfp)
  if(VIBE_LINUX_SELFCONTAINED)
    link_libraries(-l:libatomic.a)
  else()
    link_libraries(atomic)
  endif()
endif()
# ── 32-bit x86 (2026-10-17) — the i386 package: Atom N270 netbooks, Pentium M laptops ──────────
# ★★★ KEYED ON THE POINTER SIZE, NOT CMAKE_SYSTEM_PROCESSOR. The package is built in a linux/386
#     container on a 64-bit kernel, and `uname -m` there still says x86_64 — so a processor test
#     would take this for amd64 and bundle the amd64 cloudflared, a tunnel that can never start on
#     the machine it ships to (the 32-bit ARM branch below had exactly that bug once).
# ★★★ SSE2 IS THE FLOOR (Stuart, 2026-10-17: "keep the sse2 requirement"). Debian's i386 compiler
#     defaults to plain i686 with x87 floating point — every float through the 80-bit stack, and
#     none of the SSE2 kernels in vibedsp or the DAB headers compiled in. -mfpmath=sse moves scalar
#     float AND double into SSE registers too. That reaches the Pentium M (2003, SSE2 only) and
#     every Atom; it excludes the Pentium III and Athlon XP, which preinst refuses in words rather
#     than letting them die "Illegal instruction" on the first DSP call.
# ★★ -march=i686, NOT pentium4: the ISA comes from the explicit -msse2, and -march=pentium4 would
#    also TUNE for NetBurst — the wrong pipeline for the in-order Atoms and the Pentium M this
#    package is for. -mtune=generic leaves scheduling neutral.
# ★★ SSE3 WAS MEASURED AND IS NOT WORTH THE PENTIUM M (2026-10-17, vibeserver-bench on the Lenovo,
#    5 interleaved rounds per build): -msse3 changed 54 of 266k instructions and total CPU by -0.2 %
#    (run-to-run spread ±0.6 %); -mssse3 -0.3 %; amd64 -msse3 -0.2 %. The hot loops are hand-written
#    SSE2 intrinsics, which an -msse3 flag does not rewrite. So no higher floor.
#    What DOES matter is that those intrinsics are compiled in at all: the vibedsp gate used to be
#    __x86_64__ only, and opening it to __SSE2__ took i386 WFM 2.4 MS/s from 7.7 to 4.9 % of a core.
# ★★ RE-MEASURED FOR THE ATOMS (2026-10-18, docs/ATOM-PERFORMANCE-2026-10-18.md): -march=bonnell on
#    the whole binary ADDS 1.5-2.3 % instructions — still no reason to raise the floor, which would drop
#    every pre-2011 AMD (no SSSE3). Where SSSE3 genuinely pays (the DAB Viterbi's table lookup, -45 %)
#    it is used by RUNTIME DISPATCH (vibe_dab_fec.h: target("ssse3") + __builtin_cpu_supports), so
#    this floor stays SSE2 and the SSSE3 CPUs still get it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 4
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86|x86_64|AMD64)$")
  set(VIBE_X86_32 ON)
  # ★ A CACHE STRING, so an Atom test build can swap the ISA line (-march=bonnell …) without the
  #   default reasserting -march=i686 AFTER it: add_compile_options lands after CMAKE_CXX_FLAGS on the
  #   command line, so a -march passed through CMAKE_CXX_FLAGS was silently overridden (2026-10-18).
  set(VIBE_X86_32_FLAGS "-march=i686 -msse2 -mfpmath=sse -mtune=generic" CACHE STRING
      "ISA flags for the 32-bit x86 build (keep -mfpmath=sse)")
  separate_arguments(_vibe_x86_32_flags UNIX_COMMAND "${VIBE_X86_32_FLAGS}")
  add_compile_options(${_vibe_x86_32_flags})
  message(STATUS "32-bit x86: ${VIBE_X86_32_FLAGS}")
endif()

add_library(vibedsp STATIC
    ${VIBEDSP_SRC}
    "${CPP_DIR}/vibedsp/third_party/kissfft/kiss_fft.c"
    "${CPP_DIR}/vibedsp/third_party/kissfft/kiss_fftr.c"
    "${CPP_DIR}/vibedsp/third_party/pffft/pffft.c")
target_include_directories(vibedsp PUBLIC "${CPP_DIR}/vibedsp")
target_compile_options(vibedsp PRIVATE -O3 -ffp-contract=fast)

# ── decoders + ft8_lib ────────────────────────────────────────────────────────
# The shim references the decoder suite directly, so a server build needs them even though a
# headless server is not the place anyone decodes FT8 from. Mirrors the Android target's list.
file(GLOB DIGIMODE_SRC CONFIGURE_DEPENDS "${CPP_DIR}/decoders/digimodes/*.cpp")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/proc.cpp"
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
    # ★ The nearest town as the server moves (the owner's tick) — BeaconDB + Nominatim, town only.
    "${CMAKE_CURRENT_SOURCE_DIR}/moveloc.cpp"
    "${CPP_DIR}/local_sdr_shim.cpp"
    "${CPP_DIR}/net_shim.cpp"
    # ★ Hands a live connection to another process, so several radios can share one
    #   forwarded port without anything being proxied. See fd_passing.h.
    "${CPP_DIR}/fd_passing.cpp"
    "${CPP_DIR}/mdns_responder.cpp"
    "${CPP_DIR}/sdrplay_source.cpp"
    "${CPP_DIR}/airspyhf_source.cpp"
    "${CPP_DIR}/airspy_source.cpp"
    "${CPP_DIR}/hackrf_source.cpp"
    "${CPP_DIR}/spyserver/spyserver_client.cpp"
    "${CPP_DIR}/spyserver/spyserver_messages.cpp"
    "${CPP_DIR}/decoders/fsk_decoder.cpp"
    "${CPP_DIR}/decoders/rtty_auto.cpp"
    "${CPP_DIR}/decoders/wefax_decoder.cpp"
    "${CPP_DIR}/decoders/ft8_decoder.cpp"
    "${CPP_DIR}/decoders/ftx_fine.cpp"
    "${CPP_DIR}/decoders/sstv_decoder.cpp"
    "${CPP_DIR}/decoders/audio_nr.cpp"
    "${CPP_DIR}/decoders/auto_notch.cpp"
    "${CPP_DIR}/decoders/time_decoder.cpp"
    # ★ V12: CW — the listener's decoder and the skimmer (one FFT filterbank; decoders/cw_skimmer.h)
    "${CPP_DIR}/decoders/cw_skimmer.cpp"
    # ★ V12: NDB — every beacon in the LF band, idents by consensus (decoders/ndb_skimmer.h)
    "${CPP_DIR}/decoders/ndb_skimmer.cpp"
    # ★ V12 DRM: the OFDM receiver (decoders/drm/); the codec is the OS's (vibe_drm_audiodec.h)
    "${CPP_DIR}/decoders/drm/drm_receiver.cpp"
    # ★ V12: PSK31/63 — the listener's decoder and the skimmer (one FFT is finder and channeliser; decoders/psk_skimmer.h)
    "${CPP_DIR}/decoders/psk_skimmer.cpp"
    # ★ V12: DSC — maritime Digital Selective Calling, MF/HF and VHF ch 70 (ITU-R M.493; decoders/dsc_decoder.h)
    "${CPP_DIR}/decoders/dsc_decoder.cpp"
    # ★ V12: SITOR-A — the maritime ARQ teleprinter listener (decoders/sitor_a.h, ITU-R M.476/M.625)
    "${CPP_DIR}/decoders/sitor_a.cpp"
    # ★ V12: JTTY — the WSJT-X 3.2 keyboard mode, every signal in the passband, no slots (decoders/jtty_decoder.h)
    "${CPP_DIR}/decoders/jtty_decoder.cpp"
    # ★ V12: digital spots — the mode registry, Band mode's tuners and the spotter (decoders/digi_mode.h), and every
    #   mode engine dropped into decoders/digimodes/ (globbed: adding a mode is one file — docs/v12/DIGITAL-SPOTS.md)
    "${CPP_DIR}/decoders/digi_modes.cpp"
    "${CPP_DIR}/decoders/digi_tuner.cpp"
    "${CPP_DIR}/decoders/digi_spotter.cpp"
    ${DIGIMODE_SRC}
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
  if(VIBE_LINUX_SELFCONTAINED)
    # ★★ STATIC on a self-contained build: libncursesw.a + libtinfo.a from the build root's
    #    libncurses-dev, so the package names neither libncursesw6 nor libtinfo6. The terminfo
    #    DATABASE is a file the library reads at run time (/etc/terminfo, /lib/terminfo,
    #    /usr/share/terminfo, compiled into Debian's build) and comes from ncurses-base, which is
    #    Essential — present on every Debian and Ubuntu system, container images included.
    # ★ find_library with the archive's own FILE NAME, so it cannot settle for the .so.
    find_library(VIBE_NCURSESW_A NAMES libncursesw.a)
    find_library(VIBE_TINFO_A    NAMES libtinfo.a)
    find_path(VIBE_NCURSES_INC ncurses.h PATH_SUFFIXES ncursesw)
    if(VIBE_NCURSESW_A AND VIBE_TINFO_A AND VIBE_NCURSES_INC)
      set(CURSES_FOUND TRUE)
      set(CURSES_INCLUDE_DIRS "${VIBE_NCURSES_INC}")
      set(CURSES_LIBRARIES "${VIBE_NCURSESW_A}" "${VIBE_TINFO_A}")
    endif()
  else()
    find_package(Curses)
  endif()
endif()
add_executable(vibeserver
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rtl_eeprom.cpp"
    # ★ The pocket image's Wi-Fi + captive portal (daemon side). Inert unless the image's marker
    #   file exists, so every other install carries it and never runs a line of it.
    "${CMAKE_CURRENT_SOURCE_DIR}/pocket.cpp"
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
          "  instead of the setup screen. In the build root (a self-contained build needs its\n"
          "  static libncursesw.a + libtinfo.a, which this package carries):\n"
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
# ★★★ AND ON LINUX TOO, SINCE 2026-10-18 — the rule above ("nothing to install") is now the Linux rule
#     as well; see the VIBE_LINUX_DEPS block near the top. This comment used to say static was a macOS
#     rule because "the package manager owns the libraries" — and the package manager then copied the
#     build image's versions into Depends and refused distributions that would have run us fine.
# ★★ What made static impossible on Linux was Debian's libusb-1.0.a, which needs libudev symbols that
#    are not pulled in (Raspberry Pi, 2026-07-31). Ours is built --disable-udev (sysfs enumeration,
#    netlink hotplug), so that wall of undefined `udev_*` references cannot happen — and librtlsdr,
#    once a private .so in /usr/lib/vibeserver for exactly that reason, is a static archive like
#    the Mac's.
# ★★★ macOS RELEASE BUILDS: OUR OWN LIBRARIES, BUILT FOR THE APP'S MINIMUM macOS (2026-09-28).
#     Homebrew's archives are built for the OS of the Mac that installed them — 5.6.78 linked
#     macOS 26 objects into a 14.0 app, and ld said so only in a warning nobody saw.
#     vibeserver/mac/build-deps.sh builds librtlsdr/libusb/libopus from pinned source with the
#     deployment target, and build-app.sh passes its prefix here. Set, it is the ONLY source: every
#     path below is forced to it, so nothing can quietly resolve back to /opt/homebrew — and a
#     missing file is a configure error, not a fallback.
# ★ Unset (a plain dev `cmake`), everything below behaves exactly as it always has.
# ★★ THE LINUX PREFIX (VIBE_LINUX_DEPS, linux/build-deps.sh) HAS THE SAME LAYOUT and takes the same
#    road: every path forced to it, a missing file a configure error.
set(VIBE_MAC_DEPS "" CACHE PATH "macOS: prefix from vibeserver/mac/build-deps.sh (release builds)")
set(VIBE_DEPS_PREFIX "")
if(APPLE AND VIBE_MAC_DEPS)
  set(VIBE_DEPS_PREFIX "${VIBE_MAC_DEPS}")
  set(_deps_hint "run vibeserver/mac/build-deps.sh")
elseif(VIBE_LINUX_SELFCONTAINED)
  set(VIBE_DEPS_PREFIX "${VIBE_LINUX_DEPS}")
  set(_deps_hint "run vibeserver/linux/build-deps.sh, or rebuild the build image")
endif()
if(VIBE_DEPS_PREFIX)
  foreach(_f lib/librtlsdr.a lib/libusb-1.0.a lib/libopus.a include/rtl-sdr.h
             include/libusb-1.0/libusb.h include/opus/opus.h)
    if(NOT EXISTS "${VIBE_DEPS_PREFIX}/${_f}")
      message(FATAL_ERROR "${VIBE_DEPS_PREFIX} has no ${_f} — ${_deps_hint}")
    endif()
  endforeach()
  set(RTLSDR_LIB    "${VIBE_DEPS_PREFIX}/lib/librtlsdr.a"       CACHE FILEPATH "" FORCE)
  set(USB_LIB       "${VIBE_DEPS_PREFIX}/lib/libusb-1.0.a"      CACHE FILEPATH "" FORCE)
  set(OPUS_LIB      "${VIBE_DEPS_PREFIX}/lib/libopus.a"         CACHE FILEPATH "" FORCE)
  set(RTLSDR_INC    "${VIBE_DEPS_PREFIX}/include"               CACHE PATH "" FORCE)
  set(OPUS_INC      "${VIBE_DEPS_PREFIX}/include"               CACHE PATH "" FORCE)
  set(LIBUSB_INC    "${VIBE_DEPS_PREFIX}/include/libusb-1.0"    CACHE PATH "" FORCE)
  set(LIBUSB_INC_AS "${VIBE_DEPS_PREFIX}/include/libusb-1.0"    CACHE PATH "" FORCE)
  if(APPLE)
    message(STATUS "macOS deps: ${VIBE_DEPS_PREFIX} (built for macOS ${CMAKE_OSX_DEPLOYMENT_TARGET})")
  endif()
endif()
set(_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
if(APPLE)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()
# ★★ A finished ARMv6 build to package alongside this one (armhf only): its `vibeserver` goes to
#    lib/vibeserver/armv6, and postinst installs it on a CPU without NEON. ONE FILE since 2026-10-18 —
#    librtlsdr is linked in, so there is no ARMv6 librtlsdr.so beside it any more (and no RPATH).
set(VIBE_ARMV6_STAGE "" CACHE PATH "Directory holding an ARMv6 vibeserver to ship too")

find_library(RTLSDR_LIB NAMES rtlsdr HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_library(USB_LIB    NAMES usb-1.0 HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${_saved_suffixes})
find_path(RTLSDR_INC rtl-sdr.h HINTS /opt/homebrew/include /usr/local/include /usr/include)
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
target_compile_definitions(vibeserver_core PUBLIC VIBESERVER_VERSION_STR="${VIBESERVER_VERSION}"
    VIBE_DAB_PFFFT=1)   # ★ DAB OFDM through PFFFT — see vibe_dab_fft.h
target_link_libraries(vibeserver_core PUBLIC "${RTLSDR_LIB}" "${USB_LIB}")

# ── SDRplay (RSP): the API is dlopen()ed, NEVER LINKED — and on Linux we now SHIP one (OpenRSP) ──
# ★★ dlopen, still. A notarised hardened-runtime app that hard-links a dylib CRASHES ON LAUNCH for
#    everyone who does not have it, and most owners have no RSP: sdrplay_source.cpp opens the library on
#    first use, and a machine without one simply reports zero SDRplay devices.
# ★★★ THE HEADERS ARE OURS NOW (2026-10-21): vendor/openrsp/include — OpenRSP's own API 3.15 headers,
#     GPLv3, written for that project and proved ABI-identical to SDRplay's by its test suite. They used
#     to be SDRplay's, staged into every build root by hand because we may not redistribute them; a
#     clean chroot without them once SHIPPED two releases with RSP support silently compiled out. A
#     header in the tree cannot go missing, so that whole failure (and the staging) is gone.
set(OPENRSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/openrsp")
set(SDRPLAY_INC "${OPENRSP_DIR}/include")
if(NOT EXISTS "${SDRPLAY_INC}/sdrplay_api.h")
  message(FATAL_ERROR "vendor/openrsp/include/sdrplay_api.h is missing — the checkout is incomplete")
endif()
if(VIBE_X86_32)
  # ★★ STILL OUT ON i386 (re-checked 2026-10-21). SDRplay ships no 32-bit x86 API, and OpenRSP 3.15.2-20
  #    does not build for i386 Linux either: its client asserts the ILP32 shared-memory layout of armhf and
  #    32-bit Windows (`sizeof(ServiceState) == 3144`), where 64-bit members are 8-byte aligned; the i386
  #    SysV ABI aligns them to 4 and gets 3132. That is OpenRSP's to settle (raised with M9PSY — see
  #    docs/OPENRSP-BUNDLE-PLAN.md), not something to paper over here. Compiled OUT, as before: there is no
  #    library an owner could install for us to dlopen.
  message(STATUS "SDRplay: not built on 32-bit x86 — no API exists for it (SDRplay's or OpenRSP's)")
else()
  message(STATUS "SDRplay API headers: ${SDRPLAY_INC} (OpenRSP; runtime dlopen, not linked)")
  target_include_directories(vibeserver_core PUBLIC "${SDRPLAY_INC}")
  target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_SDRPLAY)
endif()

# ── OpenRSP: the RSP driver stack, BUILT FROM vendor/openrsp AND SHIPPED IN THE PACKAGE (Linux) ─────
# ★★★ Stuart, 2026-10-21: "bundle OpenRSP in all VibeServer builds" — an RSP needs no separate driver
#     install, exactly like RTL-SDR / Airspy / HackRF. OpenRSP (openrsp.dev, GPLv3, the OpenRSP authors)
#     replaces SDRplay's closed library, service AND USB bridge firmware; VibeSDR is GPLv3 too, and the
#     source travels with us in vendor/openrsp (see VIBESDR-CHANGES there). Plan: docs/OPENRSP-BUNDLE-PLAN.md.
# ★★ WHAT GOES IN THE .deb, all under /usr/lib/vibeserver/openrsp — NEVER at SDRplay's paths
#    (/usr/local/lib, /opt/sdrplay_api, /usr/share/sdrplay_api), which the official API and the
#    sdrplay-api-open package own; a shared path is a dpkg file conflict and a fight over who wins:
#      libsdrplay_api.so.3(.15)   the API library sdrplay_source.cpp dlopen()s
#      sdrplay_apiService         the device service, run by vibeserver-rsp.service
#      firmware/own/*             OpenRSP's own bridge firmware, built here from firmware/src with sdcc
# ★★★ A SERVICE, NOT IN-PROCESS. The library talks to a service over shared memory; on Android the
#     service is embedded in the app, but here every radio is its OWN PROCESS (vibeserver@<serial>), and
#     an embedded service in each would race the others to upload firmware and claim every RSP on the
#     bus. One service owns the USB devices; every VibeServer process is a client of it, as before.
# ★★ ExternalProject, upstream's own CMake: its version script, SIMD kernel flags and firmware rules
#    are its business, and copying them here would drift. Cross builds hand it the same toolchain file.
# ★ Not on ARMv6 (the Pi Zero / Pi 1 binary in the armhf package): Debian's armhf toolchain cannot
#   produce ARMv6 code, and SDRplay never had an ARMv6 API either. postinst removes the ARMv7 service
#   on such a CPU, and the ARMv6 vibeserver is built without the bundled path.
option(VIBE_BUNDLE_OPENRSP "Linux: build and package the bundled RSP driver (vendor/openrsp)" ON)
set(VIBE_OPENRSP_PREFIX "/usr/lib/vibeserver/openrsp")
if(UNIX AND NOT APPLE AND VIBE_BUNDLE_OPENRSP AND NOT VIBE_X86_32 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^armv6")
  find_program(VIBE_SDCC sdcc)
  if(NOT VIBE_SDCC)
    # ★★★ Same rule as every radio library: a release is never quietly missing one. sdcc builds the
    #     8051 bridge firmware; without it there is no firmware and an RSP never streams.
    if(VIBESERVER_STRICT_RADIOS)
      message(FATAL_ERROR "sdcc not found, and VIBESERVER_STRICT_RADIOS is on — the RSP bridge firmware "
                          "cannot be built (apt install sdcc; the build images carry it).")
    endif()
    message(WARNING "sdcc not found — building WITHOUT the bundled OpenRSP (RSPs need a system SDRplay API)")
  else()
    include(ExternalProject)
    set(_orsp_bin "${CMAKE_BINARY_DIR}/openrsp")
    set(_orsp_args
        -DCMAKE_BUILD_TYPE=Release
        -DSDRPLAY_BUILD_TESTS=OFF
        -DSDRPLAY_LINUX_UDEV=OFF
        -DSDRPLAY_OWN_FIRMWARE=ON
        "-DSDRPLAY_FIRMWARE_DIR=${VIBE_OPENRSP_PREFIX}/firmware")
    if(CMAKE_TOOLCHAIN_FILE)
      list(APPEND _orsp_args "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
    else()
      list(APPEND _orsp_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    endif()
    if(VIBE_LINUX_SELFCONTAINED)
      # ★★★ OUR libusb (static, no udev) and a static C++ runtime: the package's Depends stay libc6 only,
      #     and check-deb-depends.cmake + build-deb.sh read every ELF in it to prove so.
      list(APPEND _orsp_args
          "-DSDRPLAY_LIBUSB_LIBRARY=${VIBE_LINUX_DEPS}/lib/libusb-1.0.a"
          "-DSDRPLAY_LIBUSB_INCLUDE_DIR=${VIBE_LINUX_DEPS}/include/libusb-1.0"
          -DSDRPLAY_STATIC_CXX_RUNTIME=ON
          # ★★ …and the server's glibc 2.35 floor: static libstdc++ calls arc4random@GLIBC_2.36. Measured on
          #    the first test build (bookworm arm64): both binaries needed 2.36; with this, 2.35.
          "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/linux/openrsp-glibc-floor.cmake")
    endif()
    set(_orsp_fw "${_orsp_bin}/fw_own")
    ExternalProject_Add(openrsp
        SOURCE_DIR "${OPENRSP_DIR}"
        BINARY_DIR "${_orsp_bin}"
        CMAKE_ARGS ${_orsp_args}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target sdrplay_api sdrplay_apiService own_firmware
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${_orsp_bin}/libsdrplay_api.so.3.15" "${_orsp_bin}/sdrplay_apiService"
                         "${_orsp_fw}/manifest.sha256")
    # ★★ The server looks HERE first when no system SDRplay service is installed — see sdrplay_source.cpp.
    target_compile_definitions(vibeserver_core PUBLIC
        VIBE_OPENRSP_LIB="${VIBE_OPENRSP_PREFIX}/libsdrplay_api.so.3"
        VIBE_OPENRSP_SERVICE="${VIBE_OPENRSP_PREFIX}/sdrplay_apiService")
    # ★ install(FILES) recreates the soname symlink as a symlink.
    install(FILES "${_orsp_bin}/libsdrplay_api.so.3.15" "${_orsp_bin}/libsdrplay_api.so.3"
            DESTINATION lib/vibeserver/openrsp)
    install(PROGRAMS "${_orsp_bin}/sdrplay_apiService" DESTINATION lib/vibeserver/openrsp)
    install(FILES "${_orsp_fw}/fw_common.bin" "${_orsp_fw}/fw_rsp1.bin" "${_orsp_fw}/fw_rspduo.bin"
                  "${_orsp_fw}/manifest.sha256"
            DESTINATION lib/vibeserver/openrsp/firmware/own)
    install(FILES linux/vibeserver-rsp.service DESTINATION lib/systemd/system)
    # ★★ GPLv3 §4/§6: the licence goes with the binaries, and the README says where the exact
    #    Corresponding Source is (this repository's vendor/openrsp, and the openrsp.dev tarball it came from).
    install(FILES "${OPENRSP_DIR}/COPYING" DESTINATION share/doc/vibeserver RENAME COPYING.openrsp)
    install(FILES linux/README.openrsp "${OPENRSP_DIR}/VIBESDR-CHANGES" DESTINATION share/doc/vibeserver/openrsp)
    message(STATUS "OpenRSP: bundled (vendor/openrsp → ${VIBE_OPENRSP_PREFIX}, firmware via ${VIBE_SDCC})")
  endif()
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

# ── libairspy: Airspy R2 / Mini (2026-09-22) ─────────────────────────────────
# ★★ VENDORED and permissive — airspy.c/.h/commands BSD-3, iqconverter*/filters MIT
# (libairspy/LICENSE); the credits carry both notices. A DIFFERENT library from libairspyhf, for
# different radios. Upstream already has airspy_open_fd(), so it needed no door patch; since
# 2026-09-30 it carries two marked VibeSDR patches (deeper raw ring, streaming-thread hook) — see
# airspy.c, and the same pair in airspyhf.c.
# ★ Same arrangement as the HF+ block above: our copy, both platforms, one place to fix.
set(AIRSPY_VENDOR "${CPP_DIR}/libairspy")
if(EXISTS "${AIRSPY_VENDOR}/airspy.c")
  message(STATUS "libairspy: vendored at ${AIRSPY_VENDOR}")
  target_sources(vibeserver_core PRIVATE
    "${AIRSPY_VENDOR}/airspy.c"
    "${AIRSPY_VENDOR}/iqconverter_float.c"
    "${AIRSPY_VENDOR}/iqconverter_int16.c")
  target_include_directories(vibeserver_core PUBLIC "${AIRSPY_VENDOR}")
  find_path(LIBUSB_INC_AS libusb.h PATH_SUFFIXES libusb-1.0
            HINTS /opt/homebrew/include /usr/local/include /usr/include)
  if(LIBUSB_INC_AS)
    target_include_directories(vibeserver_core PRIVATE "${LIBUSB_INC_AS}")
  endif()
  target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_AIRSPY)
elseif(VIBESERVER_STRICT_RADIOS)
  # ★★★ Same guard as the SDRplay and HackRF blocks: a release must never be quietly missing a
  #     radio. Every VibeServer build supports every SDR we support (Stuart, 2026-09-28), and a
  #     checkout without the vendored tree would otherwise compile the stub and tell every R2/Mini
  #     owner "no radio".
  message(FATAL_ERROR
          "Vendored libairspy not found at ${AIRSPY_VENDOR}, and VIBESERVER_STRICT_RADIOS is on.\n"
          "A package built now would report 'no radio' to every Airspy R2/Mini owner.")
else()
  message(STATUS "libairspy: vendored tree missing — building without Airspy R2/Mini support")
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
#     android/app/src/main/cpp/CMakeLists.txt.
# ★★★ THE DESKTOP BUILDS THE SAME VENDORED COPY — macOS release AND Linux (2026-10-18). Stuart: no
#     radio but SDRplay may need anything installed beside VibeServer. Linux used to link Debian's
#     libhackrf0, and dpkg-shlibdeps copied the BUILD image's version into Depends (>= 2022.09.1)
#     though every call we make is years older — which alone refused Ubuntu 22.04 (2021.03). Built
#     in, the package names no HackRF library at all, like the two Airspys beside it.
#     It is libhackrf 2026.01.3. The Android patches are additive (hackrf_open_fd, a
#     skip_configuration flag only that entry point sets) or #ifdef __ANDROID__, so the desktop open
#     path is upstream's.
#  ★ VIBE_HACKRF_HAS_FD stays off here all the same: the desktop enumerates, it never opens by fd.
#  ★ A plain macOS dev `cmake` (no VIBE_MAC_DEPS) keeps Homebrew's, as it always has.
set(HACKRF_VENDOR "${CPP_DIR}/libhackrf")
if(EXISTS "${HACKRF_VENDOR}/hackrf.c" AND (VIBE_MAC_DEPS OR (UNIX AND NOT APPLE)))
  message(STATUS "libhackrf: vendored at ${HACKRF_VENDOR} (HackRF support: EXPERIMENTAL)")
  target_sources(vibeserver_core PRIVATE "${HACKRF_VENDOR}/hackrf.c")
  # hackrf.c includes <libusb.h> unqualified; hackrf_source.cpp's <libhackrf/hackrf.h> resolves
  # through CPP_DIR, already a PUBLIC include of the core — so it can only see the vendored header.
  # ★ The SAME libusb header the core links — ours from the deps prefix when there is one.
  if(VIBE_DEPS_PREFIX)
    target_include_directories(vibeserver_core PRIVATE "${VIBE_DEPS_PREFIX}/include/libusb-1.0")
  else()
    find_path(LIBUSB_INC_HRF libusb.h PATH_SUFFIXES libusb-1.0 HINTS /usr/local/include /usr/include)
    target_include_directories(vibeserver_core PRIVATE "${LIBUSB_INC_HRF}")
  endif()
  target_compile_definitions(vibeserver_core PUBLIC VIBE_HAS_HACKRF)
  set(HACKRF_LIB "" CACHE FILEPATH "" FORCE)   # ★ build-app.sh reads this: nothing extra to link
  set(HACKRF_VENDORED TRUE)
elseif(UNIX AND NOT APPLE AND VIBESERVER_STRICT_RADIOS)
  # ★ No quiet fallback to the system library on a Linux release: that is the Depends line above.
  message(FATAL_ERROR "Vendored libhackrf not found at ${HACKRF_VENDOR}, and VIBESERVER_STRICT_RADIOS is on.")
else()
find_library(HACKRF_LIB NAMES hackrf HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
find_path(HACKRF_INC libhackrf/hackrf.h HINTS /opt/homebrew/include /usr/local/include /usr/include)
endif()
# ★★★ PREFER THE STATIC ARCHIVE ON macOS, exactly as the librtlsdr and libairspyhf blocks do — a
#     cached dylib path is how a notarised build ships broken. Homebrew's libhackrf.a needs only
#     libusb, which this target already links, so nothing new has to be bundled or re-signed.
if(APPLE AND HACKRF_LIB)
  get_filename_component(_hrf_dir "${HACKRF_LIB}" DIRECTORY)
  if(EXISTS "${_hrf_dir}/libhackrf.a")
    set(HACKRF_LIB "${_hrf_dir}/libhackrf.a")
  endif()
endif()
if(HACKRF_VENDORED)
  # (compiled into the core above)
elseif(HACKRF_LIB AND HACKRF_INC)
  message(STATUS "libhackrf: ${HACKRF_LIB} (HackRF support: EXPERIMENTAL)")
  target_include_directories(vibeserver_core PUBLIC "${HACKRF_INC}")
  target_link_libraries(vibeserver_core PUBLIC "${HACKRF_LIB}")
  target_compile_definitions(vibeserver_core PUBLIC VIBE_HAS_HACKRF)
elseif(VIBESERVER_STRICT_RADIOS AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
  message(FATAL_ERROR
          "libhackrf not found, and VIBESERVER_STRICT_RADIOS is on.\n"
          "  A release build must contain every radio a release claims to contain — and the one\n"
          "  person who can test this radio cannot tell a missing driver from a broken one.\n"
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

# ── FAAD2 (DRM build): DRM's AAC, decoded ON THE SERVER (V12 DRM — docs/v12/DRM-AUDIO-ROUTES.md) ─────────────
# ★★★ GPL-2.0-or-later ("Any non-GPL usage … strictly forbidden"). The standalone VibeServer (this build: Linux deb,
#     Mac app, Windows exe) may link it — it is GPL itself. The App Store app must NOT: modules/vibe-local-sdr/build_ios.sh
#     never defines VIBE_HAVE_FAAD2 and fails if a FAAD2 symbol appears in its archive.
# ★ Release builds take it from the deps prefix (built from pinned source, statically, by linux/ and mac/build-deps.sh) —
#   a missing file is a configure error there. A dev build looks for an installed FAAD2's DRM library and, without
#   one, builds without it: DRM AAC stations then say why they are silent, as before.
option(VIBE_DRM_FAAD2 "DRM AAC via FAAD2's DRM build (GPL-2.0+; server builds only)" ON)
if(VIBE_DRM_FAAD2)
  if(VIBE_DEPS_PREFIX)
    set(FAAD2_LIB "${VIBE_DEPS_PREFIX}/lib/libfaad_drm.a" CACHE FILEPATH "" FORCE)
    set(FAAD2_INC "${VIBE_DEPS_PREFIX}/include" CACHE PATH "" FORCE)
    if(NOT EXISTS "${FAAD2_LIB}" OR NOT EXISTS "${FAAD2_INC}/neaacdec.h")
      message(FATAL_ERROR "${VIBE_DEPS_PREFIX} has no FAAD2 (lib/libfaad_drm.a, include/neaacdec.h) — ${_deps_hint}")
    endif()
  else()
    find_library(FAAD2_LIB NAMES libfaad_drm.a faad_drm HINTS /opt/homebrew/opt/faad2/lib /opt/homebrew/lib /usr/local/lib /usr/lib)
    find_path(FAAD2_INC neaacdec.h HINTS /opt/homebrew/opt/faad2/include /opt/homebrew/include /usr/local/include /usr/include)
  endif()
  if(FAAD2_LIB AND FAAD2_INC)
    message(STATUS "FAAD2 (DRM): ${FAAD2_LIB}")
    if(VIBE_DEPS_PREFIX AND EXISTS "${VIBE_DEPS_PREFIX}/share/licences/COPYING.faad2")
      install(FILES "${VIBE_DEPS_PREFIX}/share/licences/COPYING.faad2" DESTINATION share/doc/vibeserver)   # ★ the deb carries its licence
    endif()
    target_include_directories(vibeserver_core PUBLIC "${FAAD2_INC}")
    target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_FAAD2)
    target_link_libraries(vibeserver_core PUBLIC "${FAAD2_LIB}")
  else()
    message(STATUS "FAAD2's DRM library not found — building without DRM AAC audio (dev build)")
  endif()
endif()

# ── FAAD2 (PLAIN build, globals prefixed vibedab_): DAB+ audio IN PROCESS (V12 — vibe_dab_faad.h) ─────────────────────
# ★★ The Linux and Mac servers' ONLY DAB+ decoder since 2026-10-29 (vibe_dab_aacdec.h). The DRM build above refuses DAB+, and the plain build exports the same names, so build-deps compiles the
#    plain one with every global renamed (libfaad_dab.a) and checks it defines nothing unprefixed. Same licence as above:
#    server builds only, never the App Store archive (build_ios.sh fails on any FAAD2 symbol, prefixed ones included).
# ★ Release builds: from the deps prefix, and its absence is a configure error (re-run build-deps.sh) unless
#   -DVIBE_DAB_FAAD2=OFF. Dev builds: FAAD2_DAB_LIB, or a deps prefix's copy if there is one; without it, DAB+ is
#   unplayable on Linux (the ensemble and MP2 still work) and AudioToolbox on a Mac.
option(VIBE_DAB_FAAD2 "DAB+ AAC via FAAD2's plain build, prefixed (GPL-2.0+; server builds only)" ON)
if(VIBE_DAB_FAAD2 AND FAAD2_INC)
  if(VIBE_DEPS_PREFIX)
    set(FAAD2_DAB_LIB "${VIBE_DEPS_PREFIX}/lib/libfaad_dab.a" CACHE FILEPATH "" FORCE)
    if(NOT EXISTS "${FAAD2_DAB_LIB}")
      message(FATAL_ERROR "${VIBE_DEPS_PREFIX} has no lib/libfaad_dab.a (FAAD2 for DAB+) — ${_deps_hint}, or configure with -DVIBE_DAB_FAAD2=OFF")
    endif()
  else()
    find_library(FAAD2_DAB_LIB NAMES libfaad_dab.a HINTS "${CMAKE_CURRENT_SOURCE_DIR}/mac/deps/prefix/lib")
  endif()
  if(FAAD2_DAB_LIB)
    message(STATUS "FAAD2 (DAB+, prefixed): ${FAAD2_DAB_LIB}")
    target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_FAAD2_DAB)
    target_link_libraries(vibeserver_core PUBLIC "${FAAD2_DAB_LIB}")
  else()
    message(STATUS "FAAD2's prefixed DAB+ library not found — no DAB+ audio on Linux, AudioToolbox on a Mac (dev build)")
  endif()
endif()

# ── libxaac (decoder): DRM's xHE-AAC, decoded IN PROCESS (V12, 2026-10-29 — docs/v12/LIBXAAC.md, vibe_xaac.h) ─────────
# ★★ Apache-2.0 — compatible with VibeServer's GPL-3.0; its LICENSE and NOTICE ship beside the binary (deb: doc dir; Mac:
#    Resources). libxaac decodes every USAC tool (SBR, eSBR, the ACELP/TCX speech core, MPS 2-1-2). Server builds only —
#    the App Store app links no decoder.
# ★ Release builds take it from the deps prefix (linux/ and mac/build-deps.sh, pinned source, build-libxaac.sh) and its
#   absence is a configure error unless -DVIBE_XAAC=OFF. Dev builds: a deps prefix's copy if there is one, else without it
#   (xHE-AAC is then AudioToolbox on a Mac; on Linux the listener is told it cannot be played).
option(VIBE_XAAC "xHE-AAC via libxaac in process (Apache-2.0; server builds only)" ON)
if(VIBE_XAAC)
  if(VIBE_DEPS_PREFIX)
    set(XAAC_LIB "${VIBE_DEPS_PREFIX}/lib/libxaacdec.a" CACHE FILEPATH "" FORCE)
    if(NOT EXISTS "${XAAC_LIB}")
      message(FATAL_ERROR "${VIBE_DEPS_PREFIX} has no lib/libxaacdec.a (libxaac, xHE-AAC) — ${_deps_hint}, or configure with -DVIBE_XAAC=OFF")
    endif()
    set(_xaac_prefix "${VIBE_DEPS_PREFIX}")
  else()
    find_library(XAAC_LIB NAMES libxaacdec.a HINTS "${CMAKE_CURRENT_SOURCE_DIR}/mac/deps/prefix/lib")
    get_filename_component(_xaac_prefix "${XAAC_LIB}/../.." ABSOLUTE)
  endif()
  if(XAAC_LIB)
    # ★ the pinned release, for the listener's "decoded by" line: BUILT-FROM (Linux) or the stamp (Mac)
    set(_xaac_ver "?")
    foreach(_f "${_xaac_prefix}/BUILT-FROM" "${_xaac_prefix}/.stamp-libxaac")
      if(EXISTS "${_f}")
        file(STRINGS "${_f}" _l REGEX "^libxaac [0-9.]+")
        if(_l)
          string(REGEX REPLACE "^libxaac ([0-9.]+).*" "\\1" _xaac_ver "${_l}")
          break()
        endif()
      endif()
    endforeach()
    message(STATUS "libxaac ${_xaac_ver}: ${XAAC_LIB}")
    target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_LIBXAAC VIBE_LIBXAAC_VERSION="${_xaac_ver}")
    target_link_libraries(vibeserver_core PUBLIC "${XAAC_LIB}")
    foreach(_f LICENSE.libxaac NOTICE.libxaac)
      if(EXISTS "${_xaac_prefix}/share/licences/${_f}")
        install(FILES "${_xaac_prefix}/share/licences/${_f}" DESTINATION share/doc/vibeserver)   # ★ Apache-2.0 §4(a)/(d)
      endif()
    endforeach()
  else()
    message(STATUS "libxaac not found — building without it: no xHE-AAC on Linux, AudioToolbox on a Mac (dev build)")
  endif()
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
vibe_sanitize(test-radio-api)
target_link_libraries(test-radio-api PRIVATE vibeserver_core)

# ★ Header-only and dependency-free on purpose: the rule it guards (never emit a byte a browser
#   will hang up over) belongs to the wire, not to any one radio, so its test must be runnable
#   without a device, a socket or the rest of the core.
# ★ The moving location: the Wi-Fi list it sends, town-or-region from OpenStreetMap, the grid. No network.
add_executable(test-moveloc "${CMAKE_CURRENT_SOURCE_DIR}/test-moveloc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/moveloc.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/proc.cpp")
vibe_sanitize(test-moveloc)

add_executable(test-utf8 "${CMAKE_CURRENT_SOURCE_DIR}/test-utf8.cpp")
vibe_sanitize(test-utf8)
target_include_directories(test-utf8 PRIVATE "${CPP_DIR}")

# ★ The GPU map's file server (vibe_mapgl.h): Range parsing (a wrong 206 corrupts a tile silently),
#   path safety, and the atomic detail-pack install. Header-only, no socket, no radio.
add_executable(test-mapgl-range "${CMAKE_CURRENT_SOURCE_DIR}/test-mapgl-range.cpp")
vibe_sanitize(test-mapgl-range)
target_include_directories(test-mapgl-range PRIVATE "${CPP_DIR}")
target_link_libraries(test-mapgl-range PRIVATE Threads::Threads)

# ★★ V12 ADS-B aircraft data (vibe_adsb_data*.h, vibe_inflate.h, vibe_adsb_images_install.h): our inflater, the
#    builder, the reader, enrichment, the logo route's path safety and the logo zip installer (zip slip, bombs).
#    Header-only. Reads vibeserver/testdata-adsb/ relative to the repository root (run it from there).
add_executable(test-adsb-data "${CMAKE_CURRENT_SOURCE_DIR}/test-adsb-data.cpp")
vibe_sanitize(test-adsb-data)
target_include_directories(test-adsb-data PRIVATE "${CPP_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test-adsb-data PRIVATE Threads::Threads)
# ★ The reproducible half of the aircraft data — scripts/build-adsb-data.sh drives it (the starter header, the
#   full tables for a check). The SAME builder the server's weekly update runs.
add_executable(adsb-data-tool "${CMAKE_CURRENT_SOURCE_DIR}/adsb-data-tool.cpp")
target_include_directories(adsb-data-tool PRIVATE "${CPP_DIR}")
target_link_libraries(adsb-data-tool PRIVATE Threads::Threads)

# ★★★ THE DAB TESTS ARE PART OF THE BUILD. Seventeen of them existed and not one was a target —
#     each was "a two-line g++ command" nobody ran, which is how a 512-byte JSON buffer, an
#     un-reversed X-PAD and a P/D flag read from the wrong bit all shipped past a test tree that
#     would have caught two of the three. `ctest` (or `make dab-tests`) runs the lot; they are
#     header-only and take seconds, with no radio.
enable_testing()

set(VIBE_DAB_TESTS modes channels prs sync ofdm interleave fec punct fic ficdec msc receiver
                   resample superframe aac mp2 pad tii epg mot)
add_custom_target(dab-tests)
foreach(t ${VIBE_DAB_TESTS})
  add_executable(test-dab-${t} "${CMAKE_CURRENT_SOURCE_DIR}/test-dab-${t}.cpp")
  target_include_directories(test-dab-${t} PRIVATE "${CPP_DIR}")
  target_compile_options(test-dab-${t} PRIVATE -O2)
  vibe_sanitize(test-dab-${t})
  add_test(NAME dab-${t} COMMAND test-dab-${t})
  add_dependencies(dab-tests test-dab-${t})
endforeach()
# ★ The throttle snail (vibe_health.h), driven through a fake sysfs tree. Built TWICE: this machine's own
#   path (cpuinfo_max on ARM, calibrated peak on x86) and the x86 calibrated path forced, so both are
#   exercised whichever box runs the tests.
foreach(v native boost)
  add_executable(test-health-snail-${v} "${CMAKE_CURRENT_SOURCE_DIR}/test-health-snail.cpp")
  target_include_directories(test-health-snail-${v} PRIVATE "${CPP_DIR}")
  target_link_libraries(test-health-snail-${v} PRIVATE Threads::Threads)
  if(v STREQUAL "boost")
    target_compile_definitions(test-health-snail-${v} PRIVATE VIBE_HEALTH_TEST_BOOST)
  endif()
  add_test(NAME health-snail-${v} COMMAND test-health-snail-${v})
endforeach()
# ★★ The dynamic spectrum ceiling (vibe_spec_governor.h, 11.0.9 — the Pi 2 on DAB): starved → 20 → 10 → 5 fast, calm →
#    back up slowly, no hunting, never above the owner's top or a listener's ask; composed with the audio-first gate the
#    lower of the two wins. On a fake clock and a fake load.
add_executable(test-spec-governor "${CMAKE_CURRENT_SOURCE_DIR}/test-spec-governor.cpp")
target_include_directories(test-spec-governor PRIVATE "${CPP_DIR}")
vibe_sanitize(test-spec-governor)
target_link_libraries(test-spec-governor PRIVATE Threads::Threads)
add_test(NAME spec-governor COMMAND test-spec-governor)
# ★ The POWER card's verdict (vibe_power.h): brief supply dips amber, sustained / continuous / USB loss red.
add_executable(test-power-dips "${CMAKE_CURRENT_SOURCE_DIR}/test-power-dips.cpp")
target_include_directories(test-power-dips PRIVATE "${CPP_DIR}")
target_link_libraries(test-power-dips PRIVATE Threads::Threads)
add_test(NAME power-dips COMMAND test-power-dips)
# ★ The mDNS responder's question parser (2026-10-17): compressed questions — Apple packs several lookups into one
#   packet — were ignored, so <name>.local resolved only when ours happened to be the first, uncompressed question.
#   ★ 2026-10-20: and respond() — whole query packets in, whole responses out: A+AAAA → A + NSEC, browse → PTR with
#     SRV/TXT/A/NSEC additionals, known-answer suppression, TC, QU, legacy unicast, announce and goodbye.
add_executable(test-mdns "${CMAKE_CURRENT_SOURCE_DIR}/test-mdns.cpp")
target_include_directories(test-mdns PRIVATE "${CPP_DIR}")
vibe_sanitize(test-mdns)
target_link_libraries(test-mdns PRIVATE Threads::Threads)
add_test(NAME mdns COMMAND test-mdns)
# ★★ Wi-Fi QoS (vibe_wifi_qos.h, 2026-10-20, docs/BRIEF-portable-wifi-realtime.md): audio marked voice, the waterfall and
#    the map never; on Linux SO_PRIORITY is read back AFTER IP_TOS (which rewrites it). And the audio-first rule — the
#    waterfall yields while a listener's audio is queued — on a synthetic clock.
add_executable(test-wifi-qos "${CMAKE_CURRENT_SOURCE_DIR}/test-wifi-qos.cpp")
target_include_directories(test-wifi-qos PRIVATE "${CPP_DIR}")
vibe_sanitize(test-wifi-qos)
target_link_libraries(test-wifi-qos PRIVATE Threads::Threads)
add_test(NAME wifi-qos COMMAND test-wifi-qos)
# ★★★ THE THREAD LADDER, READ BACK (vibe_thread.h, 2026-10-20): audio send ≥ DSP > waterfall send ≥ spectrum >
#     decoders — nice via getpriority + /proc on Linux, QoS on macOS. Negative rungs NOT RUN without CAP_SYS_NICE.
add_executable(test-thread-rank "${CMAKE_CURRENT_SOURCE_DIR}/test-thread-rank.cpp")
target_include_directories(test-thread-rank PRIVATE "${CPP_DIR}")
vibe_sanitize(test-thread-rank)
target_link_libraries(test-thread-rank PRIVATE Threads::Threads)
add_test(NAME thread-rank COMMAND test-thread-rank)
set_tests_properties(thread-rank PROPERTIES SKIP_RETURN_CODE 77)
# ★★★ VibeAGC (2026-10-20): the retune-forget split, carrierless headroom, the bursty channel level and the bounded
#     record (vibe_agc_rules.h, vibe_agc_record.h) — pure, no radio. Silent: synthetic SSB speech, nothing played.
# ★★★ WHICH RSP DRIVER AND WHOSE SERVICE (vibe_sdrplay_lib.h, 2026-10-22): an installed SDRplay stack wins, the
#     bundle's own library is accepted only from inside the bundle — on a scratch root, never the real /Library.
add_executable(test-sdrplay-lib "${CMAKE_CURRENT_SOURCE_DIR}/test-sdrplay-lib.cpp")
target_include_directories(test-sdrplay-lib PRIVATE "${CPP_DIR}")
vibe_sanitize(test-sdrplay-lib)
add_test(NAME sdrplay-lib COMMAND test-sdrplay-lib)
# ★★ What each RSP model has (vibe_rsp_caps.h) — the table behind every panel's RSP controls, spelt out per model.
add_executable(test-rsp-caps "${CMAKE_CURRENT_SOURCE_DIR}/test-rsp-caps.cpp")
target_include_directories(test-rsp-caps PRIVATE "${CPP_DIR}")
vibe_sanitize(test-rsp-caps)
add_test(NAME rsp-caps COMMAND test-rsp-caps)
# ★★★ VibeAGC FOR SDRplay (vibe_rsp_vibeagc.h, 2026-10-30): the combined IF + RF loop closed against a simulated RSP
#     (Band II next to a +20 dB neighbour, weak HF, DAB, a signal arriving, the DAB rapid-step repro, fades, a rung
#     bigger than its prior, a stalled stream) — and its writes through the real gainstep.h + RxPipeline: IF steps
#     must sound like a fade, an RF rung + its IF counter-step must not click. Silent: nothing is played.
add_executable(test-rsp-vibeagc "${CMAKE_CURRENT_SOURCE_DIR}/test-rsp-vibeagc.cpp")
target_include_directories(test-rsp-vibeagc PRIVATE "${CPP_DIR}")
target_link_libraries(test-rsp-vibeagc PRIVATE vibedsp Threads::Threads)
add_test(NAME rsp-vibeagc COMMAND test-rsp-vibeagc)
add_executable(test-agc-rules "${CMAKE_CURRENT_SOURCE_DIR}/test-agc-rules.cpp")
target_include_directories(test-agc-rules PRIVATE "${CPP_DIR}")
vibe_sanitize(test-agc-rules)
add_test(NAME agc-rules COMMAND test-agc-rules)
add_executable(test-agc-record "${CMAKE_CURRENT_SOURCE_DIR}/test-agc-record.cpp")
target_include_directories(test-agc-record PRIVATE "${CPP_DIR}")
vibe_sanitize(test-agc-record)
target_link_libraries(test-agc-record PRIVATE Threads::Threads)
add_test(NAME agc-record COMMAND test-agc-record)
# ★★ Download diagnostics (diag_scrub.h, 2026-10-20): the daemon's own scrub of the file before it is downloaded.
add_executable(test-diag-scrub "${CMAKE_CURRENT_SOURCE_DIR}/test-diag-scrub.cpp")
vibe_sanitize(test-diag-scrub)
add_test(NAME diag-scrub COMMAND test-diag-scrub)
# ★★★ The tunnel address (tunnel_url.h, 2026-10-27): cloudflared's "failed to request quick Tunnel: Post
#     \"https://api.trycloudflare.com/tunnel\"" line is never an address (Kiko's TV box recycled on it every 15 s).
add_executable(test-tunnel-url "${CMAKE_CURRENT_SOURCE_DIR}/test-tunnel-url.cpp")
vibe_sanitize(test-tunnel-url)
add_test(NAME tunnel-url COMMAND test-tunnel-url)
# ★ The fatal-signal record (crash_report.h, 2026-10-06): a fault on a named thread is reported with its
#   thread, PC as an exe offset and version, and the process still dies of the same signal. Linux only
#   (prints SKIP elsewhere).
add_executable(test-crash-report "${CMAKE_CURRENT_SOURCE_DIR}/test-crash-report.cpp")
target_link_libraries(test-crash-report PRIVATE Threads::Threads)
add_test(NAME crash-report COMMAND test-crash-report)
# ★★★ The FIR kernels against the direct convolution, sample for sample (2026-10-18). On x86 this is
#     the ONLY check of the SSE2 aligned tap banks (21fe0cf7) — the Mac never compiles them, and the
#     DSP suites assert behaviours a slightly wrong kernel can pass. Built with vibedsp's own flags.
add_executable(test-fir-simd "${CMAKE_CURRENT_SOURCE_DIR}/test-fir-simd.cpp" "${CPP_DIR}/vibedsp/ddc.cpp")
target_include_directories(test-fir-simd PRIVATE "${CPP_DIR}" "${CPP_DIR}/vibedsp")
target_compile_options(test-fir-simd PRIVATE -O3 -ffp-contract=fast)
add_test(NAME fir-simd COMMAND test-fir-simd)
# ── ★★★ ICOM (docs/ICOM-SUPPORT-PLAN.md) — the receive-only promise is a TEST, and it joins ctest ──
#     test-icom-civ is plan §3.2: every cmd/sub pair at 0-6 data bytes against an independent copy of
#     the allow-list. "Nothing ships until the exhaustive allow-list test … passes" (plan §8).
add_executable(test-icom-civ "${CMAKE_CURRENT_SOURCE_DIR}/test-icom-civ.cpp")
target_include_directories(test-icom-civ PRIVATE "${CPP_DIR}")
target_compile_options(test-icom-civ PRIVATE -O2)
vibe_sanitize(test-icom-civ)
add_test(NAME icom-civ COMMAND test-icom-civ)
# The offline replay harness: the same chain over a VIBE_IQ_DUMP capture, in seconds.
# ★ dab-offline and the receiver test take the PFFFT path so the SERVER's transform is what gets
#   replayed and tested; the other DAB tests keep the header-only radix-2, which is the reference.
# ★ vibeserver-bench: the server benchmark from a shell (vibe_benchmark.h) — to check it on a box.
add_executable(vibeserver-bench "${CMAKE_CURRENT_SOURCE_DIR}/bench-server.cpp")
target_include_directories(vibeserver-bench PRIVATE "${CPP_DIR}")
target_compile_options(vibeserver-bench PRIVATE -O3 -ffp-contract=fast)
target_link_libraries(vibeserver-bench PRIVATE vibeserver_core)   # ★ the DAB rows need the DAB service and its AAC decoder
# ★ dab-switch: how long a station change takes, and what the label scan costs it — see the file's header.
add_executable(dab-switch "${CMAKE_CURRENT_SOURCE_DIR}/dab-switch.cpp")
target_include_directories(dab-switch PRIVATE "${CPP_DIR}")
target_compile_options(dab-switch PRIVATE -O3 -ffp-contract=fast)
target_link_libraries(dab-switch PRIVATE vibeserver_core)

# ★ test-dab-faad: DAB+ through FAAD2 on REAL unit logs (dab-unitlog) — timeline, concealment, damage at the holes.
add_executable(test-dab-faad "${CMAKE_CURRENT_SOURCE_DIR}/test-dab-faad.cpp")
target_include_directories(test-dab-faad PRIVATE "${CPP_DIR}")
target_compile_options(test-dab-faad PRIVATE -O3 -ffp-contract=fast)
target_link_libraries(test-dab-faad PRIVATE vibeserver_core)
# ★ dab-faad-bench: what each DAB+/FAAD2 stage costs (decode / resample) and output comparisons (dev tool).
add_executable(dab-faad-bench "${CMAKE_CURRENT_SOURCE_DIR}/dab-faad-bench.cpp")
target_include_directories(dab-faad-bench PRIVATE "${CPP_DIR}")
target_compile_options(dab-faad-bench PRIVATE -O3 -ffp-contract=fast)
target_link_libraries(dab-faad-bench PRIVATE vibeserver_core)
# ★ test-dab-faad-queue: FAAD2 on its own thread (vibe-faad2) — the ring, drops, resets, the service's decoder.
add_executable(test-dab-faad-queue "${CMAKE_CURRENT_SOURCE_DIR}/test-dab-faad-queue.cpp")
target_include_directories(test-dab-faad-queue PRIVATE "${CPP_DIR}")
target_compile_options(test-dab-faad-queue PRIVATE -O3 -ffp-contract=fast)
target_link_libraries(test-dab-faad-queue PRIVATE vibeserver_core)

# ★ dab-unitlog: one DAB+ service's access units, CRC failures and lost super frames off a captured multiplex — the
#   input test-dab-faad replays through FAAD2 (dev tool; real recordings stay in ~/VibeSDR-test-audio/dab).
add_executable(dab-unitlog "${CMAKE_CURRENT_SOURCE_DIR}/dab-unitlog.cpp")
target_include_directories(dab-unitlog PRIVATE "${CPP_DIR}")
target_compile_options(dab-unitlog PRIVATE -O3 -ffp-contract=fast)
target_link_libraries(dab-unitlog PRIVATE vibeserver_core)

add_executable(dab-offline "${CMAKE_CURRENT_SOURCE_DIR}/dab-offline.cpp" "${CPP_DIR}/vibedsp/third_party/pffft/pffft.c")
target_include_directories(dab-offline PRIVATE "${CPP_DIR}")
target_compile_options(dab-offline PRIVATE -O3 -ffp-contract=fast)   # ★ the SERVER's flags, so a replay measures what ships (was -O2, 2026-09-16)
vibe_sanitize(dab-offline)
target_compile_definitions(dab-offline PRIVATE VIBE_DAB_PFFFT=1)
target_sources(test-dab-receiver PRIVATE "${CPP_DIR}/vibedsp/third_party/pffft/pffft.c")
target_compile_definitions(test-dab-receiver PRIVATE VIBE_DAB_PFFFT=1)
if(APPLE)
  target_link_libraries(dab-offline PRIVATE "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")   # the platform decoder it now exercises
endif()
if(APPLE)
  # libusb on macOS talks to IOKit; these are system frameworks, nothing to ship.
  target_link_libraries(vibeserver_core PUBLIC
      "-framework IOKit" "-framework CoreFoundation" "-framework Security"
      "-framework AudioToolbox" "-framework CoreAudio" objc)   # ★ AudioToolbox: Icom audio, and DAB+ / xHE-AAC in a dev build without the deps (vibe_dab_aacdec.h)
endif()
find_package(Threads REQUIRED)
target_link_libraries(vibeserver_core PUBLIC Threads::Threads)

# ── ★★★ ICOM (receive only — docs/ICOM-SUPPORT-PLAN.md) ──────────────────────────────────────
#     CI-V over libusb (the radio's CP2102, DTR/RTS never raised), the scope as the waterfall and the
#     radio's USB codec as audio. Its own small library so the tests link exactly what the server does
#     — and NOTHING new from the system: libusb is ours already, ALSA is driven by its kernel ioctls
#     (no libasound), CoreAudio is a system framework.
add_library(vibe_icom STATIC
    "${CPP_DIR}/icom_source.cpp"
    "${CPP_DIR}/icom_transport.cpp"
    "${CPP_DIR}/icom_audio.cpp")
target_include_directories(vibe_icom PUBLIC "${CPP_DIR}" "${CPP_DIR}/vibedsp")
if(LIBUSB_INC)
  target_include_directories(vibe_icom PRIVATE "${LIBUSB_INC}")
endif()
target_link_libraries(vibe_icom PUBLIC "${USB_LIB}" Threads::Threads)
if(APPLE)
  target_link_libraries(vibe_icom PUBLIC "-framework IOKit" "-framework CoreFoundation" "-framework Security"
                                         "-framework AudioToolbox" "-framework CoreAudio")
endif()
target_link_libraries(vibeserver_core PUBLIC vibe_icom)
target_compile_definitions(vibeserver_core PUBLIC VIBE_HAVE_ICOM)
# ★ The pretend IC-7300 on a pty, for scripts/test-server-icom.mjs (and anyone without a radio).
add_executable(fake-icom "${CMAKE_CURRENT_SOURCE_DIR}/../tools/icom/fake-icom.cpp")
target_include_directories(fake-icom PRIVATE "${CPP_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fake-icom PRIVATE Threads::Threads)
# ★★ IcomSource against the fake radio: preflight refusals, tuning both ways, TX mute, unplug/replug,
#    and ZERO disallowed frames at the far end. Real threads and the baud rate's own pacing — ~15 s.
add_executable(test-icom-source "${CMAKE_CURRENT_SOURCE_DIR}/test-icom-source.cpp")
target_include_directories(test-icom-source PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test-icom-source PRIVATE vibe_icom)
vibe_sanitize(test-icom-source)
add_test(NAME icom-source COMMAND test-icom-source)
# ★ The scope assembler, synthetic frames + every capture under test/fixtures/icom/ (Saber's go there).
# ★★ V12 LoRa in ISM mode (vibe_lora*.h, docs/v12/LORA-MESH.md) — header-only, like test-ism; PFFFT as the server has it.
add_executable(test-lora "${CMAKE_CURRENT_SOURCE_DIR}/test-lora.cpp" "${CPP_DIR}/vibedsp/third_party/pffft/pffft.c")
target_include_directories(test-lora PRIVATE "${CPP_DIR}")
target_compile_definitions(test-lora PRIVATE VIBE_DAB_PFFFT=1)
target_compile_options(test-lora PRIVATE -O3 -ffp-contract=fast)
target_link_libraries(test-lora PRIVATE Threads::Threads)
vibe_sanitize(test-lora)
add_test(NAME lora COMMAND test-lora)
add_executable(test-icom-scope "${CMAKE_CURRENT_SOURCE_DIR}/test-icom-scope.cpp")
target_include_directories(test-icom-scope PRIVATE "${CPP_DIR}")
target_compile_definitions(test-icom-scope PRIVATE VIBE_ICOM_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../test/fixtures/icom")
vibe_sanitize(test-icom-scope)
add_test(NAME icom-scope COMMAND test-icom-scope)

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
  # ★★ NO LIBRARIES BESIDE THE BINARY (2026-10-18). Until 11.0.5 our own librtlsdr travelled as a
  #    private .so in /usr/lib/vibeserver with a $ORIGIN RPATH; it is linked in now (see the
  #    VIBE_LINUX_DEPS block), so an upgrade simply removes the old file and nothing points at it.
  if(VIBE_ARMV6_STAGE)
    install(PROGRAMS "${VIBE_ARMV6_STAGE}/vibeserver" DESTINATION lib/vibeserver/armv6)
  endif()
  install(FILES linux/vibeserver.service DESTINATION lib/systemd/system)
# ★ The per-radio template and the reconciler that starts the second and later radios.
install(FILES linux/vibeserver@.service linux/vibeserver-radios.service
        DESTINATION lib/systemd/system)
  # ★ /lib/udev/rules.d is the PACKAGE path; /etc/udev/rules.d is for the ADMIN's own overrides.
  #   Shipping into /etc would fight the sysadmin and survive `apt purge`.
  install(FILES linux/99-vibeserver-sdr.rules DESTINATION lib/udev/rules.d)
  # ★★★ And keep the kernel's DVB driver off the dongles — see the file for the wedge it caused.
  install(FILES linux/vibeserver-blacklist-dvb.conf DESTINATION lib/modprobe.d)
  # ★★★ A CAP ON THE JOURNAL, or a receiver fills the disk it was installed on. /usr, not /etc —
  #     so it is a DEFAULT the operator overrides from /etc rather than a policy that fights them.
  #     See the file for the measurement that prompted it.
  install(FILES linux/vibeserver-journald.conf
          DESTINATION lib/systemd/journald.conf.d RENAME 50-vibeserver.conf)
  # ★★ THE MAINTENANCE BUTTONS' PRIVILEGE. The service runs unprivileged AND with
  #    NoNewPrivileges=yes, so it cannot elevate at all — sudo is impossible by design here. The
  #    daemon writes a request file instead; the .path unit notices it and runs the helper as
  #    root. See linux/vibeserver-maintenance for the permitted actions.
  install(PROGRAMS linux/vibeserver-maintenance linux/vibeserver-diagnostics DESTINATION lib/vibeserver)
  install(FILES linux/vibeserver-maintenance.path
                linux/vibeserver-maintenance.service
          DESTINATION lib/systemd/system)
  # ★ Run as root from the unit's ExecStartPre — the daemon is unprivileged and cannot write
  #   sysfs. See linux/vibeserver-governor for why a wrong governor costs 25% of the machine.
  install(PROGRAMS linux/vibeserver-governor linux/vibeserver-radios DESTINATION lib/vibeserver)
  # ★★ THE POCKET IMAGE'S WI-FI (2026-10-10). Shipped to every install so a pocket box is fixed by
  #    apt like anything else, but NOT enabled by postinst, and its unit will not start without the
  #    image's marker (/etc/vibeserver-pocket/enabled). Needs python3, which Raspberry Pi OS Lite has;
  #    no Depends is added because no other install ever runs it.
  # ★★ …and its power-cut hardening: the settings partition (vibeserver-pocket-data) and the one-time
  #    read-only-root seal. Same rule: dormant without the marker, enabled only by the image.
  # ★★★ …and the Portable's OPERATING SYSTEM, so a box flashed once updates fully through Update (Stuart, 2026-10-13:
  #     "after initial setup we need to make sure that VibeServer Portables can be fully updated without needing a full
  #     ISO again"). vibeserver-pocket-converge applies what the image used to do only at flash time (customise.sh calls
  #     it too — one source of truth); pocket-packages.txt is the system packages a Portable needs, which the admin
  #     Update installs when missing. Same rule again: both do NOTHING without the marker, and the list is never a
  #     Depends/Recommends — an ordinary install must not pull a hotspot stack or an initramfs builder.
  install(PROGRAMS linux/pocket/vibeserver-pocket linux/pocket/vibeserver-pocket-data
                   linux/pocket/vibeserver-pocket-seal linux/pocket/vibeserver-pocket-converge
                   linux/pocket/vibeserver-pocket-cpu
          DESTINATION lib/vibeserver)
  install(FILES linux/pocket/pocket-packages.txt DESTINATION lib/vibeserver)
  # ★★ vibeserver-pocket-cpu (2026-10-25): a Portable card picks the ARMv6 build on a CPU without NEON at boot —
  #    the card was installed on the BUILD machine, so postinst's own pick was for the wrong processor.
  # ★★ An Update's initramfs rebuild inside overlayroot-chroot (MODULES=dep cannot find / there) — see the file.
  install(FILES linux/pocket/vibeserver-pocket-initramfs.conf
          DESTINATION share/initramfs-tools/conf.d RENAME vibeserver-pocket)
  install(FILES linux/pocket/vibeserver-pocket.service linux/pocket/vibeserver-pocket-data.service
                linux/pocket/vibeserver-pocket-seal.service linux/pocket/vibeserver-pocket-cpu.service
          DESTINATION lib/systemd/system)

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
  if(VIBE_X86_32)
    # ★ Before the processor tests, for the reason given at VIBE_X86_32: uname says x86_64 in the
    #   linux/386 build container.
    set(_vibe_cf "${_vibe_cf_src}/cloudflared-linux-386")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_vibe_cf "${_vibe_cf_src}/cloudflared-linux-arm64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    # ★ 32-bit ARM (VibeServer Lite: a Pi 2, an old Armbian box). This used to fall to the else()
    #   and bundle the AMD64 binary — a tunnel that could never start on the box it shipped to.
    set(_vibe_cf "${_vibe_cf_src}/cloudflared-linux-arm")
  else()
    set(_vibe_cf "${_vibe_cf_src}/cloudflared-linux-amd64")
  endif()
  if(EXISTS "${_vibe_cf}")
    install(PROGRAMS "${_vibe_cf}" DESTINATION lib/vibeserver RENAME cloudflared)
    install(FILES "${_vibe_cf_src}/LICENSE" DESTINATION share/doc/vibeserver
            RENAME LICENSE.cloudflared)
    message(STATUS "bundling cloudflared: ${_vibe_cf}")
  elseif(VIBESERVER_ALLOW_NO_TUNNEL)
    # ★ The documented escape hatch: a fresh clone that has not run fetch.sh still builds and runs, and the
    #   person asking for it has said out loud that they know the tunnel will be missing.
    message(WARNING "cloudflared not bundled (VIBESERVER_ALLOW_NO_TUNNEL) — the public listing will need one "
                    "on PATH, and installing this package REMOVES any cloudflared a previous one installed")
  else()
    # ★★★ FATAL BY DEFAULT, BECAUSE A PACKAGE WITHOUT IT TAKES SERVERS OFF THE PUBLIC MAP. On 2026-09-20 I
    #     built an amd64 package on Stuart's Lenovo from a tree with no tools/ directory; this branch printed a
    #     WARNING, the package installed, and it DELETED /usr/lib/vibeserver/cloudflared. The machine served
    #     perfectly on the LAN while its public address was dark for forty minutes. Stuart noticed. I did not,
    #     because I had filtered the build output down to "error:".
    # ★★ VIBESERVER_STRICT_RADIOS was no help: only the apt publish scripts pass it, and the packages that go
    #    on Stuart's own machines are built by hand without it. A guard that is off exactly when a human is
    #    driving is not a guard. So the DEFAULT is refusal, and the escape hatch has to be asked for by name.
    message(FATAL_ERROR
            "cloudflared not found at ${_vibe_cf}.\n"
            "A package built now would REMOVE the tunnel from every machine that installs it, and a server\n"
            "behind CGNAT would lose the only address it has.\n"
            "Fix: run tools/cloudflared-desktop/fetch.sh (or copy tools/cloudflared-desktop/bin/ into this\n"
            "tree). To build without it on purpose: -DVIBESERVER_ALLOW_NO_TUNNEL=ON.")
  endif()
  # ★ NO FFmpeg IN THE PACKAGE, AND NONE USED (V12, 2026-10-29). AAC is FAAD2 (DRM AAC, DAB+) and libxaac (xHE-AAC),
  #   both linked in above (docs/v12/LIBXAAC.md). build-deb.sh refuses a package that carries or depends on ffmpeg.
  # ★★ NOT a plain install() — the config is the one file the user edits, and apt must never
  #   clobber it on upgrade. Staged elsewhere and copied by postinst ONLY if absent.
  install(FILES linux/vibeserver.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/vibeserver)

  # ── ★★★ THE BUNDLED VECTOR MAP — THE *BASIC* PACK ONLY ────────────────────────────────────────
  #
  # The client draws its own map from assets/mapdata/v1 instead of fetching OpenStreetMap tiles, so
  # a receiver in a loft with no outbound internet still has coastlines. The dataset is in two packs
  # (declared in that directory's index.json): `basic`, ~22 MB, which travels inside this package
  # and is therefore always present; and `detail`, ~221 MB, which does NOT — see
  # briefs/BRIEF-vibeserver-mapdata.md and the Recommends: line below.
  #
  # ★★★ THE FILE LIST COMES FROM index.json, NEVER FROM A LIST HERE. The data is generated
  #     (scripts/gen-map-data.mjs) and its shape changes — shards appear, a layer is added, the
  #     relief becomes tiles. A hand-copied list would ship a package that installs cleanly and is
  #     missing a layer, which reads as a renderer bug. Reading the generator's own manifest means
  #     the package cannot disagree with the data.
  # ★★ INTO lib/vibeserver/mapdata, beside the other things we own. The binary finds it at
  #    $exe/../lib/vibeserver/mapdata (see vibe_mapdata.h), which is the same "wherever we are
  #    unpacked" relation as everything else in lib/vibeserver — not an absolute path baked in.
  # ★ NOT gzipped here: the server will serve a `<file>.gz` sibling if one exists, but a .deb is
  #   already xz-compressed as a whole, so a pre-gzipped payload would compress twice for nothing.
  set(_vibe_map_src "${CMAKE_CURRENT_SOURCE_DIR}/../assets/mapdata/v1")
  set(_vibe_map_files "")
  if(EXISTS "${_vibe_map_src}/index.json")
    file(READ "${_vibe_map_src}/index.json" _vibe_map_index)
    string(JSON _vibe_map_n ERROR_VARIABLE _vibe_map_err
           LENGTH "${_vibe_map_index}" packs basic files)
    if(_vibe_map_n AND NOT _vibe_map_err)
      math(EXPR _vibe_map_last "${_vibe_map_n} - 1")
      foreach(i RANGE ${_vibe_map_last})
        string(JSON _f GET "${_vibe_map_index}" packs basic files ${i})
        # ★ A declared file that is not on disk is a HALF-GENERATED dataset, and shipping it would
        #   produce exactly the "layer missing on the live server" fault this list exists to avoid.
        if(EXISTS "${_vibe_map_src}/${_f}")
          list(APPEND _vibe_map_files "${_vibe_map_src}/${_f}")
        else()
          message(WARNING "map data: ${_f} is in index.json's basic pack but not on disk — "
                          "re-run scripts/gen-map-data.mjs")
        endif()
      endforeach()
      list(APPEND _vibe_map_files "${_vibe_map_src}/index.json")
    endif()
  endif()
  list(LENGTH _vibe_map_files _vibe_map_count)
  if(_vibe_map_count GREATER 20)
    install(FILES ${_vibe_map_files} DESTINATION lib/vibeserver/mapdata)
    message(STATUS "bundling map data: ${_vibe_map_count} file(s) (basic pack) -> lib/vibeserver/mapdata")
  elseif(VIBESERVER_ALLOW_NO_MAPDATA)
    message(WARNING "map data NOT bundled (VIBESERVER_ALLOW_NO_MAPDATA) — the client will draw no "
                    "map, and installing this package REMOVES the map data a previous one installed")
  else()
    # ★★★ FATAL BY DEFAULT, FOR THE REASON THE cloudflared BRANCH ABOVE LEARNED THE HARD WAY: a
    #     package built from a tree without the data does not merely lack it, it DELETES the copy
    #     already installed on every machine that upgrades. The server keeps serving radio
    #     perfectly — the map simply goes blank — so nothing in a log would say what happened.
    message(FATAL_ERROR
            "no usable map data at ${_vibe_map_src} (need index.json with a packs.basic file list).\n"
            "A package built now would REMOVE the map from every machine that installs it.\n"
            "Fix: node scripts/gen-map-data.mjs\n"
            "To build without it on purpose: -DVIBESERVER_ALLOW_NO_MAPDATA=ON.")
  endif()

  # ── ★★★ THE GPU MAP — renderer, style, glyphs, icons, basic + relief PMTiles ──────────────────
  # Served at /mapgl/ by vibe_mapgl.h, found at $exe/../lib/vibeserver/mapgl. The optional High
  # Detail pack is NOT here: the owner downloads it from the admin page into
  # /var/lib/vibeserver/mapgl (StateDirectory= makes that ours; nothing in postinst needed).
  # ★ Sourced from directory/public/mapgl (plain names: "JetBrains Mono Bold/", ".js") — NOT
  #   assets/mapgl, whose names are mangled for Metro — plus the style from web/mapkit.
  #   scripts/sync-mapgl-assets.mjs fills both from build/maptiles.
  # ★★★ FATAL WHEN MISSING, for the reason the mapdata block above gives: a package built without
  #     them REMOVES the files from every machine that upgrades, and the maps quietly degrade.
  set(_vibe_gl_src "${CMAKE_CURRENT_SOURCE_DIR}/../directory/public/mapgl")
  set(_vibe_gl_style "${CMAKE_CURRENT_SOURCE_DIR}/../web/mapkit/vibemap-style.json")
  set(_vibe_gl_missing "")
  foreach(_f vendor/maplibre-gl.js vendor/maplibre-gl.css vendor/pmtiles.js vendor/vibemapgl.js
             vendor/vibemapgl-compat.js vibemap-basic.pmtiles vibemap-relief.pmtiles vibemap-runways.pmtiles)
    if(NOT EXISTS "${_vibe_gl_src}/${_f}")
      list(APPEND _vibe_gl_missing "${_vibe_gl_src}/${_f}")
    endif()
  endforeach()
  if(NOT EXISTS "${_vibe_gl_style}")
    list(APPEND _vibe_gl_missing "${_vibe_gl_style}")
  endif()
  file(GLOB _vibe_gl_fonts LIST_DIRECTORIES true "${_vibe_gl_src}/fonts/*")
  file(GLOB _vibe_gl_icons "${_vibe_gl_src}/icons/*.png")
  if(NOT _vibe_gl_fonts)
    list(APPEND _vibe_gl_missing "${_vibe_gl_src}/fonts/*")
  endif()
  if(NOT _vibe_gl_icons)
    list(APPEND _vibe_gl_missing "${_vibe_gl_src}/icons/*.png")
  endif()
  if(NOT _vibe_gl_missing)
    install(FILES "${_vibe_gl_src}/vibemap-basic.pmtiles" "${_vibe_gl_src}/vibemap-relief.pmtiles" "${_vibe_gl_src}/vibemap-runways.pmtiles"
                  "${_vibe_gl_style}"
            DESTINATION lib/vibeserver/mapgl)
    install(DIRECTORY "${_vibe_gl_src}/vendor/" DESTINATION lib/vibeserver/mapgl/vendor
            FILES_MATCHING PATTERN "*.js" PATTERN "*.css" PATTERN "LICENSE*")
    install(DIRECTORY "${_vibe_gl_src}/fonts/" DESTINATION lib/vibeserver/mapgl/fonts
            FILES_MATCHING PATTERN "*.pbf")
    install(FILES ${_vibe_gl_icons} DESTINATION lib/vibeserver/mapgl/icons)
    message(STATUS "bundling GPU map files from ${_vibe_gl_src} -> lib/vibeserver/mapgl")
  elseif(VIBESERVER_ALLOW_NO_MAPDATA)
    message(WARNING "GPU map files NOT bundled (VIBESERVER_ALLOW_NO_MAPDATA): missing ${_vibe_gl_missing}")
  else()
    message(FATAL_ERROR
            "GPU map files missing:\n  ${_vibe_gl_missing}\n"
            "A package built now would REMOVE the GPU map from every machine that installs it.\n"
            "Fix: build the packs, then node scripts/sync-mapgl-assets.mjs\n"
            "To build without it on purpose: -DVIBESERVER_ALLOW_NO_MAPDATA=ON.")
  endif()

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
  # ★★★ SINCE 2026-10-18 IT HAS ONE THING LEFT TO SAY: the libc6 floor. Every other library is linked
  #     in (see the VIBE_LINUX_DEPS block), and check-deb-depends.cmake below refuses the package if
  #     shlibdeps ever finds another — a library line here means something linked the system's copy.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
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
  # ★★★ ffmpeg IS NOT A RECOMMENDS, AND MUST NEVER BE ONE AGAIN (V12, 2026-10-29) — VibeServer does not use it: DAB+ and
  #     DRM AAC are FAAD2, xHE-AAC is libxaac, all in process. History: until V12 ffmpeg decoded DAB+ on Linux and was a
  #     Recommends. build-deb.sh fails a package whose control file names it.
  # ★★★ AND THE OPTIONAL MAP DETAIL PACK — RECOMMENDS, EMPHATICALLY NOT DEPENDS. 82 MB compressed,
  #     ~221 MB on disk, and it is the zoomed-in half of the map: coastline shards, second-level
  #     boundaries, minor roads, small places. A receiver is perfectly usable without it (the basic
  #     pack inside this package covers the whole world down to zoom 7) and a Pi on a 4 GB card may
  #     not have room for it at all — so it must never be able to refuse an install.
  #  ★★ Recommends rather than Suggests because apt installs Recommends by default AND keeps them
  #     upgraded, which is what makes the data track the server on a machine with the disk for it. `--no-install-recommends`, or a machine tight on space,
  #     simply gets the basic map.
  #  ★ The server says which layers it has not got, per request, rather than failing silently — see
  #    the 404 wording in vibe_mapdata.h.
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "vibeserver-mapdata-detail")
  # ★★★ THE GUARD: a release package whose Depends names ANY library but libc6 is refused and
  #     deleted, so "nothing from the system but glibc" cannot regress quietly — a -dev package
  #     creeping back into a build image, a new find_library, a dynamic libatomic. Release builds
  #     only (VIBESERVER_STRICT_RADIOS); a dev box's cpack of a system-linked build is its own business.
  if(VIBESERVER_STRICT_RADIOS)
    set(CPACK_VIBE_ALLOWED_DEPENDS "adduser;curl;gzip;libc6")
    # ★★ And the glibc floor: 2.35 is Ubuntu 22.04 (Debian 12 is 2.36). See linux/glibc_floor.c.
    set(CPACK_VIBE_MAX_LIBC "2.35")
    set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/linux/check-deb-depends.cmake")
  endif()
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${CMAKE_CURRENT_SOURCE_DIR}/linux/debian/preinst"
      "${CMAKE_CURRENT_SOURCE_DIR}/linux/debian/postinst"
      "${CMAKE_CURRENT_SOURCE_DIR}/linux/debian/prerm"
      "${CMAKE_CURRENT_SOURCE_DIR}/linux/debian/postrm")
  include(CPack)
endif()
