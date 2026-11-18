#!/usr/bin/env bash
# test-openrsp-names.sh — build and run vibeserver/test-openrsp-names.cpp: OpenRSP's Enumerator (vendor/openrsp) driven
# through a fake USB bus, proving every RSP gets a name of its own (same-serial clones, no-serial RSP1s). Compiles the
# core + service sources with the portable stub platform: no libusb, no firmware, no sdcc. Silent.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
o="$root/vendor/openrsp"
out="$(mktemp -d "${TMPDIR:-/tmp}/test-openrsp-names.XXXXXX")"
# ★ Only ever the directory mktemp just made (it must exist and match the template) — never a bare variable.
trap 'case "$out" in */test-openrsp-names.??????) [ -d "$out" ] && rm -rf -- "$out" ;; esac' EXIT
srcs=$(find "$o/src/core" -name '*.cpp'; printf '%s\n' "$o/src/service/"{ServiceCore,GlobalChannel,DeviceChannel,Enumerator,Firmware,Heartbeat,ServiceDevice}.cpp;
       find "$o/src/service/stream" -name '*.cpp';
       printf '%s\n' "$o/src/platform/stub/PlatformStub.cpp" "$o/src/platform/common/IpcStallRecoveryNone.cpp" \
                     "$o/src/platform/common/ThreadStd.cpp" "$o/src/platform/common/CpuFeatures.cpp" \
                     "$o/src/platform/common/ClockStd.cpp")
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -g -w -DSDRPLAY_FIRMWARE_DEFAULT_SET='"own"' -I"$o/src" -I"$o/include" \
    "$root/vibeserver/test-openrsp-names.cpp" $srcs -o "$out/t" -lpthread
"$out/t"
