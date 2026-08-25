#!/usr/bin/env bash
# VibeSDR V5 — build the GPL-FREE iOS static lib (libvibelocalsdr_ios.a).
#
# Compiles the clean-room engine (vibedsp + net_shim + the native-only shim +
# decoders + ft8_lib) for iOS arm64 into a single static lib. NO SDR++ / FFTW /
# VOLK / zstd — that is the whole point of V5 (App-Store-clean, RTL-TCP retained).
#
# iOS has no USB host SDR, so the shim's librtlsdr path uses the no-op
# rtl_sdr_stub.h (see ../../android/app/src/main/cpp). Compiling a plain C++ static
# lib with the Xcode 27 beta clang is fine — the beta's RN/Hermes runtime issue is
# about launching the app, not building our own code.
#
# Usage:  ./build_ios.sh           (device arm64; output -> libs/libvibelocalsdr_ios.a)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CPP="$HERE/../../android/app/src/main/cpp"          # shared native sources
OUT="$HERE/libs/libvibelocalsdr_ios.a"
WORK="$(mktemp -d)"
MIN_IOS=16.4

# Toolchain: prefer an Xcode with the iphoneos SDK (CLT alone has only macosx).
if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
  for X in /Applications/Xcode.app /Applications/Xcode-beta.app; do
    [ -d "$X" ] && export DEVELOPER_DIR="$X/Contents/Developer" && break
  done
fi
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CLANG="$(xcrun --sdk iphoneos --find clang)"
CLANGXX="$(xcrun --sdk iphoneos --find clang++)"
LIBTOOL="$(xcrun --sdk iphoneos --find libtool)"
echo "iphoneos SDK: $SDK"

ARCH="-arch arm64 -isysroot $SDK -mios-version-min=$MIN_IOS"
INC="-I$CPP -I$CPP/vibedsp -I$CPP/ft8_lib -I$CPP/spyserver"
CXXFLAGS="-std=c++17 -O3 -ffp-contract=fast -fvisibility=hidden"
CFLAGS="-O3 -ffp-contract=fast"
# vibedsp's vendored KissFFT is prefixed vibe_* to avoid clashing with ft8_lib's.
KISSPFX="-Dkiss_fft_alloc=vibe_kiss_fft_alloc -Dkiss_fft=vibe_kiss_fft \
  -Dkiss_fft_stride=vibe_kiss_fft_stride -Dkiss_fft_cleanup=vibe_kiss_fft_cleanup \
  -Dkiss_fft_next_fast_size=vibe_kiss_fft_next_fast_size \
  -Dkiss_fftr_alloc=vibe_kiss_fftr_alloc -Dkiss_fftr=vibe_kiss_fftr -Dkiss_fftri=vibe_kiss_fftri"

cd "$WORK"
n=0; objs=()
cxx() { echo "  CXX $(basename "$1")"; "$CLANGXX" $ARCH $CXXFLAGS $INC ${2:-} -c "$1" -o "o$n.o"; objs+=("o$n.o"); n=$((n+1)); }
cc()  { echo "  CC  $(basename "$1")"; "$CLANG"   $ARCH $CFLAGS   $INC ${2:-} -c "$1" -o "o$n.o"; objs+=("o$n.o"); n=$((n+1)); }

echo "== vibedsp (vibe_* KissFFT) =="
# ★ ONE SOURCE LIST — see vibedsp/SOURCES. Do not list .cpp files here.
#   Redirect the loop from a FD other than stdin: cxx runs the compiler, and a compiler
#   inheriting the loop's stdin would swallow the rest of the list.
while IFS= read -r f <&9; do
  case "$f" in *.cpp) ;; *) continue;; esac   # same rule as the CMake reads
  cxx "$CPP/vibedsp/$f" "$KISSPFX"
done 9< "$CPP/vibedsp/SOURCES"
cc "$CPP/vibedsp/third_party/kissfft/kiss_fft.c"  "$KISSPFX"
cc "$CPP/vibedsp/third_party/kissfft/kiss_fftr.c" "$KISSPFX"

echo "== shim + net + decoders =="
cxx "$CPP/local_sdr_shim.cpp"
# ★★ THE RADIO SOURCES, EVEN THOUGH iOS HAS NEITHER RADIO. Both files are written as
# "real implementation #ifdef <SDK> #else no-op stubs", and the shim REFERENCES their methods
# unconditionally — a runtime branch like useSdrplay() still emits a link-time reference. So
# omitting them does not save anything; it just fails at the link step with a wall of
# undefined SdrplaySource:: symbols (2026-07-27, first iOS build since the RSP controls
# landed). Compiling the stub side costs a few bytes and keeps the shim's one source of truth.
cxx "$CPP/sdrplay_source.cpp"
cxx "$CPP/airspyhf_source.cpp"
cxx "$CPP/net_shim.cpp"
# ★★★ fd_passing joined 2026-08-25, and its absence was a LINK error, not a compile one — the
#     shim calls vibe::sendFdTo() so the reference is emitted whether or not iOS ever passes an
#     fd. It only surfaced once the archive was rebuilt at all: the stale .a predated the CALL
#     as well as the definition, so it linked cleanly and hid the gap. Same lesson as the
#     decoders below — three build lists, one source tree.
cxx "$CPP/fd_passing.cpp"
cxx "$CPP/mdns_responder.cpp"   # hostname responder; iOS compiles it but never serves
cxx "$CPP/spyserver/spyserver_messages.cpp"
cxx "$CPP/spyserver/spyserver_client.cpp"
# ★ time_decoder joined 2026-08-11 (MSF/DCF77). iOS links a PREBUILT lib, so a decoder left
#   off this line compiles on Android and the server and is simply ABSENT on iOS — see
#   [[ios_prebuilt_dsp_lib_trap]]. Three build lists, one source tree.
for d in fsk_decoder wefax_decoder ft8_decoder sstv_decoder audio_nr auto_notch time_decoder; do cxx "$CPP/decoders/$d.cpp"; done

echo "== ft8_lib (plain KissFFT) =="
for f in "$CPP"/ft8_lib/ft8/*.c "$CPP/ft8_lib/fft/kiss_fft.c" "$CPP/ft8_lib/fft/kiss_fftr.c" "$CPP/ft8_lib/common/monitor.c"; do cc "$f"; done

echo "== archive -> $OUT =="
mkdir -p "$HERE/libs"
"$LIBTOOL" -static -o "$OUT" "${objs[@]}"
echo "done: $(ls -la "$OUT" | awk '{print $5}') bytes"
rm -rf "$WORK"
