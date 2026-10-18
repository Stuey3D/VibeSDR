#!/bin/bash
# Part of tools/atom-bench (docs/ATOM-PERFORMANCE-2026-10-18.md). Runs on the Lenovo from ~/atomwork; see images.sh.
# pgo-train.sh BUILD PLATFORM — run an instrumented (-fprofile-generate=/pgo/BUILD) server through the WFM
# sandbox at 2.4 and 1.2 MS/s with a listener, stop it with SIGTERM (so the counters are written), then run
# the instrumented vibeserver-bench (NFM/AM/SSB/DAB/decoders). Counters land in ~/atomwork/pgo/BUILD.
set -uo pipefail
bin=$1 pg=$2 p=$3; img=atom-perf:${p/386/i386}; W=$HOME/atomwork
for rate in 2400000 1200000; do
  docker rm -f atomcli atomsrv >/dev/null 2>&1
  docker run -d --name atomcli --network none --cpuset-cpus 0,1 -v $W/tools:/tools:ro -v $W/iq:/iq:ro atom-client:amd64 \
    python3 -u /tools/rtltcp_replay.py 1234 2400000=/iq/wfm2400.u8 1200000=/iq/wfm1200.u8 2048000=/iq/wfm2048.u8 >/dev/null
  sleep 1
  docker run -d --name atomsrv --platform linux/$p --network container:atomcli --cpuset-cpus 3,7 \
    -e VIBESERVER_DATA_DIR=/tmp/vd -e VIBESERVER_RUNTIME_DIR=/tmp/vr -e VIBESERVER_CONFIG=/tmp/vc.conf \
    -v $bin:/b/vs:ro -v $pg:/pgo $img bash -c "mkdir -p /tmp/vd /tmp/vr; touch /tmp/vc.conf; \
    exec /b/vs/vibeserver --tcp 127.0.0.1:1234 --rate $rate --freq 100000000 --mode wfm --fps 20 --port 48190 \
       --no-supervise --idle-grace 600 --name sandbox" >/dev/null
  sleep 3
  docker exec atomcli python3 /tools/listener.py http://127.0.0.1:48190 100200000 10 40 1024 20
  docker stop -t 60 atomsrv >/dev/null; echo "server exit: $(docker inspect -f "{{.State.ExitCode}}" atomsrv)"
  docker logs atomsrv 2>&1 | tail -2
  docker rm -f atomcli atomsrv >/dev/null 2>&1
done
docker run --rm --platform linux/$p --cpuset-cpus 1,2 -e VIBE_BENCH_OFFLINE=1 -v $bin:/b/vs:ro -v $pg:/pgo \
  -v $HOME/atom-src/vibeserver/bench-clip/dab-bench-clean.vbu8:/clip.vbu8:ro $img /b/vs/vibeserver-bench 2 /clip.vbu8 >/dev/null 2>&1
echo "bench exit $?"; find $pg -name "*.gcda" | wc -l
