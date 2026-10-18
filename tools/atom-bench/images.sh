#!/bin/bash
# images.sh — the helper images the Atom bench uses, built ON the Lenovo from the bookworm build images:
#   atom-perf:<arch>  = vibeserver-build:bookworm-<arch> + linux-perf + valgrind   (server / bench under perf)
#   atom-client:amd64 = atom-perf:amd64 + python3-websockets                      (rtl_tcp replay + listener)
# Layout on the build host: ~/atom-src (the tree), ~/atomwork/{b,tools,iq,wrap,perf,results,pgo*}.
# ★ Remove them afterwards (docker rmi atom-perf:amd64 atom-perf:i386 atom-client:amd64); keep vibeserver-build:*.
set -euo pipefail
mkdir -p ~/atomwork/img && cd ~/atomwork/img
for A in amd64 i386; do
  printf "FROM vibeserver-build:bookworm-$A\nRUN apt-get update && apt-get install -y --no-install-recommends linux-perf valgrind && rm -rf /var/lib/apt/lists/*\n" > Dockerfile.$A
  nice -n 19 docker build -q --platform linux/${A/i386/386} -f Dockerfile.$A -t atom-perf:$A .
done
printf "FROM atom-perf:amd64\nRUN apt-get update && apt-get install -y --no-install-recommends python3-websockets && rm -rf /var/lib/apt/lists/*\n" > Dockerfile.client
nice -n 19 docker build -q -f Dockerfile.client -t atom-client:amd64 .
# the synthetic RTL captures: build any tree once with build.sh (it builds gen-iq), then
#   docker run --rm -v ~/atomwork/b/<build>:/b:ro -v ~/atomwork/iq:/iq vibeserver-build:bookworm-amd64 \
#     sh -c '/b/gen-iq 2400000 3 /iq/wfm2400.u8 && /b/gen-iq 1200000 3 /iq/wfm1200.u8 && /b/gen-iq 2048000 3 /iq/wfm2048.u8'
