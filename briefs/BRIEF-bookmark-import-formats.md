# BRIEF — Bookmark import: CSV / CHIRP / repeater directory formats

**Status:** Design brief for Claude Code implementation
**Target:** v10
**Author:** Stuart Carr (Stuey3D), with Claude
**Supersedes the geo-repeater work in `BRIEF-repeaters.md` §4**, which is parked (see §6).

---

## 1. Why this instead of a repeater data integration

The geo-repeater feature required either a licence negotiation with RepeaterBook or
hand-curating national lists from IARU societies — a lot of work, gated on other people's
goodwill, for a payoff limited to VibeServer and Local Hardware. OWRX already serves repeaters
(and they already drive the VTS), and OWRX is the only backend besides VibeServer that has any
repeater concept at all.

**The importer is strictly better on every axis:**

| | Data integration | Importer |
|---|---|---|
| Needs permission from anyone | Yes | **No** |
| Works with RepeaterBook | Only with a licence | **Yes — user exports it themselves** |
| Works with ETCC / club lists / a mate's file | No | **Yes** |
| Goes stale | Yes, we'd own the refresh | **No — user re-imports** |
| We are the redistributor | Yes | **No** |
| Scope | Repeaters | **Every frequency list in the hobby** |

The user signs into whatever directory they already use, downloads their list, imports it.
Same principle as the airband download gate: **the user obtains the data under their own
relationship with the provider.** We are a tool, not a distributor.

And the reach goes far beyond repeaters — CHIRP CSV is how the entire hobby moves frequency
lists between radios. Supporting it means VibeSDR ingests anything anyone already has.

---

## 2. Current state

`src/services/userBookmarks.ts` is already well shaped for this. Single dispatch point:

```ts
export function parseBookmarksAny(text: string, scope: string): UserBookmark[] {
  const t = text.trim();
  if (t.startsWith('{') || t.startsWith('[')) return parseBookmarksJSON(text, scope);
  try {
    const y = parseBookmarksYAML(text, scope);   // UberSDR export format
    if (y.length) return y;
  } catch {}
  return parseBookmarksJSON(text, scope);
}
```

Callers (`SDRScreen.tsx`, `vibeServer.ts`, the paste box and file picker in `MenuSheet.tsx`)
all go through `parseBookmarksAny`. **New formats are a pure addition — nothing needs
restructuring.** That is the whole reason this is a small job.

Target model:

```ts
export interface UserBookmark {
  name:            string;
  frequency:       number;   // Hz  ← note: Hz, not MHz. See §4.1.
  mode:            string;   // lowercase demodulator
  group?:          string | null;
  comment?:        string | null;
  extension?:      string | null;
  bandwidth_low?:  number | null;
  bandwidth_high?: number | null;
  scope:           string;
}
```

---

## 3. Formats to add

| Format | Source | Priority |
|---|---|---|
| **CHIRP CSV** | The hobby's lingua franca. RepeaterBook, RadioReference, ETCC, every programming workflow | **1 — do this one** |
| **Generic CSV** (column auto-map) | Anything with a header row containing a frequency and a name | 2 |
| **RepeaterBook CSV** | Their own export (differs from CHIRP) | Falls out of (2) |
| **ADIF** | Logging format — *not* a frequency list | ❌ out of scope |

CHIRP CSV plus a tolerant generic-CSV fallback covers essentially everything a user can
download today. Do not build per-directory parsers; build one good CSV path.

---

## 4. CHIRP CSV — the two traps

Header:

```
Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,
DtcsPolarity,Mode,TStep,Skip,Comment,URCALL,RPT1CALL,RPT2CALL,DVCODE
```

Row:

```
1,GB3PY,145.7250,-,0.600000,Tone,77.0,77.0,023,NN,FM,5.00,,Cambridge,,,,
```

### 4.1 TRAP ONE — frequencies are in MHz

`UserBookmark.frequency` is **Hz**. CHIRP's `Frequency` and `Offset` are **MHz**.

```ts
const hz = Math.round(parseFloat(row.Frequency) * 1e6);   // 145.7250 → 145725000
```

Miss this and every bookmark lands at 145 Hz. It will not throw; it will just silently produce
a list of useless bookmarks near DC. **Unit-test it.**

### 4.2 TRAP TWO — CHIRP `FM` is NOT broadcast FM

This is the most likely bug in the feature and the hardest to spot, because the result is
plausible rather than obviously broken.

In CHIRP, on a repeater:
- **`FM`** = 25 kHz deviation FM → VibeSDR **`nfm`**
- **`NFM`** = 12.5 kHz narrow FM → VibeSDR **`nfm`**

**Both map to `nfm`.** Mapping CHIRP `FM` → VibeSDR `wfm` would put a ~250 kHz broadcast
passband on a 2 m repeater and produce silence. A developer (or an LLM) pattern-matching
"FM means FM" will get this wrong every time.

Mapping table:

| CHIRP `Mode` | VibeSDR mode | Note |
|---|---|---|
| `FM` | `nfm` | **25 kHz FM, not broadcast** |
| `NFM` | `nfm` | 12.5 kHz |
| `WFM` | `wfm` | The *only* one that is broadcast FM |
| `AM` | `am` | |
| `LSB` | `lsb` | |
| `USB` | `usb` | |
| `CW` | `cw` | |
| `DV` / `DMR` / `DN` / `NXDN` | `nfm` | **Import, but flag** — see §4.4 |
| *(blank / unknown)* | infer from frequency | §4.3 |

### 4.3 Mode inference when absent

Generic CSVs often have no mode column. Infer from frequency, reusing the **existing
band-aware tuning rules** rather than writing a second set (`sdrTypes.ts` already encodes
this):

- < 30 MHz → `usb` above 10 MHz, `lsb` below (standard practice)
- 30–137 MHz, airband range → `am`
- 87.5–108 MHz → `wfm`
- VHF/UHF ham + PMR → `nfm`

Do not invent new rules. If the app already knows what mode 145.725 MHz should be, use that.

### 4.4 Digital-mode repeaters

DMR / D-STAR / YSF / NXDN rows **should import** — the user wants them in their list, and the
frequency is real. But VibeSDR cannot decode them (AMBE/IMBE, see README).

- Import at `nfm`.
- Put the original mode in `comment` (e.g. `"DMR — not decodable in VibeSDR"`).
- Do **not** silently drop them. A missing bookmark is a bug report; a labelled one is an
  informed user.

### 4.5 Duplex / Offset / Tone

VibeSDR is **receive-only**, so:

- **Listen on `Frequency`** — that is the repeater's *output*, which is what you hear.
- `Duplex` (`-` / `+` / `split`) and `Offset` give the *input* frequency. Informational, but
  genuinely useful: it lets a user tune the input side to hear local stations direct.
  Preserve it in `comment` — e.g. `"−600 kHz · 77.0 Hz"`.
- `rToneFreq` / `cToneFreq` / `DtcsCode` → CTCSS/DCS. Cannot be used (no TX), but belongs in
  `comment`. It is the first thing an operator wants to know about a repeater.

Suggested comment composition, keeping the original `Comment` field if present:

```
GB3PY · −600 kHz · CTCSS 77.0 · Cambridge
```

---

## 5. Generic CSV — tolerant column mapping

For any CSV that is not CHIRP-shaped. Auto-detect columns from the header, case-insensitively,
matching on aliases:

| Target | Header aliases |
|---|---|
| `frequency` | `frequency`, `freq`, `output`, `output freq`, `downlink`, `rx`, `rx freq`, `mhz`, `hz`, `khz` |
| `name` | `name`, `callsign`, `call`, `label`, `station`, `id` |
| `mode` | `mode`, `modulation`, `emission` |
| `comment` | `comment`, `notes`, `description`, `location`, `qth`, `town`, `county` |
| `group` | `group`, `band`, `category`, `folder`, `service` |

**Unit inference for frequency** — do not trust the column name:

```
value < 1000        → MHz   (145.725)
1000 ≤ value < 1e6  → kHz   (145725)
value ≥ 1e6         → Hz    (145725000)
```

Sanity-clamp to a plausible receiver range and **drop** anything outside it. This is the same
lesson as the Indonesian `8.88 MHz` airband junk in the airband brief: **never trust the
column, always sanity-check the value.**

Handle quoted fields, embedded commas, BOM, CRLF, and semicolon delimiters (European Excel
exports use `;`). Detect the delimiter from the header line rather than assuming.

---

## 6. UX

### 6.1 Dispatch

```diff
 export function parseBookmarksAny(text: string, scope: string): UserBookmark[] {
   const t = text.trim();
   if (t.startsWith('{') || t.startsWith('[')) return parseBookmarksJSON(text, scope);
+
+  // CSV: a header line with a delimiter and a recognisable frequency column.
+  if (looksLikeCSV(t)) return parseBookmarksCSV(text, scope);
+
   try {
     const y = parseBookmarksYAML(text, scope);
     if (y.length) return y;
   } catch {}
   return parseBookmarksJSON(text, scope);
 }
```

`parseBookmarksCSV` internally detects CHIRP (by its distinctive header) and falls back to
generic column mapping. **One entry point, no user-facing format picker** — consistent with the
standing principle that complexity belongs in the engine, not the settings.

### 6.2 File picker

`MenuSheet.tsx:943` currently reads:

```
📁 IMPORT FILE (JSON / YAML)
```

Change to `📁 IMPORT FILE (JSON / YAML / CSV)`, and widen the `DocumentPicker` MIME types to
include `text/csv`, `text/comma-separated-values`, `application/csv`, and `text/plain` — CHIRP
files are frequently served as `text/plain`, and iOS is fussy. Include `*/*` as a last resort
rather than have a user unable to select a file they can see.

Paste box placeholder (`MenuSheet.tsx:956`) → *"Paste bookmarks (JSON, YAML or CSV) here…"*.

### 6.3 Preview before commit — worth doing

Import is destructive-ish (it merges into the user's list). Show a preview before committing:

> **Import 47 bookmarks?**
> `GB3PY · 145.725 MHz · NFM · −600 kHz · CTCSS 77.0`
> `GB3PI · 145.675 MHz · NFM · −600 kHz · CTCSS 71.9`
> … 45 more
>
> ⚠️ 3 rows skipped (frequency out of range)
>
> **[ Import ]** [ Cancel ]

This is not settings complexity — it is showing the user what is about to happen, which is the
one place friction earns its keep. It also surfaces a botched MHz/Hz conversion instantly:
"145 Hz" in the preview is unmissable, where a silently-imported list of dead bookmarks is not.

Report skipped rows with a reason. Never fail the whole import because of one bad row.

---

## 7. Acceptance criteria

1. A **CHIRP CSV exported from RepeaterBook** imports cleanly, with correct frequencies in Hz.
2. **`145.7250` becomes `145725000`, not `145`.** Unit test.
3. **A CHIRP row with `Mode=FM` imports as `nfm`, not `wfm`.** Unit test. This is the one that
   will get "fixed" wrongly by someone later.
4. A row with `Mode=DMR` imports at `nfm` with a comment noting it is not decodable — not
   dropped, not silently broken.
5. Duplex/offset and CTCSS appear in the comment.
6. A **generic CSV with no mode column** infers modes from frequency using the existing
   band-aware rules.
7. A semicolon-delimited CSV (European Excel) imports.
8. A CSV with a stray row at 8.88 MHz labelled as a 2 m repeater is **dropped**, with the skip
   reported — not imported.
9. Preview shows before commit; skipped rows are counted and explained.
10. JSON and YAML import continue to work exactly as before. **No regression** — they are the
    UberSDR interop path and must not move.

---

## 8. Parked (from `BRIEF-repeaters.md`)

Still worth doing, cheap, independent of all of the above:

- **§1 — the OWRX repeater detection bug.** `\bkm away\b` never matches (word boundary fails
  between the digit and the `k` in `"12km away"`), so **every off-air repeater is currently
  mis-tagged as a plain user bookmark**. Ten minutes plus a unit test. Do this regardless.
- **§2 — the `srcFile` one-line PR to OpenWebRX+.** Kills the prose-sniffing permanently and
  hands us EiBi source tagging that `stations.ts` already has a field for and cannot currently
  populate.
- **§3 — richer repeater display** (callsign, shift, tone, status, distance) rather than just
  `📡`. Now partly served by the importer, since imported repeaters carry shift and tone in the
  comment.

Genuinely parked, revisit only if the landscape changes:

- **§4 — geo repeater datasets.** Blocked on RepeaterBook licensing (their API is a *proximity
  query*, not a bulk export — so it cannot be cached offline anyway) or on hand-curating IARU
  national lists. Revisit if ARD matures beyond the US and stabilises its URL.
