# BRIEF — Decoder box lifecycle + expandable spot rows

**Scope:** two related but independently shippable changes to `DecoderPanel.tsx`.

1. **Decoder box lifecycle** — a proper X (close) alongside the existing minimise, with teardown and menu sync.
2. **Expandable spot rows** — an expand/collapse-all toggle that adds a second line carrying the full FT8/FT4 message.

Part 2 has a hard dependency on `msg` reaching `SpotRow`, which may not be satisfiable on all backends. See **Blocking dependency** before starting it. Part 1 has no dependencies and can ship alone.

Not in scope: the menu rework (`USB: DIG` composite labels), OWRX mode labelling, CQ-only filtering.

---

## Part 1 — Decoder box lifecycle

### The problem

`panelOn = !!activeDecoder || isSpotsMode`, and nothing in the demodulator change path clears either flag, so the panel lingers after it stops being relevant. There is a minimise control but no consistent close.

### Behaviour

**Two controls in the header, both always present:**

| Control | Meaning | Effect |
|---|---|---|
| **Minimise** (existing) | "Give me the waterfall back, keep decoding" | Collapses the panel body. Decoder keeps running. |
| **X** (new) | "I'm done" | Full teardown — see below. |

Minimise exists for WEFAX/SSTV specifically: those run for minutes, you are locked to the signal, and the only thing you want back is the spectrum to confirm the signal is still there. It must **not** stop the decode.

**X performs a full teardown:**

- Sets `activeDecoder = null` / `spotsKind = null`.
- **Stops the decoder.** Tears down the native sidecar, or unsubscribes the spots feed. A hidden decoder still burning CPU is the bug this is meant to prevent.
- Clears `decoderText` and the image canvas.
- **For spots: clears the 200-row buffer.** Reopening gives a fresh list, not a twenty-minute-old one with the AGE filter quietly hiding most of it.
- Syncs the demodulator menu — see below.

**No auto-close on demodulator change.** An explicit X makes the heuristic unnecessary, and the heuristic would have been wrong for spots anyway (the UberSDR feed is instance-wide and does not care what you are demodulating; the Kiwi/local sidecar keys off dial frequency, not mode).

### Menu sync

Decoders are strictly **additive secondaries**. USB is the primary that produces the audio; WEFAX is the decoder that consumes it. You cannot have the secondary without the primary.

So closing a decoder removes the secondary and leaves the primary untouched. There is **no previous state to restore** — no `prevMode` stash, no passband restoration.

Concretely: `USB: FAX` → X → `USB`.

> **TRAP:** `activeDecoder` and the menu's highlighted state must read from **one** source. A decoder entry lit in `MenuSheet` while `activeDecoder` is null is the same class of drift as the lingering panel. If the menu holds its own selection state, that is the actual bug.

### Also fix while in here

`openDecoder` and `onSpotsToggle` do not clear each other. Opening RTTY while spots are up (or vice versa) appears to leave both flags set with one silently winning. Verify and make them mutually exclusive.

### `decoderStatus` — leave it alone

Earlier thinking was to repurpose this slot. **Do not.** On Kiwi and local RTL-SDR the spots come from an on-device audio decoder that must be tuned onto the signal, so the status line is the only thing telling the user whether the decoder is getting audio. A "waiting for audio" there is meaningful, not stale.

It is genuinely idle on UberSDR (instance-wide feed, nothing to report), but that is not worth a per-backend divergence in a slot this small.

---

## Part 2 — Expandable spot rows

### Data availability — resolved

**UberSDR already sends everything line 2 needs.** Confirmed against the UberSDR web UI's own Digital Spots table (2026-07-19), which displays twelve columns: Time (UTC), Age, Mode, Freq (MHz), Band, Callsign, Country, Grid, Distance, **Bearing**, SNR, **Message**.

`DecoderClient`'s `digital_spot` parser currently reads only nine fields — `timestamp`, `mode`, `band`, `callsign`, `snr`, `frequency`, `distance_km`, `grid`, `country`. **`message` and `bearing` are being sent and silently discarded.** No protocol work is needed; this is a parser change.

> **TRAP:** the exact JSON key names are not yet confirmed — the web UI's column headings are not necessarily the wire field names. Log one raw `digital_spot` payload before the map and read the keys, rather than guessing at `message` / `msg` / `text`.

Per backend:

- **UberSDR** — extend the parser. Both fields available.
- **Kiwi / local RTL-SDR** — the on-device sidecar decodes the message in full; verify it is emitted through the bridge. Bearing must be **derived** here from `grid` using `bearingDeg` (already used for ADS-B in `OwrxAdapter`), the same way `distKm` is already derived in `startSpotFlush`.
- **OWRX** — `OwrxAdapter` parses `v.msg` in the WSJT default case, but those records go to the decoder *text* output and never become `SpotRow`s. Routing them into the spots pipeline is a separate, larger job. **Out of scope.**

Add to `SpotRow`:

```ts
msg?:     string;   // full FT8/FT4 message text ("CQ DX JA8KSF QN03")
bearing?: number;   // degrees from receiver; server-supplied on UberSDR, derived from grid on-device
```

### Row layout — line 1 (unchanged shape, one reorder)

Current order is `Time · Band · Mode · SNR · Call · Country · Distance`.

**New order:** `Time · Call · Band · Mode · SNR · Country · Distance`

Call moves from fifth to second. It is the identity of the row and reading it after three metadata cells is backwards. Everything else stays, including Mode — the UberSDR list is combined, so with the MODE cycler on ALL you need to distinguish FT8 from WSPR (a −24 dB WSPR spot and a −24 dB FT8 spot say very different things about the path).

> **TRAP:** `spotCall` is currently `flex: 1.2` with `marginLeft: 6`, sitting between two fixed-width cells. In second position it flexes against Band, Mode, Country and Distance instead, so the proportions shift. This is the cell that truncates on the SE if the new ratios are wrong — test there first, in Display Zoom.

Distance and SNR are the two columns the user actually reads. If any width can be reclaimed (Time without the colon, Mode at 30px rather than 38), it goes to Distance — five-digit values (`12,450km`) truncate in the current 52px.

### Row layout — line 2 (new, shown only when expanded)

```
14:32  G4ABC   40m  FT8  -12   ENG    1,240km
       CQ DX G4ABC IO92 · IO92nh · 312° · 14:32:15
```

Line 2 contains, in order:

- **The message**, verbatim (`msg`). The primary payload — this is what tells you CQ from a signal report from a completed QSO.
- **Grid** (`s.grid`) — already captured, never displayed. Shown separately from the message because report and `73` messages carry no locator.
- **Bearing** — server-supplied on UberSDR; derived from `grid` on Kiwi/local.
- **Exact UTC** to the second.

**Frequency is deliberately omitted.** FT8 and FT4 sit on fixed per-band watering holes, so the Band cell already says it more legibly.

Style line 2 dimmer than line 1 — it is context, not the scan target.

### Expand/collapse-all, not per-row

A single boolean in `DecoderPanel`, toggled by a header control alongside the existing MODE / BAND / AGE cyclers. Collapsed = exactly the current single-line list. Expanded = every row two lines.

Rationale: it avoids per-row expansion state entirely, avoids the tap-to-expand versus tap-to-tune collision, and cannot get subtly wrong. Per-row expansion is the better end state but carries the moving-list, memo-props and `getItemLayout` problems all at once. This can be upgraded later without being undone.

The virtualiser renders ~12 visible rows; expanded that becomes ~6. Acceptable, because expanded mode is a mode you deliberately entered to read rather than scan.

### Defaults and resets

**Opens collapsed, every time.** Collapsed is the scanning default; a sticky expanded state means the panel occasionally opens in the wrong shape with no obvious reason why.

> **TRAP — this is the one that will bite.** `DecoderPanel` does **not** unmount when the panel closes. `if (!panelOn) return null;` returns null from render but the component stays mounted, so `useState` survives and a plain `useState(false)` will persist for the entire session. Reset it in the existing appear effect, in the same branch that already does `setMinimised(false)` when `panelOn` goes true.

Also reset when `spotsKind` flips between `digi` and `cw` — different lists, different useful columns.

### Discoverability

Use the existing `Coachmark.tsx` on first open of the spots panel, persisted once dismissed. **Not** a permanent header label — the header already holds the dot, title, `decoderStatus` (with `statusGrow`), and three cyclers, and on the SE that is already fighting.

Wording should say what you get, not what to do: *"tap EXPAND for the full message and grid"*, not *"tap to see more"*. A gesture with an unknown payoff does not get tried.

### Tap-to-tune

Row tap currently calls `onTuneHz(s.freqHz)` and remains the row-tap action in both collapsed and expanded modes — the expand/collapse-all approach means there is no collision to resolve.

**But make it backend-conditional:**

- **UberSDR / OWRX** — keep. The list spans bands, so tapping a 40m spot from 20m is a real jump.
- **Kiwi / local RTL-SDR** — **remove.**

> **TRAP:** on Kiwi and local the on-device decoder only produces spots while you are tuned to the FT8 watering hole. If `freqHz` is the spot's own frequency rather than your dial, tapping a row retunes you off the watering hole and the decoder **silently stops producing spots**, with no error and no explanation. Same conditional-by-backend shape as the tap handler itself.

### FlatList mechanics

- `SpotRowView` is `React.memo`'d — add `expanded` to its props, add it to `renderSpot`'s `useCallback` deps, and set `extraData` on the `FlatList`.
- If `getItemLayout` is set, variable row heights break it. Either drop it or return the expanded height conditionally (heights are uniform within a mode, so the conditional version is straightforward).
- Spots flush every 400ms and **prepend**, so in expanded mode a message you are reading will scroll down under your thumb. Watch for this in testing; if it is bad enough to matter, pausing the prepend while expanded (with a `PAUSED` indicator in the header) is the honest fix.

---

## Noted for later — not in scope

The UberSDR web UI offers filters VibeSDR does not: **Min SNR**, **Min Distance**, **Country**, and a free-text **Callsign** filter, alongside the Mode / Age / Band cyclers VibeSDR already mirrors.

**Min Distance** is the interesting one — it answers "what is the furthest thing I have heard tonight" directly, which is the question this panel largely exists to answer. Worth considering as a fourth cycler (`DIST` → 1000 / 3000 / 5000 km) once the row work has settled. Deliberately deferred: it is orthogonal to this brief and adds header pressure on the SE.

A **CQ-only** filter becomes trivial once `msg` is present (a substring test), and would answer the "scan the session for callable stations" question without any second line at all. Also deferred.

---

## Sideband warning (small, ships with either part)

FT8 and FT4 are **USB by convention on every band**, including 40m and 80m where voice is LSB. A user sitting on LSB for voice who opens the digital decoder gets no decodes and nothing telling them why.

Warn when a decoder that requires USB is opened on LSB, on the Kiwi and local backends. Unlike the existing OWRX warning in `ModeSelector` — which fires on anything outside `COMMON_IDS`, including standalone modes like ADSB and MESHCORE where sideband is a meaningless concept — this one fires only where the sideband genuinely determines whether it works.

---

## Test matrix

- **iPhone SE, Display Zoom** — the layout floor. Call cell truncation after the reorder; expanded row height against the panel height in `dp.wrap`; whether an expanded row pushes rows off the bottom.
- **UberSDR** — combined list with MODE on ALL; message present or absent; tap-to-tune across bands.
- **Kiwi + local RTL-SDR** — tap-to-tune removed; `decoderStatus` still meaningful; spot production survives everything the user can do to the panel.
- **Lifecycle** — X during an active WEFAX decode (does the sidecar actually stop?); minimise during WEFAX (does it keep running?); close and reopen spots (fresh buffer?); close and reopen the panel (collapsed again?).
