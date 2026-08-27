# BRIEF: VibeSDR Remote-Control Mesh (the watch as a universal remote)

**Status:** DESIGN ONLY — not for V10. Candidate for the version after. Captured 2026-07-21
while V10 is being finished (watch polish, FT8, button controls).

**One-liner:** the watch (Buddy, and later Jr) can drive *whichever* VibeSDR instance the
owner is sitting at — iPhone, iPad, or Mac — the crown tunes that instance, Handoff-style,
scoped to the owner's own local hardware.

---

## The core realisations (read these first — they kill the obvious wrong turns)

1. **WCSession only ever reaches the paired iPhone.** It physically cannot talk to a Mac or
   iPad. So "the phone relays to the Mac" means watch →(WCSession)→ iPhone →(LAN)→ Mac. The
   phone is a *bridge*; that path only works while the phone is awake and running VibeSDR.

2. **The watch CAN open a direct network socket** — standalone Jr already does exactly this to
   reach SDR servers (`URLSessionWebSocketTask` on watchOS). So "direct connect over WiFi" is
   NOT the abandoned path. What was abandoned (see memory `jr_transport_ws_two_modes`) was
   watch↔phone WS *out-of-house*; direct watch→Mac over the **same LAN** is feasible today.

3. **A VibeSDR instance is currently a CLIENT, not a controllable node.** It views a server; it
   does not accept being remote-controlled. THE REAL BUILD is giving each instance a small
   **control endpoint**: accept `tune/mode/bw/...` and stream back a lightweight state +
   waterfall. Everything else (transports, discovery) is plumbing we already mostly have.

4. **Being connected to the phone does NOT block any of this.** "Buddy connected to phone" ≠
   "Buddy controlling phone." The WCSession link is just the watch's pipe to the phone; what
   the phone DOES with a command is a routing choice. With the phone present, the watch does
   nothing new at all — the phone relays to the target and streams that target's view back over
   the same pipe. Direct watch→Mac is only the no-phone fallback.

---

## Ownership + discovery — the Handoff model, replicated

Goal: Buddy lists and controls ONLY the owner's own VibeSDR instances, and only ones that are
**local right now** — exactly the Continuity/Handoff feel.

- **iCloud account = the ownership gate.** Each instance writes a tiny **presence/identity
  record** into the user's store (device name + a per-user **shared token**). A private
  iCloud store is scoped to one Apple ID — Apple enforces that nobody else can read it. That
  is the "only the owner's hardware" guarantee, for free.
- **Bonjour on the LAN = "is it here now?"** Live discovery via `NWBrowser`, so only instances
  on the same network appear (a home Mac won't show when you're out). Snappier than CloudKit,
  which lags seconds.
- **Token = "prove it."** The iCloud-shared token authenticates the actual LAN connection, so a
  stranger sharing your WiFi who can SEE the Bonjour service still can't control your Mac.

**Split of duties:** `iCloud says WHO is yours (+ token + friendly name)` · `Bonjour says WHO
is here now (+ address)` · `token authenticates the link`. iCloud does NOT need to carry live
addresses — so identity fits in `NSUbiquitousKeyValueStore` alongside the settings/bookmarks
sync (same layer we're already adding for Jr bookmarks). No heavy CloudKit needed for v1.

---

## Transport routing (decided)

Rule: **the watch always talks to the phone if it can; the phone reaches the targeted instance.**

- **Phone present (normal):** watch →(WCSession)→ phone → target.
  - Target = iPhone → phone executes locally (today's Buddy).
  - Target = Mac/iPad → phone relays commands over LAN and streams that instance's
    waterfall/state back to the watch over the SAME WCSession. No new watch-side transport;
    works over Bluetooth exactly like Buddy does now; no extra watch battery.
- **Phone absent, watch on WiFi (fallback):** watch opens a direct LAN socket to the target
  (Jr-style direct path). Only used because the bridge is unavailable.

New watch-side UI: a small **target picker** ("which VibeSDR am I driving?").
New phone-side logic: "relay to instance X" instead of "execute locally." Both are ADDITIVE —
nothing about today's phone-connected Buddy has to be torn down or changed.

---

## Phasing (each step independently useful)

1. **iCloud KV layer** (already planned for Jr bookmarks/settings) — also becomes the
   identity/token backbone for the mesh. Ship this first regardless.
2. **Control endpoint in VibeSDR** — one instance remote-controls another *on the same LAN*.
   Prove it Mac↔phone first, NO watch involved. This is where the genuinely new engineering is.
3. **Buddy target picker** — Buddy lists the owner's instances (iCloud identity + Bonjour
   presence), you pick one; commands route via phone-bridge, else direct LAN. **The
   crown-tunes-the-Mac demo lands here.**
4. **Handoff polish** — battery/latency-aware transport selection; seamless switch as you move
   between devices.

## Open questions for when we pick this up
- Reuse VibeServer's existing WS/PIN handshake as the control endpoint, or a new lighter one?
- Does Jr (standalone) get the target picker too, or Buddy-only at first?
- Waterfall back-channel: full stream, or state-only + let the watch keep its own render (Jr
  already renders its own pixels — could just feed it bins)?
- Presence record TTL / staleness handling so dead instances drop off the list.

## Do NOT start until
V10 is shipped AND Jr local bookmarks + the iCloud KV layer are solid on-device. The mesh sits
on top of that iCloud layer; building it before the foundation is settled is out of order.
