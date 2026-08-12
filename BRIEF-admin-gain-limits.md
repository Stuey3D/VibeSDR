# BRIEF — admin-settable gain limits, per band

**Status:** specified 2026-08-12 (Stuart), not started.

## The problem, in the owner's words

> "An admin settable gain range. So either have a master gain range or a per band one so that a
> user can limit the gain for instance on Broadcast FM to prevent overload, but then allow
> unlimited gain in HF mode where the signals are weaker." — Stuart, 2026-08-12

> "So users can still set a gain, just limited by the admin."

★★★ **A CEILING, NOT A LOCK.** The listener's gain control keeps working and keeps being theirs to
move; it simply cannot go past the admin's cap in that band. This is the whole shape of the
feature, and it is what separates it from the existing admin gate — which takes the control away
entirely. An owner capping FM does not want to answer gain requests all evening; they want the
front end protected and the listener left alone.

★★ **PER BAND SUBSUMES A MASTER LIMIT** — a master cap is one rule spanning the radio's whole
coverage — so only per-band needs building. The FM/HF example is the point of the feature: the same
receiver wants a hard ceiling at 88–108 and none at all below 30 MHz.

## ★★★ THE THREE RADIOS DO NOT SHARE A GAIN MODEL, AND MUST NOT PRETEND TO

This is the rule from AGENTS.md — *"a control that only works on one radio should be removed, not
left inert"* — and it decides the whole design. There is no common unit to cap:

| radio | control | what a "limit" means |
|---|---|---|
| RTL-SDR | tuner gain, tenths of a dB, from a discrete list | a MAXIMUM gain |
| SDRplay RSP | IF gain **reduction** (dB) + LNA state | a MINIMUM reduction — the inverse |
| Airspy HF+ | no variable gain at all: 8-step attenuator + preamp | **lock to AGC** (see below) |

▶ So the cap is stored **in each radio's own control units**, per radio. The config is per radio and
a radio has one driver, so nothing has to pretend tenths-of-a-dB and gain-reduction-in-dB are the
same quantity. The admin sets it while looking at that radio's own gain control, which is also
where they noticed the overload.

### ★★★ THE RSP'S LIMIT APPLIES IN MANUAL MODE ONLY

Stuart, 2026-08-12: *"on the RSP the limits should only be in manual mode so if AGC is enabled then
it needs the full range."* Exactly right, and it would have been a real bug: a floor on IF gain
reduction is a ceiling the AGC is not allowed to lift, so an AGC left to work inside a clamped
range fights it — reduction pinned at the floor while the AGC asks for less, and the result is a
receiver that sounds wrong in a way nobody would connect to a "gain limit" setting.
▶ So: when `ifAgc` is ON, the limit is not applied at all. It governs the MANUAL control only.
★ Which also means turning AGC on is not a way around the cap — it hands the front end to the
  radio's own loop, which is the thing the cap exists to approximate.

### The Airspy HF+ — AGC only

Stuart, 2026-08-12: *"I suppose the airspy could be locked to AGC mode only… I believe the AGC on
that is good enough."* So the HF+ gets the limit that fits the hardware it actually has: the admin
may mark a band **AGC-only**, and manual attenuation is refused there while AGC is forced on. That
is an honest control rather than a gain cap wearing the wrong units — and it needs no invented
number, because the radio's own AGC is trusted to do the job.

## ★★★ AND A SAFE GAIN TO COME BACK TO

Stuart, 2026-08-12: *"we need a way for the Gain to be reset back to a safe default too… if a user
temporarily boosts the gain to a high level, when they leave the SDR should then drop back to a
safe level. So for my RTL-SDR I'd have it set to return to 19.7 dB for the antenna that it is
connected to. I'd limit it on FM to 25 dB and unlock it for other bands, but always return to
around 19.7."*

★★★ THIS IS THE OTHER HALF, AND IT IS INDEPENDENT OF THE LIMITS. A cap stops a listener going too
far; a resting gain stops what they DID from becoming the receiver's new normal. Without it the
next person — and the owner — inherit whatever the last listener happened to leave behind, which
is exactly how a receiver ends up quietly overloaded with nobody having done anything wrong.
★★ It is also the answer to the opposite failure the code already documents: a receiver that starts
at MINIMUM GAIN reads as broken hardware. A resting gain the OWNER chose for THEIR antenna is a
better default than either extreme, and only the owner can know it.

▶ **Applied when the idle park happens, not the moment the last listener disconnects.** `armIdlePark()`
already defers by the grace period precisely so a page reload is not treated as leaving —
resetting the gain on disconnect would undo a listener's setting every time they reloaded. The
park is where "everybody has gone" is actually decided.
▶ Also at START, so a restart comes back at the owner's gain rather than at whatever was persisted.
★ Per radio, in that radio's own units, exactly like the caps: 19.7 dB is an RTL tuner-gain value.
  For the RSP it is a gain-reduction figure (and only meaningful in manual mode); for the HF+ it is
  simply AGC on.
★★ INDEPENDENT of the limits, and worth stating because Stuart's own example sets both: FM capped
   at 25 dB, other bands uncapped, and ALWAYS back to 19.7 when everyone has left. A receiver may
   have a resting gain and no caps at all, which is the setup most owners will want first.

## What to build

1. **Config** — per radio, alongside `allowRanges` / `blockRanges` (same file, same shape of
   string, same parser family in `vibe_bands.h`): a list of `lo-hi:value` rules. Empty = no limit,
   which is today's behaviour exactly.
2. **A resting gain** — per radio, applied at the idle park and at start. Independent of the caps
   and useful on its own.
3. **Enforcement, server-side** — and in three places, because a cap applied at only one of them is
   a cap a listener can walk around:
   - when a listener SETS gain (`type=="gain"`, `rsp_control`, the Airspy branch) — clamp;
   - when a listener TUNES into a limited band — re-clamp, or maximum gain set on HF simply
     survives the move to FM, which is the exact overload being prevented;
   - when the radio STARTS in a limited band.
4. **Tell the client** — publish the cap with the hardware info so the slider stops at the ceiling
   instead of springing back. ★★ A control that silently undoes what you just did reads as broken;
   one that will not go past a marked limit reads as a rule.
5. **UI** — the setup page, next to the frequency allow/block list it resembles. ★ A "limit to
   here" action from the live gain control is the nicer entry (no unit confusion at all — the admin
   is looking at the radio doing the thing), and the table is how a rule is later reviewed or
   removed. Both, eventually; the table first, since it works with no radio attached.

## Watch for

- ★★ **The RSP's inverted sense.** A "limit" there is a FLOOR on gain reduction. Getting this
  backwards would silently do the opposite of what the owner asked — and the failure looks like the
  feature working (a number went in, a number came out) while the front end is being overloaded.
  Its config already carries two fields for one quantity (`gain` vs `lnaState`), which has bitten
  once — see `sdrplay_gain_bricked_the_radio`.
- ★ **Shared vs personal.** Gain is already behind `sharedGate("gain")` on a locked receiver and
  free on a personal one. A cap is orthogonal: it applies to whoever is permitted to move the
  control at all.
- ★ Loopback and admin sessions: decide deliberately whether the OWNER is capped. Recommendation —
  an authenticated admin is not, since they are the one who set the limit and may need to exceed it
  to diagnose the very overload it protects against.
