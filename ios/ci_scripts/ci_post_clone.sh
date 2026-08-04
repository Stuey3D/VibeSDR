#!/bin/sh
# Xcode Cloud post-clone step for VibeSDR (Expo / React Native bare workflow).
#
# Installs Node + JS deps + CocoaPods after Xcode Cloud clones the repo, so the
# archive can bundle the JS and link the pods.
#
# WHY XCODE CLOUD: our local build machine is on a BETA macOS (27.x); App Store
# Connect rejects binaries built on a beta OS (ITMS-90111), even with stable
# Xcode 26.6. Xcode Cloud builds on Apple's STABLE macOS images. Keep using this
# until macOS 27 ships stable.
set -e

# Run natively on arm64. Xcode Cloud runners are Apple Silicon, but the script
# can land in an x86_64/Rosetta shell (Intel Homebrew at /usr/local), which makes
# CocoaPods refuse ("Do not use pod install from inside Rosetta2"). Re-exec once
# under arm64 so brew/node/pods are all native and consistent.
if [ "$(uname -m)" != "arm64" ] && [ -x /usr/bin/arch ]; then
  echo "--- re-exec under arm64 (was $(uname -m)) ---"
  exec /usr/bin/arch -arm64 /bin/sh "$0" "$@"
fi

# Prefer arm64 Homebrew.
if [ -x /opt/homebrew/bin/brew ]; then
  eval "$(/opt/homebrew/bin/brew shellenv)"
fi

echo "--- ci_post_clone: ensuring Node is available ($(uname -m)) ---"
if ! command -v node >/dev/null 2>&1; then
  echo "Node not found; installing via Homebrew…"
  brew install node || brew link --overwrite node || true
else
  echo "Node already present."
fi
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

echo "--- node/npm versions ---"
node --version
npm --version

# JS dependencies.
#
# THE FALLBACK MUST START FROM A CLEAN TREE. It was `npm ci || npm install`, and that is a
# trap: npm ci runs `postinstall` (patch-package) as its LAST step, so a ci that dies late
# leaves node_modules ALREADY PATCHED. The `npm install` fallback then runs patch-package a
# second time over a patched tree — and our expo-modules-jsi patch CREATES a file, so
# re-applying it fails ("Failed to apply patch"), which fails the postinstall, which fails
# the whole script under `set -e`. The fallback could never have succeeded.
#
# So the retry deletes node_modules first. It is also worth SAYING that ci failed and why,
# because the fallback used to hide it — the build only ever reported the second, confusing
# error, never the first, real one.
# ★ REVERTED 2026-08-04: --legacy-peer-deps was added for the react-native-tvos alias, which has
#   now been reverted (it broke the shipping iPhone app). Plain npm ci is the known-good path.
cd "$CI_PRIMARY_REPOSITORY_PATH"
echo "--- installing JS dependencies (npm ci) ---"
if ! npm ci; then
  echo "--- npm ci FAILED (see above). Retrying from a clean node_modules… ---"
  rm -rf node_modules
  npm install
fi

# CocoaPods. RN 0.86 fetches a prebuilt "reactnative-dependencies" tarball from
# Maven Central during pod install, which can transiently reset the connection.
# Retry a few times so a flaky download doesn't fail the whole build.
cd "$CI_PRIMARY_REPOSITORY_PATH/ios"
echo "--- installing CocoaPods (with retries) ---"
n=0
until [ "$n" -ge 4 ]; do
  if pod install; then
    echo "--- pod install succeeded ---"
    break
  fi
  n=$((n + 1))
  echo "pod install failed (attempt $n/4) — retrying in 15s…"
  sleep 15
done
if [ "$n" -ge 4 ]; then
  echo "pod install failed after 4 attempts"
  exit 1
fi

echo "--- ci_post_clone: done ---"
