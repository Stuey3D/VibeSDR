#!/bin/sh
# ★★★ THE PAGE'S BAND PLAN IS GENERATED, NEVER TYPED. Run before deploying the directory:
#       scripts/gen-bandplan.sh          # regenerate
#       scripts/gen-bandplan.sh --check  # fail if the checked-in JSON has drifted from the header
#  ★ A second band table is how the landing page and the directory would come to disagree about
#    what a server offers — see briefs/BRIEF-directory-network-dial.md, step 2.
set -e
cd "$(dirname "$0")/.."
OUT=directory/public/bandplan.json
TMP=$(mktemp -d)
clang++ -std=c++17 -O0 -o "$TMP/genbp" scripts/gen-bandplan.cpp
"$TMP/genbp" > "$TMP/bandplan.json"
if [ "$1" = "--check" ]; then
  if ! diff -q "$TMP/bandplan.json" "$OUT" >/dev/null 2>&1; then
    echo "bandplan.json is STALE — vibe_bands.h has changed. Run scripts/gen-bandplan.sh" >&2
    diff "$OUT" "$TMP/bandplan.json" | head -20 >&2 || true
    rm -rf "$TMP"; exit 1
  fi
  echo "bandplan.json matches vibe_bands.h"
else
  cp "$TMP/bandplan.json" "$OUT"
  echo "wrote $OUT ($(grep -c '"id"' "$OUT") bands across 3 regions)"
fi
rm -rf "$TMP"
