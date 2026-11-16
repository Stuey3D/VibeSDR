#!/usr/bin/env bash
# build-rade.sh <rade_c dir> <opus dir> <freedv-backend dir> <prefix> — FreeDV's RADE V1 RECEIVER as one static archive,
# for VibeServer. V12, 2026-11-16 — docs/v12/FREEDV.md ("RADE").
#
# Called by vibeserver/linux/build-deps.sh (every Linux profile) and vibeserver/mac/build-deps.sh (both Mac arches), with
# CC / CXX / AR and CFLAGS already set for the target, after each caller has unpacked SHA-256-checked tarballs:
#   rade_c          github.com/freedv/rade_c          BSD-2-Clause   the RADE V1 modem, decoder and its weights
#   opus            downloads.xiph.org opus-1.6.1     BSD-3-Clause   the DNN layer code and the FARGAN vocoder + weights
#   freedv-backend  github.com/freedv/freedv-backend  BSD-2-Clause   the end-of-over callsign (reliable text, LDPC(112,56))
#
# Output:
#   <prefix>/lib/librade_vibe.a       THE RECEIVER — what the server links. One relocatable object whose ONLY global
#                                     symbols are vrade_rx_* and vrade_bench_decode (vibe_rade.h); everything else in it
#                                     is made local, so its copy of Opus's nnet / FARGAN code can never collide with the
#                                     libopus.a the server already links (or with the transmitter archive below).
#   <prefix>/lib/librade_vibe_tx.a    the transmitter, for the tests only (vrade_tx_*) — the encoder's weights and Opus's
#                                     feature extractor stay out of the server binary.
#   <prefix>/include/vibe_rade.h
#   <prefix>/share/licences/LICENSE.rade_c, COPYING.opus, LICENSE.freedv-backend
#
# ★★ SIMD: no runtime dispatch — Opus's own compile-time choice (dnn/vec.h): x86 with SSE2 (every amd64, and i386 at the
#    package's -msse2 floor) takes vec_avx.h's SSE paths, ARM with NEON (armhf -mfpu=neon-vfpv4, every arm64) takes
#    vec_neon.h, and everything else (ARMv6) the scalar code. The CALLER's CFLAGS carry the ISA, as for every other library.
# ★ Weights: Opus's debug float copies are left out (DISABLE_DEBUG_FLOAT, Opus's own default), the int8 sets are used.
# ★ RADE V2 is not built: its own README says the waveform and weights will change and on-air use is not recommended.
set -euo pipefail
RC="${1:?rade_c}"; OP="${2:?opus}"; FB="${3:?freedv-backend}"; PREFIX="${4:?prefix}"
: "${CC:?CC}" "${CXX:?CXX}" "${AR:?AR}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WRAP="$HERE/../../android/app/src/main/cpp/rade"
B="$(mktemp -d)"; trap 'rm -rf "$B"' EXIT
JOBS="${JOBS:-4}"

# ── FreeDV's reliable-text files, with a stand-in for the logging header they include ────────────────────────────────
mkdir -p "$B/fb/pipeline" "$B/fb/util/logging"
for f in rade_text.cpp rade_text.h ldpc_decode.cpp ldpc_decode.h ldpc_encode.cpp ldpc_encode.h HRA_56_56.h; do
  cp "$FB/src/pipeline/$f" "$B/fb/pipeline/"
done
cat > "$B/fb/util/logging/ulog.h" <<'EOF'
/* VibeSDR: rade_text.cpp's logging is diagnostics only — silenced here (the server has its own log). */
#pragma once
#define log_info(...) ((void)0)
#define log_debug(...) ((void)0)
#define log_warn(...) ((void)0)
#define log_error(...) ((void)0)
EOF

# ★ rade_c LAST: it carries its own kiss_fft.h (for a test tool), which must not shadow Opus's celt/kiss_fft.h
# ★★★ OUR ONE CHANGE TO OPUS, AND WHY: RADE V1's decoder has convolution layers with up to 1536 inputs
#     (RADE_MAX_CONV_INPUTS), but Opus 1.6.1 sizes compute_generic_conv1d's stack buffer for its own models only —
#     IMAX(DRED_MAX_CONV_INPUTS, 1024) = 1024 floats. Unchanged, the decoder writes past it (found 2026-11-16: ASan,
#     memcpy-param-overlap in compute_generic_conv1d, then an abort). rade_c's own build makes the same change to the Opus
#     it pins (src/opus-nnet.c.diff: 2048). A COPY of nnet.c is changed and compiled; the line is checked, not assumed.
mkdir -p "$B/opus"
sed 's/^#define MAX_CONV_INPUTS_ALL .*$/#define MAX_CONV_INPUTS_ALL 2048  \/\* VibeSDR: RADE V1 needs 1536 (build-rade.sh) \*\//' \
    "$OP/dnn/nnet.c" > "$B/opus/nnet.c"
[ "$(grep -c '^#define MAX_CONV_INPUTS_ALL 2048' "$B/opus/nnet.c" || true)" -eq 1 ] \
  || { echo "!! Opus nnet.c: MAX_CONV_INPUTS_ALL not found to raise — refusing (RADE would overrun its stack)"; exit 1; }

INC=(-I"$OP/include" -I"$OP/celt" -I"$OP/silk" -I"$OP/dnn" -I"$OP" -I"$B/fb/pipeline" -I"$WRAP" -I"$RC/src")
DEFS=(-DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF -DDISABLE_DEBUG_FLOAT -DNDEBUG)
FLAGS=(-O2 -fPIC -w -ffunction-sections -fdata-sections)
case "$(uname -s)" in Darwin) ;; *) FLAGS+=(-Wa,--noexecstack) ;; esac

COMMON_C=("$RC/src/rade_dsp.c" "$RC/src/rade_ofdm.c" "$RC/src/rade_bpf.c"
          "$B/opus/nnet.c" "$OP/dnn/nnet_default.c" "$OP/dnn/parse_lpcnet_weights.c" "$OP/dnn/freq.c" "$OP/dnn/burg.c" "$OP/dnn/lpcnet_tables.c"
          "$OP/celt/kiss_fft.c" "$OP/celt/mathops.c" "$OP/celt/celt_lpc.c" "$OP/celt/pitch.c" "$OP/celt/entcode.c")
COMMON_CXX=("$B/fb/pipeline/rade_text.cpp" "$B/fb/pipeline/ldpc_decode.cpp" "$B/fb/pipeline/ldpc_encode.cpp")
RX_C=("$RC/src/rade_acq.c" "$WRAP/vibe_rade_acq.c" "$RC/src/rade_rx.c" "$RC/src/rade_dec.c" "$RC/src/rade_dec_data.c"
      "$OP/dnn/fargan.c" "$OP/dnn/fargan_data.c" "$WRAP/vibe_rade.c")
TX_C=("$RC/src/rade_tx.c" "$RC/src/rade_enc.c" "$RC/src/rade_enc_data.c"
      "$OP/dnn/lpcnet_enc.c" "$OP/dnn/pitchdnn.c" "$OP/dnn/pitchdnn_data.c" "$WRAP/vibe_rade_tx.c")

# ★ every object compiled once, in parallel (the two weight files are ~24 MB of C each and dominate)
# ★★ The pilot search (vibe_rade_acq.c): rade_c's rade_acq_detect_pilots is renamed where it is defined and ours takes its
#    name — the same arithmetic in a loop order the compiler vectorises, at -O3 (see that file: the search was 3× the cost
#    of decoding). -fno-math-errno lets the |Dt| loop vectorise too; nothing there reads errno.
compile() {   # compile <src> <obj>
  local extra=()
  case "$1" in
    */rade_acq.c)       extra=(-Drade_acq_detect_pilots=vrade_upstream_acq_detect_pilots -Drade_acq_check_pilots=vrade_upstream_acq_check_pilots) ;;
    */vibe_rade_acq.c)  extra=(-O3 -fno-math-errno) ;;
  esac
  case "$1" in
    *.cpp) $CXX ${CXXFLAGS:-${CFLAGS:-}} -std=c++17 "${DEFS[@]}" "${INC[@]}" "${FLAGS[@]}" -c "$1" -o "$2" ;;
    *)     $CC ${CFLAGS:-} -std=gnu11 "${DEFS[@]}" "${INC[@]}" "${FLAGS[@]}" ${extra[@]+"${extra[@]}"} -c "$1" -o "$2" ;;
  esac
}
export -f compile; export CC CXX CFLAGS CXXFLAGS 2>/dev/null || true
objname() { local b; b="$(basename "$1")"; echo "$B/o/${b%.*}.o"; }
mkdir -p "$B/o"
ALL=("${COMMON_C[@]}" "${COMMON_CXX[@]}" "${RX_C[@]}" "${TX_C[@]}")
pids=(); fail=0
for s in "${ALL[@]}"; do
  [ -f "$s" ] || { echo "!! missing source $s"; exit 1; }
  compile "$s" "$(objname "$s")" &
  pids+=($!)
  if [ "${#pids[@]}" -ge "$JOBS" ]; then wait "${pids[0]}" || fail=1; pids=("${pids[@]:1}"); fi
done
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" -eq 0 ] || { echo "!! RADE compile failed"; exit 1; }

objs() { local o=(); for s in "$@"; do o+=("$(objname "$s")"); done; echo "${o[@]}"; }

# ── one relocatable object per archive, only vrade_* left global ───────────────────────────────────────────────────
partial() {   # partial <out.o> <keep-prefix regex> <objects...>
  local out="$1" keep="$2"; shift 2
  if [ "$(uname -s)" = Darwin ]; then
    "$NM" -g --defined-only "$@" 2>/dev/null | awk '{print $3}' | grep -E "^_($keep)" | sort -u > "$B/keep.txt"
    $CC -r -nostdlib -Wl,-exported_symbols_list,"$B/keep.txt" -o "$out" "$@"
  else
    "$NM" -g --defined-only "$@" 2>/dev/null | awk 'NF>=3 {print $3}' | grep -E "^($keep)" | sort -u > "$B/keep.txt"
    # ★★ --force-group-allocation: the C++ files' inline functions (std::vector<float>'s destructor, the LDPC graph's
    #    constructor) are COMDAT groups, and so are the server's own copies of the same ones. Left as groups, the final
    #    link keeps ONE copy per name and throws the other away — and if it threw ours away, the code our now-local
    #    symbols name is gone ("defined in discarded section", seen 2026-11-16 linking the two archives together).
    #    Dissolved here, they are ordinary local sections of this one object and can never be discarded for another's.
    #    GNU ld itself (the toolchain's, beside its nm), not the compiler driver: ARMv6's clang links with lld 14, which
    #    has no --force-group-allocation; binutils' ld has had it since 2.29.
    "${LD_R:-${NM%nm}ld}" -r --force-group-allocation -o "$out" "$@"
    "${OBJCOPY:-objcopy}" --keep-global-symbols="$B/keep.txt" "$out"
  fi
}
: "${NM:=nm}"
mkdir -p "$PREFIX/lib" "$PREFIX/include" "$PREFIX/share/licences"
# ★ The C++ (reliable text, LDPC) is in the RECEIVER only, once: the transmitter asks the receiver's archive for its
#   end-of-over bits (vrade_text_eoo_bits). (COMDAT groups: see partial().)
partial "$B/rade_rx.o" 'vrade_rx_|vrade_bench_|vrade_text_' $(objs "${COMMON_C[@]}" "${COMMON_CXX[@]}" "${RX_C[@]}")
partial "$B/rade_tx.o" 'vrade_tx_' $(objs "${COMMON_C[@]}" "${TX_C[@]}")
rm -f "$PREFIX/lib/librade_vibe.a" "$PREFIX/lib/librade_vibe_tx.a"
"$AR" rcs "$PREFIX/lib/librade_vibe.a" "$B/rade_rx.o"
"$AR" rcs "$PREFIX/lib/librade_vibe_tx.a" "$B/rade_tx.o"
cp "$WRAP/vibe_rade.h" "$PREFIX/include/"
cp "$RC/LICENSE" "$PREFIX/share/licences/LICENSE.rade_c"
cp "$OP/COPYING" "$PREFIX/share/licences/COPYING.opus"
cp "$FB/LICENSE" "$PREFIX/share/licences/LICENSE.freedv-backend"

# ★★ THE FEATURE, AND THE ISOLATION: the receiver's entry points are there, and NOTHING but vrade_* is global — an
#    unprefixed compute_generic_dense here would collide with (or silently replace) libopus.a's at the server's link.
for a in librade_vibe librade_vibe_tx; do
  g="$("$NM" -g --defined-only "$PREFIX/lib/$a.a" 2>/dev/null | awk 'NF>=3 {print $3}' | grep -v -E '^_?vrade_' | grep -v -E '^__x86\.get_pc_thunk\.' || true)"
  [ -z "$g" ] || { echo "!! $a.a defines globals other than vrade_*:"; echo "$g" | head; exit 1; }
done
# ★ …and nothing of Opus or RADE is left UNdefined: it would bind to libopus.a's copy at the server's link (or not link)
for a in librade_vibe librade_vibe_tx; do
  u="$("$NM" -u "$PREFIX/lib/$a.a" 2>/dev/null | awk 'NF && $NF !~ /:$/ {print $NF}' | grep -E '^_?(silk_|celt_|opus_|compute_|fargan|lpcnet|pitchdnn|rade_|linear_init|conv2d_init|parse_weights|kiss_|burg|ldpc)' || true)"
  [ -z "$u" ] || { echo "!! $a.a leaves Opus/RADE symbols undefined:"; echo "$u" | sort -u | head; exit 1; }
done
[ "$("$NM" -g --defined-only "$PREFIX/lib/librade_vibe.a" | grep -c -E ' T _?vrade_rx_process$' || true)" -gt 0 ] \
  || { echo "!! librade_vibe.a has no vrade_rx_process — refusing"; exit 1; }
ls -l "$PREFIX/lib/librade_vibe.a" "$PREFIX/lib/librade_vibe_tx.a"
echo "==> RADE V1 receiver built (librade_vibe.a)"
