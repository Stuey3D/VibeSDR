# VibeDSP 1090 MHz ADS-B / Mode S Decoder — Claude Code Handoff Brief

**Project:** VibeSDR
**Author / copyright holder:** Stuart (STUEY3D)
**Status:** Feasibility-complete, deferred. Build when ADS-B earns a roadmap slot.
**Design principle:** Complexity in the engine, never in the settings. One decoder, deployed twice.

---

## 1. Purpose and rationale

Add on-device and server-side ADS-B (1090 MHz Mode S) decoding to VibeSDR by building a clean-room decoder **inside VibeDSP** (the existing C++/NEON engine). The decoder takes 1090 MHz IQ and emits decoded aircraft state. It does **not** touch any UI: the map (phone), data table (phone + watch), and watch glance-map already exist and render from the aircraft manager. This brief covers the engine only.

**Why build rather than integrate dump1090:**
- On **iOS**, a sandboxed App Store app cannot fork/exec an external binary, so the "run dump1090 as a daemon, read its socket" pattern is unavailable. Using dump1090's code would require static linking, which reintroduces the SDR++ Brown wall: dump1090-fa is GPL-2.0-or-later, VibeSDR is not its copyright holder, and the APPSTORE-EXCEPTION.md §7 additional permission cannot be extended over third-party GPL code.
- Building our own keeps the feature **single-path** (one decoder for every backend and both platforms) and **wholly VibeSDR copyright**.

**Clean-room safety:** ADS-B / Mode S is a published open standard (ICAO Annex 10 Vol IV; RTCA DO-260 series). The algorithm is not copyrightable — same posture already established for the SpyServer wire format and VibeDSP NEON core. **Implement from the specification, not from dump1090 or any GPL source.** No reference to GPL implementations at any point.

---

## 2. Deployment boundary (the key architectural point)

VibeDSP is C++/NEON and compiles into **both** the client and VibeServer (which runs on Android). The 1090 decoder is therefore **one module deployed in two locations**, not two implementations:

| Backend | Where IQ lives | Where decode runs | Notes |
|---|---|---|---|
| **Local hardware** (USB dongle on device) | On-device | Client-side VibeDSP | Mandatory. The iOS-only-option case. |
| **VibeServer** | On the Android server host | **Server-side VibeDSP**, forward decoded aircraft | Sweet spot. Multi-client: decode once, serve all. |
| **RTL-TCP** | Remote dumb IQ pipe | Client-side VibeDSP (no server exists to decode) | Heaviest path — full IQ over network *and* on-device decode. See §7. |
| **OpenWebRX** | Server | Already decoded server-side | Consume existing feed via thin adapter. No VibeDSP involvement. |

**Bandwidth is why server-side decode wins on VibeServer.** Raw 1090 IQ at ~2.4 MS/s is the same ~4.8 MB/s regime that caused the WifiLock dropouts on RTL-TCP. Decoding on the server and forwarding decoded aircraft collapses megabytes/sec of IQ to kilobytes/sec of state, removes the data-rate problem, keeps the client cool, and serves every connected client from a single decode.

---

## 3. Canonical aircraft schema (define this first)

Every backend converges on **one** schema — ideally the exact shape VibeDSP emits natively, so VibeServer forwards it verbatim and only the OWRX adapter does any mapping. The aircraft manager and all four surfaces render from this and never know the decode origin.

```
Aircraft {
  icao24        : string   // 24-bit ICAO address, hex (primary key)
  callsign      : string?  // from TC 1-4 identification frames, trimmed
  lat           : double?  // decoded via CPR (see Stage 4)
  lon           : double?
  altBaro       : int?     // barometric altitude, feet (TC 9-18)
  altGeo        : int?     // GNSS height, feet (TC 20-22)
  gs            : double?  // ground speed, knots (TC 19)
  track         : double?  // degrees true (TC 19)
  verticalRate  : int?     // ft/min (TC 19)
  category      : string?  // emitter category from identification frame
  seen          : double   // seconds since last message (for staleness/removal)
  msgCount      : int       // total frames seen for this aircraft
  rssi          : double?  // optional signal strength, for diagnostics
  positionValid : bool      // true only after successful CPR global decode
  source        : enum { LOCAL, VIBESERVER, RTLTCP, OWRX }  // provenance, for UI/diagnostics
}
```

Notes:
- Nullable fields reflect that any given frame carries only a slice of state; the aircraft manager already fuses these over time.
- `source` lets the UI show provenance and lets connection-recovery logic reason per-backend.
- Keep the wire format for VibeServer→client a compact delta/snapshot of this struct (JSON is fine to start; consider a binary encoding later if aircraft counts get large — not a v1 concern).

---

## 4. Decoder stages

Four stages, each independently testable. **Build in this order** — early stages produce verifiable output before the hard part.

### Stage 1 — Preamble detection + bit slicing (NEON territory)
- 1090 is 2 MHz pulse-position modulation. Work in the **magnitude** stream (VibeDSP already has magnitude/correlation primitives — reuse them).
- Preamble is 8 µs: pulses at 0.0, 1.0, 3.5, 4.5 µs. Correlate the magnitude stream against this known pattern; threshold on correlation strength.
- On a preamble hit, slice the following bits by PPM: each bit is 1 µs; **first-half-high = 1, second-half-high = 0**. Ambiguous/equal halves → mark low-confidence.
- Frame length is determined by DF (see Stage 3): **short = 56 bits (7 bytes)**, **long = 112 bits (14 bytes)**. Slice the long length first; decide validity after DF + CRC.
- This stage is the hottest loop; it plays directly to VibeDSP's NEON strengths (magnitude, correlation, threshold).

### Stage 2 — CRC validation
- Mode S uses a 24-bit CRC (generator polynomial 0xFFF409).
- **The useful trick:** for DF11/DF17/DF18 the parity field is the CRC XORed with an address. For **DF17/18 extended squitter**, a valid frame's computed CRC over the 112 bits resolves to **zero** — a clean CRC both validates the frame *and* confirms the ICAO in the AA field. For DF11, the residual is the interrogator/II code.
- **Gate everything on CRC.** Only CRC-valid frames proceed. This single check removes the overwhelming majority of noise-triggered false preambles.
- **Milestone / dopamine hit:** at the end of Stage 2, emit **raw hex frames** and nothing else. Validate these against any online Mode S decoder or a parallel dump1090 run (dump1090 is fine as an *external validation oracle* — running it separately to check our output is not linking it) *before* writing any field-extraction or position logic.

### Stage 3 — Frame parsing by Downlink Format
- DF = top 5 bits of byte 0.
- Route by DF; the ADS-B payload lives in **DF17** (and DF18 for non-transponder emitters).
- For DF17: ICAO24 is the AA field (bits 9–32). The ME field (bits 33–88) carries the payload, keyed by **Type Code (TC)** = top 5 bits of ME:
  - **TC 1–4** — aircraft identification (callsign + emitter category)
  - **TC 5–8** — surface position
  - **TC 9–18** — airborne position, barometric altitude
  - **TC 19** — airborne velocity (ground speed, track, vertical rate)
  - **TC 20–22** — airborne position, GNSS height
- Extract fields into the canonical schema. Altitude uses the Gillham/25-ft encoding depending on the Q-bit — handle the Q-bit case first (the common one) and the Gillham case second.

### Stage 4 — CPR position decode (the fiddly part — budget your care here)
- Compact Position Reporting encodes lat/lon across paired **even/odd** frames to save bits. Two decode modes:
  - **Global** (unambiguous): needs a recent even+odd pair from the same aircraft within a time window. Produces an absolute position with no prior fix.
  - **Local** (relative): needs a known reference position (previous fix or receiver location); cheaper, used once you have a lock.
- **Edge cases that quietly produce aircraft in the wrong place:**
  - The **NL (number of longitude zones)** function and its **latitude-band boundaries** — off-by-one here misplaces aircraft by whole zones.
  - The **even/odd pairing window** — pairing frames that are too far apart in time yields a valid-looking but wrong position.
  - Latitude-zone consistency check between the even and odd frame — reject the pair if they disagree.
- Set `positionValid` **only** after a successful global decode. Do not surface a position from a single frame.
- Recommend implementing global decode first (works cold), then local as an optimisation once a lock exists.

---

## 5. Build order (independently testable stages)

1. **Stage 1 + 2 → raw hex frames.** Validate against external oracle. No parsing yet.
2. **Stage 3 → DF17 field extraction.** Callsign, altitude, velocity populate; position still empty.
3. **Stage 4 → CPR.** Positions light up. Test hardest here.
4. **Aircraft manager integration** — confirm the existing manager fuses the emitted schema and all four surfaces render unchanged.
5. **VibeServer deployment** — compile the same module server-side, wire decode-and-forward, confirm client renders identically with `source = VIBESERVER`.

Each stage gates the next; each has a standalone test target.

---

## 6. Interface boundary (VibeDSP ↔ rest of app)

- **Input:** an IQ tap at 1090 MHz, ~2.4 MS/s, magnitude-domain. Define a clean `feedIQ(samples)` entry that both the local-hardware path and VibeServer's IQ source call.
- **Output:** a stream/callback emitting canonical `Aircraft` deltas into the aircraft manager. Identical signature client-side and server-side.
- The decoder is **stateless above the aircraft-tracking layer** except for CPR pairing state and per-aircraft fusion — keep that state inside the module so both deployment points behave identically.
- No UI, no networking, no settings inside VibeDSP. VibeServer owns the forward-to-client transport; the client owns rendering.

---

## 7. Practical / operational notes

- **Sample rate & heat:** 1090 wants ~2–2.4 MS/s continuous — heavier and hotter than normal HF/VHF listening. Treat ADS-B as a **mode that takes over the tuner**, not something layered on a listening session. Applies to the client (local hardware) especially.
- **RTL-TCP is the awkward backend:** `rtl_tcp` is a dumb IQ pipe, not a VibeServer, so it **cannot** decode server-side. That path pulls full IQ over the network *and* decodes on the phone — heaviest combination, and the same ~4.8 MB/s that needed WifiLock tuning. **Guidance to surface to users:** for a *networked* dongle doing ADS-B, prefer VibeServer over plain RTL-TCP, because VibeServer can decode-and-forward and rtl_tcp structurally can't.
- **Antenna reality:** decode quality falls off a cliff at 1090 MHz with a poor antenna. A user on the stock whip may see 3 aircraft, not 30, and conclude the *decoder* is broken. Add gentle in-app framing so the antenna gets the blame it deserves, not the engine.
- **Connection robustness:** on VibeServer/RTL-TCP paths, the aircraft feed must be covered by the same **wsGen generation-counter / zombie-WebSocket recovery** used for the waterfall. A frozen aircraft table or map must not masquerade as empty sky. Reuse the SF Symbols hop-diagnostic glyphs as the connection-state indicator so "link dead" reads differently from "sky empty."
- **dump1090 as oracle, not dependency:** running dump1090 separately to cross-check our hex/positions during development is fine and recommended. It never enters the build.

---

## 8. Out of scope (v1)

- UPLINK / interrogation, TIS-B, ADS-R fusion beyond what the aircraft manager already does.
- Mode A/C (non-Mode-S) decoding.
- 978 MHz UAT (US-only; separate band and standard).
- Historical track persistence beyond the manager's existing behaviour.
- Binary wire encoding for VibeServer→client (JSON snapshot/delta is the v1; revisit only if aircraft counts stress it).

---

## 9. Definition of done

- VibeDSP emits canonical `Aircraft` structs from 1090 IQ, CRC-gated, with CPR global positions.
- Same module runs client-side (local hardware, RTL-TCP) and server-side (VibeServer), producing identical schema.
- Phone map, phone table, watch table, and watch glance-map render the feed with no surface-specific changes.
- OWRX adapter maps its JSON into the same schema.
- Four sources, one schema, uniform rendering — no user-facing mode switches beyond "ADS-B on."

---

*73! — build the engine once, deploy it twice, keep the settings dumb.*
