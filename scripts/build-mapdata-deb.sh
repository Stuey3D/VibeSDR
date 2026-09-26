#!/usr/bin/env bash
# build-mapdata-deb.sh — the OPTIONAL map detail pack, as its own Debian package.
#
#   scripts/build-mapdata-deb.sh                     # -> vibeserver/build/vibeserver-mapdata-detail_<v>_all.deb
#   scripts/build-mapdata-deb.sh --out /tmp          # somewhere else
#   scripts/build-mapdata-deb.sh --pack basic        # the pack vibeserver ITSELF ships (a test, not a release)
#   scripts/build-mapdata-deb.sh --rev 2             # a rebuild of the same dataset (see the revision note)
#
# ★★★ WHY A SEPARATE PACKAGE AND NOT PART OF vibeserver. The detail pack is ~221 MB on disk (82 MB
#     compressed) against the server's own couple of MB. Inside the main package it would make every
#     routine bug-fix release a 82 MB download for every receiver on the estate, including the Pi 2
#     on a 4 GB card that has no room for the data at all — and it would make the map's data version
#     and the SERVER's version the same number, so regenerating a coastline would force a server
#     release. Separate package, separate version, `Recommends:` from vibeserver
#     (vibeserver/CMakeLists.txt): apt installs it by default, `--no-install-recommends` and a full
#     disk both decline it, and nothing about that refuses the server.
#
# ★★★ ARCHITECTURE: all. It is GeoJSON and PNG. One package serves arm64, armhf and amd64, which is
#     also the only reason 82 MB in the pool is tolerable — three copies would not be.
#
# ★★ IT INSTALLS INTO THE SAME DIRECTORY AS THE BASIC PACK (/usr/lib/vibeserver/mapdata) and the two
#    file sets are DISJOINT by construction — they come from the two `packs` lists in index.json,
#    which never overlap. Two packages owning one file is a dpkg conflict, and it would appear only
#    on the machines that installed both.
# ★★ index.json ITSELF BELONGS TO THE BASIC PACK, NOT HERE. It is the manifest of both packs and it
#    ships with the server, so a machine without this package still knows what it is missing. If this
#    package carried its own copy they would fight, and the loser would be whichever installed last.
#
# ★ RUN IT ON A DEBIAN BOX (or in the bookworm container publish-apt-docker.sh already builds in):
#   it needs dpkg-deb. Nothing here compiles, so there is no cross-build problem to have.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/assets/mapdata/v1"
OUT="$ROOT/vibeserver/build"
PACK="detail"
REV="1"
while [ $# -gt 0 ]; do
  case "$1" in
    --out)  shift; OUT="$1" ;;
    --pack) shift; PACK="$1" ;;
    --rev)  shift; REV="$1" ;;
    *) echo "usage: $0 [--out DIR] [--pack detail|basic] [--rev N]"; exit 1 ;;
  esac
  shift
done

command -v dpkg-deb >/dev/null || { echo "!! dpkg-deb not found — run this on a Debian box (or in the bookworm build container)"; exit 1; }
# ★ bookworm-SLIM has neither python3 nor jq; the build image installs python3-minimal for exactly
#   this script (vibeserver/linux/Dockerfile.build). Say which, rather than dying on a missing binary.
command -v python3 >/dev/null || { echo "!! python3 not found — apt-get install python3-minimal (the build image already does)"; exit 1; }
[ -f "$SRC/index.json" ] || { echo "!! $SRC/index.json missing — run: node scripts/gen-map-data.mjs"; exit 1; }

# ★★★ THE FILE LIST COMES FROM index.json, NEVER FROM A GLOB. A glob would sweep up whatever else
#     happens to be in that directory — a half-written shard from an interrupted generator run, a
#     file that belongs to the BASIC pack and would then be owned by two packages. The generator's
#     own manifest is the only description of the dataset that cannot drift from it.
# ★ python3 rather than jq: jq is not installed on a plain bookworm image and python3 is.
read -r VERSION_DATE PACK_N <<EOF
$(python3 - "$SRC/index.json" "$PACK" <<'PY'
import json, sys, re
idx = json.load(open(sys.argv[1]))
pack = idx["packs"][sys.argv[2]]
gen = re.sub(r"[^0-9]", "", str(idx.get("generated", "")))[:8] or "00000000"
print(f'{idx.get("version", 1)}.{gen}', len(pack["files"]))
PY
)
EOF
[ -n "$VERSION_DATE" ] && [ "${PACK_N:-0}" -gt 0 ] || { echo "!! could not read pack '$PACK' from index.json"; exit 1; }
VER="$VERSION_DATE-$REV"
PKG="vibeserver-mapdata-$PACK"

# ★★★ A NEW DATASET MUST MEAN A NEW VERSION, or apt says "already the newest version" and nobody
#     ever receives it — the same silent no-op documented at length in vibeserver/CMakeLists.txt.
#     The version is <index version>.<generation date>, so regenerating the data on a later day
#     bumps it by itself. Regenerating TWICE IN ONE DAY does not: pass --rev 2 for that.
echo "==> $PKG $VER — $PACK_N files from $SRC"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
DATA="$STAGE/usr/lib/vibeserver/mapdata"
mkdir -p "$DATA" "$STAGE/DEBIAN" "$STAGE/usr/share/doc/$PKG"

python3 - "$SRC" "$DATA" "$SRC/index.json" "$PACK" <<'PY'
import json, os, shutil, sys
src, dest, index, pack = sys.argv[1:5]
files = json.load(open(index))["packs"][pack]["files"]
missing = [f for f in files if not os.path.isfile(os.path.join(src, f))]
if missing:
    # ★ A DECLARED FILE THAT IS NOT THERE IS A HALF-GENERATED DATASET. Shipping the rest would
    #   produce a package that installs perfectly and is missing a layer — which reads to everyone
    #   as a renderer bug, on the one machine that happens to zoom in there.
    sys.exit("!! declared but absent: %s%s — re-run scripts/gen-map-data.mjs"
             % (", ".join(missing[:5]), " …" if len(missing) > 5 else ""))
for f in files:
    shutil.copy2(os.path.join(src, f), os.path.join(dest, f))
print("    staged %d files, %.1f MB" % (len(files),
      sum(os.path.getsize(os.path.join(dest, f)) for f in files) / 1e6))
PY

# ★★★ THE ATTRIBUTION TRAVELS WITH THE DATA. Part of this dataset is CC BY 4.0 (RESOLVE Ecoregions),
#     which permits redistribution ONLY with attribution — a bundled file without its licence is how
#     a permitted act becomes an infringing one, exactly as noted for cloudflared in the CMake file.
#     The client credits the sources on screen; this is the copy dpkg and any Debian tooling can see.
#  ★ Generated from index.json's own `licences` block, so a new source added by the generator cannot
#    be credited in one place and forgotten in the other.
python3 - "$SRC/index.json" > "$STAGE/usr/share/doc/$PKG/copyright" <<'PY'
import json, sys
idx = json.load(open(sys.argv[1]))
print("Upstream-Name: VibeSDR map data")
print("Source: https://vibesdr.net\n")
print("Files: usr/lib/vibeserver/mapdata/*")
for key, lic in sorted(idx.get("licences", {}).items()):
    print("\n Source: %s" % key)
    print("  Layers: %s" % ", ".join(lic.get("layers", [])))
    print("  Licence: %s" % lic.get("licence", "see upstream"))
    if lic.get("attribution"): print("  Attribution: %s" % lic["attribution"])
    if lic.get("url"): print("  URL: %s" % lic["url"])
PY

INSTALLED_KB="$(du -sk "$STAGE/usr" | cut -f1)"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VER
Architecture: all
Maintainer: Stuart Carr <https://github.com/Stuey3D/VibeSDR/issues>
Section: hamradio
Priority: optional
Homepage: https://vibesdr.net
Installed-Size: $INSTALLED_KB
Depends: vibeserver
Description: VibeServer map data — the close-in detail pack
 The high-detail half of the map VibeServer's web client draws for itself:
 coastline and lake shards, second-level boundaries, minor roads, small
 places, runways and the shaded-relief tiles, for zoom levels 8 and in.
 .
 Entirely optional. VibeServer ships the basic pack (the whole world down to
 zoom 7) inside its own package and works without this one; a receiver short
 of disk space can leave it out, or remove it, and lose only close-in detail.
EOF

# ★★ Depends: vibeserver, and nothing shy of that. The files are useless on their own — the only
#    thing that reads them is the server, and the directory they live in belongs to it. Without the
#    dependency, `apt remove vibeserver` would leave 221 MB of orphaned GeoJSON on a Pi.
mkdir -p "$OUT"
DEB="$OUT/${PKG}_${VER}_all.deb"
# ★ xz at -6: the pack is JSON, so this is the difference between 82 MB and ~110 MB in the pool, and
#   the compression runs once per RELEASE while the download happens once per receiver.
dpkg-deb --build -Zxz -z6 --root-owner-group "$STAGE" "$DEB" >/dev/null

# ★★★ VERIFY THE ARTEFACT, NOT THE COMMAND. "A Mac-only compile is not a build": the same rule
#     applies to a package, and a staged tree that silently copied nothing produces a perfectly
#     valid 4 KB .deb. Count the files INSIDE the finished package and refuse to call it a release
#     if the count disagrees with the manifest.
GOT="$(dpkg-deb -c "$DEB" | grep -c 'usr/lib/vibeserver/mapdata/.*[^/]$' || true)"
[ "$GOT" = "$PACK_N" ] || { echo "!! $DEB carries $GOT map files, index.json declares $PACK_N"; exit 1; }
echo "==> $DEB"
echo "    $GOT files, $(du -h "$DEB" | cut -f1) compressed, version $VER"
echo
echo "To publish: copy it into the apt pool beside the server packages and re-index —"
echo "  cp \"$DEB\" \$APT_DIR/pool/main/v/vibeserver/"
echo "  ( cd \$APT_DIR && dpkg-scanpackages --arch <each arch> pool /dev/null > dists/stable/main/binary-<arch>/Packages )"
echo "then re-sign Release exactly as scripts/publish-apt.sh does. An Architecture: all package is"
echo "included in EVERY architecture's index, so every arch must be re-scanned, not just one."
