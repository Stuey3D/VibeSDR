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

### The Airspy HF+ — AGC only

Stuart, 2026-08-12: *"I suppose the airspy could be locked to AGC mode only… I believe the AGC on
that is good enough."* So the HF+ gets the limit that fits the hardware it actually has: the admin
may mark a band **AGC-only**, and manual attenuation is refused there while AGC is forced on. That
is an honest control rather than a gain cap wearing the wrong units — and it needs no invented
number, because the radio's own AGC is trusted to do the job.

## What to build

1. **Config** — per radio, alongside `allowRanges` / `blockRanges` (same file, same shape of
   string, same parser family in `vibe_bands.h`): a list of `lo-hi:value` rules. Empty = no limit,
   which is today's behaviour exactly.
2. **Enforcement, server-side** — and in three places, because a cap applied at only one of them is
   a cap a listener can walk around:
   - when a listener SETS gain (`type=="gain"`, `rsp_control`, the Airspy branch) — clamp;
   - when a listener TUNES into a limited band — re-clamp, or maximum gain set on HF simply
     survives the move to FM, which is the exact overload being prevented;
   - when the radio STARTS in a limited band.
3. **Tell the client** — publish the cap with the hardware info so the slider stops at the ceiling
   instead of springing back. ★★ A control that silently undoes what you just did reads as broken;
   one that will not go past a marked limit reads as a rule.
4. **UI** — the setup page, next to the frequency allow/block list it resembles. ★ A "limit to
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
