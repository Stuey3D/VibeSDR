#!/usr/bin/env bash
# build-x86-debs.sh — build the x86 VibeServer packages (amd64 + i386) on the Lenovo, natively.
#
#   scripts/build-x86-debs.sh --rev 2                      # both arches, version <cmake>-2
#   scripts/build-x86-debs.sh --rev 0~test1 --arch i386    # a test package, never published
#   VIBE_EXTRA_CFLAGS=-mtune=bonnell scripts/build-x86-debs.sh --rev 1+atom1 --arch amd64
#   VIBE_X86_32_FLAGS='-march=i686 -msse2 -mfpmath=sse -mtune=bonnell' scripts/build-x86-debs.sh --rev 1+t --arch i386
#   VIBE_PGO=generate VIBE_PGO_DIR=pgo-i386 scripts/build-x86-debs.sh --rev 1+pgo --arch i386   # instrumented binaries
#   VIBE_PGO=use      VIBE_PGO_DIR=pgo-i386 scripts/build-x86-debs.sh --rev 1+atom2 --arch i386 # package built from them
#
# ★ VIBE_X86_32_FLAGS replaces the i386 ISA line (see vibeserver/CMakeLists.txt) — a -mtune in VIBE_EXTRA_CFLAGS
#   is overridden there by the default -mtune=generic, which is what happened to the first i386 +atom1 test build.
# ★★ PROFILE-GUIDED (VIBE_PGO, 2026-10-18, docs/ATOM-PERFORMANCE-2026-10-18.md): `generate` builds the tree with
#    -fprofile-generate=/pgo and leaves vibeserver + vibeserver-bench in ~/vibeout/<arch>/pgogen/ instead of a
#    package; run them on representative work (the counters land in ~/<VIBE_PGO_DIR> on the build host, which is
#    mounted at /pgo); then `use` builds the package with -fprofile-use=/pgo. Both passes use THIS script's layout
#    (/build/VibeSDR/vibeserver/build), because GCC names each counter file after its object's absolute path —
#    a profile trained on a binary built anywhere else is silently not found.
# ★ VIBE_X86_SRC_DIR: the directory on the build host the tree is synced to (default vibesdr-src). Give a test
#   build its own, so it cannot pull the tree out from under somebody else's build there.
#
# ★★★ WHY THIS EXISTS. The Mac is Apple Silicon and cannot run x86 at all, so amd64 under qemu
#     segfaulted the compiler on a different file every run (3 failures out of 3 — see the memory note
#     build_emulated_arch_on_real_hardware), and publish-apt-docker.sh REFUSES it. i386 joined amd64
#     on 2026-10-17 and is the same case. The Lenovo/OWRX box (192.168.86.13) is x86-64 hardware: a
#     linux/amd64 container there is native, and so is linux/386 — an x86-64 CPU runs 32-bit code
#     directly. The recipe used to live in a memory note and was typed by hand each release; with two
#     architectures it is a script, because the one typed by hand is the one that forgets a flag.
# ★★★ ALWAYS IN A BOOKWORM CONTAINER, NEVER ON THE HOST. The host is Ubuntu 24.04 (glibc 2.39), and
#     CPack stamps Depends: from what the build links against — a host build demands libc6 >= 2.38 and
#     is uninstallable on Debian 12. The container is vibeserver/linux/Dockerfile.build, unchanged.
# ★★ IT IS A LIVE RECEIVER (private — Stuart is the only listener). So: nice 19, at most 4 jobs, and
#    no container is left running — every `docker run` is --rm.
# ★★ THE REVISION MUST BE THE ONE publish-apt.sh WILL EXPECT (one more than the pool's high-water mark
#    for that arch) or the version check refuses it. Test builds use a `0~…` revision, which sorts
#    BELOW -1, so a test package can never shadow the real one on a machine that installed it.
# ★ Only the .debs come back, into vibeserver/build/prebuilt/ (gitignored); build trees stay over there.
set -euo pipefail

HOST="${VIBE_X86_HOST:-192.168.86.13}"
RSRC="${VIBE_X86_SRC_DIR:-vibesdr-src}"
PGO="${VIBE_PGO:-}"; PGO_DIR="${VIBE_PGO_DIR:-}"
case "$PGO" in ""|generate|use) ;; *) echo "!! VIBE_PGO is generate or use"; exit 1;; esac
[ -z "$PGO" ] || [ -n "$PGO_DIR" ] || { echo "!! VIBE_PGO needs VIBE_PGO_DIR (a directory on $HOST)"; exit 1; }
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCHES="amd64 i386"
REV=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rev)  shift; REV="$1" ;;
    --arch) shift; ARCHES="${1//,/ }" ;;
    *) echo "usage: $0 --rev <REV> [--arch 'amd64 i386']"; exit 1 ;;
  esac
  shift
done
[ -n "$REV" ] || { echo "usage: $0 --rev <REV> [--arch 'amd64 i386']   (publish-apt.sh says which REV it wants)"; exit 1; }
for A in $ARCHES; do
  case "$A" in amd64|i386) ;; *) echo "!! $A is not an x86 architecture — arm64/armhf build on the Mac"; exit 1;; esac
done

# ★ The SDRplay headers are a build dependency staged from this machine, exactly as publish-apt-docker.sh
#   does (they are SDRplay's to license, so never committed). i386 compiles SDRplay out, but the image
#   is shared and its COPY step needs them.
SDRPLAY_INC="${SDRPLAY_INC:-/usr/local/include}"
STAGE="$SRC_DIR/vibeserver/linux/sdrplay-inc"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp "$SDRPLAY_INC"/sdrplay_api*.h "$STAGE"/ || { echo "!! no SDRplay headers in $SDRPLAY_INC"; exit 1; }
# ★ And the bundled tunnel: CMake refuses a package without it, so check here rather than after a build.
bash "$SRC_DIR/tools/cloudflared-desktop/fetch.sh" >/dev/null

echo "==> syncing the tree to $HOST:~/$RSRC"
rsync -a --delete --exclude node_modules --exclude .git --exclude 'android/app/build' \
  --exclude 'android/app/.cxx' --exclude 'ios/Pods' --exclude 'vibeserver/build' \
  --exclude 'vibeserver/build-*' --exclude 'web/dist' --exclude .claude --exclude spike \
  --exclude tvos --exclude lite --exclude '*.zip' --exclude '*.raw' --exclude '*.iq16' --exclude '*.cf32' \
  --exclude fuzz-corpus --exclude .armhf-out \
  "$SRC_DIR/" "$HOST:$RSRC/"

OUT="$SRC_DIR/vibeserver/build/prebuilt"
mkdir -p "$OUT"
DEBS=""
for A in $ARCHES; do
  IMAGE="vibeserver-build:bookworm-$A"
  echo "==> [$A] build image (linux/$A, native on $HOST)"
  ssh "$HOST" "cd ~/$RSRC/vibeserver/linux && nice -n 19 docker build --platform linux/$A -q \
    -f Dockerfile.build -t $IMAGE . >/dev/null"
  echo "==> [$A] build + package, revision $REV"
  # ★★ `set -euo pipefail` INSIDE, and no `| tail` on the compile: a string handed to bash -c does not
  #    inherit the outer shell's options, and `cmake --build | tail -1` reports TAIL's status — that
  #    almost shipped a package built from stale objects (see publish-apt.sh).
  # ★★★ `rm -rf build build-*` FIRST, IN THE COPY. rsync --delete never deletes an EXCLUDED path on the
  #     receiving side, so ~/vibesdr-src/vibeserver/build from some earlier hand build survives every
  #     sync — and its amd64 CMakeCache.txt was picked up by the first i386 build (2026-10-17):
  #     pointer size 8, libusb from /usr/lib/x86_64-linux-gnu, link failure. Quietly worse on amd64:
  #     a stale cache keeps whatever flags it was configured with. The host's directories are left
  #     alone (they may be somebody's); only the throwaway copy in the container is cleaned.
  # ★ The WHOLE tree, not --target vibeserver: cpack's install step builds `all` anyway — single-job
  #   and with its output hidden in cpack.log — so a narrow target only moves the cost somewhere slower.
  CF="${VIBE_EXTRA_CFLAGS:-}"; PGOMNT=""; TARGET=""
  case "$PGO" in
    generate) CF="$CF -fprofile-generate=/pgo -fprofile-update=atomic"; PGOMNT="-v \$HOME/$PGO_DIR:/pgo"
              TARGET="--target vibeserver vibeserver-bench" ;;
    use)      CF="$CF -fprofile-use=/pgo -fprofile-partial-training -Wno-missing-profile"; PGOMNT="-v \$HOME/$PGO_DIR:/pgo:ro" ;;
  esac
  [ -z "$PGO" ] || ssh "$HOST" "mkdir -p ~/$PGO_DIR"
  ssh "$HOST" "rm -rf ~/vibeout/$A && mkdir -p ~/vibeout/$A && docker run --rm --platform linux/$A --cpus 4 \
    -v \$HOME/$RSRC:/work/VibeSDR:ro -v \$HOME/vibeout/$A:/out $PGOMNT \
    -e VIBE_EXTRA_CFLAGS='${CF# }' -e VIBE_X86_32_FLAGS='${VIBE_X86_32_FLAGS:-}' $IMAGE /bin/bash -euo pipefail -c '
      mkdir -p /build && cp -a /work/VibeSDR /build/VibeSDR
      cd /build/VibeSDR/vibeserver
      rm -rf build build-*
      nice -n 19 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVIBESERVER_DEB_REV=$REV \
        -DVIBESERVER_STRICT_RADIOS=ON \${VIBE_EXTRA_CFLAGS:+\"-DCMAKE_C_FLAGS=\$VIBE_EXTRA_CFLAGS\" \"-DCMAKE_CXX_FLAGS=\$VIBE_EXTRA_CFLAGS\"} \
        \${VIBE_X86_32_FLAGS:+\"-DVIBE_X86_32_FLAGS=\$VIBE_X86_32_FLAGS\"} > /out/cmake.log
      nice -n 19 cmake --build build $TARGET -j4 > /out/build.log 2>&1 \
        || { tail -30 /out/build.log; exit 1; }
      # ★ chmod: the container runs as root, and a root-owned directory under ~/vibeout stops the next
      #   run from emptying it (rm -rf) — the && chain then skips the whole build without saying so.
      if [ \"$PGO\" = generate ]; then mkdir -p /out/pgogen && cp build/vibeserver build/vibeserver-bench /out/pgogen/ \
                                        && chmod -R a+rwX /out/pgogen; exit 0; fi
      cd build && nice -n 19 cpack > /out/cpack.log
      cp vibeserver_*.deb /out/'"
  if [ "$PGO" = generate ]; then
    echo "==> [$A] instrumented binaries in $HOST:~/vibeout/$A/pgogen/ — train them (counters -> ~/$PGO_DIR), then VIBE_PGO=use"
    continue
  fi
  DEB="$(ssh "$HOST" "ls ~/vibeout/$A/vibeserver_*_$A.deb" | head -1)"
  [ -n "$DEB" ] || { echo "!! [$A] no package came out"; exit 1; }
  # ★★ THE SAME GATES publish-apt.sh RUNS, here as well, so a bad package is caught before it travels.
  #    Run in the build image (bookworm dpkg), not on the host. grep -c, never -q, under pipefail.
  ssh "$HOST" "docker run --rm --platform linux/$A -v \$HOME/vibeout/$A:/out:ro $IMAGE /bin/bash -euo pipefail -c '
      d=\$(ls /out/vibeserver_*.deb)
      [ \"\$(dpkg-deb -f \$d Architecture)\" = $A ] || { echo \"!! wrong Architecture\"; exit 1; }
      [ \"\$(dpkg-deb --fsys-tarfile \$d | grep -ca \"First-time setup\" || true)\" -gt 0 ] \
        || { echo \"!! no settings wizard in the binary\"; exit 1; }
      if dpkg-deb -f \$d Depends | tr , \"\\n\" | grep -E \"libc6 \\(>= 2\\.(3[6-9]|[4-9])|libstdc\\+\\+6 \\(>= 1[3-9]\"; then
        echo \"!! needs a newer glibc than 2.35 (Ubuntu 22.04)\"; exit 1; fi
      # ★ Nothing but libc6 and the exec()d tools (2026-10-18) — every library is linked in.
      x=\$(dpkg-deb -f \$d Depends | tr , \"\\n\" | sed \"s/^ *//; s/ *(.*\\\$//\" | grep -vxE \"adduser|curl|gzip|libc6\" || true)
      [ -z \"\$x\" ] || { echo \"!! depends on system libraries: \$x\"; exit 1; }
      echo \"    Depends: \$(dpkg-deb -f \$d Depends)\"'"
  scp -q "$HOST:$DEB" "$OUT/"
  DEBS="$DEBS $OUT/$(basename "$DEB")"
  echo "==> [$A] $(basename "$DEB")"
done
echo "==> built:$DEBS"
# ★ A test revision (0~…, …+atom1 — anything not a plain number) must never reach the pool: it is a
#   hand-out to one tester. Say so instead of printing a publish line someone might paste.
case "$REV" in
  *[!0-9]*) echo "    TEST revision '$REV' — hand it out, never publish it (publish-apt.sh would refuse the version anyway)" ;;
  *)        echo "    publish with:  VIBE_PREBUILT_DEB='${DEBS# }' scripts/publish-apt-docker.sh" ;;
esac
