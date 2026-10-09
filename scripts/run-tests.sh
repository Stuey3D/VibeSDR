#!/usr/bin/env bash
# run-tests.sh — build and run every C++ test in vibeserver/.
#
# ★★ THEY WERE NOT REGISTERED ANYWHERE. Each test-*.cpp was compiled by hand when it was written
#    and never again, so nothing would have told us if one started failing — and on this project
#    the tests are the method, not the paperwork. A suite nobody runs is a suite that is already
#    broken and has not been told yet.
#
# ★ Each test states its own dependencies. Ones that need downloaded data (geoip, asn) are run and
#   reported honestly rather than quietly skipped: "needs data" is a result, not a pass.
set -uo pipefail
cd "$(dirname "$0")/.."

SRC=vibeserver
OUT="${TMPDIR:-/tmp}/vibeserver-tests"
mkdir -p "$OUT"

VDSP=android/app/src/main/cpp/vibedsp
KISS="$VDSP/third_party/kissfft"

# test name -> extra compiler flags it needs
flags_for() {
  case "$1" in
    # ★ The DSP engine lives outside vibeserver/ and carries its own vendored kissfft, so this one
    #   needs both on the include path. Worth it: it is the only test that drives the real audio
    #   chain end to end.
    test-wfm-stereo) echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ Same deps as test-wfm-stereo: it drives the same real audio chain, which is the only way
    #   to measure a feature that responds to FM's triangular noise spectrum.
    test-stereo-highblend) echo "-O2 -I $VDSP -I $KISS" ;;
    test-multipath-meter)  echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ The Advanced RDS instrument (MpxMeasure) against a signal of known deviation, at several
    #   capture rates and passbands — the real pipeline end to end, so the same deps again.
    test-mpx-measure|test-rds-ps-frames) echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ WFM DSP cost vs passband — a narrow width once cost ~5x the whole chain (the Sony's
    #   161-276 % of real time). Optimised like the server, or the ratio is not the server's.
    test-wfm-narrow-cost)  echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ Every mode's DSP cost vs passband, both directions and past the ceiling — the audit that
    #   followed the WFM cliff (2026-09-29). Optimised for the same reason.
    test-passband-cost)    echo "-O2 -I $VDSP -I $KISS" ;;
    # ★★★ Every chain delivers exactly 48 000 audio samples per second of capture (2026-10-07: the RSPs
    #     ran 32-34 ppm short through a rounded, approximated resampler ratio). Optimised: 21 s of input per chain.
    test-resampler-rate)   echo "-O2 -I $VDSP -I $KISS" ;;
    # ★ The real decoders on real signals (RTTY, WEFAX, an encoded FT8 slot) — optimised so FT8's
    #   slot decode finishes in a second rather than ten.
    test-decoder-hosts)    echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    test-decfeed-resampler) echo "-O2 -I android/app/src/main/cpp" ;;
    test-bm-names) echo "-O2 -I android/app/src/main/cpp" ;;
    # ★ The benchmark's decoder rows — optimised like the server, or the costs are not the server's.
    test-bench-decoders)   echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ Hostile attach messages through the real decoders (audit 2026-10-03) — same deps as the hosts.
    test-decoder-hardening) echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ SSTV geometry + colour on synthetic pictures with a known sender clock error (audit 2026-10-04)
    #   — 16 full frames, ~25 min of audio; optimised or it takes minutes.
    test-sstv-quality)     echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ FT8's hashed-callsign table + spot filter (audit 2026-10-04, row 10), through ft8_lib's real pack/unpack.
    test-ft8-callhash)     echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    # ★ AirspyHfSource against a STUB libairspyhf (the test defines the C API) — the vendored header
    #   for the types, and the fd entry points switched on (2026-10-05).
    test-airspyhf-restart) echo "-DVIBE_HAVE_AIRSPYHF -DVIBE_AIRSPYHF_HAS_FD -I android/app/src/main/cpp/libairspyhf -lpthread" ;;
    # ★ HackRfSource + AirspySource against STUB libhackrf/libairspy, and vibe_usb_recovery.h's rules
    #   (park-not-release, the fresh-fd back-off, the re-plug advice) — 2026-10-05.
    test-fd-radio-recovery) echo "-DVIBE_HAS_HACKRF -DVIBE_HACKRF_HAS_FD -DVIBE_HAVE_AIRSPY -I android/app/src/main/cpp/libairspy -lpthread" ;;
    # ★★ FT8's extra passes, their CPU gate and the capture-clock slot (2026-10-05) — optimised, it times passes.
    test-ft8-passes)       echo "-O2 -I android/app/src/main/cpp/ft8_lib" ;;
    *)               echo "" ;;
  esac
}

# test name -> the sources it needs besides itself
deps_for() {
  case "$1" in
    test-wfm-stereo)    echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-stereo-highblend) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-multipath-meter) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-mpx-measure|test-rds-ps-frames) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-wfm-narrow-cost) echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-passband-cost)   echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    test-resampler-rate)  echo "$VDSP/pipeline.cpp $VDSP/mpxmeasure.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp $VDSP/iqclean.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c $VDSP/third_party/pffft/pffft.c" ;;
    # ★★ proc.cpp goes with anything that SHELLS OUT (curl, mostly): geoip, asndb and radiodns all
    #    call vibeproc::run. It was missing, so those three "did not build" — and a suite that does
    #    not build is not a suite that passes, it is one nobody is running. Caught 2026-09-21.
    test-config-radios) echo "$SRC/vibeserver_config.cpp" ;;
    test-converter)     echo "$SRC/vibeserver_config.cpp" ;;
    test-rtl-eeprom)    echo "$SRC/rtl_eeprom.cpp" ;;
    test-fd-passing)    echo "android/app/src/main/cpp/fd_passing.cpp" ;;
    # ★★ The HF+'s recovery ORDER — hung-restart deadline, fd dup ownership, rate guard, fresh-fd reopen.
    test-airspyhf-restart) echo "android/app/src/main/cpp/airspyhf_source.cpp" ;;
    # ★★ The same recovery for the HackRF and the R2/Mini — dup ownership, deadline release, fresh-fd replay.
    test-fd-radio-recovery) echo "android/app/src/main/cpp/hackrf_source.cpp android/app/src/main/cpp/airspy_source.cpp" ;;
    test-parent-watch)  echo "$SRC/parent_watch.cpp" ;;
    test-connlog)       echo "" ;;
    # ★★★ An admin read must never hold the connection log's lock while it works (Pi 2 IQ overruns
    #     with the admin page open, 2026-10-01). Header-only; slow resolvers make it CPU-independent.
    test-connlog-lock)  echo "" ;;
    # ★★ The map's shared byte budget (vibe_bulk_pace.h), on a synthetic clock.
    test-bulk-pace)     echo "" ;;
    # ★★ A burst of tunes: the newest wins, paced by the server's load (vibe_tune_pace.h, 2026-10-05).
    test-tune-pace)     echo "" ;;
    test-time-decoder)  echo "android/app/src/main/cpp/decoders/time_decoder.cpp" ;;
    test-radiodns-ecc)  echo "$SRC/radiodns.cpp $SRC/proc.cpp" ;;
    test-radiodns-name) echo "$SRC/radiodns.cpp $SRC/proc.cpp" ;;
    test-geoip)         echo "$SRC/geoip.cpp $SRC/proc.cpp" ;;
    test-asndb)         echo "$SRC/asndb.cpp $SRC/proc.cpp" ;;
    test-admin-banlist) echo "" ;;
    test-decoder-hardening) echo "android/app/src/main/cpp/spyserver/spyserver_messages.cpp android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp \
                              android/app/src/main/cpp/decoders/sstv_decoder.cpp android/app/src/main/cpp/decoders/time_decoder.cpp \
                              android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    # ★ NAVTEX's CCIR 476 coder, word by word (audit 2026-10-04, rows 11-13); the audio half is scripts/test-navtex.sh.
    test-navtex-fec)    echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp" ;;
    test-ft8-callhash)  echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    test-ft8-passes)    echo "android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    test-decoder-hosts|test-bench-decoders) echo "android/app/src/main/cpp/decoders/fsk_decoder.cpp android/app/src/main/cpp/decoders/rtty_auto.cpp android/app/src/main/cpp/decoders/wefax_decoder.cpp \
                              android/app/src/main/cpp/decoders/sstv_decoder.cpp android/app/src/main/cpp/decoders/time_decoder.cpp \
                              android/app/src/main/cpp/decoders/ft8_decoder.cpp" ;;
    test-sstv-quality)  echo "android/app/src/main/cpp/decoders/sstv_decoder.cpp" ;;
    test-wefax-phasing) echo "android/app/src/main/cpp/decoders/wefax_decoder.cpp" ;;
    *)                  echo "" ;;
  esac
}

# test name -> C sources it needs. ★ Compiled as C, separately: g++ would compile a .c file as C++,
#   and ft8_lib is C that C++ refuses (malloc without a cast). Prints the object files built.
FT8C=android/app/src/main/cpp/ft8_lib
cobjs_for() {
  case "$1" in
    test-decoder-hosts|test-bench-decoders|test-decoder-hardening|test-sstv-quality|test-ft8-callhash|test-ft8-passes)
      local d="$OUT/cobj-ft8"; mkdir -p "$d"
      for f in $FT8C/ft8/*.c $FT8C/fft/kiss_fft.c $FT8C/fft/kiss_fftr.c $FT8C/common/monitor.c; do
        local o="$d/$(basename "$f" .c).o"
        [ "$o" -nt "$f" ] || cc -O2 -c -I "$FT8C" -o "$o" "$f" || return 1
        printf '%s ' "$o"
      done ;;
    *) ;;
  esac
}

pass=0; fail=0; broke=0; notrun=0
for t in "$SRC"/test-*.cpp; do
  name="$(basename "$t" .cpp)"
  # ★ Not every test fits this harness. It compiles ONE .cpp with a named handful of deps, which is
  #   what keeps it fast and dependency-free — but a test that needs the whole core (real drivers,
  #   the shim, opus, fftw) cannot be expressed that way and is a CMake target instead. Listing it
  #   here rather than renaming it keeps it discoverable beside its siblings.
  #     test-radio-api — links vibeserver_core; built by `cmake --build … --target test-radio-api`.
  case "$name" in
    test-radio-api) printf '\n\033[1m── %s ──\033[0m\n   \033[2mskipped — a CMake target (needs the whole core); build it with cmake\033[0m\n' "$name"; continue ;;
  esac
  printf '\n\033[1m── %s ──\033[0m\n' "$name"
  # shellcheck disable=SC2046
  if ! cobj="$(cobjs_for "$name" 2>"$OUT/$name.buildlog")" || \
     ! g++ -std=c++17 -I "$SRC" -I android/app/src/main/cpp $(flags_for "$name") \
        -o "$OUT/$name" "$t" $(deps_for "$name") $cobj 2>>"$OUT/$name.buildlog"; then
    printf '   \033[33mdid not build\033[0m — %s\n' "$OUT/$name.buildlog"
    head -5 "$OUT/$name.buildlog" | sed 's/^/     /'
    broke=$((broke+1)); continue
  fi
  if "$OUT/$name"; then pass=$((pass+1)); else fail=$((fail+1)); fi
done

# ★ The setup page is a C++ raw string, so no compiler ever looks at its JavaScript. A syntax
#   error there ships silently and the page simply does nothing — the worst failure this tree has,
#   because that page is what a new owner meets first.
printf '\n\033[1m── setup page ──\033[0m\n'
if node scripts/check-setup-page.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ Fork labelling (directory forkOf): a fork's name is shown, checked; official names refused as fork names.
if node --no-warnings directory/scripts/test-fork.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ Behaviour, not syntax: the page's own functions driven against a stubbed DOM, because the bug
#   this catches — one radio's sample rate landing in another's config during a tab switch — is
#   perfectly valid JavaScript and perfectly well-formed HTML.
if node scripts/test-setup-tabswitch.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The admin log folds a visit's per-radio rows into one. Pure logic, so it is testable without a
#   browser — and the cases that matter are the ones it must NOT fold (session-less refusals).
if node scripts/test-visit-grouping.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ A DATA MIGRATION, so it is tested: bookmarks move from being keyed on the URL you arrived by
#    to the server's own identity. The failure modes are somebody's bookmarks going invisible on
#    upgrade, or one server adopting another's — neither of which looks wrong in a screenshot.
if node scripts/test-bookmark-scope.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE NR SLIDER'S TWO CONVERSIONS MUST BE EXACT INVERSES. The server echoes the strength it is
#     using and the client renders it back onto the slider, then dispatches `input` — which sends
#     it again. In that loop a units mismatch is not an off-by-a-bit, it is a RATCHET: the setting
#     climbed 30 -> 100% on its own, on air (2026-08-26). Neither half looks wrong on its own.
if node scripts/test-nr-roundtrip.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The two ends of the advanced-RDS message must agree on field NAMES. A stray "R." prefix meant
#   the deviation readout never populated at all, and neither half looked wrong on its own.
if node scripts/check-rdsx-wire.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The web playout (worklet AND the main-thread fallback) must come back clean by itself after the
#   server has delivered audio in bursts — the Sony's narrow-WFM overload, 2026-09-29. Silent.
if node scripts/test-web-playout-burst.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The web decoder socket says whose it is (user_session_id) and takes a refusal — shown in the
#   server's words and FORGOTTEN, so the 3-s reconnect cannot turn one "no" into a loop (B6).
if node scripts/test-web-decoder-refusal.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE WEB CONTROL COLOURS (controlColours.ts): the default look sets no variable (today, to the
#    pixel), every swatch combination stays ≥ 3:1, SOLID is opaque with no backdrop-filter anywhere, the
#    keys travel as VIEW_KEYS, and an older browser starts solid unless a stored choice says otherwise.
if node --no-warnings scripts/test-web-control-colours.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE DESKTOP BAR STAYS GONE. The compact card (#mcard) is the web layout at every width; the old
#     bar shipped hidden for months, still wired, and fixes landed on it that reached nobody. Checks the
#     BUILT page (web/dist — run node scripts/build-web.mjs): no removed id as an element, a selector or
#     a script literal, and the bar's real leftovers (#linkStats, search, bandwidth row) where they live.
if node scripts/test-web-no-desktop-bar.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE FACEPLATE RULES (app). Nixie One is neon in every chassis × display × colour, dot/seg can
#     never reach white by any route (picker, migration, stored prefs), and the DEFAULT settings
#     resolve to today's literal colours — the pixel-identical promise, checked as data.
#  ★ Node runs the .ts directly (type stripping), so this needs nothing installed.
if node --no-warnings scripts/test_faceplate.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ proxy.kiwisdr.com IS 8073 ONLY (2026-10-07): 47 % of public Kiwis are listed portless (= port 80, refused).
if node --no-warnings scripts/test_kiwi_proxy_port.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ WHAT A VFD CAN SHOW (faceplates §7). Every printable ASCII character is exactly one DSEG14
#    cell (its space is 200 wide and its ! is the BLANK cell), accents fold like a display ROM,
#    units keep their case, and a string that folds to nothing shows the frequency, never tofu.
if node --no-warnings scripts/test_faceplate_text.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE VCR MODE BOX (2026-10-06): every label the app composes fits the ten-cell field whole (WFM keeps the
#    rings' slot, every other mode gets those cells), and the "-88+88 dB F S" readout lights only electrodes it has.
if node --no-warnings scripts/test_faceplate_segfield.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ EVERY dB ON THE VCR HAS A LOWER-CASE d (2026-10-06, Stuart: "nitpickers will have us for it"): the VTS strip,
#    the DAB meter, the notices, the status row and its chips, the mode box — and no D in a word (DAB, BBC, AUDIO…) moves.
if node --no-warnings scripts/test_faceplate_vcr_db.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ EVERY BIG ON-SCREEN ELEMENT FOLLOWS THE MAIN DISPLAY'S FONT (2026-10-06): one source (faceplate.ts ScreenText),
#    every listed element reading it, Nixie One's glyphs and widths from its TTF, and the status row fitting in it.
if node --no-warnings scripts/test_faceplate_screenfont.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE DOT MODE BOX (2026-10-06): the same fields as true 5 × 7 dot-matrix cells — Doto's own dots (read out of the
#    TTF and checked against it), every label whole, the rings in dots, "S9+27 dBFS" / "kHz" fixed width.
if node --no-warnings scripts/test_faceplate_dotfield.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ NIXIE: real hardware never adds or removes a tube. Units change only the lit bulb, leading
#     zeros are switched off, and the TUBE shrinks to its window (smallest window first) — domes
#     are never clipped.
if node --no-warnings scripts/test_faceplate_nixie.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE METERS (faceplates §4): one deck height across meter × shared, the squelch ring on the SAME
#     table the LEDs light from, Φ((μ − T)/σ) with its σ floor, the eye filter as the only easing, the
#     steady-LED hysteresis, and the needle's 300 ms / 1 % ballistics — the maths, checked as data.
if node --no-warnings scripts/test_faceplate_meters.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ WORKLETS AS THE UI THREAD RUNS THEM (the 11 B7 LED VU crash): a default parameter that names a
#     module constant THROWS on the UI thread (the plugin unpacks captures inside the body), so every
#     worklet in src/ is compiled with the app's plugin and checked, and the meters' frame callbacks are
#     rebuilt from the plugin's output with no module scope and run — the test above cannot see it.
if node --no-warnings scripts/test_worklet_defaults.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ No photo leaves this repo carrying EXIF — GPS above all (2026-10-03: twelve did, two of them on the live website).
if node --no-warnings scripts/test_safe_text.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ Untrusted URLs (deep links, directories, a server's landing link) and the map page's literal/path guards.
if node --no-warnings scripts/test_safe_url.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ The web client's `host` is an authority (+ /r/<id>) and nothing else (CodeQL #97-#99, 2026-10-05).
if node --no-warnings scripts/test_server_host.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ The server screen's settings are read in ONE store call (Lite's "eternity" to load, 2026-10-06).
if node --no-warnings scripts/test_server_prefs.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ A gzipped spectrum-socket frame cannot inflate past its ceiling (gzip bomb).
if node --no-warnings scripts/test_bounded_inflate.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ The map WebView page parses, for all three kinds (a template-literal page tsc cannot check).
if node scripts/check-map-overlay.mjs >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ node scripts/check-map-overlay.mjs"; fi
# ★★ The DAB transmitter-site importers (2026-10-06): coordinates, the three regulators' file shapes,
#    provenance in every generated header. Fixtures only — no network. (test-dab-txdb.cpp above is the matching.)
if python3 scripts/dab-sites/test_dabsites.py; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ python3 scripts/dab-sites/test_dabsites.py"; fi
# ★★ …and the panel says whose record each licensed site is (app + web share dabLicensedTail).
if node --no-warnings scripts/test_dab_licensed.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
echo "── image metadata (EXIF / GPS / XMP) ──"
if python3 scripts/strip-image-metadata.py --check; then pass=$((pass+1)); echo "  none"; else fail=$((fail+1)); echo "  ✗ run: python3 scripts/strip-image-metadata.py"; fi
# ★★★ A FACEPLATE CANNOT LOCK YOU OUT (faceplate.ts CRASH SAFETY): the mark is armed before a risky
#     faceplate draws and cleared after 5 s / on leaving the foreground; a launch that finds it comes up
#     on HYPER / BAR / DEFAULT with the choice kept aside — a crash, a clean exit, a swipe-away, a torn
#     mark and a refusing disk, across fake launches.
if node --no-warnings scripts/test_faceplate_safety.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE DRUM WELLS (faceplates §6): the default well is today's drum as data, the needle is gone on
#    every chassis, the aluminium notch pair inverts (and its draw order is a token), the LED pool's
#    geometry, the controls colour at the brief's brightness, and the tuner keys' 31 % / 34 % layout.
if node --no-warnings scripts/test_faceplate_wells.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE LANDSCAPE DECK (faceplates §9): never taller than today's band at 568 → 1366 pt for every
#     chassis × meter × shared, four equal keys, no negative or overlapping column, the drums' 80 pt,
#     real Nixie tubes that fit the window, and the SE's fall-backs (no labels; the bar).
if node --no-warnings scripts/test_faceplate_landscape.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE STEREO LIGHT IN DAB is the playing service's (its audio headers), never the FM pilot's — the
#    server's `stereo` and, for an older server, its codec line; app and web share the one function.
if node --no-warnings scripts/test_dab_stereo.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE DAB RECEPTION METER (2026-10-06): three bars + a sentence in place of the signal bar, judged on ~5 s
#     of the server's counters with hysteresis; thresholds held to the on-air cases (app + web share it).
if node --no-warnings scripts/test_dab_quality.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ An unchanged DAB service list keeps its array and objects across the once-a-second reports.
if node --no-warnings scripts/test_dab_share_services.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ DAB tuning keys: a burst of presses is ONE block change on the server (the one stopped on), and
#    leaving DAB drops the DAB station from the VTS (Stuart, 2026-10-05 — both).
if node --no-warnings scripts/test_dab_stepper.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ ANSWERS TO "Anyone know what this is?" (Stuart, 2026-10-09): none on the everyday pad, only the dial's band while a
#     question is open, every link a Signal Identification Wiki page, every DECODE a decoder we run, the watches agree.
if node --no-warnings scripts/test_dial_answers.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ A RADIO WITH ITS OWN PIN IS NEVER OPENED WITHOUT IT (2026-10-09, Buddy → the Pi 500's Airspy, blank screen).
if node --no-warnings scripts/test_radio_pin_paths.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ TUNE PACING ON THE CLIENT (tunePace.ts, Stuart 2026-10-05: "if the server's CPU is reporting that it is
#     struggling we need to slow down the amount of tune commands"): the health rung, the snail and the ping
#     set the gap; latest wins, the first tune goes at once, the last one always lands.
if node --no-warnings scripts/test_tune_pace.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node --no-warnings scripts/test_sprite_sizing.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node --no-warnings scripts/test_sprite_cache.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★ The waterfall jitter buffer's pooled frame copies (framePool.ts): independent copies, reuse, bounded.
if node --no-warnings scripts/test_frame_pool.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node --no-warnings scripts/test_platformCopy.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ The diagnostics header's RC label (version.ts RELEASE_LABEL) matches About's newest release (2026-10-07).
if node --no-warnings scripts/test_release_label.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node --no-warnings scripts/test_grid.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
if node scripts/test_derived_values_pure.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE DECODER BOXES (faceplates §10.2): every text role ≥ 4.5:1 (WCAG luminance, composited over
#    what it sits on) in every chassis × controls colour × text colour × Transparency on/off; the
#    meaning colours never move, the text colour never reaches a box, no blur on silver / black or OFF.
if node --no-warnings scripts/test_decoder_contrast.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ TRANSPARENCY EFFECTS: each low-end signal alone turns the default OFF, a stored choice always
#     wins and the auto default is never saved as chosen, decoderBg migrates (solid = chosen OFF,
#     transparent = NOT chosen), OFF is alpha 1.0 exactly with no scrim or drop shadow, no darker/lighter than the glass on a
#     dark waterfall, and every file that draws a BlurView reads the switch.
if node --no-warnings scripts/test_transparency.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ FRAME RATE (power audit 2026-10-01): the 60 Hz row is hidden on a 60 Hz panel and when the binary
#     cannot say, labelled with the panel's real rate, defaults to no cap, survives the crash reset,
#     and both native modules export the two methods JS calls.
if node --no-warnings scripts/test_frame_rate.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ MAC VOLUME + MUTE (2026-10-01): shown only on iOS-on-a-Mac, unity gain everywhere else, MUTE is gain 0,
#     every native audio ingress plays through the one mixer that carries the gain, recordings are taken
#     before it, and the deck's speaker key shows a prohibition sign in the legend colour (never red).
if node --no-warnings scripts/test_mac_audio.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ POPUPS TAKE THE CHASSIS (faceplates §10.3): the default chassis is today's gold, value for value;
#     no popup holds a hard-coded gold; engraved text clears 4.5:1 on silver and black at the plate's
#     worst lighting; the tune entry follows the Display (Nixie One only ever neon); and with
#     Transparency OFF no popup dims the waterfall or blurs it.
if node --no-warnings scripts/test_popup.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ EDGE CHIPS ON THE GLASS (edgeChipGeometry.ts, Stuart 2026-10-06): the health / time cards and their tabs sit at
#     the PHYSICAL edge with content padded by the safe-area inset — portrait, both landscapes, iPad, a Mac resize.
if node --no-warnings scripts/test_edge_chip.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_edge_chip.ts"; fi
# ★★★ THE CANNED CHAT PAD (chatPad.ts, capSheen.ts): the phrase pad scrolls inside a cap that never
#     overflows the drawer on any phone (the SE lost phrases 7–14 below its edge), and a dome key's
#     sheen is flex shares, never percentage heights (Yoga drew ghost slabs over a multi-line wrap).
if node --no-warnings scripts/test_chat_pad.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ The FM-DX screen grows its dial + logo into a big window; a phone-sized window keeps today's layout exactly.
if node --no-warnings scripts/test_fmdx_layout.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_fmdx_layout.ts"; fi
if node --no-warnings scripts/test_wefax_align.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_wefax_align.ts"; fi
if node --no-warnings scripts/test_wefax_crisp.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_wefax_crisp.ts"; fi
if node --no-warnings scripts/test_wefax_lost.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_wefax_lost.ts"; fi
# ★★ NAVTEX MESSAGES (src/utils/navtex.ts, 2026-10-05): ZCZC…NNNN blocks from the text stream with a damaged header
#    or trailer, [start lost] / [end lost], one PREV like WEFAX, every chunk size.
if node --no-warnings scripts/test_navtex.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_navtex.ts"; fi
if bash scripts/test-rtty-auto.sh >/dev/null 2>&1; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test-rtty-auto.sh"; fi
if node --no-warnings scripts/test_tune_hint.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_tune_hint.ts"; fi
# ★★★ A REOPENED SPECTRUM SOCKET CARRIES THE LISTENER'S OWN VFO BACK on a per-listener dial, and NEVER on a
#     shared one (2026-10-05: the RSP on a locked range reset to its landing on every resume from background).
if node --no-warnings scripts/test_reopenTune.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_reopenTune.ts"; fi
# ★ The directory Worker against real SQLite: dead-address pages, and the 90-day retention (PRIVACY.md, Play form).
for t in directory/scripts/test-gone.mjs directory/scripts/test-retention.mjs; do
  if node --no-warnings "$t" >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ $t"; fi
done
if node --no-warnings scripts/test_rtty_spec.ts >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test_rtty_spec.ts"; fi
# ★★ NAVTEX on audio (audit 2026-10-04): impulse bursts + selective fading, CER against the old decoder's numbers, and
#    the BEL control byte never in the output.
if bash scripts/test-navtex.sh >/dev/null 2>&1; then pass=$((pass+1)); else fail=$((fail+1)); echo "  ✗ scripts/test-navtex.sh"; fi
# ★★★ SHARE A STATION (canned chat, app + web): a bookmark's LABEL never reaches the payload, the server's
#     relayed line is what is drawn, and TUNE asks first on a shared dial somebody else is on. The server half
#     (validation, naming, the line itself) is vibeserver/test-chat-share.cpp, run with the C++ tests above.
if node --no-warnings scripts/test_chat_share.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ HIDE THIS USER (chatHide.ts, 2026-10-05): a hidden sender's lines go, past and new; own and system lines
#     never; the name is matched exactly; session only, client-side only, never in canned mode.
if node --no-warnings scripts/test_chat_hide.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE SMALL-SCREEN STATUS ROW (faceplates §8.2): dropped strictly in order (IF first, the recording
#     timer last), SHARED TUNER shortens before it goes, the connection meter never goes, portrait never
#     drops, the row packs before anything drops, and a 1 pt wobble cannot flap (hysteresis).
if node --no-warnings scripts/test_faceplate_status.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ FACEPLATE LIGHTING + VFD GLASS (briefs/BRIEF-lighting-and-vfd-glass.md): the filament wires (count by
#    height, device-pixel snapped), the one light angle (LEFT = today exactly), MOTION EFFECTS, LIGHT ANGLE.
if node --no-warnings scripts/test_faceplate_lighting.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE BLIND TUNES OBEY THE SHARED DIAL (blindTuneGate.ts): lock-screen / headset / car ⏮⏭, a car
#     pick and Siri are refused on a shared dial with others listening (and on spectator / listen-only),
#     as FM-DX's always were; alone or exclusive they work; the native switch and a press agree.
if node --no-warnings scripts/test_blindTuneGate.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ "IS THIS ACTUALLY NEWS?" (renderChurn.ts): a repeated RDS label / bookmark list keeps the old
#    object so the radio screen does not re-render for it, any drawn change still gets through, and
#    the controls-bar clock re-renders on the minute, not every second (power audit, 2026-10-01).
if node --no-warnings scripts/test_renderChurn.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ CONNECTION REFRESH (connectionRefresh.ts): every backend the radio screen hosts gets one, the
#    audio it restarts matches the screen's own mount gates (native Opus after re-registration, the
#    VibeServer/dongle pump, the OWRX/Kiwi adapter), and the row is hidden wherever it is a no-op.
if node --no-warnings scripts/test_connectionRefresh.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★ THE VTS STATION LINE (vtsLine.ts), app AND web: "PI: C363 / Name: RadioText" with no dangling
#    punctuation for a missing part, DAB's SId never called a PI, the coloured runs identical to the
#    plain line, and the web pill dropping band → RDS mark → flag only as far as its measured widths
#    demand (2026-10-01).
if node --no-warnings scripts/test_vtsLine.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi
# ★★★ THE SHARED CHUNK CACHE (chunkCache.ts / chunkVerify.ts / directory store.html): a stored map or
#     admin chunk runs ONLY if it hashes to this server's own page; the build's embedded hash is the
#     served file's (web/dist AND vibe_web_page.h); server B loads from the store with zero fetches; a
#     tampered entry is refused and refetched; the store answers only *.vibeserver.vibesdr.net. Needs
#     web/dist (node scripts/build-web.mjs).
if node --no-warnings scripts/test_chunk_cache.ts; then pass=$((pass+1)); else fail=$((fail+1)); fi

# ★★★ THE REAL SERVER, END TO END (B6): per-listener decoders on a locked range, the decoder limit's
#     refusal, Advanced RDS only to whoever asked on a shared dial, and an audio socket that opens
#     first keeping its codec — through the same WebSockets the clients use, against fake-rtl-tcp.
#  ★ It needs a vibeserver BUILT FROM THIS TREE (VIBESERVER_BIN=…). Without one it is reported as
#    NOT RUN — counted separately, never as a pass: a stale binary would test yesterday's server.
printf '\n\033[1m── server decoders (end to end) ──\033[0m\n'
node scripts/test-server-decoders.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ A RUNNING DECODER KEEPS THE AUDIO CHAIN (2026-10-07, Pi 500 Airspy: the listener's sockets blipped, the
#     chain idled, and the WEFAX socket got nothing for 8 s). Decoder socket alone on a shared dial with
#     --idle-grace 0: lines keep coming, no idle and no park, presence unchanged, and it idles once the
#     decoder stops. Same VIBESERVER_BIN rule.
# ★★★ ONE CANNED VOCABULARY IN FIVE PLACES (2026-10-08, nine social phrases): server, app, web and both watches list
#     the same ids, and a live shared dial carries every one and drops an unknown one. Same VIBESERVER_BIN rule.
printf '\n\033[1m── shared-dial chat phrases (end to end) ──\033[0m\n'
node scripts/test-chat-phrases.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ A TUNNEL WHOSE PROCESS LIVES BUT WHOSE ADDRESS IS DEAD IS REPLACED (2026-10-09, the Sony "online but not
#     responding all day"): a stand-in cloudflared prints a hostname that does not exist and stays alive; the server
#     must fetch its own address, see it dead, replace the tunnel and tell a loopback stand-in directory the new one.
# ★★★ THE EXTERNAL ANTENNA SWITCH (2026-10-09): the real server against a fake MQTT broker playing a Tasmota board —
#     antennas in hwinfo, read-back, break before make, per-band preset, the owner's lock, the admin-only search.
printf '\n\033[1m── the external antenna switch (end to end) ──\033[0m\n'
VIBESERVER_BIN="${VIBESERVER_BIN:-}" node scripts/test-antenna-switch.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

printf '\n\033[1m── a dead tunnel address is replaced (end to end) ──\033[0m\n'
node scripts/test-tunnel-selfcheck.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

printf '\n\033[1m── a decoder alone keeps the audio chain (end to end) ──\033[0m\n'
node scripts/test-server-decoder-alone.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ A POCKETED APP KEEPS ITS CHANNEL (Stuart, 2026-10-05: "why did the full socket drop when minimised
#     though, especially as I had audio and a decoder running"): on a per-VFO radio, closing ONLY the
#     spectrum socket keeps audio and the decoder running; the returning socket adopts the same channel
#     (its first config is its own VFO); closing everything leaves the memo. Same VIBESERVER_BIN rule.
printf '\n\033[1m── per-VFO channel survives the background (end to end) ──\033[0m\n'
node scripts/test-server-vfo-background.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ A RADIO SET TO START IN DAB STARTS IN DAB (Stuart, 2026-10-05: "No server seems to honour the start
#     in DAB mode"): the owner's admin arrival first on a shared dial, a client's restored tune refused, a
#     stranger joining, the exemption standing after the start, and a no-landing negative. Same VIBESERVER_BIN rule.
printf '\n\033[1m── DAB landing (end to end) ──\033[0m\n'
node scripts/test-server-dab-landing.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ EXIT DAB STICKS (Stuart, 2026-10-05 23:06, Pi 2: the box "popped up again … over the MW signal"): no
#     `dab` report after `dab_off` over many cycles, none in reply to a service pick out of DAB, no re-entry,
#     and the log names who asked. Same VIBESERVER_BIN rule.
printf '\n\033[1m── DAB exit (end to end) ──\033[0m\n'
node scripts/test-server-dab-exit.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ SHARE A STATION THROUGH THE REAL SERVER: two listeners on a shared dial; the line the room receives is
#     named from the receiver's own store, a smuggled label is not relayed, out-of-range / closed-mode shares
#     are refused with a reason, flood control covers shares. Same VIBESERVER_BIN rule: not run without one.
printf '\n\033[1m── tune burst (end to end) ──\033[0m\n'
# ★★★ A BURST OF TUNES LANDS ON ITS LAST FREQUENCY (vibe_tune_pace.h, 2026-10-05 — the Pi 2 in the garage):
#     50 tunes in a second, the same 50 as one clump, a mode switch inside a clump, and a loaded server's
#     hold timer landing the trailing tune on its own. Same VIBESERVER_BIN rule: not run without one.
node scripts/test-server-tune-burst.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

printf '\n\033[1m── chat share (end to end) ──\033[0m\n'
node scripts/test-server-chat-share.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ THE WEB CLIENT, AS THE SERVER HANDS IT OUT: the page and every /vs/ script, in every encoding,
#     byte for byte against web/dist, `immutable` on the scripts and no-store on the page, through a
#     /r/<id>/ prefix as well. Same VIBESERVER_BIN rule as above: not run without one.
printf '\n\033[1m── web client serving (end to end) ──\033[0m\n'
node scripts/test-web-serving.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ A PERMANENT SCROLLBAR NEVER COVERS OR SQUEEZES A PANEL'S CONTROLS (Stuart, 2026-10-01: Windows /
#     Edge keeps the bar on screen). Every panel at 390 and 1280 px with a classic 15 px bar forced on:
#     the gutter is reserved, nothing scrolls sideways, every control ends left of the bar. Headless
#     Edge, --mute-audio, throwaway profile. Same VIBESERVER_BIN rule.
printf '\n\033[1m── web panels: the scrollbar lane (real browser) ──\033[0m\n'
node scripts/test-web-scrollbar-lane.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ THE SHARED CHUNK CACHE IN A REAL BROWSER, ACROSS REAL SERVERS: two vibeservers behind a local
#     https front as a/b/c.vibeserver.vibesdr.net, this tree's store.html as the directory's. Server B
#     opens the map and the admin panel with ZERO requests for either; a tampered IndexedDB entry is
#     not run on C. Headless Edge, --mute-audio, throwaway profile. Same VIBESERVER_BIN rule.
printf '\n\033[1m── shared chunk cache (real browser) ──\033[0m\n'
node scripts/test-web-chunk-cache.mjs; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ A LISTENER WHOSE TURN RAN OUT, BACK ON A FREE RADIO (B10, Kiko's server): ended → refused inside
#     the cooldown → after it, admitted on BORROWED time and kept until somebody else wants the radio →
#     handed over with notice when somebody does. Real server, through the LAN address (loopback is
#     exempt). ★ ~4 minutes (a 1-minute turn + the 2-minute cooldown), so only with VIBESERVER_SLOW=1;
#     test-session-turns.cpp above covers the same rule on a synthetic clock in milliseconds.
printf '\n\033[1m── session turn: borrowed time after the cooldown (end to end) ──\033[0m\n'
if [ "${VIBESERVER_SLOW:-0}" = "1" ]; then
  node scripts/test-session-turn.mjs; rc=$?
else
  echo '   not run — ~4 minutes; set VIBESERVER_SLOW=1 (test-session-turns covers the rule quickly)'; rc=3
fi
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ AN OPEN DECODER KEEPS ITS LISTENER'S TURN (2026-10-04, Stuart's minimised + muted WEFAX on the Airspy came
#     back to a FRESH 30 minutes). Shared dial, hard 1-minute limit: decoder-only listener ended at the end of the
#     ORIGINAL turn, decoder refused inside the cooldown. ~80 s, so only with VIBESERVER_SLOW=1.
printf '\n\033[1m── session turn: an open decoder is present (end to end) ──\033[0m\n'
if [ "${VIBESERVER_SLOW:-0}" = "1" ]; then
  node scripts/test-session-decoder-presence.mjs; rc=$?
else
  echo '   not run — ~80 s; set VIBESERVER_SLOW=1'; rc=3
fi
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ MODERN ROOTS FOR OLD PHONES (B10: a Lite on Android 5.1 could not fetch RIPE/APNIC, so Europe had
#     no flags). VibeTls.kt itself, on the desktop JVM, against the real chains, with an old store
#     simulated — and still refusing expired / self-signed / untrusted / wrong-host. Needs the network.
printf '\n\033[1m── TLS roots for old Android (VibeTls) ──\033[0m\n'
bash scripts/test-tls-roots.sh; rc=$?
if [ $rc -eq 0 ]; then pass=$((pass+1)); elif [ $rc -eq 3 ]; then notrun=$((notrun+1)); else fail=$((fail+1)); fi

# ★★★ ONE VERSION, EVERYWHERE IT IS WRITTEN DOWN. app.json does NOT reach the iOS build — the
#     pbxproj owns MARKETING_VERSION and only `expo prebuild` would copy it across, which this
#     project deliberately never runs — so the App Store shipped 10.2 while the app's own About
#     overlay, its User-Agent and the Android build all said 10.3. Caught only because a build was
#     inspected by hand before submitting.
if node scripts/check-versions.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi

printf '\n\033[1m%d suite(s) passed, %d failed, %d did not build, %d not run\033[0m\n' "$pass" "$fail" "$broke" "$notrun"
[ "$fail" -eq 0 ] && [ "$broke" -eq 0 ]
