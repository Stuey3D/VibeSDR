#!/usr/bin/env bash
# build-debs-remote.sh — build the VibeServer packages for ALL FOUR architectures on the Lenovo.
#
#   scripts/build-debs-remote.sh --rev 2                          # amd64 i386 armhf arm64, version <cmake>-2
#   scripts/build-debs-remote.sh --rev 0~test1 --arch i386        # a test package, never published
#   VIBE_EXTRA_CFLAGS=-mtune=bonnell scripts/build-debs-remote.sh --rev 1+atom1 --arch amd64
#   VIBE_X86_32_FLAGS='-march=i686 -msse2 -mfpmath=sse -mtune=bonnell' scripts/build-debs-remote.sh --rev 1+t --arch i386
#   VIBE_PGO=generate VIBE_PGO_DIR=pgo-i386 scripts/build-debs-remote.sh --rev 1+pgo --arch i386   # instrumented binaries
#   VIBE_PGO=use      VIBE_PGO_DIR=pgo-i386 scripts/build-debs-remote.sh --rev 1+atom2 --arch i386 # package built from them
#
# ★★★ A RELEASE DOES NOT CALL THIS BY HAND — scripts/publish-apt-remote.sh does, with the revision the
#     repository expects, then indexes on the Lenovo and signs on the Mac. Run this directly for test and
#     hand-out builds (a `0~…` or `…+tag` revision).
#
# ★★★ WHY THE LENOVO BUILDS EVERYTHING (Stuart, 2026-10-18: "the docker build can we do that remotely on the
#     lenovo going forward that has loads of space"). The Mac's Docker VM (~/.colima, 13 GB) was deleted to
#     free disk. The Lenovo (192.168.86.13, Ubuntu 24.04, x86-64, Docker, ~50 GB free) now builds:
#       amd64, i386   natively — a linux/amd64 or linux/386 container on an x86-64 CPU (Dockerfile.build)
#       armhf         cross, in an amd64 container (Dockerfile.cross-armhf; + the ARMv6 binary)
#       arm64         cross, in an amd64 container (Dockerfile.cross-arm64) — used to be native on the Mac
#     ✗ NEVER an emulated container (qemu): amd64 under qemu on the Mac segfaulted the compiler on a
#       different file every run (3 failures of 3). An arm64 container on the Lenovo would be the same.
#     The recipe inside each container is ONE script for all four: vibeserver/linux/build-deb.sh.
# ★★★ ALWAYS IN A BOOKWORM CONTAINER, NEVER ON THE HOST. The host is Ubuntu 24.04 (glibc 2.39), and CPack
#     stamps Depends: from what the build links against — a host build demands libc6 >= 2.38 and is
#     uninstallable on Debian 12.
# ★★★ IT IS A LIVE RECEIVER (VibeServer + an RSP1A; Stuart listens). So: nice 19 (the image's dependency
#     build too — DEPS_NICE), at most 4 jobs (--cpus 4, -j4), and no container is left behind — every
#     `docker run` is --rm and named, so a run cut off by a dropped ssh is removed by the next one.
#     Nothing else on that machine is touched: not the vibeserver services, not the stopped owrx container.
# ★★ ITS WI-FI DROPS. Every ssh/rsync has a connect timeout and keep-alives, so a dead link FAILS within
#    about a minute and says so, instead of hanging a release.
# ★★ THE REVISION MUST BE THE ONE publish-apt.sh WILL EXPECT (one more than the pool's high-water mark for
#    that arch) or the version check refuses it. Test builds use a `0~…` revision, which sorts BELOW -1, so a
#    test package can never shadow the real one on a machine that installed it.
# ★ PROFILE-GUIDED (VIBE_PGO, docs/ATOM-PERFORMANCE-2026-10-18.md): `generate` leaves vibeserver +
#   vibeserver-bench in ~/vibeout/<arch>/pgogen/ instead of a package; run them on representative work (the
#   counters land in ~/<VIBE_PGO_DIR> on the build host, mounted at /pgo); then `use` builds the package with
#   -fprofile-use=/pgo. Both passes build at /build/VibeSDR/vibeserver/build, because GCC names each counter
#   file after its object's absolute path.
# ★ VIBE_REMOTE_SRC_DIR: the directory on the build host the tree is synced to (default vibesdr-src). Give a
#   test build its own, so it cannot pull the tree out from under somebody else's build there.
# ★ The .debs come back into vibeserver/build/prebuilt/ (gitignored) AND stay in ~/vibeout/<arch>/ on the
#   Lenovo, where publish-apt-remote.sh indexes them. Build trees never leave the container.
set -euo pipefail

HOST="${VIBE_REMOTE_HOST:-${VIBE_X86_HOST:-192.168.86.13}}"
RSRC="${VIBE_REMOTE_SRC_DIR:-${VIBE_X86_SRC_DIR:-vibesdr-src}}"
PGO="${VIBE_PGO:-}"; PGO_DIR="${VIBE_PGO_DIR:-}"
case "$PGO" in ""|generate|use) ;; *) echo "!! VIBE_PGO is generate or use"; exit 1;; esac
[ -z "$PGO" ] || [ -n "$PGO_DIR" ] || { echo "!! VIBE_PGO needs VIBE_PGO_DIR (a directory on $HOST)"; exit 1; }
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCHES="amd64 i386 armhf arm64"
REV=""
SYNC_ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rev)  shift; REV="$1" ;;
    # ★ Stage the headers and sync the tree, then stop — publish-apt-remote.sh needs the tree over there to
    #   ask publish-apt.sh --next-rev which revision to build, before it can call this to build it.
    --sync-only) SYNC_ONLY=1 ;;
    --arch) shift; ARCHES="${1//,/ }" ;;
    *) echo "usage: $0 --rev <REV> [--arch 'amd64 i386 armhf arm64']"; exit 1 ;;
  esac
  shift
done
[ -n "$REV" ] || [ -n "$SYNC_ONLY" ] || { echo "usage: $0 --rev <REV> [--arch 'amd64 i386 armhf arm64']   (publish-apt-remote.sh picks it)"; exit 1; }
for A in $ARCHES; do
  case "$A" in amd64|i386|armhf|arm64) ;; *) echo "!! unknown architecture $A (amd64 i386 armhf arm64)"; exit 1;; esac
done
[ -z "$PGO" ] || [ "$ARCHES" = amd64 ] || [ "$ARCHES" = i386 ] || { echo "!! VIBE_PGO is for ONE x86 architecture"; exit 1; }

# ── a link that fails fast ─────────────────────────────────────────────────────────────────────
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=4"
rsh() {   # rsh <remote command> — exit 255 is ssh's own failure, never the remote command's
  local rc=0
  # shellcheck disable=SC2086
  ssh $SSH_OPTS "$HOST" "$@" || rc=$?
  if [ "$rc" = 255 ]; then
    echo "!! ssh to $HOST failed or DROPPED (exit 255) — is the Lenovo on the network? (its Wi-Fi drops)" >&2
    echo "!! Nothing was published. A build container cut off mid-run is removed by the next run." >&2
  fi
  return "$rc"
}
rsync_to_host() { rsync -a --timeout=60 -e "ssh $SSH_OPTS" "$@" || { echo "!! rsync to/from $HOST failed (exit $?)" >&2; return 1; }; }

rsh true || { echo "!! cannot reach $HOST"; exit 1; }
[ -n "$SYNC_ONLY" ] || echo "==> $HOST before: $(rsh uptime)"

# ★ No SDRplay headers to stage any more (2026-10-21): the API headers are OpenRSP's, committed in
#   vendor/openrsp/include, and the images no longer COPY a sdrplay-inc/ directory.
# ★ And the bundled tunnel: CMake refuses a package without it, so check here rather than after a build.
bash "$SRC_DIR/tools/cloudflared-desktop/fetch.sh" >/dev/null

echo "==> syncing the tree to $HOST:~/$RSRC"
rsync_to_host --delete --exclude node_modules --exclude .git --exclude 'android/app/build' \
  --exclude 'android/app/.cxx' --exclude 'ios/Pods' --exclude 'vibeserver/build' \
  --exclude 'vibeserver/build-*' --exclude 'web/dist' --exclude .claude --exclude spike \
  --exclude tvos --exclude lite --exclude '*.zip' --exclude '*.raw' --exclude '*.iq16' --exclude '*.cf32' \
  --exclude fuzz-corpus --exclude .armhf-out --exclude '.prebuilt-*' \
  "$SRC_DIR/" "$HOST:$RSRC/"

[ -z "$SYNC_ONLY" ] || exit 0

OUT="$SRC_DIR/vibeserver/build/prebuilt"
mkdir -p "$OUT"
DEBS=""
for A in $ARCHES; do
  case "$A" in
    amd64) IMAGE=vibeserver-build:bookworm-amd64; DF=Dockerfile.build;        PLAT=linux/amd64 ;;
    i386)  IMAGE=vibeserver-build:bookworm-i386;  DF=Dockerfile.build;        PLAT=linux/386 ;;
    armhf) IMAGE=vibeserver-cross:bookworm-armhf; DF=Dockerfile.cross-armhf;  PLAT=linux/amd64 ;;
    arm64) IMAGE=vibeserver-cross:bookworm-arm64; DF=Dockerfile.cross-arm64;  PLAT=linux/amd64 ;;
  esac
  echo "==> [$A] build image $IMAGE ($DF, $PLAT — native on $HOST, never emulated)"
  rsh "cd ~/$RSRC/vibeserver/linux && nice -n 19 docker build --platform $PLAT --build-arg DEPS_NICE=19 -q \
    -f $DF -t $IMAGE . >/dev/null"
  echo "==> [$A] build + package, revision $REV"
  PGOMNT=""
  case "$PGO" in
    generate) PGOMNT="-v \$HOME/$PGO_DIR:/pgo"; rsh "mkdir -p ~/$PGO_DIR" ;;
    use)      PGOMNT="-v \$HOME/$PGO_DIR:/pgo:ro" ;;
  esac
  NAME="vibe-build-$A"
  ENVS="-e JOBS=4 -e VIBE_PGO=$(printf %q "$PGO") -e VIBE_EXTRA_CFLAGS=$(printf %q "${VIBE_EXTRA_CFLAGS:-}")"
  ENVS="$ENVS -e VIBE_X86_32_FLAGS=$(printf %q "${VIBE_X86_32_FLAGS:-}")"
  rsh "docker rm -f $NAME >/dev/null 2>&1 || true
       rm -rf ~/vibeout/$A && mkdir -p ~/vibeout/$A && \
       docker run --rm --name $NAME --platform $PLAT --cpus 4 \
         -v \$HOME/$RSRC:/work/VibeSDR:ro -v \$HOME/vibeout/$A:/out $PGOMNT $ENVS \
         $IMAGE nice -n 19 bash /work/VibeSDR/vibeserver/linux/build-deb.sh $A $(printf %q "$REV") /out"
  if [ "$PGO" = generate ]; then
    echo "==> [$A] instrumented binaries in $HOST:~/vibeout/$A/pgogen/ — train them (counters -> ~/$PGO_DIR), then VIBE_PGO=use"
    continue
  fi
  DEB="$(rsh "ls ~/vibeout/$A/vibeserver_*_$A.deb" | head -1)"
  [ -n "$DEB" ] || { echo "!! [$A] no package came out"; exit 1; }
  rsync_to_host "$HOST:$DEB" "$OUT/"
  DEBS="$DEBS $OUT/$(basename "$DEB")"
done
echo "==> $HOST after: $(rsh uptime)"
echo "==> built:$DEBS"
# ★ A test revision (0~…, …+atom1 — anything not a plain number) must never reach the pool: it is a hand-out
#   to one tester. Say so instead of printing a publish line someone might paste.
case "$REV" in
  *[!0-9]*) echo "    TEST revision '$REV' — hand it out, never publish it (publish-apt.sh would refuse the version anyway)" ;;
  *)        echo "    a release is published by scripts/publish-apt-remote.sh (it builds, indexes on $HOST, signs here)" ;;
esac
