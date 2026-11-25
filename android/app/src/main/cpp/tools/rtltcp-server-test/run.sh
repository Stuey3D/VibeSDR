#!/bin/bash
# Build the host harness (the REAL rtl_tcp_server.cpp + net_shim.cpp over a fake librtlsdr) and run the test.
# Silent and self-contained; needs only clang++ and python3. ~5 s.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CPP="$HERE/../.."
OUT="${TMPDIR:-/tmp}/rtltcp-server-test"
mkdir -p "$OUT"
clang++ -std=c++17 -O1 -pthread -I"$HERE/fake" -I"$CPP" \
  "$HERE/harness.cpp" "$HERE/fake_rtlsdr.cpp" "$CPP/rtl_tcp_server.cpp" "$CPP/net_shim.cpp" \
  -o "$OUT/harness"
python3 "$HERE/test_rtltcp_server.py" "$OUT/harness"
