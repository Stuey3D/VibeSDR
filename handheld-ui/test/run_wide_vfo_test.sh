#!/usr/bin/env bash
# The R36S's wide VFO (wide_vfo.h) against the app's and the web's (src/utils/wideVfo.ts): the TS answers on a grid of
# cases, replayed through the port natively — and, with DOCKER=1, with the container's aarch64 g++ (the R36S's compiler
# family, image vibe-hh-map:trixie). Fails on any difference in the table, a passband, a label or a fill decision.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
node --no-warnings "$H/test/gen_wide_vfo_golden.ts" > "$T/golden.jsonl"
c++ -std=c++17 -O1 -Wall -Wextra -o "$T/t" "$H/test/wide_vfo_test.cpp"
"$T/t" "$T/golden.jsonl"
if [ "${DOCKER:-0}" = 1 ]; then
  docker run --rm -i --platform linux/arm64 -v "$H:/w:ro" "${IMG:-vibe-hh-map:trixie}" sh -c \
    'cat > /tmp/g.jsonl && g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/t /w/test/wide_vfo_test.cpp && /tmp/t /tmp/g.jsonl' < "$T/golden.jsonl"
fi
