# BRIEF — Auto squelch (the algorithm, for every client)

**Status:** shipped in the web client (2026-09-25), to be ported to the app and to Jr
**Scope:** client-side, on every backend

## Why this is CLIENT-side and stays that way

The obvious optimisation is to move it into VibeServer: the server already measures the channel and
the noise, it could gate exactly, and each client would need one flag. **Do not do it.**

Jr and the app speak to **UberSDR, OpenWebRX, KiwiSDR, FM-DX and VibeServer**. A server-side auto
squelch works on the last of those only, so the feature would vanish on four backends out of five —
a control that only works in one scenario, which AGENTS.md says should not exist. Stuart, 2026-09-25:
*"Jr's squelch is platform agnostic and I want to preserve that."*

The cost is that the algorithm below exists more than once (TypeScript in the web client and the app,
Swift in Jr). That is why it is written down here: **three implementations that agree are fine; three
that drift are not.** Change this file when you change any of them.

## The one thing that matters most

★★★ **Track the CHANNEL's own quiet level — never a wideband noise floor.**

Measured on an empty FM channel (2026-09-25): the channel figure sits **12.8 to 30.3 dB above** the
wideband floor, median 20.7, and the gap moves with bandwidth and band activity. A threshold set as
"floor + margin" is therefore far too low on a wide FM passband and wrong again on a 2.7 kHz SSB one.
Referencing the channel against *itself* makes the bandwidth cancel out — which is why one number
(12 dB) worked unchanged on 40 m SSB **and** on MW.

Use whatever per-channel level the backend already gives the meter (VibeServer: `sig.chan`).

## The algorithm

Per signal update (VibeServer sends `sig` at ~20/s; use whatever rate the backend gives):

```
if (channel key changed)  base = NaN            # freq | mode | bandwidth — see RESEED
openish = (now - openAt) < HANG
a       = (chan < base) ? FALL : (openish ? 0 : RISE)
base    = isFinite(base) ? base + (chan - base) * a : chan     # SEED BEFORE ANY READER
decide  = base + MARGIN - (openish ? HYST : 0)
if (chan >= decide) openAt = now
gateOpen = (now - openAt) < HANG
threshold = gateOpen ? (OFF + 1) : decide       # what is SENT / applied
shown     = decide                              # what the UI needle draws
```

| constant | value | why |
|---|---|---|
| `MARGIN` | **12 dB** (user-adjustable 4-20) | The baseline tracks the noise MINIMUM, so the margin must clear the noise's own ~7 dB spread first. Confirmed three ways: replay of a captured trace, Stuart by ear on 40 m, and OpenWebRX's effective figure (~13 in our terms). |
| `HYST` | 3 dB | On SSB the whole gap between noise and a weak voice is only a few dB, so the buffer must fit inside it. |
| `HANG` | 700 ms | Covers the pause between words. |
| `FALL` | 0.20 | Towards a new low: about a quarter of a second. |
| `RISE` | 0.002 | Away from it: tens of seconds. |
| `OFF` | -100 | The "squelch off" sentinel. |

### The four traps, each of which cost a build

1. ★★★ **The hang must hold the gate GENUINELY OPEN**, not merely lower the threshold. The gate is a
   bare `chan < threshold` comparison with no hang of its own, and in a syllable gap the signal falls
   all the way to the noise — straight past a threshold only `HYST` down. Hence two thresholds:
   `decide` (internal, judges presence) and the one applied. **Judging on the applied value latches
   permanently open.** A user recording proved the first attempt did nothing: 37 closures, median
   50 ms, none over a second — a 700 ms hang cannot produce a 50 ms gap.
2. ★★★ **Seed the baseline BEFORE anything reads it.** A NaN sentinel resolved after `decide` is
   computed produces a NaN threshold; `NaN !== NaN` then makes the "only on a change" test true for
   ever, so it is resent every sample and the squelch never engages.
3. ★★ **The baseline rises only while the gate is shut.** Otherwise a long over drags it up into the
   speaker and the squelch talks itself into silence part-way through a transmission.
4. ★★ **RESEED on a channel change** (frequency, mode or bandwidth), detected by comparing a key
   INSIDE the update — not wired into each tune call site, where the one that gets missed is the bug.
   Without it, arriving somewhere noisier with the gate open blocks the slow rise for ever and the
   only escape is toggling auto off and on.

## UI (match across clients)

- An **AUTO SQUELCH** toggle beneath the manual bar.
- While active: the bar **fades but stays visible**, the draggable ball is replaced by a **red line
  that moves on its own**, and **AUTO SQUELCH ACTIVE** is overlaid on the bar.
- ★ The red line draws `shown` (the setting), **not** the applied threshold — otherwise it slams to
  the far left every time the hang opens the gate.
- An **ABOVE NOISE** slider (4-20 dB) beside the toggle, shown only while auto is on, remembered per
  client. "Margin" is what the code and OpenWebRX call it; it means nothing to a listener.
- A drag on the bar switches auto OFF and restores the remembered manual value.
- If the backend reports no usable signal figure, the toggle is **disabled with a reason**, never
  silently dead.

## How to tune it (do not do this by ear)

`scratchpad/sigcap.mjs` captures a real channel/floor trace from a server; `sqlsim.mjs` replays this
algorithm over it and reports closures per second, median closure length, and how many are under
300 ms (chopping). A 45 s trace settles in seconds what an hour of listening cannot: margin 5 gave
86 closures with 85 of them chopping; margin 12 gives one closure and none.

★ Stuart: *"People love it so we need to get it right but I avoid it because I cannot get it right
myself."* That is the case FOR the feature — if the expert cannot set it by hand, a listener has no
chance — and the reason it is worth this much care.

## Against the field

- **OpenWebRX** (`DemodulatorPanel.js`): `squelchMargin = 10`, applied as `smeter_level + margin`.
- **GQRX** (`MainWindow::setSqlLevelAuto`): `get_signal_pwr() + 3.0`.
- Both are ONE-SHOT off the current level (the noise *average*); this tracks continuously off the
  noise *minimum*, 3.3 dB lower on the measured trace — so their margins are ≈+13 and ≈+6 in our
  terms, and 12 sits between them.
- ★★★ **csdr's gate has no hysteresis and no hang** — `if (power >= squelch_level) pass; else zeros;`
  So OpenWebRX chops syllables exactly as this did before the hang was added. Continuous tracking,
  hysteresis and hang are all things we have and they do not.
