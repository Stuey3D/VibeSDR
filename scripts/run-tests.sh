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

# test name -> the sources it needs besides itself
deps_for() {
  case "$1" in
    test-config-radios) echo "$SRC/vibeserver_config.cpp" ;;
    test-rtl-eeprom)    echo "$SRC/rtl_eeprom.cpp" ;;
    test-fd-passing)    echo "android/app/src/main/cpp/fd_passing.cpp" ;;
    test-geoip)         echo "$SRC/geoip.cpp" ;;
    test-asndb)         echo "$SRC/asndb.cpp" ;;
    test-admin-banlist) echo "" ;;
    *)                  echo "" ;;
  esac
}

pass=0; fail=0; broke=0
for t in "$SRC"/test-*.cpp; do
  name="$(basename "$t" .cpp)"
  printf '\n\033[1m── %s ──\033[0m\n' "$name"
  # shellcheck disable=SC2046
  if ! g++ -std=c++17 -I "$SRC" -I android/app/src/main/cpp \
        -o "$OUT/$name" "$t" $(deps_for "$name") 2>"$OUT/$name.buildlog"; then
    printf '   \033[33mdid not build\033[0m — %s\n' "$OUT/$name.buildlog"
    head -5 "$OUT/$name.buildlog" | sed 's/^/     /'
    broke=$((broke+1)); continue
  fi
  if "$OUT/$name"; then pass=$((pass+1)); else fail=$((fail+1)); fi
done

printf '\n\033[1m%d suite(s) passed, %d failed, %d did not build\033[0m\n' "$pass" "$fail" "$broke"
[ "$fail" -eq 0 ] && [ "$broke" -eq 0 ]
