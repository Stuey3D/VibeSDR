#!/usr/bin/env bash
# test-bundled-ffmpeg.sh — the bundled Linux FFmpeg (tools/ffmpeg-linux/bin) decodes xHE-AAC the way VibeServer feeds it.
#
#   scripts/test-bundled-ffmpeg.sh [arch ...]     # default: all five (amd64 i386 arm64 armhf armv6)
#
# ★★ WHAT IT PROVES (V12, 2026-10-29). ISO's USAC conformance streams, as published in FFmpeg's test suite with their
#    reference decodes (fate-suite.ffmpeg.org — fetched once into ~/VibeSDR-test-audio/usac), are rewritten by
#    vibedrm::MkvStream — the DRM path's own writer (vibeserver/drm-usac-stream.cpp) — and piped through each bundled
#    binary with EXACTLY the arguments vibe_drm_audiodec.h's DrmFfmpegChild uses (Matroska in on a pipe, 48 kHz stereo f32
#    out). A second decode at the stream's own rate is compared with the conformance reference (aligned for the decoder's
#    start-up delay, which a live stream does not trim).
# ★ The binaries run under qemu-user in the build image (vibeserver-ffmpeg:bookworm) — running, never compiling; i386 as an
#   Atom N270 (SSE2/SSSE3 — a real host, Kiko's netbook). arm64 is native. Silent: nothing is played, everything is files.
# ★ CPU: one container, 2 CPUs, nice 19.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/tools/ffmpeg-linux/bin"
SAMPLES="$HOME/VibeSDR-test-audio/usac"
ARCHES="${*:-amd64 i386 arm64 armhf armv6}"
NAMES="Fd_1_c1_0x03 Fd_2_c1_0x03 Fd_2_c1_0x05 Fd_2_c1_Tns_0x04 Fd_2_c1_Ms_0x01 Fd_2_c1_Ms_0x04"

mkdir -p "$SAMPLES"
for n in $NAMES; do
  case "$n" in Fd_2_c1_Ms_*) d=aac ;; *) d=aac/usac ;; esac
  for ext in mp4 s16; do
    [ -s "$SAMPLES/$n.$ext" ] || curl -fsSL "https://fate-suite.ffmpeg.org/$d/$n.$ext" -o "$SAMPLES/$n.$ext"
  done
done
W="$SAMPLES/work"; rm -rf "$W"; mkdir -p "$W"
TOOL="$W/drm-usac-stream"
clang++ -std=c++17 -O1 -I"$ROOT/android/app/src/main/cpp" -o "$TOOL" "$ROOT/vibeserver/drm-usac-stream.cpp"
for n in $NAMES; do "$TOOL" "$SAMPLES/$n.mp4" "$W/$n.mkv" > "$W/$n.info"; done
cat > "$W/run.sh" <<'EOF'
set -u
cd /s/work
for A in $ARCHES; do
  case "$A" in
    amd64) Q="qemu-x86_64 -L /usr/x86_64-linux-gnu" ;;
    i386)  Q="qemu-i386 -cpu n270 -L /usr/i686-linux-gnu" ;;
    arm64) Q="" ;;
    armhf) Q="qemu-arm -cpu cortex-a7 -L /usr/arm-linux-gnueabihf" ;;
    armv6) Q="qemu-arm -cpu arm1176 -L /opt/rpi-sysroot" ;;
  esac
  FF="$Q /bin-ff/ffmpeg-linux-$A"
  for n in $NAMES; do
    $FF -hide_banner -loglevel error -nostdin -f matroska -i $n.mkv -f s16le - > $n.$A.native.s16 2> $n.$A.native.err
    # ★ EXACTLY DrmFfmpegChild's argv (vibe_drm_audiodec.h), the stream fed through a pipe
    cat $n.mkv | $FF -hide_banner -loglevel quiet -nostdin -fflags nobuffer -flags low_delay \
      -probesize 32 -analyzeduration 0 -f matroska -i pipe:0 -f f32le -ar 48000 -ac 2 pipe:1 > $n.$A.drm48.f32
    echo "$n $A rc $?" >> rc.txt
  done
done
exit 0
EOF
docker run --rm --cpus 2 --platform linux/arm64 -e ARCHES="$ARCHES" -e NAMES="$NAMES" \
  -v "$SAMPLES":/s -v "$BIN":/bin-ff:ro vibeserver-ffmpeg:bookworm nice -n 19 bash /s/work/run.sh
python3 -I - "$W" "$SAMPLES" "$ARCHES" "$NAMES" <<'EOF'
import sys, struct, math, os
W, S, arches, names = sys.argv[1], sys.argv[2], sys.argv[3].split(), sys.argv[4].split()
def s16(p):
    b = open(p, 'rb').read(); return struct.unpack('<%dh' % (len(b) // 2), b[:len(b) // 2 * 2])
rcs = {tuple(l.split()[:2]): int(l.split()[3]) for l in open(f"{W}/rc.txt")}
fails = 0
for n in names:
    ref = s16(f"{S}/{n}.s16")
    info = dict(zip(*[iter(open(f"{W}/{n}.info").read().split())] * 2))
    ch = 1 if n.startswith("Fd_1_") else 2
    for a in arches:
        got = s16(f"{W}/{n}.{a}.native.s16")
        if not got:
            print(f"FAIL {n:18s} {a:6s} decoded nothing: {open(f'{W}/{n}.{a}.native.err').read().strip()[:200]}"); fails += 1; continue
        best = None                                   # ★ align: a live stream keeps the decoder's start-up delay
        for lag in range(0, 4096 * ch, ch):
            m = min(len(ref), len(got) - lag, 24000 * ch)
            if m <= 0: break
            e = sum((ref[i] - got[i + lag]) ** 2 for i in range(0, m, 11))
            if best is None or e < best[1]: best = (lag, e)
        lag = best[0]; m = min(len(ref), len(got) - lag)
        err = sum((ref[i] - got[i + lag]) ** 2 for i in range(m)); sig = sum(r * r for r in ref[:m]) or 1
        snr = 99.0 if err == 0 else 10 * math.log10(sig / err)
        mx = max(abs(ref[i] - got[i + lag]) for i in range(m))
        f48 = os.path.getsize(f"{W}/{n}.{a}.drm48.f32") // 8       # stereo float frames at 48 kHz
        dur = len(ref) / ch / int(info["rate"])
        ok = snr >= 60 and m >= len(ref) * 0.99 and rcs.get((n, a), 1) == 0 and abs(f48 / 48000 - dur) < 0.25
        fails += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} {n:18s} {a:6s} vs reference: SNR {snr:5.1f} dB, max |diff| {mx:4d} LSB, "
              f"delay {lag // ch:4d}   DrmFfmpegChild argv: {f48 / 48000:5.2f} s of 48 kHz stereo (stream {dur:5.2f} s)")
print(f"\ntest-bundled-ffmpeg: {fails} failed")
sys.exit(1 if fails else 0)
EOF
