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
CPPDIR="$ROOT/android/app/src/main/cpp"
OUT="$ROOT/vibeserver/build-fuzz"
mkdir -p "$OUT"

# ★★★ COMPILED DIRECTLY, NOT THROUGH vibeserver/CMakeLists.txt. Every one of these parsers is
#     header-only; going through the server's build made them inherit its librtlsdr and libusb
#     requirement and fail to configure for want of a radio driver they never touch.
# ★★ APPLE'S CLANG HAS NO libFuzzer. Xcode ships the -fsanitize=fuzzer FLAG and not
#    libclang_rt.fuzzer_osx.a, so the link fails with "library not found". Rather than put a
#    1.5 GB Homebrew LLVM on a Mac that has run out of disk before, borrow a real libFuzzer from
#    the same Debian the packages are built in — which is also what the server ships on.
FLAGS=(-O1 -g -std=c++17 "-I$CPPDIR" -fsanitize=fuzzer,address,undefined
       -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
probe="$(mktemp -t fuzzprobe).cc"
printf 'extern "C" int LLVMFuzzerTestOneInput(const unsigned char*,unsigned long){return 0;}\n' > "$probe"
if command -v clang++ >/dev/null 2>&1 && clang++ -fsanitize=fuzzer -o "$probe.bin" "$probe" >/dev/null 2>&1; then
  rm -f "$probe" "$probe.bin"
  RUN_PREFIX=(); base="$ROOT"
  for t in $TARGETS; do
    up=$(echo "$t" | tr a-z A-Z)
    clang++ "${FLAGS[@]}" -DVIBE_FUZZ_${up}=1 "$ROOT/vibeserver/fuzz-dab.cpp" -o "$OUT/fuzz-dab-$t"
  done
else
  rm -f "$probe" "$probe.bin"
  command -v docker >/dev/null || { echo "!! no libFuzzer locally and no docker to borrow one from"; exit 1; }
  echo "==> no local libFuzzer — building in Debian (clang + libclang_rt)"
  IMAGE=vibeserver-fuzz:bookworm
  docker build -q -t "$IMAGE" - >/dev/null <<'DOCKERFILE'
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang libclang-rt-14-dev ca-certificates && rm -rf /var/lib/apt/lists/*
DOCKERFILE
  for t in $TARGETS; do
    up=$(echo "$t" | tr a-z A-Z)
    docker run --rm -v "$ROOT:/work" -w /work "$IMAGE" \
      clang++ -O1 -g -std=c++17 -I/work/android/app/src/main/cpp \
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
        -DVIBE_FUZZ_${up}=1 /work/vibeserver/fuzz-dab.cpp -o "/work/vibeserver/build-fuzz/fuzz-dab-$t"
  done
  RUN_PREFIX=(docker run --rm -v "$ROOT:/work" -w /work "$IMAGE"); base=/work
fi

mkdir -p "$ROOT/vibeserver/fuzz-crashes"
fail=0
for t in $TARGETS; do
  [ -x "$ROOT/vibeserver/build-fuzz/fuzz-dab-$t" ] || { echo "no such target: $t"; exit 1; }
  mkdir -p "$ROOT/vibeserver/fuzz-corpus/$t"
  # ★ Paths are the CONTAINER's when we borrowed one; /work is the repo either way.
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
