#!/usr/bin/env bash
# publish-apt-docker.sh — build and publish VibeServer from the Mac, in a bookworm container.
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

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APT_DIR="${APT_DIR:-$HOME/VibeServer}"
KEY_FILE="${VIBESERVER_SIGNING_KEY:-$HOME/Documents/VibeSDR-keys/vibesdr-apt-signing-key.asc}"
DRY_RUN=""
# ★★★ BOTH ARCHITECTURES BY DEFAULT, and that default is the whole safety property. amd64 joined
#     the repository on 2026-08-20; if a routine release built only arm64, x86 users would sit on
#     an old package for ever with apt reporting everything up to date — a stale architecture is
#     invisible from the publishing end, which is exactly why it must not be opt-in.
# ★ `--arch arm64` is there for a genuine hurry (see the emulation note below), not for habit.
ARCHES="arm64 amd64"
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN="--dry-run" ;;
    --arch)    shift; ARCHES="${1//,/ }" ;;
    *) echo "usage: $0 [--dry-run] [--arch arm64|amd64|'arm64 amd64']"; exit 1 ;;
  esac
  shift
done

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

# ── SDRplay headers: staged from this machine into the build context ─────────
# ★★★ NOT DOWNLOADED ANY MORE. The Dockerfile used to curl them from sdrplay.com; that URL 404s as
#     of 2026-08-20 and their downloads now sit behind a recaptcha form. Releases only kept working
#     because the arm64 image had the step cached — the first CLEAN build of any architecture would
#     have failed, and a build that depends on someone else's website being unchanged is a build
#     that breaks on their schedule rather than ours.
# ★★ TO BE CLEAR ABOUT WHAT SDRplay IS HERE: the LIBRARY is the user's own business — it is
#    dlopen'd at runtime, it is never a dependency of our .deb, and RSP owners install it
#    themselves. The HEADERS are a BUILD dependency: without them the RSP backend compiles out,
#    which shipped once and is why VIBESERVER_STRICT_RADIOS refuses rather than warns.
# ★ Staged, not committed: the headers are SDRplay's to license, so they live on the build machine
#   (their own installer puts them in /usr/local/include) and are copied in per build.
SDRPLAY_INC="${SDRPLAY_INC:-/usr/local/include}"
STAGE="$SRC_DIR/vibeserver/linux/sdrplay-inc"
rm -rf "$STAGE"; mkdir -p "$STAGE"
if ! cp "$SDRPLAY_INC"/sdrplay_api*.h "$STAGE"/ 2>/dev/null; then
  echo "!! no SDRplay headers in $SDRPLAY_INC"
  echo "   They are a BUILD dependency (the RSP backend needs them to compile); the runtime"
  echo "   library is not, and users install that themselves."
  echo "   Install SDRplay's API package on this machine, or set SDRPLAY_INC to a directory"
  echo "   holding sdrplay_api*.h."
  exit 1
fi
echo "==> staged $(ls "$STAGE" | wc -l | tr -d ' ') SDRplay headers from $SDRPLAY_INC"

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
# ★ STAGED INSIDE THE SOURCE MOUNT rather than bind-mounted on its own. A second -v for the file
#   is the obvious way and it silently does not work: Docker Desktop shares a fixed set of host
#   paths, so a mount outside them yields no file and the container reports the package "does not
#   exist" — which reads as a missing build rather than a missing mount. $SRC_DIR is already
#   mounted and already proven, so the file travels the way everything else does.
PREBUILT_ENV=""; PREBUILT_STAGED=""
if [ -n "${VIBE_PREBUILT_DEB:-}" ]; then
  [ -f "$VIBE_PREBUILT_DEB" ] || { echo "!! no such file: $VIBE_PREBUILT_DEB"; exit 1; }
  PREBUILT_STAGED="$SRC_DIR/.prebuilt-$(basename "$VIBE_PREBUILT_DEB")"
  cp "$VIBE_PREBUILT_DEB" "$PREBUILT_STAGED"
  trap 'rm -f "$PREBUILT_STAGED"' EXIT
  PREBUILT_ENV="-e VIBE_PREBUILT_DEB=/work/VibeSDR/$(basename "$PREBUILT_STAGED")"
fi

for A in $ARCHES; do
  IMAGE="vibeserver-build:bookworm-$A"
  echo "==> [$A] build image"
  docker build --platform "linux/$A" -q \
    -f "$SRC_DIR/vibeserver/linux/Dockerfile.build" -t "$IMAGE" "$SRC_DIR/vibeserver/linux" >/dev/null

  # ★★ --dry-run still BUILDS and SIGNS and commits locally; it only withholds the push. That is
  #    what makes it worth running: the failures worth catching are in the build and the index.
  echo "==> [$A] build + index + sign (in bookworm)"
  # ★★★ ONE JOB WHEN THE ARCH IS EMULATED. qemu-translated compilers running four-up segfault on
  #     files this size — three separate failures on 2026-08-26, each on a different file, which is
  #     the signature of the environment rather than the code. Raising the VM from 4 to 8 to 12 GiB
  #     did not fix it, because the constraint is concurrency under emulation, not total memory.
  # ★ The native architecture keeps nproc: it is not emulated and it is not the one that breaks.
  HOSTARCH=$(uname -m); case "$HOSTARCH" in arm64|aarch64) HOSTARCH=arm64;; x86_64) HOSTARCH=amd64;; esac
  JOBS_FOR_ARCH=""; [ "$A" != "$HOSTARCH" ] && JOBS_FOR_ARCH=1
  [ -n "$JOBS_FOR_ARCH" ] && echo "==> [$A] emulated — building with JOBS=$JOBS_FOR_ARCH"
  docker run --rm --platform "linux/$A" \
    -v "$SRC_DIR":/work/VibeSDR \
    -v "$APT_DIR":/work/VibeServer \
    -v "$KEY_FILE":/tmp/signing-key.asc:ro \
    -e APT_DIR=/work/VibeServer \
    ${PREBUILT_ENV} \
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
echo "==> the repository now offers:"
for d in "$APT_DIR/dists/stable/main"/binary-*; do
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
