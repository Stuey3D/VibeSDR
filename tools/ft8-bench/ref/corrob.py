#!/usr/bin/env python3
# corrob.py MODE BAND SSE PSKXML T0 DUR OURS.txt — every ours-only decode: are its calls known to the reference (stream, any
# slot of the capture, any band) or to PSKReporter (STUEY3D's reports)? An AP decode with an unknown call is suspect.
import sys,re,json,collections,subprocess
mode,band,sse,xml,t0,dur,ours=sys.argv[1:8]
out=subprocess.run(['python3',sys.argv[0].replace('corrob.py','score_ref.py'),mode,band,sse,t0,dur,ours],capture_output=True,text=True,env={'V':'1','PATH':'/usr/bin:/bin'}).stdout
known=set()
for l in open(sse):
    if l.startswith('data: '):
        try: j=json.loads(l[6:])
        except: continue
        for t in j.get('message','').replace('<','').replace('>','').split(): known.add(t)
        if j.get('callsign'): known.add(j['callsign'])
for m in re.finditer(r'senderCallsign="([^"]+)"',open(xml).read()): known.add(m.group(1))
GRID=re.compile(r'^(R?[-+]\d\d|[A-R]{2}\d\d|RRR|RR73|73|CQ|DE|QRZ|<H>|DX|TEST|POTA)$')
c=collections.Counter(); sus=[]
for l in out.splitlines():
    if 'OURS-ONLY' not in l: continue
    s,msg,tag,snr=eval(l.split('OURS-ONLY')[1])
    calls=[t for t in msg.split() if not GRID.match(t)]
    ok=all(x in known for x in calls)
    kind='AP' if 'a' in tag else ('fine' if 'f' in tag else 'base')
    c[(kind,ok)]+=1
    if not ok: sus.append((kind,tag,snr,msg))
print(sorted(c.items()))
for x in sus: print('   UNCORROBORATED',x)
