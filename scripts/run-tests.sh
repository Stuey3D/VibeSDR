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
    *)               echo "" ;;
  esac
}

# test name -> the sources it needs besides itself
deps_for() {
  case "$1" in
    test-wfm-stereo)    echo "$VDSP/pipeline.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c" ;;
    test-stereo-highblend) echo "$VDSP/pipeline.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c" ;;
    test-multipath-meter) echo "$VDSP/pipeline.cpp $VDSP/stereo.cpp $VDSP/rds.cpp $VDSP/fft.cpp \
                              $VDSP/resampler.cpp $VDSP/ddc.cpp $VDSP/channelizer.cpp \
                              $VDSP/zoomspec.cpp $KISS/kiss_fft.c $KISS/kiss_fftr.c" ;;
    test-config-radios) echo "$SRC/vibeserver_config.cpp" ;;
    test-rtl-eeprom)    echo "$SRC/rtl_eeprom.cpp" ;;
    test-fd-passing)    echo "android/app/src/main/cpp/fd_passing.cpp" ;;
    test-parent-watch)  echo "$SRC/parent_watch.cpp" ;;
    test-connlog)       echo "" ;;
    test-time-decoder)  echo "android/app/src/main/cpp/decoders/time_decoder.cpp" ;;
    test-radiodns-ecc)  echo "$SRC/radiodns.cpp" ;;
    test-geoip)         echo "$SRC/geoip.cpp" ;;
    test-asndb)         echo "$SRC/asndb.cpp" ;;
    test-admin-banlist) echo "" ;;
    *)                  echo "" ;;
  esac
}

pass=0; fail=0; broke=0
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
  if ! g++ -std=c++17 -I "$SRC" -I android/app/src/main/cpp $(flags_for "$name") \
        -o "$OUT/$name" "$t" $(deps_for "$name") 2>"$OUT/$name.buildlog"; then
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
# ★ The two ends of the advanced-RDS message must agree on field NAMES. A stray "R." prefix meant
#   the deviation readout never populated at all, and neither half looked wrong on its own.
if node scripts/check-rdsx-wire.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi

# ★★★ ONE VERSION, EVERYWHERE IT IS WRITTEN DOWN. app.json does NOT reach the iOS build — the
#     pbxproj owns MARKETING_VERSION and only `expo prebuild` would copy it across, which this
#     project deliberately never runs — so the App Store shipped 10.2 while the app's own About
#     overlay, its User-Agent and the Android build all said 10.3. Caught only because a build was
#     inspected by hand before submitting.
if node scripts/check-versions.mjs; then pass=$((pass+1)); else fail=$((fail+1)); fi

printf '\n\033[1m%d suite(s) passed, %d failed, %d did not build\033[0m\n' "$pass" "$fail" "$broke"
[ "$fail" -eq 0 ] && [ "$broke" -eq 0 ]
