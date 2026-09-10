#!/usr/bin/env bash
# Run every host test under AddressSanitizer + UBSan, and optionally replay a capture through the
# DAB decoder under them too.
#
#   scripts/sanitize-tests.sh                     the test suites
#   scripts/sanitize-tests.sh path/to/cap.raw     …and replay that capture through dab-offline
#
# ★★★ WHY THIS EXISTS. A buffer overrun in DSP code does not crash — it reads a neighbouring
#     float, and the damage surfaces three frames later as a burst of errors nobody can place.
#     Every one of the five DAB bugs of 2026-09-07 presented that way. ASan reports the read AT
#     the instruction, with the allocation it belongs to; UBSan does the same for a shift past the
#     width, a signed overflow, or a misaligned load.
# ★★ SEPARATE BUILD DIRECTORIES. A sanitised object must never end up in a release binary, so
#    these live in *-san and nothing else looks at them.
# ★ The server binary is NEVER sanitised — see the note in vibeserver/CMakeLists.txt.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CAP="${1:-}"
# ★ Leak detection OFF: these harnesses exit without freeing on purpose, and a "leak" at exit is
#   noise that hides the reports that matter. Overruns and UB are what we are here for.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:print_stacktrace=1"
export UBSAN_OPTIONS="print_stacktrace=1"
fail=0

echo "==> vibedsp suites (ASan + UBSan)"
DSPT="$ROOT/android/app/src/main/cpp/vibedsp/test"
cmake -S "$DSPT" -B "$DSPT/build-san" -DVIBE_SANITIZE=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null 2>&1
cmake --build "$DSPT/build-san" -j "$(getconf _NPROCESSORS_ONLN)" >/dev/null 2>&1
for t in "$DSPT"/build-san/vibedsp_*_tests; do
  [ -x "$t" ] || continue
  printf '    %-34s ' "$(basename "$t")"
  if out=$("$t" 2>&1); then echo "ok"; else echo "FAILED"; echo "$out" | tail -25; fail=1; fi
done

echo "==> DAB decoder tests + dab-offline (ASan + UBSan)"
cmake -S "$ROOT/vibeserver" -B "$ROOT/vibeserver/build-san" -DVIBE_SANITIZE=ON \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null 2>&1
cmake --build "$ROOT/vibeserver/build-san" --target dab-tests dab-offline test-utf8 \
      -j "$(getconf _NPROCESSORS_ONLN)" >/dev/null 2>&1
for t in "$ROOT"/vibeserver/build-san/test-dab-* "$ROOT"/vibeserver/build-san/test-utf8; do
  [ -x "$t" ] || continue
  printf '    %-34s ' "$(basename "$t")"
  if out=$("$t" 2>&1); then echo "ok"; else echo "FAILED"; echo "$out" | tail -25; fail=1; fi
done

if [ -n "$CAP" ]; then
  echo "==> replaying $CAP through dab-offline (ASan + UBSan)"
  if out=$("$ROOT/vibeserver/build-san/dab-offline" "$CAP" 2>&1); then
    echo "$out" | tail -12
  else
    echo "FAILED"; echo "$out" | tail -40; fail=1
  fi
fi

[ "$fail" = 0 ] && echo "==> clean under ASan + UBSan" || echo "==> SANITISER FINDINGS ABOVE"
exit "$fail"
