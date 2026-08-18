# BRIEF — shared tuning, and a canned chat to ask for the dial

**Stuart, 2026-08-19.** ★ First, a correction worth keeping: a single-user radio is **not** a CPU
decision. *"The single user isn't about CPU, it is about users fighting each other for tuning."*
One VFO, many hands. Every design below follows from that.

## Three modes, not two

| Mode | Listeners | Who tunes | For |
|---|---|---|---|
| **Exclusive** (today) | 1 | the occupant | a personal receiver, or a rare radio |
| **Shared, spectator** | many | nobody but the owner | a fixed watch: a beacon band, a net frequency |
| **Shared, open tuning** | many | anyone, by asking | the FM-DX model — the one this brief is about |

## Open tuning: how people avoid fighting

The web is full of one-VFO receivers that work socially rather than technically, and FM-DX
Webserver is the proof. The rules that make it work:

- **Anyone may listen. One person holds the dial** — the *tuner*. Everyone else hears what they
  tune.
- **Asking is one tap**, not a conversation: a listener asks for the dial, the tuner accepts or
  declines. Nobody has to type.
- **The dial times out.** A tuner who has not touched anything for a few minutes releases it
  automatically, so an idle tab cannot hold a receiver hostage — the fault that makes these
  systems unpleasant everywhere else.
- **Admin always outranks it**, exactly as now.

## ★★★ THE CHAT IS CANNED, AND THAT IS THE FEATURE

Free-text chat on a public receiver is a moderation problem, an abuse vector, a translation
problem and an XSS surface. A fixed vocabulary is none of those, and it is *better* for the actual
task:

    "May I tune?"   "Go ahead"   "Please wait — recording"   "Two minutes"
    "Thanks"        "All yours"  "Staying here for now"      "Anyone using this?"

- **No moderation burden on the owner.** Nothing unpleasant can be said, so nobody has to police
  it — the single biggest reason small operators refuse to add chat at all.
- **It works on a watch.** Jr can show four buttons; it cannot show a keyboard.
- **It translates for free.** The phrases are ids on the wire, rendered in each client's language.
- ★ Extendable per server: an owner may add a few phrases of their own ("QSY to 7.1 please"),
  which keeps it useful for a net without reopening the abuse surface.

## What each client shows
- A small **occupancy strip**: who holds the dial, how many are listening, and a single ASK button.
- The tuner sees the requests and a one-tap **grant**; granting hands over cleanly rather than
  fighting over the next tune message.
- ★ Everyone hears the SAME audio, so a tune is a shared event — announce it briefly on screen
  ("moved to 6.070 MHz") or people think the receiver glitched.

## The wire
Reuse what exists — occupancy is already tracked per session (`occupantSession`, `waiting`,
`reservedFor`). Add: a `tuner` session id, `tuneRequests`, and a canned-message channel carrying
phrase ids. `/vibeserver.json` gains `tuneMode: "exclusive" | "spectator" | "open"`; absent means
exclusive, so every existing client and server stays correct.

## ★★★ WHAT IT COSTS — AND WHY THIS MODE SCALES WHERE THE OTHER ONE DOES NOT

Stuart, 2026-08-19: *"if we limit it to 1 VFO like FM-DX then it is only one CPU decode too, just
showing the same spectrum/audio to each connected user — in fact in that mode you could probably
have lots of users if you had the bandwidth."* Correct, and it is the strongest argument for the
mode:

- **DSP is CONSTANT.** One VFO means one demod and one FFT whether two people are listening or
  fifty. Today's shared path pays `perClientDsp()` per listener — measured at 80–92% of real time
  on the RSP1B with a handful — and that is precisely the cost this mode does not incur.
- **The ENCODE can be shared as well.** Everyone hears the same audio, so it is one Opus encoder
  and one packed spectrum frame, fanned out. Audio is encoded per client TODAY for a good reason
  (two listeners may want different codecs) — a reason that evaporates here for everyone on the
  default. Keep a per-client path only for the odd client asking for something else.
- **So the only thing that scales is BYTES.**

Rough arithmetic per listener: Opus audio ~7.5 KB/s + spectrum ~12 KB/s (1024 bins @ 10 fps)
≈ **20 KB/s ≈ 160 kbps**. So a 10 Mbit domestic upload carries ~60 listeners and a 20 Mbit one
~120, before the radio's CPU is anywhere near the limit.

★ And the levers already exist to make each listener cheaper: the `set_rate` divisor, and a client
  asking for fewer bins (Jr asks for 128). A watch listener costs a fraction of a browser one.
★★ Which means the SETTING an owner needs is not "how many users" but "how much of my upload may
   this radio use" — the honest constraint, and the one they can actually answer.

## Fits with the other brief
[Soft session limits](BRIEF-vibeserver-soft-session-limit.md) answers "how long may I keep the
radio?"; this answers "who may turn the knob?". A shared-tuning receiver arguably needs no session
limit at all — nobody is excluded, so nothing needs rationing.

## Not in scope
Free text, private messages, user accounts, or anything requiring an identity. The whole point is
that a stranger can be polite to another stranger without either of them signing up for anything.
