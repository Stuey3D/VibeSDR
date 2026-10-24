#!/usr/bin/env bash
# build-adsb-data.sh — the reproducible build of VibeServer's ADS-B aircraft data (V12).
#
# ★★ WHAT IT DOES, in order — and it STOPS at the first thing that is not as the research cleared it
#    (docs/v12/ADSB-DATA-RESEARCH.md):
#    1. Fetches the two CC0 / ODC-By sources ONCE into a cache (a second run re-uses them; --fresh fetches again),
#       politely: one GET each, conditional on the cached ETag.
#         VRS standing data   https://codeload.github.com/vradarserver/standing-data/tar.gz/refs/heads/main  (CC0 1.0)
#         Mictronics database https://raw.githubusercontent.com/Mictronics/aircraft-database/main/indexedDB.zip (ODC-By 1.0)
#    2. Builds adsb-data-tool (the SAME builder the server's weekly update runs — vibe_adsb_data_build.h), which
#       REFUSES a source whose licence file no longer matches the pin we read (fails loudly, exit 1).
#    3. Writes the starter compiled into every server (android/app/src/main/cpp/vibe_adsb_starter.h — CC0 tables only:
#       airlines, model types, code blocks, countries), the full tables into the cache for a look, and
#       vibeserver/adsb-data/ (CREDITS.md + both licence texts, installed with the package).
#    4. Runs test-adsb-data against the real downloads.
# ★ Resource rules: nice -n 19, one compile at a time.
# Usage: scripts/build-adsb-data.sh [--fresh] [cache dir]
set -euo pipefail
cd "$(dirname "$0")/.."
FRESH=0
if [ "${1:-}" = "--fresh" ]; then FRESH=1; shift; fi
CACHE="${1:-${TMPDIR:-/tmp}/vibesdr-adsb-data}"
mkdir -p "$CACHE/raw" "$CACHE/out"
VRS_URL=https://codeload.github.com/vradarserver/standing-data/tar.gz/refs/heads/main
MIC_URL=https://raw.githubusercontent.com/Mictronics/aircraft-database/main/indexedDB.zip
UA="VibeSDR-build (+https://vibesdr.net)"

fetch() {   # url dest
  local url="$1" dest="$2"
  if [ -s "$dest" ] && [ $FRESH -eq 0 ]; then echo "cached: $dest"; return; fi
  local etag=()
  [ -s "$dest.etag" ] && [ -s "$dest" ] && etag=(--etag-compare "$dest.etag")
  curl -fsSL --retry 2 --connect-timeout 30 -A "$UA" "${etag[@]}" --etag-save "$dest.etag.new" -o "$dest.new" "$url"
  if [ -s "$dest.new" ]; then mv "$dest.new" "$dest"; mv "$dest.etag.new" "$dest.etag"; echo "fetched: $dest"; else rm -f "$dest.new" "$dest.etag.new"; echo "unchanged: $dest"; fi
}
fetch "$VRS_URL" "$CACHE/raw/standing-data-main.tar.gz"
fetch "$MIC_URL" "$CACHE/raw/indexedDB.zip"

BIN="$CACHE/adsb-data-tool"
nice -n 19 c++ -std=c++17 -O2 -I android/app/src/main/cpp vibeserver/adsb-data-tool.cpp -o "$BIN"
nice -n 19 "$BIN" build "$CACHE/raw/standing-data-main.tar.gz" "$CACHE/raw/indexedDB.zip" "$CACHE/out"
nice -n 19 "$BIN" starter "$CACHE/raw/standing-data-main.tar.gz" android/app/src/main/cpp/vibe_adsb_starter.h

# the licence texts, out of the archives themselves (byte-for-byte what the pins cleared)
TMPX="$(mktemp -d)"
tar -xzf "$CACHE/raw/standing-data-main.tar.gz" -C "$TMPX" standing-data-main/LICENSE
cp "$TMPX/standing-data-main/LICENSE" vibeserver/adsb-data/LICENSE-VRS-standing-data-CC0-1.0.txt
unzip -p "$CACHE/raw/indexedDB.zip" LICENSE > vibeserver/adsb-data/LICENSE-Mictronics-ODC-By-1.0.txt
# ★★ `|| true`: head closes the pipe early and gzip dies of SIGPIPE, which pipefail reports as a FAILURE (the
#    librtlsdr lesson in MEMORY: grep -q under pipefail)
COMMIT="$( { gzip -dc "$CACHE/raw/standing-data-main.tar.gz" 2>/dev/null || true; } | head -c 1024 | tr -d '\0' | sed -n 's/.*comment=\([0-9a-f]*\).*/\1/p' | head -1 || true)"
DBV="$(unzip -p "$CACHE/raw/indexedDB.zip" dbversion.json | tr -dc '0-9')"
rm -rf "$TMPX"
DATE="$(date -u +%Y-%m-%d)"
cat > vibeserver/adsb-data/CREDITS.md <<CRED
# ADS-B aircraft data — sources, licences, credits

VibeServer shows what an aircraft is (registration, type, operator, country, route) from these sources. Each
server downloads them itself, weekly; the package carries only the CC0 starter. Retrieved for this build: $DATE.

| source | licence | URL | used for |
|---|---|---|---|
| Virtual Radar Server standing data — Andrew Whewell and the VRS users who submit it (commit ${COMMIT:-unknown}) | CC0 1.0 Universal ([text](LICENSE-VRS-standing-data-CC0-1.0.txt)) | https://github.com/vradarserver/standing-data | routes, airports, airlines, aircraft types, address blocks (military, territories), country names; the CC0 starter compiled into the server |
| Mictronics aircraft database (dbversion ${DBV:-unknown}) | Open Data Commons Attribution License 1.0 ([text](LICENSE-Mictronics-ODC-By-1.0.txt), https://opendatacommons.org/licenses/by/1-0/) | https://github.com/Mictronics/aircraft-database | registrations, type codes and descriptions, operators, military flag |
| ICAO Annex 10 Vol. III, Table 9-1 — 24-bit address allocations | facts; our own table (vibe_adsb_blocks.h) | — | country of registry |

**Notice (ODC-By 1.0 §4.3):** Contains information from Mictronics aircraft database, which is made available under
the ODC Attribution License.

Not used, by decision (docs/v12/ADSB-DATA-RESEARCH.md): OpenSky, adsbdb, ADS-B Exchange / tar1090-db, UK CAA G-INFO,
OpenFlights (ODbL), Wikipedia tables (CC BY-SA). Operator logos and silhouettes are never shipped or downloaded —
they are trademarks; an owner may install their own on the admin page. Aircraft photos come from Planespotters.net,
fetched by the listener's own browser or app, credited to the photographer and linked as their terms require.
CRED
echo "credits: vibeserver/adsb-data/CREDITS.md"

nice -n 19 g++ -std=c++17 -O2 -I vibeserver -I android/app/src/main/cpp vibeserver/test-adsb-data.cpp -o "$CACHE/test-adsb-data" -lpthread
VIBE_ADSB_RAW="$CACHE/raw" nice -n 19 "$CACHE/test-adsb-data"
ls -la "$CACHE/out" android/app/src/main/cpp/vibe_adsb_starter.h
