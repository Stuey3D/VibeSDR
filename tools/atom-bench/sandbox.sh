#!/bin/bash
# Part of tools/atom-bench (docs/ATOM-PERFORMANCE-2026-10-18.md). Runs on the Lenovo from ~/atomwork; see images.sh.
# sandbox.sh BUILD PLATFORM(amd64|386) RATE CPUS [WARM] [MEASURE] [FPS] [PERF:none|stat|record]
# "N450-equivalent" sandbox: the SERVER is pinned to one physical core's two SMT siblings (3,7) and throttled by a
# CFS quota (--cpus). The rtl_tcp replay + the headless listener run in a separate container on other cores
# (0-1), sharing its network namespace, so their CPU is NOT charged to the quota.
set -uo pipefail
b=$1 p=$2 rate=$3 q=$4 warm=${5:-12} meas=${6:-30} fps=${7:-20} perf=${8:-none}
img=atom-perf:${p/386/i386}
W=$HOME/atomwork; tag=$b-$rate-$q-$$
docker rm -f atomcli atomsrv >/dev/null 2>&1
docker run -d --name atomcli --network none --cpuset-cpus 0,1 -v $W/tools:/tools:ro -v $W/iq:/iq:ro atom-client:amd64 \
  python3 -u /tools/rtltcp_replay.py 1234 2400000=/iq/wfm2400.u8 1200000=/iq/wfm1200.u8 2048000=/iq/wfm2048.u8 >/dev/null
sleep 1
pre=""; [ "$perf" != none ] && pre="--privileged"; [ -n "${PRIV:-}" ] && pre="--privileged"; [ -n "${SECOFF:-}" ] && pre="--security-opt seccomp=unconfined"
docker run -d --name atomsrv $pre --platform linux/$p --network container:atomcli --cpuset-cpus 3,7 --cpu-period 20000 --cpu-quota $(python3 -c "print(int($q*20000))") \
  -e VIBESERVER_DATA_DIR=/tmp/vd -e VIBESERVER_RUNTIME_DIR=/tmp/vr -e VIBESERVER_CONFIG=/tmp/vc.conf \
  -v $W/b/$b:/b:ro -v $W/perf:/perf $img bash -c "mkdir -p /tmp/vd /tmp/vr; touch /tmp/vc.conf; \
  exec /b/vs/vibeserver --tcp 127.0.0.1:1234 --rate $rate --freq 100000000 --mode wfm --fps $fps --port 48190 \
     --no-supervise --idle-grace 600 --name sandbox" >/dev/null
sleep 3
pid=$(docker inspect -f '{{.State.Pid}}' atomsrv)
# listener in the background; it warms for $warm s, measures for $meas s
docker exec atomcli python3 /tools/listener.py http://127.0.0.1:48190 100200000 $warm $meas 1024 $fps > /tmp/lis.$$ 2>&1 &
lp=$!
sleep $warm
cpu() { awk '{print $14+$15}' /proc/$pid/stat; }
c0=$(cpu); t0=$(date +%s.%N)
for t in /proc/$pid/task/*; do echo "$(cat $t/comm) $(awk '{print $14+$15}' $t/stat)"; done > /tmp/th0.$$
if [ "$perf" = stat ]; then
  docker exec atomsrv perf stat -e instructions,cycles,cache-misses,LLC-load-misses -p 1 -- sleep $((meas-2)) 2> $W/perf/$tag.stat
elif [ "$perf" = irec ]; then
  docker exec atomsrv perf record -e instructions:u -c 200000 -p 1 -o /perf/$tag.idata -- sleep $((meas-2)) 2>/dev/null
elif [ "$perf" = record ]; then
  docker exec atomsrv perf record -F 999 -g -p 1 -o /perf/$tag.data -- sleep $((meas-2)) 2>/dev/null
else sleep $((meas-2)); fi
c1=$(cpu); t1=$(date +%s.%N)
for t in /proc/$pid/task/*; do echo "$(cat $t/comm) $(awk '{print $14+$15}' $t/stat)"; done > /tmp/th1.$$
wait $lp
hz=$(getconf CLK_TCK)
thr=$(python3 - /tmp/th0.$$ /tmp/th1.$$ $hz $t0 $t1 <<'PY'
import sys,collections
a=collections.Counter(); b=collections.Counter()
for f,c in ((sys.argv[1],a),(sys.argv[2],b)):
    for l in open(f):
        n,v=l.rsplit(' ',1); c[n]+=int(v)
hz=float(sys.argv[3]); dt=float(sys.argv[5])-float(sys.argv[4])
print(','.join(f'"{k}":{100*(b[k]-a[k])/hz/dt:.1f}' for k in sorted(b, key=lambda k:-(b[k]-a[k])) if b[k]-a[k]>0))
PY
)
echo "{\"build\":\"$b\",\"rate\":$rate,\"cpus\":$q,\"fps\":$fps,\"serverCpuPct\":$(echo "100*($c1-$c0)/$hz/($t1-$t0)" | bc -l | xargs printf %.1f),\"threads\":{$thr},\"listener\":$(tail -1 /tmp/lis.$$)}"
[ "$perf" = stat ] && grep -E "instructions|cycles|misses" $W/perf/$tag.stat | sed 's/^/  /' >&2
docker logs atomsrv > $W/perf/$tag.log 2>&1
docker rm -f atomcli atomsrv >/dev/null 2>&1
rm -f /tmp/lis.$$ /tmp/th0.$$ /tmp/th1.$$
