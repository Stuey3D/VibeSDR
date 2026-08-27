# VibeServer — Multi-Client (Per-Radio) Model — Claude Code Handoff Brief

**Status:** design locked, ready to implement
**Supersedes:** the "first-connected user owns the RF centre, everyone else rides the same 2.4 MHz" model from the earlier VibeServer protocol foundations work.
**Related:** VibeServer protocol foundations brief (session tokens, per-hardware access levels); FM Broadcast Scan brief (v10 scanner). This document is the source of truth where the two overlap.

---

## 0. Why this exists — the one decision everything hangs off

An RTL-SDR only sees ~2.4 MHz of spectrum instantaneously. That single physical fact drives the whole design.

A KiwiSDR can give every connected user an independent VFO because it digitises the *entire* 0–30 MHz band — the hardware already captured everything, so independent tuning is just software slicing what's already there. An RTL-SDR captures only a 2.4 MHz window around one LO. There is nothing to slice across users. So the old model — first user owns the centre, everyone else gets whatever 2.4 MHz that user happens to be sitting on — is antisocial by construction: if user 1 makes big jumps or tunes continuously, users 2..N get a garbage experience they can't control.

**The fix: virtualise at the hardware layer, not the VFO layer. One physical radio per user.** A server with N dongles serves N simultaneous controlling users. This trades maximum user count for guaranteed per-user quality — the right trade for a non-public enthusiast server (owner + a few mates), not a 100-slot public WebSDR.

Everything below follows from this. Keep the reasoning in the engine; keep the settings simple.

---

## 1. Radio enumeration & capacity — platform-agnostic

There is **no per-platform radio cap.** A Raspberry Pi, a macOS host, and a high-end phone (e.g. a Galaxy S26 with USB3 + a powered hub) all run identical logic. "One dongle per phone" is **documentation guidance, not a code branch** — most phones are plugged in without a powered hub and an RTL-SDR v4 will sag an unpowered port, but a well-provisioned phone genuinely is a multi-radio server and the software must not pretend otherwise.

- On startup, the server enumerates every RTL-SDR it can reach.
- The **owner** chooses how many of the enumerated radios to expose (see §2).
- The real ceiling is **host throughput/power, discovered at runtime, not declared.** 3× 2.4 Msps ≈ 14 MB/s raw USB plus per-dongle DSP. Exceeding what the host can sustain shows up as **RTL-SDR sample drops**, not a clean error.
- Implement a **runtime health guard**: monitor dropped-sample counts / buffer underruns per stream. If a host is over its real ceiling, surface it (log + admin-visible warning + optionally refuse to bring the N-th radio online) rather than silently corrupting everyone's audio.

**Do not** hardcode "phone → 1". Enumerate, let the owner expose, guard at runtime.

---

## 2. Capability descriptor (per radio)

Each radio carries a descriptor. This is the single abstraction the rest of the system derives behaviour from — there are no user-facing "modes."

```jsonc
{
  "id": "rtlsdr-serial-or-index",   // stable per physical device
  "name": "HF Longwire",             // owner-set, human-facing
  "antenna": "80m OCF dipole",       // label shown in the picker
  "freqMin": 500000,                 // Hz — owner-imposed limit (may be < tuner range)
  "freqMax": 30000000,               // Hz
  "gainPolicy": "auto",              // or fixed value / AGC policy
  "ppm": 0,                          // calibration
  "public": true,                    // false = admin-reserved "dark" dongle (§4, §7)
  "controls": {                      // PER-CONTROL policy, owner's choice — see below
    "biasT":          "hidden",      // free | locked | hidden
    "gain":           { "policy": "range", "min": 0, "max": 300 },
    "autoGain":       "free",
    "ppm":            "locked",
    "agc":            "free",
    "directSampling": "hidden",
    "sampleRate":     "locked"
  }
}
```

### ★ Per-control exposure — a PRODUCT feature, not a demo one

**The admin chooses, per control, exactly what a guest gets.** Not one blanket lock: hide bias-T
entirely, restrict gain to a range, leave AGC free, lock PPM — every hardware control is
independently settable.

Three states, and the difference between the last two is meaningful rather than cosmetic:

| policy | client behaviour | says to the guest |
|---|---|---|
| `free` | normal control | yours to change |
| `locked` | **shown, greyed, with a reason** | exists, but the owner holds it |
| `hidden` | not rendered at all | not part of this receiver |

…plus **`range`** for continuous controls, where the owner permits a subset rather than all-or-nothing
— chiefly gain, e.g. capping it below the point where a strong local signal desensitises the front
end for everyone.

★ **Default to `locked` for anything unspecified.** A descriptor from an unknown server that omits a
control must fail CLOSED — an older server talking to a newer client should never hand out a hardware
control it never agreed to expose.

★★ **The default profile: GAIN, and nothing else.** Stuart: *"in most cases for an RTL-SDR the only
control a user will need is the Gain Slider."* Everything else is the owner's calibration of their own
station, so:

```jsonc
"controls": {                     // DEFAULT for a shared RTL-SDR
  "gain": "free", "autoGain": "free",   // the one thing a guest actually needs
  "biasT": "hidden", "ppm": "hidden", "agc": "hidden",
  "directSampling": "hidden", "sampleRate": "hidden"
}
```

This makes `hidden` the common case, which is the RIGHT default for the UI as well as for safety: a
guest sees **one slider**, not a panel of dead controls. `locked` is then reserved for the rarer case
where an owner deliberately wants a guest to SEE that something exists but is theirs to hold.

Someone sharing a radio for the first time should be safe by default, not safe only if they read the
docs — and the guest gets a cleaner screen out of the same decision.

### ★ The one exception: DIRECT SAMPLING on dongles without an upconverter

An RTL-SDR **v4 has a built-in upconverter**, so HF works natively and direct sampling is meaningless
there — hide it. A **v3 or clone does not**: direct sampling (Q branch) is the ONLY way it reaches HF.
Hide it on one of those and a guest simply cannot hear anything below ~24 MHz.

So the default should be **derived from the hardware and the radio's own frequency range**, not fixed
— consistent with §3's "derived, never selected":

| dongle | radio's freqMin/freqMax | direct sampling |
|---|---|---|
| v4 (has upconverter) | anything | **`hidden`** — meaningless |
| v3 / clone | entirely HF (< ~24 MHz) | **`locked` ON** — the owner's antenna is HF; the guest never needs to touch it |
| v3 / clone | spans HF *and* VHF/UHF | **`free`** — the guest MUST be able to switch, or half the radio is unreachable |

Note the third row is a genuine trap: on a v3 with direct sampling ON, **VHF/UHF stops working**. A
guest who switches it and cannot switch back has broken the receiver for themselves — which is an
argument for exposing it *with* a clear label, not for hiding it.

A guest on someone else's radio must not be able to change the hardware. This is not primarily a UX
concern:

- **Bias-T has PHYSICAL consequences.** Switching it OFF kills DC to an active antenna or LNA and
  degrades reception for the owner and everyone after them. Switching it ON pushes DC into a feed
  that may not be expecting it. A stranger should never hold that switch.
- **Gain / AGC / PPM / direct sampling / sample rate** are the owner's calibration of their own
  station. A guest changing them is vandalism-by-accident.

The **client already knows how to do this** — `lockedRate` is advertised precisely so the picker is
**hidden** rather than *"offering a control whose every use is silently dropped"*, and `maxFftRate`
was added the same way (2026-07-19). `locked[]` generalises that: the server names what it will not
accept, and the client hides or greys accordingly.

**Hide vs grey:** hide what is meaningless to a guest (sample rate, centre); grey **with a reason**
where they would otherwise wonder where it went (*"fixed by the owner"*). All-dead reads as broken;
all-missing reads as feature-poor — the reason text avoids both.

★ This is also why the public demo server needs **no forked client**
(`BRIEF-public-demo-server.md` §2b): it is simply an owner who has locked everything.

- `public: false` → the radio is **never advertised**, never appears in any picker, never counts toward public capacity. It is the admin's private dongle (§7).
- `freqMin/freqMax` are owner-chosen constraints and may be narrower than the tuner's physical range (e.g. an airband-only dongle).
- The descriptor is edited through the existing **one-config-schema / three-editors** design (GUI + admin web config page + SSH/JSON). No new config surface.

---

## 3. Pool vs. picker — derived, never selected

The owner never picks "pool mode" or "picker mode." The client **derives** the connect UX from whether the exposed descriptors are distinguishable:

- **All exposed radios identical** (same antenna + coverage — e.g. 3× RTL-SDR v4 on a splitter fed from one wideband powered antenna): every radio is interchangeable, so the client **auto-assigns** the next free one and shows **no picker**. No user gets a sub-par experience because there is no meaningful choice to make.
- **Exposed radios differ** (different antennas / coverage): the client shows a **splash-screen picker** so the user chooses the radio covering the band they want. The owner sets up each dongle and its limits; the picker presents them.

The picker reuses the existing **instance-picker card grammar**. Cards lead with **coverage**, not hardware identity — e.g. "HF 0.5–30 MHz · 80m OCF dipole", not a dongle serial. Show **per-radio availability** (see §4), because "full" is per-capability: if the single airband dongle is busy, an airband user is blocked even with two HF dongles free.

---

## 4. Occupancy & advertising — three states, not a boolean

A radio's public availability is **not** "has a user? / no user?". It is a function of *both* occupant slots (controlling user **and** admin presence):

| State | Meaning | Advertised as free? |
|---|---|---|
| `free` | no controlling user, no admin presence | **yes** |
| `user-held` | a user controls the LO (grace-held counts as held) | no |
| `admin-held` | admin present (tapping or controlling), regardless of user | no |

**Rule:** a radio advertises as available only when it has **neither** a controlling user **nor** any admin presence (tap or control). Admin presence alone keeps it **dark-but-occupied**.

Consequence to implement carefully: if an admin takes control and the user then leaves, the radio is occupied by the admin only — it must **stay dark and not re-advertise** until the admin also leaves. Advertise only when *both* are gone.

Public capacity advertised to the directory/QR connect = `count(public && free)`. Reserved (`public:false`) dongles never contribute. Consider advertising "2 of 3 free" so a user sees capacity **before** connecting rather than after.

---

## 5. Session lifecycle — first-come control + warm-dongle grace

- **First-come control.** A connecting user is assigned a radio (auto or via picker) and controls its LO for the duration of their session.
- **Drop handling (grace window).** If a connection drops (bad internet, tunnel blip, phone loses signal), the **specific dongle holds that session** for a grace window — default **60 s, admin-configurable**. If the user reconnects within the window they get their session back; if not, the window expires, the radio is freed and re-advertised.
- **Token → radio affinity (warm dongle).** The held session is bound to the **same physical device** via the existing session-token work. On reconnect the user lands back on the *same warm dongle*: gain, PPM, bias-tee state and USB enumeration are already applied and streaming — **zero re-init, no cold-start, no USB re-enumeration hiccup**, and (critically on a multi-antenna server) they can't be silently handed a *different* antenna/band. Handing back "any free dongle" would be both colder and wrong; always return the same device.

Why the grace window is safe here and wasn't under the old model: holding a dropped session holds *that user's dongle*, which with N radios blocks nobody except the (N+1)-th user. Under the old shared-centre model, holding a session held the whole receiver hostage. The grace window and the per-radio decision are the same decision viewed twice.

---

## 6. The invariant (call this out in code comments)

> **At any instant, exactly one occupant controls a radio's RF centre (LO). Every other session on that radio is a passive listener with an independent audio-VFO inside the shared 2.4 MHz window.**

This is the property the entire redesign exists to guarantee. The admin model (§7) is allowed to move the *label* "controller" between the user and the admin — via an announced transition each way — but never to have two sessions driving one LO at once. Any code path that could put two tuners on one LO is a bug.

---

## 7. Admin model — a four-state baton

The admin is **not a user of a radio; the admin is a user of the *server*.** A public user's session is bound to a dongle. The admin's session is bound to nothing until they *choose* an intent. This is why "all dongles full" is not a wall for the admin — it is their front door.

Admin auth: password. Admin password **always** works, even at full capacity. On admin connect, all users get message ① (§8).

The admin then has four distinct, self-chosen intents — none forced by connecting:

1. **Overview (consumes no dongle).** A live, admin-privileged render of the existing session table: per active radio show the user, connect duration, current RF centre, mode/VFO, SNR, antenna/capability label, and grace-hold status. This is the "I only joined to change a backend setting / monitor, I don't want to listen" case. No dongle is consumed. This is a real screen with its own lifecycle, not an error state. It is also the landing screen when the admin authenticates at full capacity.

2. **Private-dark.** The admin listens on the reserved `public:false` dongle (§2, §4). Invisible to everyone; consumes the reserved radio only. This is the admin's private receiver and is the *only* place an admin listens without touching anyone else's session.

3. **Passive tap.** The admin drops onto an **in-use** radio to hear what that user hears. Architecturally this is a **fan-out of one radio's stream to two destinations** (user audio + admin audio) — **the admin is read-only on the LO; only the user's tuning input is wired to the hardware centre.** The admin gets an independent listen-VFO *within* the user's existing 2.4 MHz window (pure DSP, no hardware command). The tapped user gets message ② (§8). Example: user is on 40m, admin taps and listens to 41m in the same window — both happy, nothing moves.

4. **Announced takeover — and return.** The admin explicitly seizes the LO. This is the **one** moment the §6 controller label flips from user to admin. It is a deliberate, announced action (message ③), **never** a silent slide out of the passive tap. On takeover the **user is demoted, not evicted**: they keep their session, remain on the radio, and keep an independent listen-VFO they can move within the window — they simply no longer control the RF centre. Control can hand **back**: when the admin releases the LO (or leaves), control returns to the user with message ④. If the admin fully leaves while the user is still present, the user resumes control (message ④). If the user left while the admin held control, the radio stays `admin-held`/dark until the admin also leaves (§4).

**Drop vs. bump vs. takeover — different causes, different lifecycles.** A *network drop* gets the §5 grace hold. A *takeover* is not a bump — the user is not freed, they are demoted in place and keep their session. There is no scenario in this model where an admin action hard-boots a user; that is the whole social point.

---

## 8. Transition messages — fire once on state entry

Four messages. Each fires **once on the state transition** (not repeated on every admin action), and each except ① is **scoped to the affected radio's user(s)** only. Wording is deliberately **control-not-session**: an admin costs a user *control*, never their session, so no message may imply a boot.

- **① Admin connects (all users):**
  *"An admin user has connected and may take control of this SDR. You won't be disconnected — if that happens you'll keep listening but lose tuning control, and can leave if you prefer."*

- **② Admin taps this radio (this radio's user only):**
  *"An admin user is also using this SDR; you can continue to tune and use as normal, however an admin may take control at any time."*

- **③ Admin takes control (this radio's user only):**
  *"An admin user has now taken control of this SDR; you may continue listening, however the SDR may change frequency without notice."*

- **④ Control returns (this radio's user only):**
  *"Control of this SDR has returned to you."*

Message ④ is essential: a demoted user has no other signal that they're driving again and will otherwise sit idle assuming they still can't tune.

---

## 9. VFO / RF-centre rule — controller-driven, window-preserving, edge-follow

This governs how the shared 2.4 MHz window moves and what happens to passive listeners. It is a **single boundary rule**, not a magnitude rule — there is no "slight vs. large move" threshold to define.

**Principle:** the RF centre is controller-driven but *window-preserving*. It moves only when the controlling VFO would otherwise leave the passband, and it moves the **minimum** needed. Passive VFOs are left exactly where parked as long as they remain in-window.

- **Takeover does *not* move the centre.** If user (40m) and admin (41m) are both already inside the window, seizing control changes only *who holds the LO*, not where it points. No gratuitous snap.
- **Controller tunes within the window** → centre unmoved, passive VFOs untouched.
- **Controller tunes toward the edge** → the centre shifts by the **minimum** required, using **edge-follow**: bring the controller's VFO to the window edge **plus a small guard margin**. Edge-follow (not recentre-on-controller) is chosen deliberately because it leaves the **maximum** room on the far side, so any passive VFO survives in-window as long as physically possible.
- **Passive VFO snap (last resort only)** → a passive VFO (the demoted user's, or the admin's while tapping) is moved **only** at the instant the centre has been dragged far enough that the passive VFO would fall **outside** the new window. Then it snaps to the nearest in-band point (or centre).
- **Hard ceiling:** two VFOs more than one window (minus guard margins) apart **cannot** coexist. This is best-effort coexistence within one window's width, not a guarantee. When it can't be kept, the **controller wins** and the passive user is moved (consistent with §6).

Net UX: the takeover warning ③ ("may change frequency without notice") stays literally true, but the user's frequency now changes **later and less often** than a snap-on-takeover model — only when genuinely dragged out. The lived experience is gentler, which is the point.

---

## 10. Interaction with the FM Broadcast Scan (v10 scanner)

The per-radio model is what makes the whole-band FM scan **socially safe**. An FM broadcast scan hops the LO across ~20 MHz for a few seconds (see the FM Broadcast Scan brief). Under the old shared-centre model, one user scanning would drag every other connected user across 20 MHz of dead air. Under one-radio-per-user, a scan only ever hops the **scanning user's own dongle**. An admin *tapping* that radio sees the sweep happen live but is **read-only** and cannot fight it. The two features are mutually reinforcing; no special-casing required.

Note also: VibeServer is currently single-client and owns its LO, so it behaves like local USB for the scanner's purposes — the centre-hopping survey works on VibeServer today, exactly as on a local dongle.

---

## 11. State diagram (per radio)

```
                        ┌─────────────────────────────────────────────┐
                        │                                             │
        (advertise) ┌───▼───┐  user connects        ┌──────────────┐  │
   ─────────────────► free  ├───────────────────────► user-held    │  │
                    └───▲───┘                        │ (user = LO)  │  │
                        │                            └──┬────────┬──┘  │
        grace expires   │                    admin taps │        │ user drops
        (window freed)  │                     (msg ②)   │        │ (session
                        │                               ▼        ▼  held 60s)
                    ┌───┴────────────┐        ┌────────────────────────┐
                    │ user-held      │        │ user-held + admin-tap   │
                    │ (grace hold,   │        │ (user = LO, admin R/O)  │
                    │  same warm     │        └──────────┬─────────────┘
                    │  dongle)       │      admin takeover│ (msg ③)
                    └───▲────────────┘                    ▼
              reconnect │              ┌──────────────────────────────────┐
              in window │              │ admin-held + user-passive         │
                        │              │ (admin = LO, user demoted, keeps  │
                        └──────────────┤  session + independent listen VFO)│
                                       └───┬───────────────────┬──────────┘
                              admin releases│ (msg ④)   user leaves│
                              → back to      │                    ▼
                              user-held +    │        ┌────────────────────────┐
                              admin-tap      │        │ admin-held (no user)    │
                                             │        │ STAYS DARK — not        │
                                             │        │ advertised until admin  │
                                             │        │ also leaves             │
                                             │        └───────────┬────────────┘
                                             │      admin leaves   │
                                             ▼                     ▼
                                    (user resumes control)     → free (advertise)
```

Admin **overview** and **private-dark** sit outside this per-radio diagram: overview consumes no radio; private-dark occupies only the reserved `public:false` dongle, which never enters this advertising cycle.

---

## 12. Owner-configurable knobs (keep minimal)

- Grace window duration (default 60 s).
- Which enumerated radios are exposed, and each radio's descriptor (§2), incl. the `public:false` dark flag.
- Per-radio `freqMin/freqMax`, gain policy, PPM.
- (Optional, later) whether the reserved dark dongle may *spill over* into the public pool at low priority when the admin isn't using it, yanked back instantly on admin demand. **For v1: keep the reserved dongle always-dark.** Note spillover as a future option only.

Everything else is derived. No "mode" toggles.

---

## 13. Test matrix (minimum)

- Identical-radios server → no picker, auto-assign, N users get N radios, (N+1)-th sees "full".
- Mixed-radios server → picker shows coverage-led cards + per-radio availability; blocking is per-capability (busy airband dongle blocks airband user with HF free).
- Drop + reconnect within grace → **same warm dongle**, no re-init, lands where left off.
- Drop + grace expiry → radio freed and advertised; a waiting user can take it.
- Admin connect at full capacity → lands in overview; all users saw msg ①.
- Admin tap → fan-out audio, user retains LO, admin R/O, user saw msg ②; admin VFO independent in-window.
- Admin takeover with both VFOs in-window → **centre does not move**; user saw msg ③, keeps session, can still tune in-window.
- Admin tunes to edge → edge-follow shifts centre minimally; passive user VFO preserved until forced out, then snaps.
- User leaves while admin controls → radio stays dark; only advertises after admin also leaves.
- Admin releases / leaves with user present → control returns, user saw msg ④.
- Reserved `public:false` dongle → never in any picker, never in advertised capacity.
- Runtime over-throughput host → sample-drop guard fires; N-th radio flagged/refused rather than corrupting streams.
- FM scan running on one radio → other radios/users unaffected; admin tap sees sweep live, read-only.
