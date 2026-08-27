# BRIEF — Repeaters: OWRX differentiation fix + geo repeater lists

**Status:** Design brief for Claude Code implementation
**Target:** v10 (companion to the Airband brief — shares the download gate, location resolution
and grouped-card UI)
**Author:** Stuart Carr (Stuey3D), with Claude
**Depends on:** Airband brief §5.1a (download gate), §5.2 (location resolution), §9 (UI patterns)

---

## Summary

Three pieces of work, in ascending order of size:

1. **Bug: OWRX repeaters are not reliably tagged.** The existing detection in
   `OwrxAdapter.ts` regex-matches an English prose description, and one of its two branches
   *never matches at all*. Root-caused in §1. Small, self-contained fix.
2. **Upstream one-liner to OpenWebRX+** that makes the guessing unnecessary — and gives us
   EiBi source tagging for free. §2.
3. **Feature: geo repeater lists for VibeServer and Local Hardware**, which have no server to
   supply bookmarks. This is the airband pattern again, but the *data* situation is
   materially worse and the brief is honest about that. §4.

---

## 1. Bug — OWRX repeater bookmarks are not differentiated

### 1.1 Current code

`src/services/OwrxAdapter.ts`, in the `bookmarks` message handler:

```ts
// Repeater-DB entries carry an auto-generated "On-air, Nkm away. Last
// updated …" description — use that signature to tag them vs user bookmarks.
.map((b) => ({
  name: String(b.name ?? b.modulation ?? b.frequency),
  frequency: b.frequency,
  mode: b.modulation,
  repeater: /\bkm away\b|on[- ]air|repeater/i.test(String(b.description ?? '')),
}));
```

### 1.2 What OWRX actually sends

From `owrx/web/repeaters.py :: getDescription()`, the description is built as:

```
"{status}, {N}km away. Last updated {date}. {comment}"
```

e.g. `"On-air, 12km away. Last updated 2024-03-01."` or `"Off-air, 7km away. …"`.

### 1.3 The two failures

**(a) `\bkm away\b` never matches. Ever.** `\b` requires a word/non-word transition, and in
`"12km away"` the character before `k` is a digit — both are word characters, so there is no
boundary. The branch is dead code. Verified:

```js
/\bkm away\b/.test('12km away')   // → false
```

**(b) `on[- ]air` matches "On-air" but not "Off-air".** So *only on-air repeaters* are ever
tagged, and every off-air one is silently classified as a plain user bookmark. Entries with
no `status` field (description begins "Last updated…") match nothing at all.

Net effect: the feature half-works, inconsistently, with no error — which is precisely the
reported symptom ("for some reason our app doesn't differentiate them"). Worth internalising
the general lesson: **a heuristic that fails silently is worse than one that throws.** If the
regex had been asserted against a known sample at startup, this would have surfaced in
minutes.

### 1.4 The real discriminator, which OWRX does not send

`owrx/bookmarks.py` tags every `Bookmark` with `srcFile`:

| Source | `srcFile` |
|---|---|
| RepeaterBook / ARD | `"RepeaterBook"` |
| EiBi schedules | `"EIBI"` |
| `bookmarks.d` file | the filename |
| User bookmark | `None` |

This is authoritative, unambiguous, and **not serialised** — `Bookmark.__dict__()` emits only
`name`, `frequency`, `modulation`, `underlying`, `description`, `scannable`. So the client is
forced to guess from prose. That is the underlying cause; §2 fixes it at source.

### 1.5 Fix (client-side, works on today's servers)

We do not control the servers our users connect to, so this is needed regardless of §2.

```diff
       case 'bookmarks':
         this.owrxBookmarks = ((json.value || []) as any[])
           .filter((b) => b && typeof b.frequency === 'number')
-          .map((b) => ({ name: String(b.name ?? b.modulation ?? b.frequency), frequency: b.frequency, mode: b.modulation,
-                         repeater: /\bkm away\b|on[- ]air|repeater/i.test(String(b.description ?? '')) }));
+          .map((b) => ({
+            name: String(b.name ?? b.modulation ?? b.frequency),
+            frequency: b.frequency,
+            mode: b.modulation,
+            ...classifyOwrxBookmark(b),
+          }));
         this.emitBookmarks();
         break;
```

```ts
/**
 * Classify an OWRX bookmark by origin.
 *
 * OWRX tags bookmarks internally with srcFile ("RepeaterBook" / "EIBI" / a
 * bookmarks.d filename / None for user bookmarks) but does NOT serialise it
 * (owrx/bookmarks.py :: Bookmark.__dict__). An upstream PR adds it — prefer it
 * whenever the server is new enough to send it.
 *
 * Fallback for older servers: sniff the auto-generated description, which OWRX
 * builds as "{status}, {N}km away. Last updated {date}. {comment}"
 * (owrx/web/repeaters.py :: getDescription).
 *
 * NB the previous regex used `\bkm away\b`, which NEVER matched — \b fails
 * between the digit and the 'k' in "12km away" — so only "On-air" entries were
 * ever tagged and every "Off-air" one was silently missed.
 */
function classifyOwrxBookmark(b: any): { repeater: boolean; source: 'eibi' | 'server' | 'user' } {
  // Authoritative path (server sends srcFile).
  if (typeof b.srcFile === 'string' || b.srcFile === null) {
    const s = b.srcFile;
    if (s === 'RepeaterBook') return { repeater: true,  source: 'server' };
    if (s === 'EIBI')         return { repeater: false, source: 'eibi' };
    if (s === null)           return { repeater: false, source: 'user' };
    return { repeater: false, source: 'server' };   // a bookmarks.d file
  }

  // Fallback: description sniffing.
  const d = String(b.description ?? '');
  const isRpt =
       /\d\s*km away/i.test(d)          // "12km away" — deliberately no \b before km
    || /\bo(n|ff)[-\s]?air\b/i.test(d)  // on-air AND off-air
    || /\brepeater\b/i.test(d);
  return { repeater: isRpt, source: 'server' };
}
```

**Add a unit test with the real strings**, including at minimum:

```
"On-air, 12km away. Last updated 2024-03-01."     → repeater
"Off-air, 7km away. Last updated 2023-11-02."     → repeater   ← regression: was false
"Last updated 2024-03-01."                        → not repeater (no signal)
""                                                → not repeater
{ srcFile: "RepeaterBook" }                       → repeater   (authoritative wins)
{ srcFile: null }                                 → user bookmark
```

This test is the point of the exercise. A silent heuristic without one is how we got here.

---

## 2. Upstream PR to OpenWebRX+ (`luarvique/openwebrx`)

One line, additive, no behaviour change to the OWRX web UI:

```diff
     def __dict__(self):
         return {
             "name": self.getName(),
             "frequency": self.getFrequency(),
             "modulation": self.getModulation(),
             "underlying": self.getUnderlying(),
             "description": self.getDescription(),
             "scannable": self.isScannable(),
+            "srcFile": self.getSrcFile(),
         }
```

**Why it is worth doing rather than just living with the fallback:**

- It removes prose-sniffing from a wire protocol, which is the correct thing on principle.
- **It gives VibeSDR EiBi source tagging for free.** `src/services/stations.ts` already
  declares `source?: 'eibi' | 'server' | 'user'` — currently unpopulatable from OWRX. One
  line fixes both problems.
- It is exactly the kind of small, obviously-correct contribution that gets merged, and it
  puts VibeSDR on the right side of the "independent clients are welcome" principle we have
  applied everywhere else.

**Also report to luarvique (separate issue):** the ARD fallback URL in `owrx/web/repeaters.py`
is **dead**:

```
https://raw.githubusercontent.com/Amateur-Repeater-Directory/ARD-RepeaterList/
    refs/heads/main/MasterList/MasterRepeater.json   → 404
```

The ARD project appears to have restructured or rebranded. This is a **latent** bug, not an
active outage — RepeaterBook is tried first and works, so servers are fine today. But it means
OWRX's safety net is gone: **if RepeaterBook ever fails, changes terms, or closes the
unauthenticated endpoint, every server without an API key silently gets zero repeaters.** Worth
flagging to Nathan too, in case UberSDR inherits the same link.

---

## 3. Differentiation is more than an emoji

Today a repeater gets `📡` prefixed to its name in the search list
(`MenuSheet.tsx:808`) and nothing else. That is not differentiation; it is a sticker.

**A repeater is a different kind of object from a bookmark.** A bookmark is "a frequency with
a name". A repeater has:

| Field | Why it matters |
|---|---|
| **Output freq** | what you listen on (this is the only one we currently model) |
| **Input freq / shift** | what the *user* transmits on; also what you tune to hear the input side |
| **CTCSS / DCS tone** | required to access it |
| **Mode** | FM / DMR / D-STAR / YSF / M17 / NXDN |
| **Status** | on-air / off-air / testing |
| **Location (lat/lon)** | distance and bearing — is it even plausibly in range? |
| **Callsign** | GB3xx etc. — the actual identity |

Extend `ServerBookmark` in `src/services/stations.ts`:

```ts
export interface RepeaterInfo {
  callsign?: string;
  output:    number;     // Hz — the existing `frequency`
  input?:    number;     // Hz
  shiftHz?:  number;     // convenience: input - output (negative for standard 2m/70cm)
  ctcssHz?:  number;
  dcs?:      string;
  status?:   'on-air' | 'off-air' | 'testing' | 'unknown';
  lat?:      number;
  lon?:      number;
  distanceKm?: number;   // computed at materialisation, not stored upstream
  bearingDeg?: number;
}

export interface ServerBookmark {
  // … existing fields …
  repeater?:     boolean;
  repeaterInfo?: RepeaterInfo;
}
```

**UI:** repeaters get their own card treatment in the bookmark/search list — callsign, shift,
tone, status, distance/bearing. Reuse the FM-DX transmitter-info component, which already
renders exactly this shape of data and which the user already recognises.

**Practical payoff:** "which repeater is this, is it up, can I hear it, and what's its input"
is the actual question a listener has. `📡` answers none of it.

**Mode note:** DMR / D-STAR / YSF / NXDN repeaters will appear in the list, and VibeSDR
cannot decode them (AMBE/IMBE — see the README). **Show them, tag the mode clearly, and do not
pretend.** A user tuning a DMR repeater and hearing noise is a support ticket; a user seeing
"DMR — not decodable in VibeSDR" is an informed user. Codec2-based M17 is the exception and
remains a genuine candidate.

---

## 4. Geo repeater lists (VibeServer, Local Hardware, rtl_tcp)

OWRX servers supply repeaters over the wire. **VibeServer, Local Hardware and rtl_tcp have no
server to ask** — they need the same treatment as airband: fetch a dataset, filter by receiver
location, materialise into bookmarks.

### 4.1 Honest data reality — read before planning

**There is no OurAirports-equivalent for repeaters.** The airband brief was able to say "27,708
rows, 235 countries, public domain, done". Nothing like that exists here. Surveyed:

| Source | Coverage | Licence / access | Verdict |
|---|---|---|---|
| **RepeaterBook** | Global, comprehensive | Works unauthenticated **for OWRX specifically**; server-side proximity query, no bulk export | ❌ as-is — see below |
| **ARD** (Amateur Repeater Directory) | **US only**, incomplete (working toward 50 states) | Open, clean provenance (explicitly forbids copying proprietary sources) | ⚠️ promising, immature, **URL currently 404** |
| **caluml/repeaters** (GitHub) | **34 UK repeaters** | Open; README explicitly invites app use | ❌ too small to be useful |
| **RSGB ETCC** | UK, authoritative | Published by the UK repeater co-ordinating body | ✅ **the GB answer** |
| **National IARU societies** | Per-country, authoritative | Varies, but the cultural default is open | ✅ **the model** |

**Why RepeaterBook works for OWRX and will not work for us.** Two independent reasons — and
note the first correction: **RepeaterBook is OWRX's primary source and its unauthenticated
path works fine.** `_loadFromWeb()` tries RepeaterBook first; ARD is only reached `if not
result`. A server with no API key still gets repeaters. (An earlier draft of this brief
claimed OWRX's key-free path was broken. It is not. The ARD 404 is a *latent* bug that only
bites when RepeaterBook fails — still worth reporting, but not the primary path.)

1. **The unauthenticated arrangement is OpenWebRX's, not ours.** The no-key branch sends:
   ```python
   hdrs = { "User-Agent": "(OpenWebRX+ " + openwebrx_version + ", luarvique@gmail.com)" }
   ```
   The code comment calls this "the old application-specific method" — i.e. it is a standing
   understanding between RepeaterBook and the OpenWebRX project, with the maintainer's email
   in the header so RepeaterBook knows whose traffic it is. The API-key path is the newer,
   politer mechanism they are migrating sysops toward. **Sending that UA from VibeSDR would
   be borrowing another project's goodwill under its name; sending our own UA to an endpoint
   we have not been granted is unauthorised use with extra steps.** Neither is acceptable.

2. **It is a proximity query, not a dataset — so the airband model does not even apply.**
   ```
   https://www.repeaterbook.com/api/{script}?qtype=prox&dunit=km&lat={lat}&lng={lon}&dist={range}
   ```
   RepeaterBook does the geo-filtering **server-side**. There is no bulk file to fetch and
   cache. VibeSDR could not do "download once, filter locally, work offline forever" — it
   would have to hit their API from **every phone, on every location change**. That is
   thousands of anonymous mobile clients against an endpoint currently serving a few thousand
   identifiable, long-lived servers. It is a different order of magnitude of load, and it is
   precisely the sort of traffic that gets an unauthenticated endpoint closed — for OWRX as
   well as for us.

**This sharpens the email considerably.** The ask is not "may we use your API". It is:

> *Is there a **bulk export** we could cache on-device, and would you license it for a free,
> open-source, non-commercial app that credits you prominently?*

That is a better question in every direction: it is **less** load on RepeaterBook than the
per-query model, it is honest about the scale involved, and it gives them something concrete
to say yes to. **Stuart action, not a code task.**

### 4.2 The model that actually works — and why it is easier than airband

**Repeater data is ham data, published by ham societies, for hams to use.** The default posture
of the sources is *share it* — the exact opposite of the airband situation, where the good
lists (UKAFG, Pooleys) are commercial products. This is the tailwind.

So: the same cited, per-country schema as the airband FIS tier, but sourced from national
societies and co-ordinating bodies.

```jsonc
// src/data/repeaters-national.json
{
  "schema": 1,
  "countries": {
    "GB": {
      "source": "RSGB ETCC (Emerging Technology Co-ordination Committee) — the UK repeater
                 co-ordinating body",
      "verified": null,           // null = not yet verified = NOT SHIPPED
      "entries": [
        // TODO(stuart): ETCC publishes the authoritative UK list (voice, DV, DMR, beacons)
        // with locations, shifts and tones. Check whether they offer a structured
        // download/API; if not, this is a scrape-with-permission conversation, not a
        // scrape-and-hope one.
      ]
    }
  }
}
```

**Same rules as the airband national tier, and for the same reason:**

- **Every entry carries a `cite`. CI fails the build without one.**
- **Claude Code must not populate frequency, tone or shift values.** It may scaffold schema,
  sources, and TODOs. A confidently-wrong CTCSS tone is a user sitting on a repeater they
  cannot open, blaming VibeSDR.
- A country with `verified: null` ships nothing rather than shipping a guess.

### 4.3 Priority order

1. **GB via ETCC.** Stuart's own country, authoritative source, and he is a licensed operator
   who can talk to them as a peer. Do this one first and prove the schema.
2. **Ask RepeaterBook.** If they say yes, most of the world is solved at once and §4.2 becomes
   a fallback rather than the plan.
3. **Watch ARD.** If it stabilises, matures beyond the US, and keeps its clean provenance, it
   becomes the OurAirports of repeaters — and it is explicitly built to be consumed by
   third-party apps. Revisit in six months.
4. **Other national societies**, contribution-driven, via `CONTRIBUTING-REPEATERS.md`. Same
   one-country-per-PR, cite-or-reject discipline as airband.

### 4.4 The radius control — and why it means different things per backend

The user wants a slider: "show me repeaters within N km of the receiver". Correct instinct,
but **the radius is not one thing**, and a slider that silently does nothing on some backends
is worse than no slider.

**OWRX filters server-side.** `connection.py`:

```python
range = self.stack["repeater_range"]        # server config, 0 = disabled
if range > 0:
    bookmarks += [b.__dict__() for b in
        Repeaters.getSharedInstance().getBookmarks(frequencyRange, rangeKm=range)]
```

`repeater_range` is a **sysop setting** (default `0`; `MAX_DISTANCE` caps it at 200 km),
evaluated against the server's own `receiver_gps`. The server sends only what is already
inside that radius.

**Therefore:**

- **We cannot widen it.** Dragging past the server's setting produces nothing, because the
  server never sent those repeaters. A slider that appears to control this would be lying to
  the user.
- **We CAN narrow it, client-side, and should.** A server configured at 200 km will flood a
  user in a city who wants 30. The distance is already in the description string
  (`"On-air, 12km away."`) — parse it out into `RepeaterInfo.distanceKm` while we are in
  there for the detection fix (§1.5). Nearly free.

**Also note:** OWRX only sends repeaters within the **current spectrum window**
(`cf ± samp_rate/2`), so repeaters arrive per-band as the user tunes, not as one global list.
Not a bug, but the list will look sparse if a user expects everything at once. Worth a
one-liner in the UI on OWRX backends.

| Backend | Radius control | Behaviour |
|---|---|---|
| **OWRX / OWRX+** | **Narrow only** | Slider max clamps to the server's `repeater_range`; label it *"server limit: 200 km"* so the ceiling is visible and honest |
| **VibeServer / Local Hardware / rtl_tcp** | **Full control** | Slider drives our own geo filter over the downloaded dataset — this is the case the download exists for |
| **KiwiSDR / UberSDR / FM-DX** | n/a | No repeater bookmarks from these backends; hide the control |

**Slider range:** 10 / 25 / 50 / 100 / 200 km, default **50 km**. (OWRX's `MAX_DISTANCE` of
200 km is a sensible ceiling — repeaters are sited high, but 200 km is optimistic for a phone
whip. 50 km is the honest default; let the keen widen it.)

**The radius is measured from the *receiver*, not the phone** — same location cascade as the
airband brief §5.2: backend-published lat/lon first, device location second, locale country
last. If you are in Huntingdon listening to a VibeServer at a friend's QTH in Devon, you want
Devon's repeaters. This is the same principle as airband and it must not be re-litigated per
feature.

### 4.5 Plumbing (all reused from the airband brief — build none of it twice)

- **Download gate** — repeaters ride the *same* opt-in download as airband. One payload, one
  prompt, one consent. Do not add a second gate; that is friction with no purpose.
  **Copy note:** unlike airband, there is no legality caveat for amateur repeaters — the
  listener is very likely licensed, and repeater frequencies are published by amateur societies
  precisely so people use them. The prompt must not imply otherwise. If the download covers
  both datasets, the legality line stays scoped to the airband part.
- **Location resolution** — identical cascade (§5.2 of the airband brief). Do not fork it.
- **Grouped cards** — same collapsible pattern, but grouped by **band** (2m / 70cm / 6m / 23cm)
  rather than by site, because that is how operators actually think about repeaters. Within a
  band, sort by distance.
- **Offline** — once downloaded, repeaters work with no network. Same as airband, and it
  matters for the same reason: the interesting places to operate have poor mobile data.

---

## 5. Acceptance criteria

1. **An OWRX server with off-air repeaters tags them as repeaters.** (Regression: currently
   silently missed.)
2. An OWRX server with the `srcFile` PR merged tags repeaters *and* EiBi entries correctly via
   the authoritative path, with no description sniffing.
3. An older OWRX server without the PR still tags repeaters correctly via the fallback.
4. **Unit tests cover the real description strings from `getDescription()`**, including the
   `"Off-air, 7km away."` case that the old regex missed.
5. A repeater card shows callsign, shift, tone, status and distance — not just `📡`.
6. A DMR/D-STAR/YSF repeater is listed, clearly labelled as not decodable, and does not
   silently produce noise.
7. On Local Hardware with no server, repeaters appear from the downloaded dataset, filtered to
   the receiver's location.
8. **The radius slider narrows the list on an OWRX backend and is clamped to the server's
   `repeater_range`, with the ceiling shown.** It never appears to offer a radius the server
   cannot supply.
9. **On VibeServer / Local Hardware / rtl_tcp the slider has full range** and re-filters the
   local dataset live.
10. Distance is parsed out of the OWRX description (`"…12km away…"`) into
    `RepeaterInfo.distanceKm` and displayed.
11. The radius is measured from the **receiver's** location, not the phone's — verified by
    connecting to a remote backend in another region.
12. **CI fails if any `repeaters-national.json` entry lacks a `cite`.**
13. No API key ships in the app. No dependency on RepeaterBook without written permission.

---

## 6. Open questions for Stuart

1. **ETCC.** Do they publish a structured download, or is this a "may we use your data, with
   credit, in a free GPL app" email? You are an RSGB-affiliated licensed operator writing to a
   volunteer co-ordinating committee about a free app for hams — that is about as favourable a
   framing as exists.
2. **RepeaterBook.** Ask specifically about a **bulk export** we can cache on-device, not API
   access — it is less load on them than per-query, and it is the only shape that lets VibeSDR
   work offline. Worst case they say no and we are exactly where we are now.
3. **Tell luarvique the ARD URL is dead.** It is not currently biting anyone (RepeaterBook is
   tried first and works), but it means his fallback is gone: if RepeaterBook ever changes its
   terms or closes the unauthenticated endpoint, every keyless server silently loses repeaters.
   A useful bug report, and a good way to introduce yourself before the `srcFile` PR.
4. **Nathan / UberSDR** — does UberSDR carry repeater bookmarks, and if so from where? If it
   shares the ARD link it shares the breakage. And if UberSDR is going to expose repeaters,
   agreeing a common wire shape now (the `RepeaterInfo` fields in §3) saves a second adapter
   later.
