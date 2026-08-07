#!/usr/bin/env bash
# publish-apt.sh — build VibeServer and publish it to the VibeSDR APT repository.
#
#   scripts/publish-apt.sh              # build, index, sign, push
#   scripts/publish-apt.sh --dry-run    # everything except the push
#
# ★★★ RUN THIS ON A DEBIAN BOX (the Pi). It needs dpkg-scanpackages, apt-ftparchive and the
# signing key — none of which exist on the Mac. That is not a limitation worth working around:
# the .deb is built there anyway, and an arm64 package should be built on arm64.
#
# ★★★ WHY THE VERSION IS BUMPED AUTOMATICALLY. `apt install ./x.deb` is a SILENT NO-OP when the
# version already installed matches — it prints "already the newest version" and exits 0. That cost
# an hour on 2026-08-01 when a rebuilt binary was "deployed" and the old one kept running. On a
# public repo the same thing means nobody ever receives an update, and nothing anywhere reports it.
# So every publish gets a new Debian revision, derived from what is already in the pool. The
# upstream version still comes from CMake; this only ever appends `-N`.
set -euo pipefail

REPO_URL="https://github.com/Stuey3D/VibeServer.git"
APT_DIR="${APT_DIR:-$HOME/VibeServer}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SIGN_KEY="packages@vibesdr.net"
ARCH="$(dpkg --print-architecture)"
DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

for t in dpkg-scanpackages apt-ftparchive gpg dpkg-deb cmake git rsync; do
  command -v "$t" >/dev/null || { echo "missing: $t (apt install dpkg-dev apt-utils gnupg)"; exit 1; }
done
gpg --list-secret-keys "$SIGN_KEY" >/dev/null 2>&1 || { echo "no signing key for $SIGN_KEY"; exit 1; }

# ── The artefact repository (clone on first run) ─────────────────────────────
if [ ! -d "$APT_DIR/.git" ]; then
  echo "==> cloning $REPO_URL -> $APT_DIR"
  git clone "$REPO_URL" "$APT_DIR"
else
  git -C "$APT_DIR" pull --ff-only -q || true
fi
POOL="$APT_DIR/pool/main/v/vibeserver"
DIST="$APT_DIR/dists/stable"
mkdir -p "$POOL" "$DIST/main/binary-$ARCH"

# ── Version: upstream from CMake, revision from what is already published ────
UPSTREAM="$(grep -oE 'project\([^)]*VERSION[[:space:]]+[0-9.]+' "$SRC_DIR/vibeserver/CMakeLists.txt" \
            | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
[ -n "$UPSTREAM" ] || { echo "could not read the version from vibeserver/CMakeLists.txt"; exit 1; }
# ★★★ NEVER REUSE A REVISION NUMBER. This used to walk up from 1 looking for a gap — and the
# PRUNE below deletes old revisions, so it kept finding the numbers it had just freed. A publish
# after a prune therefore shipped a DIFFERENT package under a version that had already been
# public: apt on a machine holding the original sees the same version string and refuses to
# upgrade, silently, for ever. (Hit on 2026-08-07: a build went out as 2.0.0-1 for the second
# time, minutes after 2.0.0-4 was pruned.)
# ★★ So the high-water mark comes from three places, and we take the largest: what is in the pool,
#    what the PUBLISHED index still advertises, and a marker that survives pruning. Any one of
#    them alone can be made to forget.
HIGH=0
for f in "$POOL"/vibeserver_"${UPSTREAM}"-*_*.deb; do
  [ -e "$f" ] || continue
  r="$(basename "$f" | sed -nE "s/^vibeserver_${UPSTREAM}-([0-9]+)_.*/\1/p")"
  [ -n "$r" ] && [ "$r" -gt "$HIGH" ] && HIGH="$r"
done
PKGS="$DIST/main/binary-$ARCH/Packages"
if [ -f "$PKGS" ]; then
  r="$(grep -oE "^Version: ${UPSTREAM}-[0-9]+" "$PKGS" | grep -oE '[0-9]+$' | sort -n | tail -1)"
  [ -n "$r" ] && [ "$r" -gt "$HIGH" ] && HIGH="$r"
fi
MARK="$APT_DIR/.highest-revision"
if [ -f "$MARK" ]; then
  r="$(tr -cd '0-9' < "$MARK")"
  [ -n "$r" ] && [ "$r" -gt "$HIGH" ] && HIGH="$r"
fi
REV=$((HIGH + 1))
FULLVER="${UPSTREAM}-${REV}"
echo "==> publishing vibeserver $FULLVER ($ARCH)"

# ── Build, INSIDE A DEBIAN BOOKWORM ROOT ─────────────────────────────────────
# ★★★ NOT ON THE HOST. CPack derives Depends: from whatever the build machine links against, so
#     building on this Pi (trixie) stamped the package `libc6 (>= 2.38), libstdc++6 (>= 14)` and
#     it became UNINSTALLABLE on Debian 12 and Ubuntu 22.04 — while nothing in the source needs
#     either, as it is plain C++17 that gcc-12 compiles unchanged.
#
# ★★ AND THE FAILURE WAS WORSE THAN A REFUSAL. apt names the offending libraries, so the obvious
#    response is to upgrade them — which on a distribution not built for them takes the rest of
#    the machine with it. It cost a user their working OpenWebRX before we understood why.
#
# ★ The build root is the OLDEST distribution we intend to support, and that is the whole trick:
#   glibc and libstdc++ are backward compatible, so a binary built against 2.36 runs on 2.41,
#   but never the other way round. Create it once with:
#     sudo debootstrap --arch=arm64 --variant=buildd bookworm "$BUILD_ROOT"
BUILD_ROOT="${VIBESERVER_BUILD_ROOT:-/srv/bookworm}"
if [ ! -x "$BUILD_ROOT/usr/bin/g++" ]; then
  echo "!! no build root at $BUILD_ROOT — see the comment above; refusing to build against"
  echo "   this machine's libraries, which is what shipped an uninstallable package before."
  exit 1
fi
sudo mkdir -p "$BUILD_ROOT/build"
sudo cp /etc/resolv.conf "$BUILD_ROOT/etc/resolv.conf"
for d in proc sys dev dev/pts; do
  sudo mountpoint -q "$BUILD_ROOT/$d" || sudo mount --bind "/$d" "$BUILD_ROOT/$d"
done
sudo rsync -a --delete --exclude build --exclude .git "$SRC_DIR/" "$BUILD_ROOT/build/VibeSDR/"
sudo chroot "$BUILD_ROOT" /bin/bash -c "
  cd /build/VibeSDR/vibeserver
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVIBESERVER_DEB_REV=$REV \
        -DVIBESERVER_STRICT_RADIOS=ON >/dev/null
  cmake --build build -j$(nproc) | tail -1
  cd build && cpack >/dev/null"
DEB="$(ls -t "$BUILD_ROOT/build/VibeSDR/vibeserver/build"/vibeserver_*.deb | head -1)"

# ★★ ASSERT THE BASELINE, every publish. The point of the build root is the Depends: line it
#    produces, so a silent regression to a newer libc is the one failure that must never ship.
BAD="$(dpkg-deb -f "$DEB" Depends | tr ',' '\n' | grep -E 'libc6 \(>= 2\.(3[7-9]|[4-9])|libstdc\+\+6 \(>= 1[3-9]' || true)"
if [ -n "$BAD" ]; then
  echo "!! this package demands a newer runtime than the build root should produce:"
  echo "$BAD"
  echo "   it would be uninstallable on the systems the build root exists to support."
  exit 1
fi
echo "==> built $(basename "$DEB")"

# ★ Sanity: the package must actually carry the version we think it does, or the pool fills with
#   files whose names and contents disagree and apt believes the FILENAME.
PKGVER="$(dpkg-deb -f "$DEB" Version)"
[ "$PKGVER" = "$FULLVER" ] || { echo "version mismatch: package says $PKGVER, expected $FULLVER"; exit 1; }
cp "$DEB" "$POOL/"
# ★ Record the high-water mark BEFORE pruning, so a pruned revision can never be handed out again.
echo "$REV" > "$APT_DIR/.highest-revision"

# ── Prune ────────────────────────────────────────────────────────────────────
# ★★ THE POOL IS NOT AN ARCHIVE. Every publish adds a .deb and nothing ever removed one, so the
#    repository — and the clone on a build box with a 32 GB card (Stuart) — grows forever to hold
#    versions nobody will install: apt only ever offers the newest. Keeping the last few is enough
#    to roll back by hand if a release turns out bad, which is the only reason to keep any.
KEEP=3
mapfile -t OLD < <(ls -t "$POOL"/vibeserver_*_"$ARCH".deb 2>/dev/null | tail -n +$((KEEP+1)))
if [ ${#OLD[@]} -gt 0 ]; then
  printf '==> pruning %d old package(s), keeping the newest %d\n' "${#OLD[@]}" "$KEEP"
  rm -f "${OLD[@]}"
fi

# ── Index ────────────────────────────────────────────────────────────────────
# ★ Paths in Packages must be RELATIVE TO THE REPOSITORY ROOT, so scanpackages runs from there.
( cd "$APT_DIR" && dpkg-scanpackages --arch "$ARCH" pool /dev/null \
    > "dists/stable/main/binary-$ARCH/Packages" )
gzip -9kf "$DIST/main/binary-$ARCH/Packages"
echo "==> indexed $(grep -c '^Package:' "$DIST/main/binary-$ARCH/Packages") package(s)"

cat > "$APT_DIR/aptftp.conf" <<EOF
APT::FTPArchive::Release::Origin "VibeSDR";
APT::FTPArchive::Release::Label "VibeSDR";
APT::FTPArchive::Release::Suite "stable";
APT::FTPArchive::Release::Codename "stable";
APT::FTPArchive::Release::Architectures "$ARCH";
APT::FTPArchive::Release::Components "main";
APT::FTPArchive::Release::Description "VibeServer — turn an SDR into a network receiver";
EOF
( cd "$APT_DIR" && apt-ftparchive -c aptftp.conf release dists/stable > dists/stable/Release )

# ── Sign ─────────────────────────────────────────────────────────────────────
# InRelease (inline signature) is what modern apt prefers; Release.gpg is kept for older clients.
rm -f "$DIST/InRelease" "$DIST/Release.gpg"
gpg --default-key "$SIGN_KEY" --batch --yes --clearsign -o "$DIST/InRelease" "$DIST/Release"
gpg --default-key "$SIGN_KEY" --batch --yes -abs -o "$DIST/Release.gpg" "$DIST/Release"
gpg --armor --export "$SIGN_KEY" > "$APT_DIR/KEY.gpg"
echo "==> signed with $(gpg --list-keys --keyid-format=long "$SIGN_KEY" | sed -n '2p' | tr -s ' ')"

# ── Landing page ─────────────────────────────────────────────────────────────
sed -e "s/__VERSION__/$FULLVER/g" -e "s/__ARCH__/$ARCH/g" \
    "$SRC_DIR/scripts/apt-index.html" > "$APT_DIR/index.html"
touch "$APT_DIR/.nojekyll"     # Pages would otherwise skip files starting with an underscore

# ── Commit locally — the Mac does the pushing ────────────────────────────────
# ★★★ THIS MACHINE HOLDS NO GITHUB CREDENTIAL, deliberately. Stuart: "I may clean down the Pi at
#     any time and lose everything on it." A build box that can publish is a build box whose
#     wipe-and-reinstall has to be planned around; one that only commits is disposable. So the
#     work happens here (arm64, Debian tooling) and `scripts/publish-apt-push.sh` on the Mac
#     fetches this commit over SSH and pushes it — the Mac already has the credential, the key
#     backup and Time Machine, and its clone is the copy of the pool that must survive.
cd "$APT_DIR"
git add -A
git -c user.name="VibeSDR release" -c user.email="packages@vibesdr.net" \
    commit -q -m "vibeserver $FULLVER ($ARCH)" || { echo "nothing to commit"; exit 0; }
echo "==> committed $FULLVER locally ($APT_DIR)"
[ "$DRY_RUN" = "1" ] && { echo "==> dry run: stopping here"; exit 0; }
echo "==> now run on the Mac:  scripts/publish-apt-push.sh"
echo "    (it pushes, then reclaims this clone — nothing here needs to survive)"

