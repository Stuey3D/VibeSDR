#!/usr/bin/env bash
# Fuzz the DAB parsers that take bytes off the air.
#
#   scripts/fuzz-dab.sh              60 s on each of the five
#   scripts/fuzz-dab.sh 600          ten minutes on each
#   scripts/fuzz-dab.sh 600 mot      ten minutes on the MOT assembler alone
#
# ★★★ WHY. These parsers are fed by a demodulator whose input is noise half the time — a fading
#     multiplex hands them byte soup on every deep fade. A read one byte past an array does not
#     crash there; it returns a neighbouring value and surfaces frames later as errors nobody can
#     place, which is exactly how the five DSP bugs of 2026-09-07 presented. Built with ASan and
#     UBSan so the fuzzer stops AT the instruction.
# ★★ THE CORPUS IS KEPT (vibeserver/fuzz-corpus/<target>) and grows across runs — that is what
#    makes the next run start where the last one left off rather than from nothing.
# ★ A crash is written to vibeserver/fuzz-crashes/ and the run stops non-zero. Reproduce with
#   `vibeserver/build-fuzz/fuzz-dab-<t> <that file>`.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECS="${1:-60}"
ONLY="${2:-}"
TARGETS="${ONLY:-fib mot pad spi epg}"

# ★★★ APPLE'S CLANG HAS NO libFuzzer. Xcode ships the compiler flag and NOT
#     libclang_rt.fuzzer_osx.a, so -fsanitize=fuzzer links and fails ("library not found"). Rather
#     than put a 1.5 GB Homebrew LLVM on a Mac that has run out of disk before, run the fuzzers in
#     the SAME Debian container the packages are built in: real libFuzzer, no Mac disk cost, and
#     the platform the server actually ships on. A local clang that CAN link it is used directly.
BUILD_IN_DOCKER=1
if command -v clang >/dev/null 2>&1; then
  probe="$(mktemp -t fuzzprobe).cc"
  printf 'extern "C" int LLVMFuzzerTestOneInput(const unsigned char*,unsigned long){return 0;}\n' > "$probe"
  if clang++ -fsanitize=fuzzer -o "${probe%.cc}.bin" "$probe" >/dev/null 2>&1; then BUILD_IN_DOCKER=0; fi
  rm -f "$probe" "${probe%.cc}.bin"
fi

if [ "$BUILD_IN_DOCKER" = 1 ]; then
  command -v docker >/dev/null || { echo "!! no libFuzzer locally and no docker to borrow one from"; exit 1; }
  echo "==> no local libFuzzer — building the fuzzers in Debian (clang + libclang_rt)"
  IMAGE=vibeserver-fuzz:bookworm
  docker build -q -t "$IMAGE" - >/dev/null <<'DOCKERFILE'
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends       clang cmake make libclang-rt-14-dev ca-certificates && rm -rf /var/lib/apt/lists/*
DOCKERFILE
  docker run --rm -v "$ROOT:/work" -w /work "$IMAGE" bash -euo pipefail -c '
    cmake -S vibeserver -B vibeserver/build-fuzz -DVIBE_FUZZ=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo           -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
    cmake --build vibeserver/build-fuzz --target dab-fuzzers -j "$(nproc)" >/dev/null'
  RUN_PREFIX=(docker run --rm -v "$ROOT:/work" -w /work "$IMAGE")
else
  cmake -S "$ROOT/vibeserver" -B "$ROOT/vibeserver/build-fuzz" -DVIBE_FUZZ=ON \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null 2>&1
  cmake --build "$ROOT/vibeserver/build-fuzz" --target dab-fuzzers \
        -j "$(getconf _NPROCESSORS_ONLN)" >/dev/null 2>&1
  RUN_PREFIX=()
fi

mkdir -p "$ROOT/vibeserver/fuzz-crashes"
fail=0
for t in $TARGETS; do
  [ -x "$ROOT/vibeserver/build-fuzz/fuzz-dab-$t" ] || { echo "no such target: $t"; exit 1; }
  mkdir -p "$ROOT/vibeserver/fuzz-corpus/$t"
  # ★ Paths are the CONTAINER's when we borrowed one; /work is the repo either way.
  if [ ${#RUN_PREFIX[@]} -gt 0 ]; then base=/work; else base="$ROOT"; fi
  bin="$base/vibeserver/build-fuzz/fuzz-dab-$t"
  corpus="$base/vibeserver/fuzz-corpus/$t"
  printf '==> %-4s %ss  ' "$t" "$SECS"
  if out=$("${RUN_PREFIX[@]}" "$bin" "$corpus" -max_total_time="$SECS" -print_final_stats=1 \
             -artifact_prefix="$base/vibeserver/fuzz-crashes/$t-" 2>&1); then
    echo "$out" | grep -E "^stat::number_of_executed_units|^#[0-9]+.*DONE" | tail -1 \
      | sed 's/^/    /' || echo "clean"
  else
    echo "CRASH"; echo "$out" | tail -30; fail=1
  fi
done
[ "$fail" = 0 ] && echo "==> no crashes; corpora in vibeserver/fuzz-corpus/" || echo "==> CRASHES in vibeserver/fuzz-crashes/"
exit "$fail"
