#!/usr/bin/env python3
"""synth_sitor_a.py — a SITOR-A (AMTOR mode A, ARQ) link as a third station hears it: both transmitters, the whole
protocol, with noise, impulse bursts and fading. Written from ITU-R M.476-5 / M.625-4 / M.491-1 (no other source).

  python3 synth_sitor_a.py out.wav [snr_db=15] [fade_db=0] [seconds=120] [bursts_per_min=0] [burst_db=10] [seed=0]

The link (each turn strictly alternates master, slave — the ARQ cycle, M.476 §3.1.1.3, 450 ms):
  * the master (a ship, MMSI 364775427 = P E A R D B Y) CALLS the slave (a coast station, MMSI 002320204 when
    CALL=7, the M.625 three-block call; selcall 32610 = Q C X T when CALL=4, the two-block call) — the call is sent
    several times before the slave answers (M.625 §3.5.4);
  * CALL=7: the slave answers CS4 and the M.625 §3.6 identification follows (the master's identification blocks,
    the slave's check-sum signals, the end-of-identification block RQ RQ RQ), then CS1;
  * traffic from the master; at its end "+?" (figure shift, plus, question mark) — the IRS answers CS3, the ISS sends
    β α β, and the slave becomes ISS (RQ RQ RQ, as slave, §3.7.11.5); its reply ends "+?" too, and the master takes
    over again (one RQ, as master) to send a last line and the end of communication (α α α, §3.1.9.3);
  * every block the IRS receives is MUTILATED with probability RQP (default 0.12) → it sends the same control signal
    again and the ISS repeats the block (§3.1.3.3/4); every control signal the ISS receives is mutilated with
    probability CSP (default 0.04) → it sends RQ RQ RQ (§3.1.2.6). These are the protocol's errors, chosen by dice —
    independent of the noise in THIS recording, which is the listener's own.
Env: CF (1000 Hz audio centre), D_MS (30: the slave's delay after the master's signal ends), SLAVE_DB (-6: its level),
  MASTER_HZ / SLAVE_HZ (0 / +12: frequency errors), CALL (7|4), RQP, CSP, ONLY=M (only the master is heard),
  INV=1 (tones swapped), BAUD (100: a clock error), FS (48000).
Writes out.wav (mono int16) and out.wav.txt (what a perfect listener prints, all stations, in order) and
out.wav.json (the facts: identities, number of repeats, sides)."""
import sys, os, wave, json, random
import numpy as np

a = sys.argv
out = a[1]; snr = float(a[2]) if len(a) > 2 else 15; fd = float(a[3]) if len(a) > 3 else 0
secs = float(a[4]) if len(a) > 4 else 120; bpm = float(a[5]) if len(a) > 5 else 0
bdb = float(a[6]) if len(a) > 6 else 10; seed = int(a[7]) if len(a) > 7 else 0
env = os.environ.get
FS = int(env('FS', 48000)); BAUD = float(env('BAUD', 100.0)); CF = float(env('CF', 1000.0)); SH = 170.0
D = float(env('D_MS', 30)) / 1000; SLAVE_DB = float(env('SLAVE_DB', -6)); MHZ = float(env('MASTER_HZ', 0)); SHZ = float(env('SLAVE_HZ', 12))
CALL = int(env('CALL', 7)); RQP = float(env('RQP', 0.12)); CSP = float(env('CSP', 0.04)); ONLY = env('ONLY', '')
INV = env('INV', '0') == '1'

def sig(by): return sum(1 << i for i, c in enumerate(by) if c == 'B')
TABLE = [('A', '-', 'BBBYYYB'), ('B', '?', 'YBYYBBB'), ('C', ':', 'BYBBBYY'), ('D', None, 'BBYYBYB'), ('E', '3', 'YBBYBYB'),
         ('F', None, 'BBYBBYY'), ('G', None, 'BYBYBBY'), ('H', None, 'BYYBYBB'), ('I', '8', 'BYBBYYB'), ('J', None, 'BBBYBYY'),
         ('K', '(', 'YBBBBYY'), ('L', ')', 'BYBYYBB'), ('M', '.', 'BYYBBBY'), ('N', ',', 'BYYBBYB'), ('O', '9', 'BYYYBBB'),
         ('P', '0', 'BYBBYBY'), ('Q', '1', 'YBBBYBY'), ('R', '4', 'BYBYBYB'), ('S', "'", 'BBYBYYB'), ('T', '5', 'YYBYBBB'),
         ('U', '7', 'YBBBYYB'), ('V', '=', 'YYBBBBY'), ('W', '2', 'BBBYYBY'), ('X', '/', 'YBYBBBY'), ('Y', '6', 'BBYBYBY'),
         ('Z', '+', 'BBYYYBB')]
L = {l: sig(by) for l, f, by in TABLE}; F = {f: sig(by) for l, f, by in TABLE if f}
CR, LF, LTRS, FIGS, SP = sig('YYYBBBB'), sig('YYBBYBB'), sig('YBYBBYB'), sig('YBBYBBY'), sig('YYBBBYB')
CS1, CS2, CS3, CS4, CS5 = sig('BYBYYBB'), sig('YBYBYBB'), sig('BYYBBYB'), sig('BYBYBBY'), sig('BYYBYBB')
BETA, ALPHA, RQ = sig('BBYYBBY'), sig('BBBBYYY'), sig('YBBYYBB')
for v in list(L.values()) + [CR, LF, LTRS, FIGS, SP, BETA, ALPHA, RQ, CS2]: assert bin(v).count('1') == 4

IDS = 'VXQKMPCYFSTBUEOIRZDA'                       # M.625 Table 3a / M.491 Table III, by value 0-19
def id7(num):
    v = int(num); s = ''
    for _ in range(7): s = IDS[v % 20] + s; v //= 20
    return s
SET_V, SET_T = 'VXQKMPCYFS', 'TBUEOIRZDA'
PAT = ['TVVV', 'VTVV', 'VVTV', 'VVVT', 'TTVV', 'TVTV', 'TVVT', 'VTTV', 'VTVT', 'VVTT']
def id4(num): return ''.join((SET_T if p == 'T' else SET_V)[int(d)] for p, d in zip(PAT[int(num[0])], num[1:]))
def cksum(i7):
    n = [IDS.index(c) for c in i7]
    return ''.join(IDS[(x + y + z) % 20] for x, y, z in ((n[0], n[1], n[2]), (n[2], n[3], n[4]), (n[4], n[5], n[6])))
assert id7('364775427') == 'PEARDBY' and cksum('PEARDBY') == 'ZER' and id4('32610') == 'QCXT'

MASTER_NUM = '364775427'; SLAVE_NUM = '002320204' if CALL == 7 else '32610'
MASTER_ID = id7(MASTER_NUM); SLAVE_ID = id7(SLAVE_NUM) if CALL == 7 else id4(SLAVE_NUM)

WORDS = ('QTH POSITION ETA PORT CARGO CREW WEATHER WIND SEA SWELL MASTER AGENT BUNKERS PILOT BERTH ANCHOR TUG '
         'REPAIR ENGINE SPARES MEDICAL OWNER CHARTER VOYAGE ROUTE DRAFT TONNES CONTAINERS OK CONFIRM PLEASE ADVISE '
         'REGARDS THANKS RECEIVED').split()
def text(k, lines):
    r = random.Random(500 + k); out = []
    for _ in range(lines):
        w = ' '.join(r.choice(WORDS) for _ in range(r.randint(3, 7)))
        if r.random() < 0.5: w += f' {r.randint(1, 31):02d}/{r.randint(1, 12):02d} {r.randint(0, 23):02d}{r.randint(0, 59):02d}Z'
        if r.random() < 0.3: w += f' ({r.randint(10, 99)}.{r.randint(0, 9)}N {r.randint(1, 179):03d}.{r.randint(0, 9)}W)'
        out.append(w)
    return '\r\n'.join(out) + '\r\n'

def encode(t):
    codes, ref, fig = [LTRS], '', False
    for ch in t:
        if ch == '\r': codes.append(CR); continue
        if ch == '\n': codes.append(LF); ref += '\n'; continue
        if ch == ' ': codes.append(SP); ref += ' '; continue
        if ch in L and not fig: codes.append(L[ch]); ref += ch; continue
        if ch in F and fig: codes.append(F[ch]); ref += ch; continue
        if ch in F: codes += [FIGS, F[ch]]; fig = True; ref += ch; continue
        codes += [LTRS, L[ch]]; fig = False; ref += ch
    return codes, ref
def blocks(codes):
    while len(codes) % 3: codes = codes + [BETA]
    return [codes[i:i + 3] for i in range(0, len(codes), 3)]

rng = random.Random(9000 + seed)
# three passages of traffic: master (ends "+?"), slave (ends "+?"), master (then the end of communication)
n_lines = max(2, int(secs / 25))
t1, t2, t3 = text(1, n_lines), text(2, max(1, n_lines // 2)), text(3, 1)
c1, r1 = encode(t1 + '+?'); c2, r2 = encode(t2 + '+?'); c3, r3 = encode(t3)
queues = {'M': [blocks(c1), blocks(c3)], 'S': [blocks(c2)]}
reference = r1 + r2 + r3 if ONLY != 'M' else r1 + r3   # only the master heard: only its text is owed

# ── the protocol, turn by turn (M, S, M, S …) ──
turns = []                         # (station, codes or None)
def call_blocks():
    i = SLAVE_ID
    return [[L[i[0]], RQ, L[i[1]]], [RQ, L[i[2]], L[i[3]]], [L[i[4]], L[i[5]], L[i[6]]]] if CALL == 7 else \
           [[L[i[0]], RQ, L[i[1]]], [L[i[2]], L[i[3]], RQ]]
cb = call_blocks()
for rep in range(3):               # the call, three times round before the slave answers
    for b in cb:
        turns += [('M', b), ('S', None)]
turns[-1] = ('S', [CS4 if CALL == 7 else CS1])
repeats = 0; rq_blocks = 0
if CALL == 7:
    mi = MASTER_ID; ck = cksum(SLAVE_ID)
    for b, k in (([L[mi[0]], ALPHA, L[mi[1]]], ck[0]), ([ALPHA, L[mi[2]], L[mi[3]]], ck[1]), ([L[mi[4]], L[mi[5]], L[mi[6]]], ck[2])):
        turns += [('M', b), ('S', [L[k]])]
    turns += [('M', [RQ, RQ, RQ]), ('S', [CS1])]
    rq_blocks += 1

iss, irs = 'M', 'S'
last_cs = CS1                      # the IRS's last control signal (the ISS answers it next)
cur, cur_num, done = None, 0, False
def next_block():
    q = queues[iss]
    if q and q[0]: return q[0].pop(0)
    return None
pending_over = False
while not done and len(turns) < 2 * int(secs / 0.45) + 40:
    # ── the ISS's turn: it answers the last control signal ──
    mut_cs = rng.random() < CSP
    if mut_cs:
        blk = [RQ, RQ, RQ]; rq_blocks += 1
    elif last_cs == CS3:
        blk = [BETA, ALPHA, BETA]
    else:
        num = 1 if last_cs == CS1 else 2
        if num != cur_num or cur is None:
            nb = next_block()
            if nb is None:
                if queues[iss] and not queues[iss][0]: queues[iss].pop(0)
                # nothing more: master ends the communication, a slave waits for the +? to be answered (it has been)
                nb = [ALPHA, ALPHA, ALPHA] if iss == 'M' and not queues['M'] and not queues['S'] else [BETA, BETA, BETA]
            cur, cur_num = nb, num
        blk = cur
    turns.append((iss, blk))
    # ── the IRS's turn ──
    mutilated = rng.random() < RQP
    if mutilated or RQ in blk:
        cs = last_cs
        if not RQ in blk: repeats += 1
    elif blk == [BETA, ALPHA, BETA]:
        # change-over: the IRS becomes ISS (§3.7.11.5) — slave: RQ RQ RQ, master: one RQ
        turns.append((irs, [RQ, RQ, RQ] if irs == 'S' else [RQ]))
        turns.append((iss, [CS1]))  # the old ISS, now IRS, starts the flow (§3.7.11.6)
        iss, irs = irs, iss
        if queues[irs] and not queues[irs][0]: queues[irs].pop(0)
        last_cs, cur, cur_num = CS1, None, 0
        continue
    elif blk == [ALPHA, ALPHA, ALPHA]:
        turns.append((irs, [CS2 if last_cs == CS1 else CS1])); done = True; break
    else:
        # accepted: a block that completes "+?" asks for the change-over (§3.1.6.2)
        over = len(blk) and (blk[-1] == F['?'] or (blk[-1] == BETA and F['?'] in blk))
        cs = CS3 if over else (CS2 if last_cs == CS1 else CS1)
    turns.append((irs, [cs])); last_cs = cs
# pad: the link is gone, silence to the end
# ── the signal ──
cyc = 0.45; t0 = 0.3
M_AMP, S_AMP = 1.0, 10 ** (SLAVE_DB / 20)
n_total = int(secs * FS)
sigl = np.zeros(n_total)
# turns alternate M, S; cycle k = turns[2k] (master, at the cycle start) and turns[2k+1] (slave, D after it ends)
assert all(t[0] == ('M' if i % 2 == 0 else 'S') for i, t in enumerate(turns)), 'turns must alternate'
spb = FS / BAUD
rng2 = np.random.default_rng(7 + 1000 * seed)
def fade(sd):
    if fd <= 0: return np.ones(n_total)
    r = np.random.default_rng(sd); kk = r.standard_normal(int(secs * 2) + 4)
    t = np.arange(n_total) / FS
    x = np.interp(t, np.arange(len(kk)) / 2, kk); x = (x - x.min()) / (x.max() - x.min())
    return 10 ** (-fd * x / 20)
g = {('M', 1): fade(31 + 1000 * seed), ('M', 0): fade(43 + 1000 * seed), ('S', 1): fade(57 + 1000 * seed), ('S', 0): fade(71 + 1000 * seed)}
sent_chars = {'M': 0, 'S': 0}
end_time = 0
for k in range(0, len(turns), 2):
    tm = t0 + (k // 2) * cyc
    for j, (st, codes) in enumerate(turns[k:k + 2]):
        m_codes = turns[k][1]
        start = tm if st == 'M' else tm + (len(m_codes) if m_codes else 1) * 0.07 + D
        if codes is None or (ONLY == 'M' and st == 'S'): continue
        bits = [(c >> i) & 1 for c in codes for i in range(7)]
        a0 = int(round(start * FS)); n = int(round(len(bits) * spb))
        if a0 + n >= n_total: continue
        idx = np.minimum((np.arange(n) / spb).astype(int), len(bits) - 1)
        b = np.array(bits, bool)[idx]
        if INV: b = ~b
        off = MHZ if st == 'M' else SHZ
        f = np.where(b, CF + SH / 2 + off, CF - SH / 2 + off)
        ph = 2 * np.pi * np.cumsum(f) / FS + rng2.uniform(0, 2 * np.pi)
        amp = M_AMP if st == 'M' else S_AMP
        ramp = np.minimum(1, np.minimum(np.arange(n), n - 1 - np.arange(n)) / (0.001 * FS))
        gg = np.where(b != INV, g[(st, 1)][a0:a0 + n], g[(st, 0)][a0:a0 + n])
        sigl[a0:a0 + n] += amp * gg * ramp * np.cos(ph)
        end_time = max(end_time, (a0 + n) / FS)
noise = rng2.standard_normal(n_total) * np.sqrt(0.5) * 10 ** (-snr / 20) * np.sqrt(FS / 3000)   # SNR in 3 kHz, master
nb = 0
if bpm > 0:
    tt = rng2.exponential(60.0 / bpm)
    while tt < secs:
        d = rng2.uniform(0.020, 0.200); a0, a1 = int(tt * FS), min(n_total, int((tt + d) * FS))
        noise[a0:a1] += rng2.standard_normal(a1 - a0) * np.sqrt(0.5) * 10 ** (bdb / 20) * np.sqrt(FS / 3000)
        nb += 1; tt += d + rng2.exponential(60.0 / bpm)
y = np.clip((sigl + noise) * 0.25, -1.0, 1.0)
with wave.open(out, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS); w.writeframes((y * 32767).astype('<i2').tobytes())
# what was sent completely before the recording ended goes in the reference (a link cut short: the whole is not owed)
complete = end_time < secs - 0.5
open(out + '.txt', 'w').write(reference)
json.dump({'master': MASTER_ID, 'master_number': MASTER_NUM, 'slave': SLAVE_ID, 'slave_number': SLAVE_NUM,
           'repeats': repeats, 'rq_blocks': rq_blocks, 'cycles': len(turns) // 2, 'link_seconds': round(end_time, 1),
           'complete': complete, 'bursts': nb}, open(out + '.json', 'w'))
print('wrote', out, f'{secs}s snr {snr} dB fade {fd} dB, {len(turns)//2} cycles ({end_time:.0f} s of link), '
      f'{repeats} IRS repeats, {rq_blocks} RQ blocks, call {CALL}-signal{"" if complete else "  ★ LINK CUT SHORT — make it longer"}')
