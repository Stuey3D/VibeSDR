# VibeServer — Benchmark & Bottleneck Finder — Brief

**Status:** design draft, not started
**Related:** `VibeServer-MultiClient-Brief.md` (§1 — "the real ceiling is host throughput, discovered
at runtime, not declared"), `tools/pi-bench`, `BRIEF-public-demo-server.md`

---

## 0. Why

A VibeServer owner currently has no way to answer the only question that matters before they share a
receiver: **"what can my hardware actually do, and what will break first?"** They find out by
inviting people in and watching it fall over.

The multi-client brief already commits to discovering the ceiling at runtime rather than declaring it
per-platform. This is the owner-facing half of that: a **Benchmark** button in VibeServer setup that
measures the host and the link, then **names the bottleneck in plain English**.

The CPU counter on the server screen (shipped 2026-07-19) is the live version of this. The benchmark
is the deliberate one.

---

## 1. Two halves, one verdict

### A. Local hardware
Reuse **`tools/pi-bench`'s workload** so results are directly comparable with the Pi figures we
already trust (SSB 30% / WFM 108% of a Pi 3 core, post-optimisation).

Measure, at 2.4 MSPS:
- Max sustainable **fftRate** before frames are dropped
- **Per-mode demod cost** — SSB, AM, WFM stereo (WFM is ~3.5× SSB, and it is the one that decides)
- Derived: **how many concurrent listeners** this host supports at each rate

### B. The link
**Upload is what matters** — serving is upstream. A download-heavy speedtest measures the wrong
direction and would flatter a typical domestic connection.

- Sustained **upstream** throughput, plus jitter (which is what the Link Management ladder reacts to)
- ★ **Distinguish LAN from internet serving.** Serving to a laptop in the same house needs no internet
  at all, and telling that owner their upload is poor is noise. Ask, or infer from whether the server
  is LAN-bound.

### C. The verdict — measured numbers, then a hard ceiling, then a recommendation

The report reads in that order, because each part earns the next:

```
CPU per listener, at 2.4 MS/s
  SSB / AM      8%  of one core
  WFM stereo   28%  of one core
Upload          9.4 Mbit/s sustained

HARD CEILING     6 listeners   (limited by CPU, on WFM)
RECOMMENDED      4 listeners   — leaves headroom for transient load
```

- ★ **The ceiling is `min(CPU-limited, upload-limited)`, and the report must SAY WHICH.** "6 users"
  is not actionable; "6, limited by CPU" tells an owner that a faster connection buys them nothing
  and a better phone does.
- ★ **Quote the WFM figure as the headline ceiling.** An owner cannot control what mode their guests
  pick, so a ceiling computed on SSB is a promise the server cannot keep the moment somebody tunes a
  broadcast station. Show the SSB number too — the difference is ~3.5× and it is genuinely useful —
  but recommend on the worst case.
- **Recommended ≈ 70% of the hard ceiling**, and say why in one line: transient load (a GC pause, the
  OS deciding to index something, another app waking) is what turns "exactly at capacity" into
  dropped samples for everyone at once. Never present the ceiling as the target.
- **Round down, never up.** A ceiling of 6.8 is 6.
- If the honest answer is 1, **say 1**. A phone that can serve one listener well is a useful thing,
  and inflating it produces a bad experience for two people instead of a good one for one.

---

## 1b. Two tiers — Quick and Sustained

**Quick (~30s)** — local workload once, plus the upload test. Gives a cold-hardware ceiling. Fine
for "roughly what have I got".

**Sustained / thermal (10–20 min)** — loops the LOCAL workload only. ★ **No repeated speedtest** — it
would burn the owner's data for a number that barely moves.

The sustained run is the one that produces a trustworthy figure, because **a phone serving on a shelf
all evening is a thermally-limited device and the quick test measures a cold one.** Its output is the
shape of the decline, not a single number:

```
HARD CEILING     6 listeners cold
                 4 listeners sustained  — throttled after 11 min, settled at 68% of peak
RECOMMENDED      3 listeners
```

- ★ **Report WHEN throttling began, not just that it did.** A phone that holds full speed for 20
  minutes is fine for casual sharing; one that throttles at 3 minutes is not, and the ceiling alone
  cannot tell those apart.
- **Recommend on the SUSTAINED figure** whenever a sustained run exists. The cold number is a
  curiosity; the settled one is what an owner's guests will actually experience.
- Sample **battery temperature** alongside (`BatteryManager.EXTRA_TEMPERATURE`) so the decline can be
  attributed to heat rather than guessed at.
- ★ **Test in the state you will SERVE in.** A phone on charge runs hotter and throttles sooner, so an
  unplugged benchmark flatters a server that will live plugged in on a shelf. Say this, and note in
  the result which state it ran in.
- **Abortable at any point**, keeping the results gathered so far — 15 minutes is a long time to be
  unable to change your mind.
- **Warn before starting**: it will get hot, it will use battery, and it should probably be plugged in.

---

## 2. ★ Honesty constraints (the part that makes it worth having)

- **Peak ≠ sustained.** A 10-second burst will not reveal the thermal throttling that shows up 20
  minutes in — and a phone on a shelf serving all evening is exactly the thermally-limited case. Either
  run long enough to matter, or say plainly that it is a short-run figure.
- **A speedtest measures one moment.** Contended evening broadband is not the same connection as
  10am. Present it as an indication, not a guarantee.
- **Never run it unasked.** It burns data and battery. Explicit button, with the data cost stated
  before it starts — an owner on a metered connection must not discover the cost afterwards.
- **Don't invent a bottleneck.** If the host is comfortable and the link is fine, say so. A tool that
  always finds a problem trains people to ignore it.
- ★ **Say what the benchmark CANNOT see.** It measures the host and the link; it cannot see the
  antenna system. **Bias-T is the big one** — an LNA or active antenna draws 80–120 mA through the
  coax from the same USB port the phone is powering (v4 supplies up to ~180 mA), which is 25–40% on
  top of the dongle's own ~300 mA and takes measured runtime from ~9h to ~6½–7h. The app KNOWS the
  Bias-T state, so a battery/runtime figure must factor it in rather than quietly assuming the
  favourable case. Same for a powered hub, or a dongle sharing the port.

---

## 2b. ★★ ADDED 2026-07-26 — two requirements the multi-user work needs

**1. Measure the PER-LISTENER cost, not just per-radio.** Stuart's Pi 5 already runs 2 SDRplays and
4 RTL-SDRs with WSJT-X decoding, so per-RADIO headroom is proven in the field. What is NOT proven is
what one extra *listener* costs: a listener is a continuous DDC + filter chain + demodulator + Opus
encode, and on WFM that is a 200 kHz channel at 48 kHz stereo — far heavier than a 3 kHz FT8 channel
that decodes once a cycle. ★ The expensive spectrum work is SHARED (one FFT per radio, then a cheap
~1024-bin resample per view), so the answer is dominated by AUDIO. The single number that sizes
multi-user is: **how many simultaneous WFM listeners can one Pi 5 radio carry?**

Bandwidth is no longer the binding constraint: ~27 KB/s per listener (19 spectrum + 8 Opus audio)
means a 20 Mbps uplink is ~90 listeners before the network cares. CPU will bite long first — which
is the better problem, and was NOT true before 2026-07-26 when a listener cost ~200 KB/s.

**1b. ★ THE FIELD DATAPOINT WE ALREADY HAVE (Stuart, 2026-07-26).** A **Pi 5** carried **10 browsers
on one OWRX profile** — one shared FFT plus ten independent audio demod chains — **while the other
radios kept decoding in the background** (2 SDRplays + 4 RTL-SDRs on WSJT-X). That is the VibeServer
multi-user shape under a realistic background load, so the architecture is already demonstrated at
~10 listeners on the target hardware. ★ The benchmark's job is therefore to find where it ACTUALLY
breaks and on which mode, not to establish whether 10 is possible.

★ And VibeServer should carry those same 10 for roughly a QUARTER of the bandwidth: OWRX force-feeds
every client the whole profile FFT (~80 KB/s each, ~1 MB/s for ten), where VibeServer shares one FFT
and sends a ~1024-bin per-view resample (~19 KB/s each, ~270 KB/s for ten). OWRX's spectrum cost
multiplies per listener; VibeServer's does not.

**2. ★★ MEASURE BOTH ENDS, AND COMPARE REQUESTED WITH ACHIEVED.** A benchmark that reports only what
the server was CONFIGURED for is worse than none. On 2026-07-26 the Mac reported "5 fps" while
actually emitting 2.9, because `fftRate` is the requested value and only `specBytesPerSec` is
measured — and a client-sent `set_rate` divisor was halving the output at SEND time, where nothing
upstream could see it. Hours were lost to it. The benchmark must therefore report, per run:

- requested rate vs **achieved** rate (frames AND bytes), at the server;
- the same at the client, so a divergence localises the loss;
- time the emit thread spends BLOCKED in `send()` — the signature of a slow client throttling the
  server invisibly;
- the actual frame size, which exposes the bins in play.

★ A single A/B — a loopback probe against the real client, on ONE running instance — localised in
one step what a whole afternoon of reasoning could not. Build that comparison INTO the benchmark.

## 3. Where it fits

- **Standalone VibeServer** — Stuart: *"a useful metric going forward for the standalone VibeServer."*
  A headless server needs this more than the phone app does; there is no screen to feel warm.
- **Multi-radio pool** — feeds the capacity guard directly ([[vibeserver_multiradio]]): the benchmark
  says how many radios/clients the host can carry, the runtime guard enforces it.
- **The Pi 5 demo server** — would have answered "can 20 users share this?" by measurement rather
  than by my arithmetic in `BRIEF-public-demo-server.md` §3.
