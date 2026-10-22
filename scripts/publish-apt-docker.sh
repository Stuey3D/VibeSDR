#!/usr/bin/env bash
# publish-apt-docker.sh — build and publish VibeServer from the Mac, in a bookworm container.
#
# ★★★ THIS IS THE BACKUP ROUTE (2026-10-18). THE NORMAL ONE IS scripts/publish-apt-remote.sh: every package
#     is built and indexed on the Lenovo (192.168.86.13), and the Mac only signs, commits and pushes. Use this
#     script only when the Lenovo is unavailable. Both run the same publish-apt.sh, so the rules are the same.
# ★★★ ~/.colima WAS DELETED ON 2026-10-18 to free 13 GB, so this route needs its VM recreated first —
#     SMALL, as explained further down (a 40 GB disk once took the Mac to 134 MB free):
#         colima start --cpu 4 --memory 4 --disk 12
#         brew install docker-buildx      # + "cliPluginsExtraDirs" in ~/.docker/config.json (see below)
#     and removed completely afterwards:  colima stop && colima delete -f && rm -rf ~/.colima/_lima/_disks
#     amd64 and i386 still come from the Lenovo here (VIBE_PREBUILT_DEB) — this Mac cannot build x86 — so if
#     the Lenovo is down, this route can publish arm64 + armhf only, and it says so before refusing.
#
#   scripts/publish-apt-docker.sh                # BOTH arches, index, sign, commit, push
#   scripts/publish-apt-docker.sh --dry-run      # everything except the push
#   scripts/publish-apt-docker.sh --arch arm64   # one architecture only (see the warning below)
#
# ★★★ WHY THIS EXISTS. The build box used to be the Pi — which is also the machine we format to
#     test fresh installs. Every wipe destroyed the chroot, the signing key and the source clone;
#     it happened twice in one evening, and the hurried rebuild is what shipped a release with the
#     setup wizard compiled out. The build environment now lives somewhere a format cannot reach,
#     and is described by vibeserver/linux/Dockerfile.build rather than by memory.
#
# ★★ IT RUNS publish-apt.sh — the same script, not a second copy of it. That script detects it is
#    already on bookworm and builds natively instead of nesting a chroot. Two publish paths would
#    drift, and the one that drifts is the one that quietly stops checking something.
#
# ★ The signing key is mounted READ-ONLY from the backup and used inside a throwaway GNUPGHOME, so
#   nothing is left behind in the image or in a layer.
set -euo pipefail

# ★★★ THE COMPATIBILITY GATE (docs/PROTOCOL.md, BRIEF-v11-compatibility §8). Nothing is published
#     unless every captured client fixture passes against a server running THIS tree — set
#     VIBE_COMPAT_TARGET to it (a bench box on the candidate build). A publish without a target is
#     refused unless VIBE_COMPAT_SKIP names the reason, which is printed and lands in the log.
if [ -n "${VIBE_COMPAT_TARGET:-}" ]; then
  echo "==> compat gate against $VIBE_COMPAT_TARGET"
  bash "$(dirname "$0")/compat/run.sh" "$VIBE_COMPAT_TARGET" || { echo "!! compat gate FAILED — not publishing"; exit 1; }
elif [ -n "${VIBE_COMPAT_SKIP:-}" ]; then
  echo "==> compat gate SKIPPED: $VIBE_COMPAT_SKIP"
else
  echo "!! no VIBE_COMPAT_TARGET (a server on the candidate build) and no VIBE_COMPAT_SKIP=<reason> — refusing to publish"; exit 1
fi

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# ★★★ THE WEB PAGE IS A GENERATED FILE, AND A STALE ONE SHIPS SILENTLY (2026-10-18). Two controller fixes
#     sat in web/client all day while vibe_web_page.h was still the 11.0.5 build, and 11.0.6-1 went to apt,
#     Lite, the Mac and Android without them — every check passed, because every check tests the header.
#     Rebuild it here and refuse if that changes anything: the page in the package must be the page in git.
_page="$SRC_DIR/android/app/src/main/cpp/vibe_web_page.h"
( cd "$SRC_DIR" && node scripts/build-web.mjs >/dev/null ) || { echo "!! scripts/build-web.mjs failed — not publishing"; exit 1; }
if ! git -C "$SRC_DIR" diff --quiet -- "$_page"; then
  echo "!! vibe_web_page.h was out of date with web/client — it has been rebuilt; review and commit it, then publish"
  exit 1
fi
APT_DIR="${APT_DIR:-$HOME/VibeServer}"
KEY_FILE="${VIBESERVER_SIGNING_KEY:-$HOME/Documents/VibeSDR-keys/vibesdr-apt-signing-key.asc}"
DRY_RUN=""
# ★★★ BOTH ARCHITECTURES BY DEFAULT, and that default is the whole safety property. amd64 joined
#     the repository on 2026-08-20; if a routine release built only arm64, x86 users would sit on
#     an old package for ever with apt reporting everything up to date — a stale architecture is
#     invisible from the publishing end, which is exactly why it must not be opt-in.
# ★ `--arch arm64` is there for a genuine hurry (see the emulation note below), not for habit.
ARCHES="arm64 amd64 armhf i386"   # ★ armhf joined 2026-09-22 — cross-compiled; i386 2026-10-17 — built on the Lenovo
# ★★ i386 IS 32-BIT x86 — Kiko's Atom N270 netbook in Brazil ran 11.0.4 built from source because apt
#    had nothing for it, and the mission is exactly that machine (Stuart, 2026-10-17). SSE2 floor;
#    preinst refuses a CPU without it. Built natively on the Lenovo like amd64 (see the refusal below).
# ★★ --suite beta (2026-10-22): publish a V12 beta into suite `beta` only — the same rules and guards as
#    publish-apt-remote.sh (see its header); publish-apt.sh does the work from VIBE_SUITE.
SUITE="stable"
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN="--dry-run" ;;
    --arch)    shift; ARCHES="${1//,/ }" ;;
    --suite)   shift; SUITE="${1:-}" ;;
    *) echo "usage: $0 [--dry-run] [--arch arm64|amd64|armhf|i386|'arm64 amd64 armhf i386'] [--suite stable|beta]"; exit 1 ;;
  esac
  shift
done
case "$SUITE" in stable|beta) ;; *) echo "!! --suite must be stable or beta"; exit 1;; esac
_up="$(grep -oE 'project\([^)]*VERSION[[:space:]]+[0-9.]+' "$(cd "$(dirname "$0")/.." && pwd)/vibeserver/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
_pre="$(sed -n 's/^set(VIBESERVER_PRERELEASE "\([A-Za-z0-9]*\)").*/\1/p' "$(cd "$(dirname "$0")/.." && pwd)/vibeserver/CMakeLists.txt" | head -1)"
case "$SUITE:$_pre" in
  beta:)        echo "!! $_up is not a prerelease — a beta is versioned like 12.0.0~beta1 (VIBESERVER_PRERELEASE)"; exit 1 ;;
  stable:beta*) echo "!! $_up~$_pre is a BETA — it would be offered to every server. Publish it with --suite beta"; exit 1 ;;
esac
echo "==> suite: $SUITE"

command -v docker >/dev/null || { echo "docker not found (brew install colima docker)"; exit 1; }
# ★★★ BUILDKIT, AND IT IS NOT OPTIONAL ONCE A SECOND ARCHITECTURE EXISTS. The LEGACY builder
#     accepts `--platform linux/amd64`, IGNORES IT, cheerfully builds the whole image for the host
#     architecture — and only trips at the first COPY, with "image ... does not provide the
#     specified platform". Every apt line in the log says arm64 while the build claims to be amd64,
#     so the error arrives ten minutes late and describes a symptom rather than the cause.
# ★★ Worse, without the COPY it might not have tripped AT ALL: a legacy cross-"build" with no COPY
#    step would have produced an arm64 binary in a package stamped amd64. The failure we got is the
#    kind you want.
# ★ buildx is a separate component and is not in colima's docker by default.
export DOCKER_BUILDKIT=1
docker buildx version >/dev/null 2>&1 || {
  echo "!! docker buildx is missing, and cross-architecture builds need it."
  echo "   brew install docker-buildx"
  echo "   then add to ~/.docker/config.json:"
  echo '     "cliPluginsExtraDirs": ["/opt/homebrew/lib/docker/cli-plugins"]'
  exit 1; }
# ★★★ START IT SMALL, AND SAY HOW TO REMOVE IT. `colima start --disk 40` allocates a 40 GB image
#     under ~/.colima and it GROWS to its ceiling as builds fill it — it took this Mac to 134 MB
#     free, at which point nothing could run at all, not even `df`. A build box for one 1 MB .deb
#     does not need 40 GB.
# ★★★ AND `colima delete` DOES NOT REMOVE IT. It deletes the instance and leaves the named data
#     disk at ~/.colima/_lima/_disks/colima/datadisk, so the cleanup that looks like it worked
#     reclaims about 1 GB of 38. To remove it completely:
#         colima stop && colima delete -f && rm -rf ~/.colima/_lima/_disks
# ★ 12 GB is comfortably more than the base image, the source copy and the build tree need.
if ! docker info >/dev/null 2>&1; then
  echo "==> starting the build VM (colima)"
  colima start --cpu 4 --memory 4 --disk 12 || {
    echo "could not start colima — run 'colima start --disk 12' yourself"; exit 1; }
fi

# ★★ THE KEY IS THE ONE THING THAT CANNOT BE REBUILT. Check it before a long build, and say where
#    it should be rather than just that it is absent.
[ -r "$KEY_FILE" ] || { echo "no signing key at $KEY_FILE"; echo "(set VIBESERVER_SIGNING_KEY)"; exit 1; }
grep -q "PRIVATE KEY BLOCK" "$KEY_FILE" || {
  echo "!! $KEY_FILE is not a secret key — it looks like the PUBLIC one."
  echo "   The public key (KEY.gpg) is what users import; it cannot sign anything."
  exit 1; }

# ★ The apt repo clone lives on the Mac and is the copy that must survive; the container only
#   borrows it. Cloned here rather than inside, so a failed run never leaves a half repo.
if [ ! -d "$APT_DIR/.git" ]; then
  echo "==> cloning the apt repository -> $APT_DIR"
  git clone https://github.com/Stuey3D/VibeServer.git "$APT_DIR"
fi

# ── SDRplay headers: NOT STAGED ANY MORE (2026-10-21) ─────────────────────────
# ★ The API headers are OpenRSP's (GPLv3), committed in vendor/openrsp/include; the images no longer
#   COPY sdrplay-inc/. RSP support cannot compile out for want of a header on this machine.

# ★★ ONE ARCHITECTURE AT A TIME, SEQUENTIALLY — they share the apt clone, and two publishes
#    writing the same Packages/Release/index.html at once would interleave into nonsense.
# ★★★ amd64 ON APPLE SILICON IS EMULATED, and it is slow: the image build compiles librtlsdr from
#     source under emulation. That is a cost per BUILD, not per publish, so the layer cache makes
#     the second release cheap — do not "optimise" it by skipping the arch.
# ★ The image is tagged per architecture. One shared tag meant the second run reused the FIRST
#   architecture's image, and docker would not have complained: it is a valid image, for the wrong
#   machine, and the .deb it produces is stamped with the wrong Architecture:.
# ★★★ A PACKAGE BUILT ON A REAL MACHINE OF THAT ARCHITECTURE — see VIBE_PREBUILT_DEB in
#     publish-apt.sh. Set it on the host and the compile is skipped for that arch; every check
#     still runs, and the signing key stays here. Built for the emulated arch, which segfaults
#     the compiler under qemu on a different file every run.
# ★★★ SEVERAL PRE-BUILT PACKAGES, ONE PER x86 ARCHITECTURE (2026-10-17). With i386 beside amd64 there
#     are two architectures the Mac cannot build, and a single VIBE_PREBUILT_DEB could only ever cover
#     one of them per run — so a default run would have refused i386 or amd64 every time, and the
#     "obvious" workaround is publishing them separately, which is the stale-architecture trap again.
#     VIBE_PREBUILT_DEB now takes a SPACE-SEPARATED LIST and each file goes to the pass for its own
#     architecture. scripts/build-x86-debs.sh builds both on the Lenovo and prints the exact line.
# ★★ ROUTED BY THE CANONICAL FILENAME, vibeserver_<ver>-<rev>_<arch>.deb — and a file not named that
#    way is refused here, because the indexer ignores it in the pool and the run still says
#    "published:" (build_emulated_arch_on_real_hardware, trap 1). publish-apt.sh then asks the
#    PACKAGE its Architecture: as well, so a renamed file cannot slip through either.
HOSTARCH=$(uname -m); case "$HOSTARCH" in arm64|aarch64) HOSTARCH=arm64;; x86_64) HOSTARCH=amd64;; esac
PREBUILT_STAGED_ALL=""
prebuilt_for() {   # prebuilt_for <arch> — the staged path for that arch, or nothing
  local f; for f in $PREBUILT_STAGED_ALL; do
    case "$(basename "$f")" in *_"$1".deb) echo "$f"; return;; esac
  done; }
if [ -n "${VIBE_PREBUILT_DEB:-}" ]; then
  for f in $VIBE_PREBUILT_DEB; do
    [ -f "$f" ] || { echo "!! no such file: $f"; exit 1; }
    case "$(basename "$f")" in
      vibeserver_*-*_*.deb) ;;
      *) echo "!! $f is not named vibeserver_<ver>-<rev>_<arch>.deb — the indexer would ignore it"; exit 1;;
    esac
    _a="$(basename "$f" .deb)"; _a="${_a##*_}"
    [ -z "$(prebuilt_for "$_a")" ] || { echo "!! two pre-built packages for $_a"; exit 1; }
    case " $ARCHES " in *" $_a "*) ;; *) echo "!! $f is for $_a, which this run does not publish (--arch '$ARCHES')"; exit 1;; esac
    # ★ STAGED INSIDE THE SOURCE MOUNT rather than bind-mounted on its own. A second -v for the file
    #   is the obvious way and it silently does not work: Docker Desktop shares a fixed set of host
    #   paths, so a mount outside them yields no file and the container reports the package "does not
    #   exist" — which reads as a missing build rather than a missing mount. $SRC_DIR is already
    #   mounted and already proven, so the file travels the way everything else does.
    _st="$SRC_DIR/.prebuilt-$(basename "$f")"
    cp "$f" "$_st"
    PREBUILT_STAGED_ALL="$PREBUILT_STAGED_ALL $_st"
  done
  trap 'rm -f $PREBUILT_STAGED_ALL' EXIT
fi

for A in $ARCHES; do
  IMAGE="vibeserver-build:bookworm-$A"
  DOCKERFILE="$SRC_DIR/vibeserver/linux/Dockerfile.build"
  PLATFORM="$A"
  CROSS_ENV=""
  PREBUILT_ENV=""
  # ★★★ armhf IS A CROSS-COMPILE ON THE HOST'S OWN ARCHITECTURE — never an arm/v7 container.
  #     Apple Silicon cannot execute AArch32, so that would be qemu, the emulation that segfaults.
  #     Measured on the Pi 2 against its native build: 174% vs 174% of a core (-marm matters —
  #     see armhf-toolchain.cmake). ARMv7+NEON only: the package refuses a CPU without NEON.
  if [ "$A" = "armhf" ]; then
    IMAGE="vibeserver-cross:bookworm-armhf"
    DOCKERFILE="$SRC_DIR/vibeserver/linux/Dockerfile.cross-armhf"
    PLATFORM="$HOSTARCH"
    CROSS_ENV="-e VIBE_CROSS_ARCH=armhf"
  fi
  # ★★★ A PRE-BUILT PACKAGE IS INDEXED IN THE HOST'S OWN CONTAINER (2026-10-17). Nothing is compiled
  #     on that pass — only dpkg-deb, dpkg-scanpackages and gpg, none of which care what architecture
  #     they run on — so emulating the target just to index it bought a qemu image build (librtlsdr
  #     compiled under emulation, ~1 GB more on a tight disk) and nothing else. VIBE_PUBLISH_ARCH
  #     tells publish-apt.sh which architecture it is publishing; it still checks the package agrees.
  _pb="$(prebuilt_for "$A")"
  if [ -n "$_pb" ]; then
    IMAGE="vibeserver-build:bookworm-$HOSTARCH"
    DOCKERFILE="$SRC_DIR/vibeserver/linux/Dockerfile.build"
    PLATFORM="$HOSTARCH"
    CROSS_ENV=""
    PREBUILT_ENV="-e VIBE_PREBUILT_DEB=/work/VibeSDR/$(basename "$_pb") -e VIBE_PUBLISH_ARCH=$A"
  fi
  echo "==> [$A] build image"
  docker build --platform "linux/$PLATFORM" -q \
    -f "$DOCKERFILE" -t "$IMAGE" "$SRC_DIR/vibeserver/linux" >/dev/null

  # ★★ --dry-run still BUILDS and SIGNS and commits locally; it only withholds the push. That is
  #    what makes it worth running: the failures worth catching are in the build and the index.
  echo "==> [$A] build + index + sign (in bookworm)"
  # ★★★ ONE JOB WHEN THE ARCH IS EMULATED. qemu-translated compilers running four-up segfault on
  #     files this size — three separate failures on 2026-08-26, each on a different file, which is
  #     the signature of the environment rather than the code. Raising the VM from 4 to 8 to 12 GiB
  #     did not fix it, because the constraint is concurrency under emulation, not total memory.
  # ★ The native architecture keeps nproc: it is not emulated and it is not the one that breaks.
  # ✗✗✗ amd64 UNDER qemu IS NOT A BUILD ROUTE, AND THIS REFUSES IT RATHER THAN RELYING ON
  #     WHOEVER IS DRIVING TO REMEMBER. It has now failed 3 attempts out of 3 across sessions
  #     (iqbalancer.c at 84 %, resampler.cpp at 8 %, and a whole afternoon of segfaults on a
  #     DIFFERENT file each run — which is how you tell an environment fault from a code fault),
  #     all already at JOBS=1. Stuart, 2026-09-27: "DAMNIT why are you emulating X86 you have an
  #     x86 box with docker you can use … we go through this every time".
  #  ★★★ "WE GO THROUGH THIS EVERY TIME" IS A SCRIPT BUG, NOT A MEMORY PROBLEM. The knowledge was
  #      written down, starred, and titled "THE DEFAULT ROUTE, NOT A FALLBACK" — and it still lost
  #      to a script that quietly offered the slow, broken path. A rule that depends on being
  #      remembered gets forgotten; one the tool enforces does not. So this is the enforcement.
  #  ★★ THE TRAP IT ALSO CLOSES: the arm64 half succeeds every time, so an abandoned amd64 leaves
  #     an ARM-ONLY release — and a stale architecture is invisible from the publishing end. x86
  #     users would sit on an old package with apt reporting everything up to date.
  #  ★ The Lenovo/OWRX box is amd64 hardware on the LAN and builds this in about a minute, in a
  #    BOOKWORM CONTAINER (never on the host — it is Ubuntu 24.04, and CPack would stamp
  #    libc6 >= 2.38 onto a package meant for Debian 12). It is a live receiver but a private one
  #    with Stuart as the only listener, so building there disturbs nobody.
  #  ★ armhf is deliberately NOT covered: it cross-builds here reliably and has no box to go to.
  #  ★★ i386 IS THE SAME CASE (2026-10-17): Apple Silicon runs no x86 at all, so a linux/386 container
  #     here is qemu too. On the Lenovo it is native — an x86-64 CPU executes 32-bit code directly.
  if { [ "$A" = "amd64" ] || [ "$A" = "i386" ]; } && [ "$HOSTARCH" != "amd64" ] && [ -z "${PREBUILT_ENV:-}" ]; then
    echo "!! REFUSING to build $A under emulation — amd64 has failed 3/3 and takes ~25 min to fail."
    echo "!! Build it natively on the Lenovo/OWRX box (192.168.86.13, ~1 min per arch):"
    echo "!!   scripts/build-debs-remote.sh --rev <REV> --arch 'amd64 i386'   # (or the normal route: publish-apt-remote.sh)"
    echo "!!   VIBE_PREBUILT_DEB='/path/vibeserver_<ver>-<rev>_amd64.deb /path/vibeserver_<ver>-<rev>_i386.deb' $0"
    echo "!! See the memory note build_emulated_arch_on_real_hardware for the exact container run."
    echo "!! ✗ Do not work around this by dropping $A: publishing one arch hides the other."
    echo "!! (VIBE_ALLOW_EMULATION=1 overrides, if you have a reason qemu will behave today.)"
    [ -z "${VIBE_ALLOW_EMULATION:-}" ] && exit 1
  fi
  JOBS_FOR_ARCH=""; [ "$PLATFORM" != "$HOSTARCH" ] && JOBS_FOR_ARCH=1
  [ -n "$JOBS_FOR_ARCH" ] && echo "==> [$A] emulated — building with JOBS=$JOBS_FOR_ARCH"
  docker run --rm --platform "linux/$PLATFORM" \
    -v "$SRC_DIR":/work/VibeSDR \
    -v "$APT_DIR":/work/VibeServer \
    -v "$KEY_FILE":/tmp/signing-key.asc:ro \
    -e APT_DIR=/work/VibeServer \
    -e VIBE_SUITE="$SUITE" \
    ${PREBUILT_ENV} ${CROSS_ENV} \
    -e JOBS="$JOBS_FOR_ARCH" \
    "$IMAGE" /bin/bash -euo pipefail -c '
    export GNUPGHOME=$(mktemp -d)
    gpg --batch --quiet --import /tmp/signing-key.asc
    # ★ Ultimate trust, or gpg refuses to sign with a key it has no reason to trust — and the
    #   failure reads as "no secret key", which sends you looking for the wrong problem.
    echo "$(gpg --list-secret-keys --with-colons | awk -F: "/^fpr:/{print \$10; exit}"):6:" \
      | gpg --import-ownertrust
    git config --global --add safe.directory /work/VibeSDR
    git config --global --add safe.directory /work/VibeServer
    bash /work/VibeSDR/scripts/publish-apt.sh '"$DRY_RUN"'
    rm -rf "$GNUPGHOME"
  '
done

# ★★★ SAY WHAT THE REPOSITORY NOW OFFERS EACH ARCHITECTURE, out of the signed index rather than out
#     of what we believe we just did. A stale architecture is silent everywhere else: apt on the
#     machine left behind reports no updates and no error.
echo "==> the repository now offers ($SUITE suite):"
for d in "$APT_DIR/dists/$SUITE/main"/binary-*; do
  [ -e "$d" ] || continue
  printf '      %-8s %s\n' "$(basename "$d" | sed 's/^binary-//')" \
    "$(grep -oE '^Version: [0-9][^ ]*' "$d/Packages" 2>/dev/null | awk '{print $2}' | sort -V | tail -1)"
done

if [ -n "$DRY_RUN" ]; then
  echo "==> dry run: nothing pushed. $APT_DIR holds the result."
  exit 0
fi

echo "==> pushing"
git -C "$APT_DIR" push
echo "==> published: https://apt.vibesdr.net/"
echo "    (GitHub Pages takes a few seconds to rebuild before apt sees it)"
