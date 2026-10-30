# ★★★ RSP STEREO "DIGITAL MUD IN THE WIDTH" — WHY A ZERO-IF FRONT END HURTS L-R WHEN THE STATION SITS ON DC
#     (2026-10-30, Stuart: "its something to do with the SDRPlay and it always was"; the same 96.1 is clean on a
#     V4 on the Pi 500.) The RSP runs ifType = IF_Zero and hwOffsetHz() is 0 for it, so with the VFO centre
#     locked a WFM station is centred ON DC. Two things in the analogue I/Q path then land on the station
#     itself: its own IQ-imbalance mirror (s + eps*conj(s) — the mirror of a centred station IS the station,
#     frequency-inverted) and the DC canceller. The V4 has neither (digital I/Q, and a 15 kHz offset).
#     Mono programme (L = R), so any recovered L-R is pure damage. Offline only; no radio.
#   run: python3 tools/rsp-zeroif-stereo-sim.py      (numpy)
# Result, 2026-10-30:
#   clean -64.5 dB · IQ image -30/-40/-50 dB ON DC -> L-R mud -30/-40/-50 dB (1:1) · 250 kHz off DC -> -64.5
#   DC canceller tau 1 ms / 20 ms ON DC -> -24.6 / -38.3 dB · off DC -> -64.5
#   mono (L+R) error for the same -40 dB image: -52.7 dB — the damage is ~13 dB worse in the WIDTH than in mono.
# Stereo WFM through a zero-IF RSP-like front end: how much L-R "mud" does a station ON DC pick up from
# (a) residual IQ imbalance (the station's own mirror lands on itself) and (b) the DC canceller,
# vs the same station 250 kHz off DC. Programme is MONO (L = R), so any recovered L-R is pure damage.
import numpy as np
fs = 3_000_000; T = 0.4; n = int(fs * T); t = np.arange(n) / fs
rng = np.random.default_rng(1)

def fir_lp(cut, fs, taps=401):
    k = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cut / fs * k) * np.hamming(taps)
    return h / h.sum()

def lp(x, cut, fs, taps=401):
    return np.convolve(x, fir_lp(cut, fs, taps), mode='same')

# programme: band-limited noise (music-like) 50 Hz..15 kHz, L = R
fsa = 250_000; na = int(fsa * T)
ta = np.arange(na) / fsa
prog = lp(rng.standard_normal(na), 15000, fsa, 801)
prog /= np.max(np.abs(prog))
pilot = 0.09 * np.cos(2 * np.pi * 19000 * ta)
mpx_a = 0.9 * prog + pilot                      # L-R = 0, so no 38 kHz DSB at all
# upsample MPX to fs (linear interp is fine at 12x for a 0-60 kHz signal)
mpx = np.interp(t, ta, mpx_a)
phase = 2 * np.pi * 75000 * np.cumsum(mpx) / fs

prog2 = lp(rng.standard_normal(na), 15000, fsa, 801); prog2 /= np.max(np.abs(prog2))
phase2 = 2 * np.pi * 75000 * np.cumsum(np.interp(t, ta, 0.9 * prog2 + pilot)) / fs
def run(offset_hz, iq_db, dc_tau_s, nb_db=None, nb_hz=0, mono_err=False):
    x = np.exp(1j * (phase + 2 * np.pi * offset_hz * t))
    if nb_db is not None:
        x = x + 10 ** (nb_db / 20) * np.exp(1j * (phase2 + 2 * np.pi * nb_hz * t))
    # strong neighbour 400 kHz away so the band is not empty (its own mirror lands at -400 kHz)
    if iq_db is not None:
        eps = 10 ** (iq_db / 20) * np.exp(1j * 0.7)
        x = x + eps * np.conj(x)
    if dc_tau_s:
        a = 1.0 / (dc_tau_s * fs)
        # one-pole DC tracker, as a hardware DC canceller: dc += a*(x-dc); y = x - dc
        from itertools import accumulate
        dc = np.empty_like(x); d = 0j
        # vectorised one-pole via lfilter-equivalent recursion in chunks
        b = a; c = 1 - a
        # y[n] = c*y[n-1] + b*x[n]  -> use exponential-weighted cumsum trick in blocks
        blk = 4096
        for s in range(0, n, blk):
            seg = x[s:s + blk]
            k = np.arange(len(seg))
            w = c ** (-k)
            acc = d * c ** (k + 1) + b * c ** k * np.cumsum(seg * w)
            dc[s:s + blk] = acc
            d = acc[-1]
        x = x - dc
    x = x * np.exp(-2j * np.pi * offset_hz * t)  # bring the station back to 0 (as the VFO does)
    y = lp(x.real, 120000, fs) + 1j * lp(x.imag, 120000, fs)
    y = y[::12]                                  # 250 kS/s
    d = np.angle(y[1:] * np.conj(y[:-1])) * fsa / (2 * np.pi * 75000)
    d = np.concatenate([[0], d])
    tt = np.arange(len(d)) / fsa
    lpr = lp(d, 15000, fsa, 801)
    lmr = lp(d * 2 * np.cos(2 * np.pi * 38000 * tt), 15000, fsa, 801)
    sl = slice(int(0.05 * fsa), int(0.35 * fsa))
    if mono_err:
        return 20 * np.log10(np.std(lpr[sl] - REF[sl]) / np.std(REF[sl]))
    return 20 * np.log10(np.std(lmr[sl]) / np.std(lpr[sl]))

REF = None
def ref():
    global REF
    x = np.exp(1j * phase); y = lp(x.real, 120000, fs) + 1j * lp(x.imag, 120000, fs); y = y[::12]
    d = np.concatenate([[0], np.angle(y[1:] * np.conj(y[:-1])) * fsa / (2 * np.pi * 75000)])
    REF = lp(d, 15000, fsa, 801)
ref()
print("MONO (L+R) error, IQ -40 dB ON DC:", round(run(0, -40, 0, mono_err=True), 1), "dB;  DC tau 20 ms:", round(run(0, None, 0.02, mono_err=True), 1), "dB")
print("Another station +10 dB stronger at the MIRROR of the wanted, IQ -40 dB: on DC (mirror = itself) n/a;",
      "off DC 250 kHz with the neighbour at +500 kHz:", round(run(250000, -40, 0, nb_db=10, nb_hz=500000), 1), "dB")
print("L-R damage relative to L+R (mono programme; lower = cleaner)")
print(f"  clean, no impairment                    {run(0, None, 0):6.1f} dB")
for iq in (-30, -40, -50):
    print(f"  IQ image {iq} dB, station ON DC         {run(0, iq, 0):6.1f} dB    off DC 250 kHz: {run(250000, iq, 0):6.1f} dB")
for tau in (0.001, 0.02):
    print(f"  DC canceller tau {tau*1000:4.0f} ms, ON DC         {run(0, None, tau):6.1f} dB    off DC 250 kHz: {run(250000, None, tau):6.1f} dB")
