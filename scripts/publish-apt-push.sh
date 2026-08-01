#!/usr/bin/env bash
# publish-apt-push.sh — fetch what the build box published and push it to GitHub. RUN ON THE MAC.
#
#   scripts/publish-apt.sh          # on the Pi:  build, index, sign, commit
#   scripts/publish-apt-push.sh     # on the Mac: fetch that commit, push it
#
# ★★★ THE BUILD BOX HOLDS NO CREDENTIAL. Stuart: "I may clean down the Pi at any time and lose
# everything on it." So it builds (arm64, Debian tooling, the signing key) and commits, and this
# machine — which has the GitHub credential, the key backup, Time Machine and iCloud — owns the
# clone that has to survive. The pool of published .deb files IS state: once a version is public,
# people's machines expect it to stay at that URL, and a wiped build box must not be able to take
# it with it.
set -euo pipefail

PI="${PI:-stuey3d@192.168.86.88}"
PI_DIR="${PI_DIR:-/home/stuey3d/vibesdr-apt}"
APT_DIR="${APT_DIR:-$HOME/vibesdr-apt}"
REPO_URL="https://github.com/Stuey3D/vibesdr-apt.git"

if [ ! -d "$APT_DIR/.git" ]; then
  echo "==> cloning $REPO_URL -> $APT_DIR"
  git clone "$REPO_URL" "$APT_DIR"
fi
cd "$APT_DIR"

git remote get-url pi >/dev/null 2>&1 || git remote add pi "ssh://$PI$PI_DIR"
git remote set-url pi "ssh://$PI$PI_DIR"

echo "==> fetching from the build box"
git fetch -q pi
# ★ FAST-FORWARD ONLY. If these have diverged, something published from two places and the pool
#   may now disagree with the indexes — resolve it by hand rather than letting a merge invent a
#   repository state that was never signed.
BEFORE="$(git rev-parse HEAD)"
git merge --ff-only pi/main -q || { echo "the build box and this clone have diverged — fix by hand"; exit 1; }
AFTER="$(git rev-parse HEAD)"

if [ "$BEFORE" = "$AFTER" ]; then echo "==> nothing new to publish"; exit 0; fi
git --no-pager log --oneline "$BEFORE..$AFTER" | sed 's/^/    /'

git push -q origin HEAD
echo "==> published: https://stuey3d.github.io/vibesdr-apt/"
echo "    (GitHub Pages takes a few seconds to rebuild before apt sees it)"

# ★★ RECLAIM THE BUILD BOX'S CLONE once it is safely on GitHub and here. The Pi runs from a 32 GB
#    card and this is the one directory that would grow with every release for no reason: it is a
#    staging area, not a copy worth keeping, and publish-apt.sh re-clones it in seconds. Pass
#    --keep to leave it alone (useful when debugging a publish).
if [ "${1:-}" != "--keep" ]; then
  ssh -o BatchMode=yes "$PI" "rm -rf '$PI_DIR'" 2>/dev/null \
    && echo "==> reclaimed $PI:$PI_DIR" || echo "==> could not reclaim $PI:$PI_DIR (harmless)"
fi
