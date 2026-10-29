#!/usr/bin/env bash
# build-libxaac.sh <libxaac source dir> <prefix> <variant> — libxaac's DECODER as one static archive, for VibeServer.
# Called by vibeserver/linux/build-deps.sh (every Linux profile) and vibeserver/mac/build-deps.sh (both Mac arches),
# with CC / AR / CFLAGS already set for the target. V12, 2026-10-29 — docs/v12/LIBXAAC.md.
#
#   variant  armv8    AArch64: libxaac's NEON assembly (decoder/armv8) + its armv8 function selector
#            armv7    armhf (ARMv7 + NEON, ARM mode): decoder/armv7's assembly + its selector. The .s files are written in
#                     UAL ("subsne"), which GNU as reads only in unified mode — they go through CLANG's assembler ($ASCC)
#            c        everything else — amd64, i386 (at the package's SSE2 floor, from the caller's CFLAGS), ARMv6 and the
#                     Mac: libxaac's C, through its x86_64 function selector, which is plain portable C (libxaac has no x86
#                     SIMD). ★★ NOT decoder/generic's selector: its function-pointer prototypes have drifted from the
#                     functions (a WORD16 output declared where the function writes WORD32 — clang refuses it, older
#                     compilers only warn), and it assumes decoder/armv7's assembly wherever __ARM_NEON__ is set, which
#                     the Mac's compiler sets. __ARM_NEON__ is UNdefined here for the same reason; the AArch64 assembly is
#                     ELF/GNU as, not Mach-O, so the Mac takes this path too.
#
# Output: <prefix>/lib/libxaacdec.a, <prefix>/share/licences/{LICENSE,NOTICE}.libxaac. NO HEADERS: vibe_xaac.h declares the
# one entry point (ixheaacd_dec_api) and restates the API's command numbers itself.
# ★★ The source list is READ from libxaac's own cmake lists (common/common.cmake, decoder/libxaacdec.cmake, the DRC list,
#    and the per-arch list), never written out here, so a new upstream file is not silently left out. The encoder, the test
#    programs and the fuzzers are not built.
# ★★ -Wa,--noexecstack on every object: decoder/armv7's assembly has no .note.GNU-stack section, and ONE such object makes
#    the linker give the whole vibeserver an executable stack. Checked below.
# ★ -fwrapv as libxaac's own x86 build uses: the fixed-point code is written for two's-complement wrap.
set -euo pipefail
S="${1:?source}"; PREFIX="${2:?prefix}"; VARIANT="${3:?variant}"
: "${CC:?CC}" "${AR:?AR}"
ASCC="${ASCC:-$CC}"
B="$(mktemp -d)"; trap 'rm -rf "$B"' EXIT
srcs() { grep -ho '"${XAAC_ROOT}/[^"]*"' "$@" | tr -d '"' | sed "s#\${XAAC_ROOT}#$S#" | grep -E '\.(c|s)$' | sort -u; }
LIST="$(srcs "$S/common/common.cmake" "$S/decoder/libxaacdec.cmake" "$S/decoder/drc_src/libxaacdec_drc.cmake" \
        | grep -v '/decoder/\(x86\|x86_64\|armv7\|armv8\|generic\)/')"
INC=(-I"$S/common" -I"$S/decoder" -I"$S/decoder/drc_src")
DEFS=(-DLOUDNESS_LEVELING_SUPPORT)
case "$VARIANT" in
  armv8)   LIST="$LIST $(srcs "$S/decoder/armv8/libxaacdec_armv8.cmake")"; INC+=(-I"$S/decoder/armv8"); DEFS+=(-DARMV8) ;;
  armv7)   LIST="$LIST $(srcs "$S/decoder/armv7/libxaacdec_armv7.cmake")"; INC+=(-I"$S/decoder/armv7"); DEFS+=(-DARMV7) ;;
  c)       LIST="$LIST $S/decoder/x86_64/ixheaacd_function_selector_x86_64.c $S/decoder/generic/ixheaacd_qmf_dec_generic.c"
           DEFS+=(-U__ARM_NEON__ -U__ARM_NEON) ;;
  *) echo "!! variant is armv8, armv7 or c"; exit 1 ;;
esac
# ★ -w hides libxaac's own warnings, but a mismatched function pointer is an ERROR (see variant c)
FLAGS=(-O3 -fwrapv -fno-strict-aliasing -fPIC -w -Werror=incompatible-pointer-types)
case "$(uname -s)" in Darwin) ;; *) FLAGS+=(-Wa,--noexecstack) ;; esac
OBJS=(); n=0
for f in $LIST; do
  o="$B/$(basename "${f%.*}")-$((n++)).o"
  if [[ "$f" == *.s ]]; then $ASCC ${CFLAGS:-} "${INC[@]}" "${FLAGS[@]}" -c "$f" -o "$o"
  else $CC ${CFLAGS:-} "${DEFS[@]}" "${INC[@]}" "${FLAGS[@]}" -c "$f" -o "$o"; fi
  OBJS+=("$o")
done
mkdir -p "$PREFIX/lib" "$PREFIX/share/licences"
rm -f "$PREFIX/lib/libxaacdec.a"
"$AR" rcs "$PREFIX/lib/libxaacdec.a" "${OBJS[@]}"
cp "$S/LICENSE" "$PREFIX/share/licences/LICENSE.libxaac"
cp "$S/NOTICE" "$PREFIX/share/licences/NOTICE.libxaac"
# ★ the FEATURE, not the file: the entry point is in it
NMB="${NM:-nm}"
[ "$("$NMB" "$PREFIX/lib/libxaacdec.a" 2>/dev/null | grep -c 'T _\{0,1\}ixheaacd_dec_api$' || true)" -gt 0 ] \
  || { echo "!! libxaacdec.a has no ixheaacd_dec_api — refusing"; exit 1; }
if [ "$(uname -s)" != Darwin ]; then   # ★ no object may ask for an executable stack (see the header)
  RE="${READELF:-readelf}"
  for o in "${OBJS[@]}"; do
    [ "$("$RE" -S "$o" 2>/dev/null | grep -c 'note.GNU-stack' || true)" -gt 0 ] || { echo "!! $(basename "$o") has no .note.GNU-stack"; exit 1; }
  done
fi
echo "==> libxaac ($VARIANT): ${#OBJS[@]} objects, $(wc -c < "$PREFIX/lib/libxaacdec.a") bytes"
