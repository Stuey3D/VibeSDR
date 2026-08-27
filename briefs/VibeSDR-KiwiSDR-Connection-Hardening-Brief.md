# VibeSDR — Connection Hardening Brief (KiwiSDR / UberSDR / FM-DX)

**Status:** Planning / handoff
**Scope:** Reduce "flaky server" experiences by handling owner-set connection restrictions that VibeSDR currently ignores or mishandles.
**Design principle:** Complexity belongs in the engine, never in the settings. Every backend is an adapter into one normalized set of structures; the UI renders those structures and never branches on protocol.

---

## Problem

KiwiSDR (and Kiwi-protocol servers) enforce owner-set restrictions that VibeSDR doesn't currently account for. When we hit them the app looks broken: silent refusals, dead audio, or apparently random disconnects. Three of these are preventable/explainable client-side; the fourth is a catch-all safety net. The FM-DX session-limit warning we already built misbehaves, most likely because it assumes a limit value the server never sends.

None of these restrictions are reliably discoverable from the directory (rx.kiwisdr.com / ReceiverBook) ahead of time — they are connect-time behaviours. So the model is: attempt the connection, read what the server tells us, normalize it, and let the UI react.

---

## Normalized engine structures (single source of truth)

Every backend adapter populates these. Null / empty = feature inert for that server, UI shows nothing.

```
ident: string                      // persisted user identity, sent on every Kiwi connect

maskedRanges: Array<{              // derived from Kiwi DX labels of type "masked"
  lowHz: number,
  highHz: number
}>

sessionLimit: {                    // null when server announces no limit
  type: "24h" | "inactivity",      // or two separate fields if both are present
  secondsRemaining: number,
  resetsOnActivity: boolean        // true for inactivity, false for 24h
} | null

disconnectReason: enum             // parsed from the socket close / server message
  // e.g. inactivity | time_limit_24h | slot_taken | ip_blocked |
  //      password_required | server_closed | unknown
```

The same `maskedRanges` drives audio mute + tuning guard + waterfall blanking. The same `sessionLimit` drives the clock badge + inactivity pill + connect dialog. No consumer gets its own copy of the truth.

---

## Workstream 1 — Ident / callsign

**Why:** Some Kiwis enable "Require name/callsign entry when connecting" (admin control tab, v1.666+). It's protocol-level, not web-UI-only — the value is sent as `SET ident_user=<name>`. Separately, anonymous non-browser connections are a common blacklist target, so *not* sending an ident is itself a cause of refusals.

**Approach — capture once, always send:**
- Persist an `ident` as a user identity (onboarding or first Kiwi connect), not a per-connection prompt.
- Attach it to **every** Kiwi-protocol connection via `SET ident_user=`.
- Show the entry box only when the stored ident is empty.
- This satisfies servers that enforce the requirement AND stops us looking like the anonymous connections owners block. Do not build detection logic for "does this server require it" — just always send.

**Field constraints:**
- Any non-blank string is accepted (licensed callsign not required).
- Keep the default comfortably **under 16 characters** — that's the minimum length cap an owner can set; longer idents may be truncated/rejected on the strictest receivers.
- Frame it in the UI as "name or callsign" = identity, **not** a chat name. KiwiSDR has no chat function.

---

## Workstream 2 — Masked frequency ranges

**Why:** Owners block frequency ranges (e.g. local broadcast/MW) via the DX label system. A masked range blanks audio + waterfall in that span. It does **not** disconnect you — but dead-air-that-looks-like-a-bug reads as flakiness.

**Exposure:** Masking rides on DX labels (the reserved "masked" label type). DX labels are pushed to the client so the web UI can render them, so **we receive the masked ranges too.** Each masked label gives a centre frequency + passband width → resolve into `[lowHz, highHz]`.

**Three consumers, all off `maskedRanges`:**
1. **Audio mute** — no audio inside a masked span (matches Kiwi).
2. **Waterfall/spectrum blanking** — do not paint signal inside masked columns; draw the blank fill instead (matches Kiwi).
   - Test per-column **by frequency, not pixel offset** (waterfall centre is decoupled from VFO, so the blank must stay locked to frequency when the user pans).
   - Boundary columns: blank if the column's centre frequency is inside the mask; at low zoom, clip the fill within boundary columns if edge precision matters.
   - Scrolling history: blank the **full column** top-to-bottom (simpler, consistent) rather than per-row.
3. **Tuning guard** — velocity-gated behaviour on the drum wheel:
   - **Fling through** a masked span (velocity above threshold) → carry momentum, snap to the far edge in the direction of travel (feels like rolling over a dead patch). Reuses the boundary-wall machinery from VFO-lock/panning phase 1, applied to interior ranges.
   - **Slow dial into** the near edge (below threshold) → soft detent / wall at the near edge, don't fling across.
   - **Direct frequency entry** into a masked range → warning. Attribute to the receiver: "This receiver blocks access to this frequency." Distinguish masked (owner-blocked, in range) from genuinely out of range (below LF limit / above 30 MHz) — different messages. For the masked case, offer the nearest permitted frequency as a one-tap fix.

**Note:** A hardware filter (e.g. HPF below 1.8 MHz) has **no** mask label — it just shows as dead signal. Nothing in the protocol reveals it; nothing we can do but let the disconnect/dead-signal speak for itself.

---

## Workstream 3 — Session limits

KiwiSDR has **two independent** limits with different semantics. Treating them as one countdown is the likely cause of current misbehaviour.

### 3a. 24-hour per-IP limit → persistent clock badge
- Cumulative connection time per IP per 24h. A genuine hard cap.
- Counts down continuously; **does NOT reset on tuning.**
- UI: persistent timer badge near the clock. Consider amber under ~5 min (reuse padlock amber/green language).
- Null/zero (most receivers) → badge absent entirely, no dash/infinity.

### 3b. Inactivity limit → transient warning pill
- Rolling limit; **resets on every freq/mode/zoom change** (including drum-wheel tuning our engine sends).
- UI: a pill above the VTS, shown **only when remaining time crosses a low threshold** (e.g. surface at ~2 min left, not from connect — otherwise it flickers/never shows during active use).
- The pill **self-dismisses the instant the user interacts**, because that same action resets the server timer. Wire the pill's visibility to the reset signal, not to a free-running clock.
- Wording (second person, actionable): "Inactive — receiver will disconnect you in 1:30. Tune to stay connected."

### 3c. Connect-time dialog (when limits announced)
- On connect, if the server announces limit(s), show a one-time informational dialog.
- Attribute to the operator: "This receiver's operator has set usage limits."
- Only mention what was actually announced (if only 24h came through, don't mention inactivity).
- **Dismissible and remembered per receiver** — show on first connect to a given receiver, then suppress (quiet indicator by the clock badge thereafter). Don't block tuning; auto-dismiss after a few seconds or on dismiss.

### Presence-driven rule (fixes FM-DX)
The time-limit UI is a pure function of a real value from the server. Show it **only when a value arrives.** On servers that announce nothing (likely FM-DX, possibly UberSDR), it stays silent — which is correct. Do not assume a limit exists per protocol.

---

## Workstream 4 — Disconnect reasons (the safety net)

**Build this FIRST.** It's the fallback the other three degrade into when a server doesn't behave as assumed.

- Parse the socket close / server message into `disconnectReason`.
- Surface it honestly instead of a generic "connection lost":
  - "Dropped for inactivity"
  - "Receiver time limit reached"
  - "Channel taken by another user"
  - "Receiver refused the connection" (password / IP block)
- Covers everything not preventable client-side: slot loss, IP bans, and **unannounced** limits (a limit enforced but not announced is the worst UX — countdown never shows, drop looks random; labelling the close reason is the honest fix).

---

## Implementation order

1. **Workstream 4 (disconnect reasons)** — the net everything else falls back into. Do first.
2. **Workstream 1 (ident)** — small, high impact, removes a whole class of refusals.
3. **Workstream 2 (masked ranges)** — DX-label parse → mute + blank + guard.
4. **Workstream 3 (session limits)** — clock badge + pill + dialog, presence-driven.
5. **Fix FM-DX warning** — likely just becomes "disable when no limit announced," i.e. the presence-driven rule + the WS4 fallback. May be free once WS3/WS4 exist.

---

## Empirical verification (do before building WS3 + FM-DX fix)

Real hardware access via Nathan may be gone, but the time-limit countdown is **client-facing** (the web UI renders it), so any public Kiwi with a limit set will send the messages to a plain listener.

1. **Capture a real public Kiwi that has a limit set** (busy/popular receivers on rx.kiwisdr.com run aggressive 24h per-IP limits — easy to find). Connect as a normal client, capture the WebSocket frames. Confirm:
   - Exact time-limit message/field format.
   - How the **act vs 24h** distinction arrives (drives reset-on-activity logic — must key off the right one).
   - DX-label payload for masked labels + the passband-width fields (drives `maskedRanges` math).
2. **Capture your own UberSDR instance** (stuey3d.tunnel.ubersdr.org — full control, ideal second sample). Diff against the Kiwi capture. Three outcomes:
   - Same messages as real Kiwi → one parser, works everywhere.
   - Own variant → small adapter into `sessionLimit`.
   - Closes socket with no countdown data → no countdown on UberSDR; fall back to WS4 disconnect labelling.
3. **Capture an FM-DX session** end-to-end. Confirm whether *any* time-limit value is ever sent. If not (expected), disable the feature on that protocol — presence-driven rule handles this.

---

## Backend applicability matrix

| Feature | Real Kiwi | UberSDR (Kiwi-emulated) | FM-DX |
|---|---|---|---|
| Ident (`SET ident_user=`) | Yes | Test — likely yes | N/A |
| Masked ranges | Yes | **No** masking implemented → empty set, inert | N/A → empty set |
| 24h clock badge | Yes (if set) | **Test** (Stuart believes it has limits) | Likely none |
| Inactivity pill | Yes (if set) | Test | Likely none |
| Connect dialog | If announced | If announced | If announced (likely never) |
| Disconnect reasons | Yes | Yes | Yes |

UberSDR emulates Kiwi but does **not** implement all of it (masking confirmed absent). "Has time limits" ≠ "sends the Kiwi time-limit messages" — verify by capture, don't assume.

---

## Open questions to resolve during capture

- Exact Kiwi wire format for both time limits + the act/24h tag.
- Does current Kiwi firmware nudge the cursor away from a mask, or just blank audio? (Affects hard-wall vs soft-warning choice for the tuning guard.)
- Does UberSDR pass `ident_user` straight through, or intercept/transform it? (Affects ident handling for UberSDR-fronted connections.)
- UberSDR session-limit outcome (same / variant / unannounced) → determines whether it needs an adapter or falls back to WS4.

73!
