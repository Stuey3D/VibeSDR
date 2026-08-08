#!/usr/bin/env bash
# publish-apt-docker.sh — build and publish VibeServer from the Mac, in a bookworm container.
#
#   scripts/publish-apt-docker.sh            # build, index, sign, commit, push
#   scripts/publish-apt-docker.sh --dry-run  # everything except the push
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
IMAGE="vibeserver-build:bookworm"
DRY_RUN=""
if [ "${1:-}" = "--dry-run" ]; then DRY_RUN="--dry-run"; fi

command -v docker >/dev/null || { echo "docker not found (brew install colima docker; colima start)"; exit 1; }
docker info >/dev/null 2>&1 || { echo "no docker daemon — run: colima start"; exit 1; }

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

echo "==> build image"
docker build --platform linux/arm64 -q \
  -f "$SRC_DIR/vibeserver/linux/Dockerfile.build" -t "$IMAGE" "$SRC_DIR/vibeserver/linux" >/dev/null

# ★★ --dry-run still BUILDS and SIGNS and commits locally; it only withholds the push. That is
#    what makes it worth running: the failures worth catching are in the build and the index.
echo "==> build + index + sign (in bookworm)"
docker run --rm --platform linux/arm64 \
  -v "$SRC_DIR":/work/VibeSDR \
  -v "$APT_DIR":/work/VibeServer \
  -v "$KEY_FILE":/tmp/signing-key.asc:ro \
  -e APT_DIR=/work/VibeServer \
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

if [ -n "$DRY_RUN" ]; then
  echo "==> dry run: nothing pushed. $APT_DIR holds the result."
  exit 0
fi

echo "==> pushing"
git -C "$APT_DIR" push
echo "==> published: https://apt.vibesdr.net/"
echo "    (GitHub Pages takes a few seconds to rebuild before apt sees it)"
