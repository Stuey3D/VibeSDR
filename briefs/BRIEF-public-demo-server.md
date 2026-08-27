# VibeSDR — Public Demo Server + Live Site Background — Brief

**Status:** design draft, not started
**Hardware on hand:** spare Raspberry Pi 5, RTL-SDR v4, 100 Mb upload, Cloudflare account + domain
**Related:** `VibeServer-MultiClient-Brief.md` (the one-radio-per-user product model — this is a
deliberate *second* mode, see §1), `BRIEF-spyserver-and-network-performance.md`, Link Management
(`spike/WristSDR/WristSDR/LinkManager.swift`)

---

## 0. Why this exists

The website now claims *"we draw the waterfall, not the server"* and *"one waterfall everywhere."*
Those are the product's central differentiators and a visitor currently has to take them on faith.

A live public receiver turns both claims into something **checkable in a browser, in ten seconds,
with no install and no £2.99**. The VibeServer web client is already the same renderer as the app,
so the demo is not a mock-up of the experience — it *is* the experience.

Framing, which must stay literally true: **a real Raspberry Pi 5 with a real RTL-SDR v4 on a real
antenna. Not a simulation.**

---

## 1. This does NOT break the one-radio-per-user rule

`VibeServer-MultiClient-Brief.md` §0 rejects the shared-slice model because *"if user 1 makes big
jumps or tunes continuously, users 2..N get a garbage experience they can't control."*

That failure mode is **entirely about the LO moving.** Lock the centre frequency and it disappears:
every user gets a stable 2.4 MHz window and an independent VFO *inside* it. That is what a WebSDR
is, and it is a legitimate second mode — **"fixed slice, N listeners"** — not a weakening of the
product rule.

★ **Keep the two modes clearly named and separately configured.** The demo's rules (many users, one
radio, time-limited) must never leak into the product's model (one user, one radio, unlimited). If
that boundary blurs, the multi-client brief's core decision quietly rots.

---

## 2. The slice

- **Centre ~7.1 MHz @ 2.4 MSPS → roughly 5.9–8.3 MHz.**
- Covers **49m broadcast (5.9–6.2)**, the **whole of 40m (7.0–7.2)**, and **41m broadcast (7.2–7.45)**.
- Deliberately chosen so a visitor hears **three different things**: ham SSB, CW, and shortwave AM.
- **Centre is LOCKED.** No user may retune the LO. This is the whole basis of §1.
- **Do not offer WFM.** It is meaningless below 30 MHz and it is the only expensive mode we have.

---

## 3. Capacity — the real numbers

From the Pi benchmark (`tools/pi-bench`, post-optimisation), with a Pi 5 core ≈ 3× a Pi 3 core:

| | per user | 20 users |
|---|---|---|
| SSB demod | ~30% of a Pi 3 core → **~10% of a Pi 5 core** | **~2 of 4 cores** |
| Spectrum | one shared FFT, sliced per view | ~1 core |
| Bandwidth | ~12 KB/s spectrum + ~10 KB/s Opus ≈ **200 kbit/s** | **~4 Mbit/s of 100** |

**CPU is the ceiling, not the pipe.** The band choice keeps it cheap (SSB/CW/AM only). Compute the
FFT **once** and slice/decimate per user view rather than running N full FFTs.

Guard at runtime per the multi-client brief §1: watch dropped-sample counts and refuse the N-th user
rather than silently corrupting everyone's audio.

---

## 2b. ★ No hardware controls — and this should be a CONFIGURATION, not a fork

**The demo is a static setup: a fixed 2.4 MHz slice, the UI and the demodulators, and no hardware
control at all.** Gain, bias-T, PPM, AGC, direct sampling, sample rate and the centre frequency are
all off the table — hidden or greyed.

What a visitor DOES get: tune within the slice, pick a demod, set bandwidth, and all the display
settings. That is the whole VibeDSP experience, which is the point of the demo.

★ **The SERVER is its own thing entirely** (Stuart, 2026-07-19). It is a one-off installation with
its own rules — shared slice, queue, time limits, everything locked. **Do not build that into the
shipped product.** Making every VibeServer carry demo-only plumbing for the benefit of one Pi is the
wrong trade, and §1 already requires the demo's rules never leak into the product's model. Keeping it
separate also means it can be tuned freely without any risk of regressing the app.

★★ **But the CLIENT should not be forked either** — because the mechanism it needs already exists:

- `lockedRate` is advertised precisely so the client **hides** its rate picker rather than
  *"offering a control whose every use is silently dropped"* (`local_sdr_shim.cpp` hwinfo).
- `maxFftRate` was added the same way (2026-07-19, `96943d3`).

So the demo server simply **declares more things locked** through the descriptor the client already
understands. Separate server, unmodified client. That is also what keeps the demo honest as a demo:
a visitor is running the real client, not a special build made to look good.

**Hide vs grey:** follow the existing precedent — **hide** a control that is simply meaningless here
(sample rate, centre), **grey with a reason** where a user would otherwise wonder where it went
(gain, bias-T: *"fixed on the demo receiver"*). A screen full of dead controls reads as broken; a
screen missing everything reads as feature-poor. The reason text is what stops either.

---

## 3b. ★ Link Management must move stages for THIS mode only

`fftRate` is currently a **DSP-level** lever, not a transmit-level one: it lives on `Impl` (one per
engine) and the comment says why — *"the engine skips the FFT work entirely
(`specStride_ = sampleRate/fps`)"*, so both the CPU and the Wi-Fi radio wind down. That is exactly
right for the product model and must not be changed there.

**In the normal VibeServer model this is a non-issue.** One user per radio, **independent DSP engines
per radio** — each user's rate is already their own, and a user on a bad connection cannot affect
anyone else. The ladder works per-user for free.

**This demo is the deliberate exception** (§1): many users, ONE radio, therefore ONE engine. A global
`fftRate` here means **the worst connection in the room sets the frame rate for everybody** — which is
the same antisocial-by-construction failure §1 was written to avoid, wearing a different hat.

So for the shared-slice mode only:

| | product model (1 user / radio) | this demo (N users / radio) |
|---|---|---|
| where the rate applies | **DSP** — skip the FFT | **per-connection send** — compute once, decimate per client |
| saves | CPU *and* wire | wire only |
| adaptivity | per-user already | per-user, needs the new stage |

Compute the FFT at the highest rate any connected client wants, then **drop frames per connection on
the way out**. Dropping frames is nearly free; re-running FFTs per client is not. Same for bin count —
compute full, decimate per client (recall a zoom-in raises bytes/frame at constant fps, so bins are a
lever in their own right).

★ **Do not "fix" the DSP-level lever in the product to suit this.** Keep both: a single-client engine
should still collapse to the DSP lever and keep the CPU saving, which is what makes a solar or
battery-powered VibeServer viable.

---

## 4. Sessions

- **20 concurrent users, 5–10 minutes each**, then the slot recycles.
- **A countdown in the top corner of the spectrum**, showing time remaining, visible from the moment
  the session starts. Not a warning that appears at the end — a clock that was always there. A timer
  you can see is a fair rule; a timer you discover when it fires is an ambush, and the difference
  costs nothing to implement.
  - Goes amber under a minute, and that is when the "extend / rejoin" affordance appears.
  - It must read as *house rules*, not as a trial nag. This is a shared radio with people waiting,
    and saying so plainly is more respectable than pretending the limit is a sales tactic.
- **The queue must be gracious**: show position, warn before the boot, and offer a one-tap rejoin.
  Being cut off mid-QSO with no warning is a bad last impression, and last impressions are what a
  demo is for.
- Consider a longer slot when the server is quiet — a hard 5-minute cut with 3 people online is
  needless. If the timer extends itself because nobody is waiting, **say so** ("no one waiting —
  extended"), or a growing number looks like a bug.

---

## 5. Networking — Cloudflare Tunnel, NOT port forwarding

**Do not port-forward to a home LAN.** Use a **Cloudflare Tunnel** (`cloudflared` on the Pi):

- No open inbound ports on the home router
- Home IP never exposed
- TLS + DDoS protection included
- **Removes the DDNS requirement entirely** — the tunnel reconnects itself across IP changes

⚠ **Open question to settle before depending on it:** Cloudflare's terms restrict disproportionate
audio/video streaming across their network. At ~4 Mbit/s this is small, but read the current ToS
rather than assume. If it is a problem, the fallback is a cheap VPS relay, still not a port forward.

---

## 6. PARKED — the live site background — "ON AIR"

> **DROPPED 2026-07-19, not rejected.** Stuart: *"I didn't realise that it would smash my upload so
> forget that option, just stick with the 20 users shared SDR."*
>
> The Durable Object fan-out below *does* solve the upload problem — the Pi would send one stream
> regardless of audience. But it costs a paid Workers plan plus real complexity, for a decorative
> feature, on top of a demo server that has to exist and be reliable first. Right call to defer.
>
> **Revisit condition — explicit, not vague.** Stuart: *"if we start making money from sales to
> afford it then the live background is not out of the question, but I'm not paying for something
> that so far has been a hobby."*
>
> So: **revisit only when App Store revenue comfortably covers the recurring cost**, and only after
> §§1–5 are live and stable. This is not merely about £5/mo — the project's framing is that £2.99
> covers Apple's fee rather than being a paywall, and taking on a standing bill to make the site look
> impressive is the wrong trade while it is a hobby. The demo server itself costs nothing but
> hardware already owned, which is exactly why it goes first.
>
> Kept in full because the idea is good and the analysis is done, so it is cheap to pick up later.

Render the demo receiver's waterfall as the **live background of vibesdr.net**, with an **ON AIR**
indicator. No competitor does this, and it demonstrates the product in the most literal way available.

### ★ The scaling trap
"Reserve one slot for the page" **understates the cost by a factor of the visitor count.** Every
visitor needs their own copy of the stream: 100 visitors = 100 feeds off the home upload, not one. A
front-page feature would saturate the 100 Mb link *and* take the demo down with it.

**Fix — fan out through Cloudflare:**
```
Pi ──(ONE upstream WS)──> Cloudflare Durable Object ──(N)──> every page visitor
```
The Pi's upload stays flat regardless of audience; Cloudflare absorbs the egress. Requires the
**paid Workers plan (~$5/mo)** — that is the true cost of this feature.

**Plus: run the background at the bottom of the rate ladder.** It is decorative, so 5 fps is ample
(~4 KB/s per viewer instead of ~12). Spectrum only — **no audio** on the background feed.

### ★ The ON AIR sign is a truth claim
It **must** be driven by the actual socket state. If the feed drops and the static image takes over,
the sign goes dark or reads OFF AIR. A lit ON AIR sign over a canned loop is exactly the kind of
small lie the whole positioning refuses — and it is the first thing a sceptical reader would test.

### Fallback + hygiene
- Static background image, visually continuous with the live one, so the failure is invisible except
  for the sign.
- **Capacity valve, not just an error path**: past N concurrent viewers, serve the static image.
- Respect `prefers-reduced-motion`.
- Keep it **dim** — a moving background behind hero copy is the classic way to look impressive and
  read badly. Legibility wins.
- Pause the feed when the tab is hidden (`visibilitychange`) — do not stream to a background tab.

---

## 7. Site integration — ★ the ON AIR sign IS the way in

The ON AIR sign survives the parking of §6, repurposed as **the entry point itself**:

- **ON AIR** — lit, clickable → straight into the live receiver.
- **OFF AIR** — dark, not clickable, plainly worded ("the receiver is offline right now").

This is strictly better than a plain link plus a separate health indicator, because **the health
state and the affordance are the same object** — it cannot dangle, and it cannot lie. It also keeps
the showmanship that made §6 appealing, at essentially zero streaming cost: a small JSON health
poll, not a spectrum feed.

```jsonc
// GET /demo/status  — small, cacheable
{ "onAir": true, "slotsFree": 3, "slotsTotal": 20, "queue": 2 }
```

- Show the live detail on the sign when it is up: **"ON AIR · 3 of 20 free"**, or **"ON AIR · 2
  waiting"**. It is more honest than a bare light and it is more enticing — a visitor can see the
  thing is genuinely in use by other people.
- ★ **Cache the status JSON at the Cloudflare edge (~15–30s).** Otherwise every visitor's poll hits
  the Pi directly and a traffic spike DoSes the very box the sign is advertising. This is the same
  per-visitor scaling trap that killed §6, in miniature — it is cheap here only because the payload
  is tiny *and* cacheable.
- Fail **OFF AIR on any doubt**: timeout, stale timestamp, malformed response. The failure mode must
  be "closed", never "lit but broken".
- Label it plainly next to the sign: *running on a Raspberry Pi 5 with an RTL-SDR v4 — not a
  simulation.*

---

## 8. The side benefit worth having

**This is the only realistic source of Link Management field data.** Twenty strangers on unknown
connections is exactly the population the untuned 60% / 85% and 3s / 20s thresholds need, and it
cannot be manufactured at home. Log rung transitions per session and use them to tune the ladder.

---

## 9. Build order (suggested)

1. Locked-centre, single-user server on the Pi — prove the slice, the antenna and the CPU numbers.
2. Multi-user from one shared IQ stream: one FFT, N views, N demods. Runtime health guard.
3. Sessions + queue + countdown.
4. Cloudflare Tunnel; public URL; `/demo/status` endpoint, edge-cached.
5. The **ON AIR / OFF AIR sign** on the site, wired to that endpoint (§7) — this is the entry point.
6. ~~Live background + ON AIR~~ — **parked, see §6.** Everything above is useful without it, and the
   demo itself is the differentiator.
