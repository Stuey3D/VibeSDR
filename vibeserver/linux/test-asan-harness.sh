#!/usr/bin/env bash
# test-asan-harness.sh — run INSIDE the bookworm build image (vibeserver-build:bookworm-<arch>):
#   docker run --rm -v <dir with vibeserver/>:/src:ro vibeserver-build:bookworm-arm64 bash /src/vibeserver/linux/test-asan-harness.sh
# ★★ Proves the memory-check build's recipe (VIBESERVER_ASAN in CMakeLists.txt + linux/asan_options.c) does its job:
#    a heap overflow on a named thread is caught AT THE WRITE, symbolised to file:line with no llvm-symbolizer, the thread
#    named, libasan linked statically (no NEEDED libasan), and the process exits non-zero instead of carrying on.
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
FLAGS=(-O2 -fsanitize=address -fno-omit-frame-pointer -g1 -gz)   # ★ keep in step with VIBESERVER_ASAN
gcc "${FLAGS[@]}" -c "$SRC/asan_options.c" -o "$W/ao.o"
g++ "${FLAGS[@]}" -std=c++17 -pthread "$SRC/test-asan-harness.cpp" "$W/ao.o" \
    -fsanitize=address -static-libasan -static-libstdc++ -static-libgcc -o "$W/harness"
fails=0
check() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }
set +e; "$W/harness" > "$W/out.txt" 2>&1; rc=$?; set -e
sed 's/^/    | /' "$W/out.txt" | head -40
check "exits non-zero (halt_on_error, abort_on_error=0 → exit code 1) [rc=$rc]" '[ "$rc" = 1 ]'
check "reports a heap-buffer-overflow" 'grep -q "ERROR: AddressSanitizer: heap-buffer-overflow" "$W/out.txt"'
# ★ the memset is a tail call, so the writer's frame is the line that called writePastTheEnd — file:line all the same
check "names the WRITE and its thread (vibe-usrp)" 'grep -qE "WRITE of size [0-9]+ .*thread T1 \(vibe-usrp\)" "$W/out.txt"'
check "symbolised to file:line in process (-g1, no llvm-symbolizer)" 'grep -qE "in operator\(\) .*test-asan-harness.cpp:1[56]" "$W/out.txt"'
check "names where the memory came from (calloc)" 'grep -q "calloc" "$W/out.txt"'
check "names the thread (T1 created by …)" 'grep -qE "Thread T1" "$W/out.txt"'
check "prints the SUMMARY line the report leads with" 'grep -q "^SUMMARY: AddressSanitizer: heap-buffer-overflow" "$W/out.txt"'
check "libasan is linked statically (no NEEDED libasan)" '! readelf -d "$W/harness" | grep -q "NEEDED.*libasan"'
check "and no leak report at exit (detect_leaks=0)" '! grep -q "LeakSanitizer" "$W/out.txt"'
[ "$fails" -eq 0 ] && echo "ALL PASS" || { echo "FAILED ($fails)"; exit 1; }
