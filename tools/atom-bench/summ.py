import json,sys
for line in sys.stdin:
    line=line.strip()
    if not line.startswith("{"): continue
    d=json.loads(line); l=d["listener"]
    print(d["build"], d["rate"], d["cpus"], "srv%", d["serverCpuPct"], "fps", l["specFps"], "aud", l["audioFrames"], "gaps", l["audioGaps150"], "worst", l["audioWorstGapMs"], "dspCpu", l["dspCpu"], d["threads"], flush=True)
