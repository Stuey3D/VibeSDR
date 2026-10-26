#!/usr/bin/env bash
# image/sbc/build-all.sh — every Portable card for a release, ONE AT A TIME, on a Linux Docker host (the Lenovo).
#
#   sudo image/sbc/build-all.sh <version e.g. 11.0.11-1> <out-dir> [card …]
#   cards: pi-arm64 pi-armhf <board…from boards.txt>      (default: both Pi cards, then every "planned"/"built" board)
#
# ★★ Runs ON the build host (start it with setsid nohup): the host's Wi-Fi drops, and a build must not die with the ssh
#    session that started it. Each card: fetch (verified by apt / Armbian's signature), build, boot-test under
#    emulation, then the work files and the base image are DELETED before the next card starts — disk is the limit.
# ★ One card failing is logged and the next one starts; the summary at the end says which.
# ★ CPU: every step is the existing builders' own (2 CPUs, nice 19, xz -T2), strictly one after another.
# ★ Nothing is uploaded or published. Cards and their checksums land in <out-dir>.
set -uo pipefail
VER="${1:?usage: build-all.sh <version> <out-dir> [card …]}"
OUT="${2:?out dir}"; shift 2
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
BASE="$OUT/base"; mkdir -p "$BASE"
CARDS=("$@")
if [ ${#CARDS[@]} = 0 ]; then
  CARDS=(pi-arm64 pi-armhf $(awk '!/^#/ && NF && ($8 == "planned" || $8 == "built") { print $1 }' "$HERE/boards.txt"))
fi
# ★ Every card is named with the VibeServer version it carries (11.0.13-1 → "11.0.13").
export CARD_VERSION="${VER%%-*}"
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"
say() { printf '\n==== %s  %s\n' "$(date -u +%H:%M:%S)" "$*"; }
note() { printf '%-16s %s\n' "$1" "$2" | tee -a "$SUMMARY"; }
space() { df -h "$OUT" | awk 'NR==2 {print $4 " free"}'; }

boot_test() {   # <card name> <img.xz>
  local name="$1" img="$2"
  # ★ Everything but the one-word verdict goes to stderr (the log): the verdict is captured into the summary line.
  say "$name: boot test (emulated)" >&2
  rm -rf "$OUT/boot-test-$name"
  if "$HERE/run-in-docker.sh" boot-test "$img" "$OUT/boot-test-$name" >&2; then
    cp "$OUT/boot-test-$name/boot-test/boot-test-result.txt" "$OUT/$(basename "$img" .img.xz).boot-test.txt" 2>/dev/null || true
    echo "passed"
  else
    cp "$OUT/boot-test-$name/boot-test/boot-test-result.txt" "$OUT/$(basename "$img" .img.xz).boot-test.txt" 2>/dev/null || true
    echo "FAILED"
  fi
  rm -rf "$OUT/boot-test-$name"
}

for c in "${CARDS[@]}"; do
  say "$c: start ($(space))"
  case "$c" in
  pi-arm64|pi-armhf)
    A="${c#pi-}"
    P="$REPO/image/pocket"
    rm -rf "$P/out/debs" "$P/out/work"; mkdir -p "$P/out/work"
    if ! "$P/fetch-release-deb.sh" "$A" "$VER"; then note "$c" "FAILED: could not fetch vibeserver $VER $A"; continue; fi
    if ! nice -n 19 "$P/build-image.sh" "$A"; then note "$c" "FAILED: build-image.sh"; rm -rf "$P/out/work"; continue; fi
    IMG="$(ls -t "$P"/out/vibeserver-pocket-"$A"-*.img.xz | head -1)"
    mv -f "$IMG" "$IMG.sha256" "$OUT/"
    IMG="$OUT/$(basename "$IMG")"
    rm -rf "$P/out/work" "$P/out/debs"
    # ★ The Raspberry Pi OS base image is kept in out/cache only until the other Pi card has used it… then deleted.
    [ "$A" = armhf ] && rm -f "$P"/out/cache/*raspios*
    [ "$A" = arm64 ] && rm -f "$P"/out/cache/*arm64-lite*
    note "$c" "$(basename "$IMG") $(du -h "$IMG" | cut -f1) sha256 $(cut -d' ' -f1 "$IMG.sha256") boot-test $(boot_test "$c" "$IMG" | tail -1)"
    ;;
  *)
    if ! F="$("$HERE/fetch-armbian.sh" "$c" "$BASE" | tail -1)"; then note "$c" "SKIPPED: the Armbian image could not be fetched"; continue; fi
    F="${F%% (*}"
    if ! "$HERE/run-in-docker.sh" "$c" "$F" "$OUT"; then
      note "$c" "FAILED or SKIPPED in build.sh (see build-all.log: checksum / signature / family / checks)"
      rm -f "$F" "$F.sha" "$F.asc"; rm -rf "$OUT/work-$c"; continue
    fi
    rm -f "$F" "$F.sha" "$F.asc"; rm -rf "$OUT/work-$c"
    IMG="$(ls -t "$OUT"/vibeserver-portable-"$c"-*.img.xz | head -1)"
    N="$(grep -c "^  vibeserver $VER " "${IMG%.img.xz}.manifest.txt" || true)"
    [ "$N" = 1 ] || { note "$c" "FAILED: the card does not carry vibeserver $VER (see its manifest)"; continue; }
    note "$c" "$(basename "$IMG") $(du -h "$IMG" | cut -f1) sha256 $(cut -d' ' -f1 "$IMG.sha256") boot-test $(boot_test "$c" "$IMG" | tail -1)"
    ;;
  esac
done
rm -rf "$BASE" "$OUT/cache" "$REPO/image/pocket/out/cache"
say "all done ($(space))"
cat "$SUMMARY"
