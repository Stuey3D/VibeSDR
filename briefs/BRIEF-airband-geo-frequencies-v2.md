# BRIEF — Airband: geo-derived ATC frequency lists (v2)

**Status:** Design brief for Claude Code implementation
**Target:** v10 (companion to the Scanner brief — the scanner is the mechanism, this is what it scans)
**Author:** Stuart Carr (Stuey3D), with Claude
**Depends on:** Scanner brief (detection engine, dwell/hang), Instance picker (location plumbing)
**Supersedes:** v1. Rewritten after an empirical census of the OurAirports data (§3.3),
which materially changed the plan.

---

## 1. Problem statement

Airband (118–137 MHz AM) is currently unusable in VibeSDR in any practical sense. Two
compounding problems:

1. **The user doesn't know where the signals live.** There is no airband band plan beyond
   "somewhere between 118 and 137". Nothing in the app says "Sywell Radio is on this
   channel and it is 2 km from you".
2. **The transmissions are fleeting.** An airband exchange lasts two to five seconds. By
   the time you have tuned by hand, it is over. Manual tuning cannot catch airband — this
   is a categorical mismatch between the interaction model and the signal, not a UX polish
   issue.

Solving (1) without (2) gives a bookmark list that is interesting to read and useless to
listen to. Solving (2) without (1) gives a scanner with nothing to scan. **Both ship
together or neither ships.**

**Secondary goal, equally important:** the user wants to know *what they are listening to*.
A bare frequency is not the product. "121.850" is noise; "Sywell Radio" is a radio station.
The label is the feature.

---

## 2. Standing principles (inherited)

- **Complexity belongs in the engine, never in the settings.** The user picks "Airband" and
  it works. No detection-mode picker, no dwell slider, no squelch strategy.
- **Zero-permission fallback.** Location stays optional. Every path degrades gracefully to
  a country-level list with no location grant.
- **Licence hygiene.** Nothing enters the tree that cannot ship in a GPL-3.0 app under the
  App Store exception.
- **Never assert a frequency we have not verified.** See §4. This is the load-bearing
  principle of the whole brief.

---

## 3. Data sources

### 3.1 Primary: OurAirports (public domain)

Two static CSVs over plain HTTPS. No auth, no API key, no rate limit:

```
https://davidmegginson.github.io/ourairports-data/airports.csv
https://davidmegginson.github.io/ourairports-data/airport-frequencies.csv
```

Also mirrored at `github.com/davidmegginson/ourairports-data` (useful — the raw
`github.io` host may not be reachable from every build environment; the git repo is).

`airports.csv`: `id`, `ident` (ICAO where one exists), `type`, `name`, `latitude_deg`,
`longitude_deg`, `iso_country`, `municipality`.
`airport-frequencies.csv`: `airport_ref` (FK → `airports.id`), `airport_ident`, `type`
(service code), `description` (free text, typically the callsign as a pilot would say it),
`frequency_mhz`.

**Licence:** public domain. No attribution obligation, no share-alike, no NC clause. Safe
to bundle and redistribute in App Store and Play Store builds. Credit it in the README
anyway — it is the decent thing to do and costs nothing.

**Rejected: openAIP.** Richer (airspace, en-route sectors) but restrictive terms and an API
key requirement. Do not use. Do not revisit without a licence review.

### 3.2 Secondary: curated national FIS list (hand-maintained, cited, in-repo)

See §4. This is the *top-up* layer, not the foundation.

### 3.3 CENSUS — what the data actually contains (measured 2026-07-14)

**Read this section before writing any code. It overturns the assumptions in v1 of this
brief.**

The full frequency file has **30,315 rows**. After filtering to 118.000–137.000 MHz and
dropping closed airports:

> **27,708 usable airband rows, spanning 235 countries.**

Per-country usable rows (in-band, non-closed):

| CC | rows | | CC | rows | | CC | rows | | CC | rows |
|---|---|---|---|---|---|---|---|---|---|---|
| US | 12073 | | ES | 229 | | AR | 135 | | PT | 79 |
| AU | 3669 | | TR | 188 | | NL | 132 | | BE | 75 |
| CA | 1464 | | CN | 186 | | NO | 129 | | DK | 70 |
| DE | 1266 | | ID | **180** | | CH | 111 | | IE | 57 |
| GB | 622 | | ZA | 178 | | PL | 107 | | AT | 55 |
| FR | 496 | | NZ | 177 | | PH | 100 | | AE | 41 |
| BR | **336** | | SE | 163 | | CL | 99 | | VN | 30 |
| JP | 336 | | CZ | 163 | | GR | 99 | | SG | **19** |
| RU | 317 | | FI | 160 | | MY | 93 | | | |
| IT | 293 | | MX | 154 | | TH | 139 | | | |
| IN | 250 | | KR | 149 | | | | | | |

**Headline finding: every country VibeSDR has users in already gets a working airband list
today, from public-domain data, with zero curation effort.** Brazil 336 rows. Indonesia
180. Singapore 19 (small, but Singapore *is* small — that is Changi, Seletar, Paya Lebar,
Tengah, and it is essentially complete). This is not a thin fallback; it is the feature.

**Second finding, equally important: the en-route / FIS data in OurAirports is NOT usable
and must not be imported as such.** The `INFO`, `ACC`, `CNTR`, `FSS` and `FIS` type codes
exist, but outside the US and Canada they are patchy, inconsistent, and in places simply
wrong. Actual observed rows:

- **Indonesia**, rows tagged `INFO`: `8.88 MHz`, `295.60 MHz`, `534.00 MHz`, `558.00 MHz`.
  None of these are airband. HF, UHF, and outright junk, mis-tagged by contributors.
- **Brazil**: **zero** en-route rows. Not sparse — zero.
- **Singapore**: one row (`WSAP / ACC / SINGAPORE RADAR / 127.3`).
- **UK**: includes `Lomgside radio` (sic) and Shanwick Oceanic mis-tagged as `ACC`.

This is community-grade data. It is excellent for aerodromes, where thousands of pilots
have checked it, and unreliable for en-route, where far fewer have. **Therefore: import the
aerodrome tier wholesale; hand-curate the national tier from AIPs; never blend them.**

---

## 4. The national FIS tier — sourcing discipline

### 4.1 The rule

**No frequency enters `airband-national.json` without a citation to a published national
AIP (or equivalent authority) and a date on which it was checked.**

This is not bureaucracy. An airband frequency that is *confidently wrong* is worse than one
that is absent: the user sits on dead air, blames the receiver, and — critically — stops
trusting the 27,708 frequencies that *are* correct. One bad number poisons the whole
feature. The cost of a missing country is "you get aerodromes only", which is a working
product. The cost of a wrong country is a loss of trust that does not come back.

**Explicitly: an LLM must not generate these numbers.** Asked for "the FIS frequency for
Uzbekistan", a language model will produce a plausible, well-formatted, confident, and
frequently incorrect answer, with no reliable signal about which entries are the incorrect
ones. Claude Code must not populate frequency values in this file. It may scaffold the
schema, the source URLs, the region metadata and the `TODO` markers — and nothing else.

### 4.2 The three tiers of confidence

| Tier | Coverage | Effort | Confidence | Source |
|---|---|---|---|---|
| **Universal** | 235 countries | Zero — hardcode | Certain (ICAO-fixed) | §4.3 |
| **Aerodrome** | 235 countries, 27,708 rows | Zero — it's the CSV | Good, community-maintained | §3.1 |
| **National FIS** | Incremental, cited | ~20–40 min/country from AIP | High, dated, verifiable | §4.4 |

**No user ever sees an empty Airband screen.** Universal is the floor and it costs nothing.

### 4.3 Universal entries (safe to hardcode — ICAO-fixed worldwide)

```jsonc
{
  "universal": [
    { "freq_mhz": 121.500, "label": "Emergency (VHF Guard)",     "service": "EMG" },
    { "freq_mhz": 123.100, "label": "SAR / On-scene",            "service": "EMG" },
    { "freq_mhz": 243.000, "label": "Military Emergency (Guard)","service": "EMG", "out_of_band": true }
  ]
}
```

`out_of_band: true` marks entries outside 118–137 (military UHF guard, 225–400 MHz AM
military airband). Exclude from the scan list unless the tuned backend's hardware can
actually reach them — an RTL-SDR can, an HF-only KiwiSDR cannot. **Gate on the backend's
published frequency range, silently.**

These three are the only frequencies in this entire brief that may be written without
consulting a source, because they are fixed by international agreement and will not change.

### 4.4 National schema

```jsonc
// src/data/airband-national.json
{
  "schema": 1,
  "universal": [ /* §4.3 */ ],
  "countries": {
    "GB": {
      "source": "UK AIP (NATS), ENR 2.2 / GEN 3.4 — nats-uk.ead-it.com",
      "verified": null,                 // ISO date. null = NOT YET VERIFIED = do not ship.
      "channel_spacing_khz": 8.33,      // see §6
      "regions": [],                    // empty = country is small enough for a flat list
      "entries": [
        // TODO(stuart): transcribe from UK AIP ENR 2.2.
        // London Information, Scottish Information, London Control sectors,
        // Swanwick Military, D&D, London Volmet (Main/South/North), Scottish Volmet.
        // DO NOT populate from memory. Cite the AIP section per entry.
      ]
    },

    "BR": {
      "source": "DECEA / AISWEB — aisweb.decea.mil.br, ENR 2.1",
      "verified": null,
      "channel_spacing_khz": 25,        // NOT 8.33 — see §6
      "regions": [
        { "id": "SBAZ", "label": "Amazônica FIR",  "centroid": [-3.5, -60.0],  "radius_nm": 700 },
        { "id": "SBBS", "label": "Brasília FIR",   "centroid": [-15.8, -47.9], "radius_nm": 500 },
        { "id": "SBCW", "label": "Curitiba FIR",   "centroid": [-25.4, -49.3], "radius_nm": 450 },
        { "id": "SBRE", "label": "Recife FIR",     "centroid": [-8.1, -34.9],  "radius_nm": 500 },
        { "id": "SBAO", "label": "Atlântico FIR",  "centroid": [-10.0, -30.0], "radius_nm": 900 }
      ],
      "entries": [ /* TODO — each entry carries a "region" id */ ]
    },

    "ID": {
      "source": "AirNav Indonesia AIP — aims.airnavindonesia.co.id, ENR 2.1",
      "verified": null,
      "channel_spacing_khz": 25,
      "regions": [
        { "id": "WIIF", "label": "Jakarta FIR",       "centroid": [-2.0, 106.0], "radius_nm": 800 },
        { "id": "WAAF", "label": "Ujung Pandang FIR", "centroid": [-4.0, 130.0], "radius_nm": 900 }
      ],
      "entries": [ /* TODO */ ]
    },

    "SG": {
      "source": "CAAS AIP Singapore, ENR 2.1",
      "verified": null,
      "channel_spacing_khz": 25,
      "regions": [],                    // one FIR, flat list is correct
      "entries": [ /* TODO — small and quickly completable; likely the most COMPLETE
                     national list in the app once done. Good first target. */ ]
    }
  }
}
```

**Entry shape:**

```jsonc
{
  "freq_mhz": 124.600,
  "label": "London Information",
  "service": "FIS",              // FIS | ACC | EMG | VOLMET | MIL
  "region": null,                // region id, or null for country-wide
  "cite": "UK AIP ENR 2.2 §3.1", // MANDATORY. No cite, no merge.
  "notes": "South of 5530N"      // optional
}
```

**Validation, enforced in CI:**

- An entry without a non-empty `cite` **fails the build**. Make it structurally impossible
  to add an uncited frequency.
- A country with `verified: null` is **not shipped** — it is scaffolding, not data. Ship
  the universal + aerodrome tiers for that country instead.
- Every `region` id referenced by an entry must exist in that country's `regions` array.
- `freq_mhz` must be within 118–137 unless `out_of_band` is set.

### 4.5 Regional grouping (why Brazil and Indonesia broke the v1 design)

v1 assumed a flat entries list per country. **Brazil has five FIRs and is larger than the
contiguous US. Indonesia has two FIRs spanning 5,000 km of archipelago.** "The Brazilian
FIS frequency" is not a thing. A user in São Paulo must not be shown a card full of Amazon
sector frequencies they will never hear.

Resolution: national entries carry an optional `region`. Entries with `region: null` are
country-wide and always shown (emergency, national VOLMET). Entries with a `region` are
filtered by distance from the **resolved receiver location** (§5.2) to the region centroid,
against `radius_nm`, using the same great-circle helper as the aerodrome radius filter.

Singapore, with `regions: []`, proves the flat case still works and needs no special
handling. The two models coexist; the flat case is just the degenerate one.

### 4.6 Community contribution

This is how the national tier actually scales, and it is the same move EiBi represents:
consume someone else's carefully-maintained data rather than maintaining it yourself.

Add `CONTRIBUTING-AIRBAND.md`:

- One country per PR.
- Every entry cites an AIP section.
- State the date you checked, and the AIP effective date (AIRAC cycle if known).
- Maintainer does not verify the numbers — the *citation* is the contract. If it is wrong,
  it is wrong in a way that is traceable and fixable, which is the property that matters.

**Reject any PR without citations, however plausible the numbers look.** No exceptions.
The schema validation in §4.4 should make this automatic rather than a judgement call.

**Surface staleness in the app.** If a country's `verified` date is more than 18 months
old, show a quiet `last checked 2026-01` line on the National card. Not alarming — honest.
It creates gentle social pressure for someone to refresh it, and it is the truth.

Amateur radio and aviation-monitoring people are exactly the demographic who enjoy this
kind of contribution. Airband is the feature most likely to bring in people who want to
contribute *data* rather than code — a different and useful sort of contributor.

---

## 5. Pipeline

### 5.1 Fetch, filter, cache

- Fetch both CSVs on first airband use, then weekly. `ETag` / `If-None-Match` — a 304
  costs nothing and these files move slowly.
- `airports.csv` is ~12 MB. **Do not hold it in JS memory.** Stream-parse, keep only what
  survives the filter, write to the on-device store.

**Sanity filter — mandatory, and the Indonesia junk in §3.3 is why:**

```
DROP  apt_type == 'closed_airport'
DROP  frequency_mhz is null / unparseable
DROP  frequency_mhz < 118.000 or > 137.000     (unless flagged out_of_band)
DROP  type in {ATIS, AWOS, ASOS, ARCAL} from the SCAN list (keep for manual tune — §7)
```

The band filter is not an optimisation. It is the thing that stops `8.88 MHz` and
`534.00 MHz` reaching the user as "Indonesian airband". **Never trust `type`; always trust
the band.**

Net after filtering: ~27,700 rows. Trivially fine in SQLite. And it means the whole world
is available **offline once fetched** — which matters enormously, because airband listening
happens at airfields, and airfields have terrible mobile data.

### 5.1a Data is downloaded on demand, not bundled

**VibeSDR does not ship the airband data in the binary.** The user downloads it, once,
when they choose to. This is a deliberate decision with three independent justifications,
and it is worth being precise about which are real:

**Real reasons:**
1. **Redistribution.** We are not the redistributor of third-party data. OurAirports is
   public domain so this is not strictly *necessary*, but the same download path will later
   carry the curated national FIS lists, whose provenance is more varied — and it is much
   easier to establish the pattern now than to retrofit it.
2. **Freshness.** The user gets the current CSVs, not whatever was current at release. Given
   VibeSDR's release cadence, a bundled snapshot would be months stale for most users.
3. **Bundle size.** ~27,700 rows plus airport metadata is not free, and the majority of
   users will never open Airband.

**NOT a reason — do not let anyone believe otherwise:** this is **not** a legal shield, and
the brief must not be implemented as though it were. Where airband monitoring is restricted,
the restricted act is **receiving the transmission**, not possessing a list of numbers.
VibeSDR already tunes 118–137 MHz today; a user can spin the drum to their local tower right
now, with no list at all. The frequencies are published by the state, in the AIP, for free.
Making the list optional is good engineering and good licence hygiene. It is not a defence,
and building a furtive UI around it would imply a guilt the feature does not carry.

**Consequence for UX:** Airband on first open shows the download prompt (§9a), not an empty
list and not a broken screen. Once downloaded, it is fully offline — which matters
enormously, because airband listening happens at airfields and airfields have terrible
mobile data.

**Implementation:**
- Filter and normalise **client-side** on download (the filter rules above), so the on-device
  store holds only usable rows. Do not store the raw CSVs.
- Store a `dataVersion` (the CSVs' ETag or fetch date) and surface it in the Airband header.
- **Refresh:** a manual "Update frequency data" control, plus a silent background refresh no
  more than weekly, only if data is already present. Never re-prompt.
- **Delete:** a "Remove downloaded frequency data" control that clears the store and returns
  Airband to its pre-download state. If the user changes their mind, they can take it back.
  That is the difference between a real choice and a fake one.
- The **national FIS lists (§4)** ride the same download — one payload, one prompt, one
  consent. Do not split them into a second gate; that is friction with no purpose.

### 5.2 Location resolution — which lat/lon do we filter around?

First hit wins:

1. **Backend receiver location.** If the tuned backend publishes lat/lon (UberSDR and
   KiwiSDR both do), use *that*. This is correct, not a compromise: if the user is in
   Huntingdon listening to a Kiwi in Norfolk, they want Norfolk's airband. **The
   frequencies follow the antenna, not the phone.**
2. **Device location**, if granted — Local Hardware, VibeServer, rtl_tcp, or any backend
   that publishes no location of its own.
3. **Locale-derived country** — the zero-permission fallback from the instance-picker
   brief. No radius filter; user gets universal + national (country-wide entries only) +
   a country-wide aerodrome list sorted by airport size. Degraded, but genuinely usable.

**VibeServer protocol addition:** add an optional `location: {lat, lon}` to the capability
handshake, configurable in the macOS and Android server config. A VibeServer in a shed at
an airfield should hand its clients that airfield's frequencies automatically. Small
protocol change, disproportionate payoff. Coordinate with the VibeServer protocol brief.

### 5.3 Radius filter

Great-circle distance from resolved location to each airport.

| Radius | Use |
|---|---|
| 10 nm | "Just my local field" |
| 25 nm | Local cluster — **default** |
| 50 nm | Regional |
| 100 nm | Everything reachable with a decent antenna |

Sort by distance ascending, then airport `type` (large before small) as tiebreak.

---

## 6. Channel spacing — and the trap

**Europe uses 8.33 kHz spacing above 118 MHz. Brazil, Indonesia and Singapore use 25 kHz.**

For 8.33 kHz countries, the published number is a **channel *name*, not a carrier
frequency**. They differ, and not by a constant. The ICAO name→carrier mapping repeats on a
25 kHz cycle; names ending `.x05 / .x30 / .x55 / .x80` are 8.33 kHz channels whose true
carriers sit at the 8.333…-kHz-spaced positions. Names ending `.x00 / .x25 / .x50 / .x75`
are 25 kHz channels and pass through **unchanged**.

**Key correction to v1:** the conversion must be **keyed off the country's
`channel_spacing_khz`, not off the fact that it is airband.** Applying the 8.33 lookup to a
Brazilian 25 kHz channel will *corrupt a frequency that was already correct as published*.
This is a data-destroying bug that produces plausible-looking wrong numbers — the worst
kind.

- Conversion is **opt-in per country**, default **off** (i.e. default 25 kHz passthrough).
- **Unit-test both directions** against a known table of name→carrier pairs: 8.33 countries
  convert; 25 kHz countries pass through untouched.
- This is the **highest-risk correctness item in the brief.** It fails quietly — the audio
  comes through muffled rather than absent, so it does not announce itself as broken.

Band-aware tuning rule for 118–137 MHz: **AM, step = country spacing, ~6 kHz passband, AGC
on.** Set automatically on band entry, consistent with existing band-aware behaviour.

---

## 7. Service mapping and labelling

| OurAirports code(s) | VibeSDR service | Label | Priority | In scan |
|---|---|---|---|---|
| *(universal, §4.3)* | `EMG` | Emergency | 0 | ✅ |
| *(national, §4.4)* | `FIS` / `ACC` | Information / Control | 0 | ✅ |
| `TWR` | `TOWER` | Tower | 1 | ✅ |
| `APP`, `A/D`, `ARR` | `APPROACH` | Approach | 1 | ✅ |
| `DEP` | `DEPARTURE` | Departure | 1 | ✅ |
| `A/G`, `AFIS`, `RDO`, `RADIO` | `AIR_GROUND` | Radio / AFIS | 1 | ✅ |
| `CTAF`, `ATF`, `MF`, `UNIC` | `CTAF` | Traffic / Unicom | 2 | ✅ |
| `CNTR`, `ARTC`, `ACC` | `CENTRE` | Centre | 2 | ✅ |
| `GND`, `RMP`, `CLD` | `GROUND` | Ground / Delivery | 3 | ✅ |
| `RCO`, `FSS`, `INFO` | `RCO` | Radio outlet | 3 | ✅ |
| `ATIS`, `AWOS`, `ASOS`, `ARCAL` | `INFO_LOOP` | ATIS / Weather | — | ❌ **excluded** |
| `MISC`, `OPS`, `PMSV`, *(unmapped)* | `OTHER` | *(raw code)* | 4 | ❌ |

**The ATIS exclusion is the single most important row in this table.** ATIS / AWOS /
VOLMET are **continuous carriers**. A squelch-driven scanner that includes them locks onto
the first one it finds and never moves again. Exclude by default; expose an explicit
"Include ATIS / weather loops" toggle (off); and when enabled, `INFO_LOOP` channels are
**manual-select only** — present and tappable in the list, never entered by the scan loop.
The user genuinely does want to *listen* to ATIS sometimes (it is how you learn the runway
in use); they never want to *scan* it.

**Label composition** — this is the "know what I'm listening to" requirement:

```
{airport short name} {service}    →   "Sywell Radio"
                                      "Cranfield Tower"
                                      "Cambridge Approach"
                                      "London Information"
```

Strip `Airport / Aerodrome / Airfield / International` from `airports.name` for the short
name. **Prefer OurAirports' `description` field where present** — it is documented as "the
way a pilot would open a call on it", which is exactly the string we want. Fall back to the
composed form.

Show distance and bearing alongside, reusing the FM-DX transmitter-info component. The user
already knows what that looks like.

The label flows through the VFO station line, VTS bar, Now Playing, lock screen, CarPlay,
Android Auto browse lists and the Apple Watch, identically to an FM-DX station name. **Reuse
the existing plumbing; do not build a second path.**

---

## 8. Scanner integration

Airband becomes a first-class scan profile. The detection engine picks its strategy from
the fact that the list is airband — **no user-facing mode switch**, per the standing
principle.

### 8.1 Local IQ backends (Local Hardware, VibeServer, rtl_tcp, SpyServer)

An RTL-SDR at 2.4 MS/s gives ~2 MHz usable — **~240 channels at 8.33 kHz, or ~80 at
25 kHz, watched simultaneously.** Do not sequentially retune. Instead:

- Park the tuner on a slice covering the densest cluster in the active list.
- Run parallel carrier / channel-power detection across every channel in the slice inside
  VibeDSP (FFT bin power vs a rolling per-channel noise floor).
- **Zero retune latency — nothing is missed.** A two-second transmission that begins while
  you are "on" another channel is still caught, because you were never *not* listening to
  it.
- Where the active list spans >2 MHz, define 2–3 slices and rotate slowly (~1 s), weighting
  slice selection by recent activity.

**This is a genuine, demonstrable advantage of local IQ over any remote backend, and no
conventional scanner — hardware or software — does it on a £25 dongle. Lead the v10 release
notes with it.**

### 8.2 Remote server backends (UberSDR, OpenWebRX, KiwiSDR)

The server owns the tuner; we get one audio stream. Sequential retune only:

- Fast dwell ~60–100 ms per channel.
- Squelch on **AM carrier presence**, not SNR — airband AM has a hard carrier, and carrier
  detection is both faster and more reliable here than an SNR estimate.
- Transmissions *will* occasionally be missed. Say so honestly in one line of UI copy when
  on a remote backend. A note, not a nag.

Most KiwiSDRs are HF-only. Detect from the backend's published frequency range and **hide
the airband profile entirely** rather than offering something that cannot work.

### 8.3 Scan behaviour (both paths)

- **Hang time ~2 s past carrier drop.** Without it you get the aircraft's call and miss the
  controller's reply — worse than useless, actively frustrating. Tune by ear; 2 s is the
  start point.
- **Priority sampling.** Rank-0 and rank-1 channels (emergency, FIS, and the nearest
  aerodrome's Tower / Approach / A-G) are sampled every N channels regardless of scan
  position. On the local-IQ path this is free — everything in the slice is always watched —
  but the *stop* logic must still prefer the higher-priority channel when two open at once.
- **Manual lockout.** Long-press to skip a channel for the session. Persist per-instance.
- **Activity log.** Rolling list of which channels opened, when, with the label.

The activity log is quietly the best part of the feature. Leave it scanning for an hour and
the user learns, *empirically*, where their local traffic actually lives. It answers the
original question — "I don't know where the signals live" — permanently, by observation
rather than by assertion. It is also the honest answer to the sparse-FIS-data problem: we
may not be able to *tell* a user in Jakarta what the sector frequencies are, but we can
help them *find out*.

---

## 9. UI

### 9a. The download prompt (first open)

Airband, before data is present, shows this — **not** an empty list, **not** a spinner:

> ### Airband frequencies
>
> Download the airband frequency list — around **27,000 air traffic control frequencies
> worldwide**, from the public-domain OurAirports database, plus national information
> frequencies where we have them.
>
> Once downloaded it works offline. About **2 MB**.
>
> Listening to aviation radio is lawful in many countries and restricted in others. Please
> check the rules where you are.
>
> **[ Download ]**  [ Not now ]

**Tone is the whole point here.** This is public-domain data published by civil aviation
authorities. It is not contraband. The copy is plain, factual, and carries the same energy
as "this will use 4 MB of data" — because that is genuinely what it is.

**Do not:**
- Use a warning triangle, red text, or a modal that must be dismissed.
- Write "Are you sure?" or require a typed confirmation.
- Imply that the app is helping the user do something it is coy about.

A furtive gate on ordinary public data invites exactly the suspicion it is trying to
deflect, and makes VibeSDR look shifty about something that is completely unremarkable in
most of the world. The one-line legality note is there because it is **true and useful**, not
because it is a shield. Say it once, plainly, and move on.

### 9b. Everything else

- **New provider `airband-geo`**, sibling to the EiBi provider. Same materialisation into
  the bookmark store, same search integration.
- **Grouped list**, reusing the collapsible-card pattern from the v10 instance picker: one
  card per airport, sorted by distance, name + distance/bearing on the card, frequencies as
  rows within. *Sywell will be the top card at Stuart's QTH and should look obviously,
  satisfyingly correct — that is the acceptance test that matters.*
- **Pinned "National" card at the top**: universal entries + the receiver country's FIS
  entries (region-filtered). Always present, no location required. If the country has no
  curated list yet, show universal only, plus a quiet one-liner: *"No national frequency
  list for {country} yet — [contribute one]"* linking to `CONTRIBUTING-AIRBAND.md`. Turns a
  gap into an invitation.
- **Radius selector** (10 / 25 / 50 / 100 nm) in the section header.
- **Staleness line** on the National card where `verified` is >18 months old.
- **One-time explainer**, dismissible, never shown again:

  > Airband is line-of-sight. From the ground you will usually hear **aircraft** — they are
  > high up and radiating down towards you — far more clearly than the **ground stations**,
  > even nearby ones. Hearing only one side of the conversation is normal, and it is not
  > your radio. Getting the antenna outside and high helps enormously.

  Pre-empts the most common support question, and is *true*. (The legality note lives in the
  download prompt, §9a — say it once, not twice.)

---

## 10. Acceptance criteria

1. **First open of Airband with no data shows the download prompt (§9a) — not an empty list,
   not an error.** Declining leaves Airband in a clean, re-promptable state.
2. **After download, Airband works fully offline** — aeroplane mode, no server, still lists
   and tunes. (This is the acceptance test that matters most in the field.)
3. **"Remove downloaded frequency data" genuinely clears it** and returns Airband to its
   pre-download state.
4. Location granted at Stuart's QTH: **Sywell (EGBK) is the nearest card**, its A/G
   frequency correctly labelled and matching the UK AIP.
5. **A Brazilian user gets 336 real frequencies, an Indonesian user 180, a Singaporean 19 —
   with no curation work done.** Verify with a spoofed location for each.
6. **No frequency outside 118–137 MHz ever appears in an Indonesian (or any) list.**
   Regression test specifically against the `8.88 / 295.60 / 534.00 / 558.00` rows.
7. A European 8.33 kHz channel tunes to the correct carrier (unit test vs ICAO table).
   **A Brazilian 25 kHz channel passes through unmodified** (unit test — this is the one
   that will get broken by a well-meaning refactor).
8. Scanning on Local Hardware catches **both halves** of a Sywell circuit exchange without
   manual intervention.
9. The scanner never locks onto an ATIS/VOLMET carrier, in any configuration.
10. Connecting to a remote UberSDR in another region swaps the list to *that receiver's*
   aerodromes, not the phone's.
11. Airband is not offered at all on an HF-only KiwiSDR.
12. **CI fails if any `airband-national.json` entry lacks a `cite`.**
13. No new dependency, no API key, no non-free data in the tree.

---

## 11. Bundled bugfix — VibeServer web client: squelch reported as a fault

**Related, and it ships with this brief because airband is squelch-driven by definition:
anyone scanning airband on VibeServer will hit this constantly.**

### Symptom
With squelch enabled, the web client's signal meter is replaced by a **pulsing red**
`NO SOUND — IS THE TAB MUTED?` banner. The tab is not muted. The squelch is working. The
warning is wrong, alarming, and it hides the meter.

### Root cause (three parts)
1. `web/client/src/audio.ts` — `health` returns `'silent'` after 5 s with no audible
   frames. Squelch is applied **server-side** (`spec.setSquelch()` → server DSP), so the
   client's audio engine has no idea squelch is even on. Closed squelch is
   indistinguishable from silence to it.
2. `web/client/src/main.ts` — `'silent'` maps to `NO SOUND — IS THE TAB MUTED?`.
3. `web/client/index.html` — **the flashing is literal**: `#sigFault` carries
   `animation: recPulse 1.6s ease-in-out infinite`, and `#sig.fault` sets
   `display:none` on `#sigFill` and `#sigPeak` — so **the meter vanishes exactly when
   squelch is on, which is precisely when you most want to watch a signal climb towards the
   threshold.**

### Fix
Teach the audio engine the squelch setting; add a distinct `squelched` state; treat it as
**information, not a fault** — amber, steady, meter stays visible.

**`audio.ts`:**
```diff
   private _muted = false;
+  /** Server-side squelch threshold, dB. <= -100 means OFF (matches the app's convention). */
+  private _squelchDb = -100;
+  set squelchDb(db: number) { this._squelchDb = db; }
+  get squelchActive() { return this._squelchDb > -100; }
@@
-  get health(): 'ok' | 'suspended' | 'no-stream' | 'muted' | 'silent' {
+  get health(): 'ok' | 'suspended' | 'no-stream' | 'muted' | 'squelched' | 'silent' {
     if (!this.ctx) return 'no-stream';
     if (this.suspended) return 'suspended';
     if (!this.streaming) return 'no-stream';
     if (this._muted) return 'muted';
-    if (this.lastAudibleAt && performance.now() - this.lastAudibleAt > 5000) return 'silent';
+    // Silence with squelch ON is the squelch doing its job — not a fault. Only with
+    // squelch OFF is silence suspicious, and then the likeliest cause is a tab mute.
+    if (this.lastAudibleAt && performance.now() - this.lastAudibleAt > 5000) {
+      return this.squelchActive ? 'squelched' : 'silent';
+    }
     return 'ok';
   }
```

**`main.ts` — `updateStatus()`:**
```diff
   let fault = '';
+  let info = '';
   switch (audio?.health) {
     case 'suspended': fault = 'AUDIO PAUSED — CLICK THE PAGE'; break;
     case 'no-stream': fault = 'AUDIO DISCONNECTED'; break;
     case 'silent':    fault = 'NO SOUND — IS THE TAB MUTED?'; break;
+    case 'squelched': info  = 'NO AUDIO — SQUELCH ENABLED'; break;
   }
-  $('sig').classList.toggle('fault', !!fault);
-  $('sigFault').textContent = fault;
-  $('sigFault').classList.toggle('show', !!fault);
+  // A fault replaces the meter and pulses. Squelch does neither: it is expected, and the
+  // meter is the one thing you want to watch while waiting for a signal to break through.
+  $('sig').classList.toggle('fault', !!fault);
+  $('sig').classList.toggle('squelched', !fault && !!info);
+  $('sigFault').textContent = fault || info;
+  $('sigFault').classList.toggle('show', !!fault || !!info);
+  $('sigFault').classList.toggle('info', !fault && !!info);
```

**`main.ts` — squelch slider + settings restore (keep the mirror in sync in BOTH places):**
```diff
   slider('sql', 'sqlVal',
     (v) => (v <= -100 ? 'OFF' : `${v} dB`),
-    (v) => spec!.setSquelch(v),
+    (v) => { spec!.setSquelch(v); if (audio) audio.squelchDb = v; },
     'squelch');
@@ pushSettingsToServer()
-  const sql = num('squelch');       if (sql !== undefined) spec.setSquelch(sql);
+  const sql = num('squelch');
+  if (sql !== undefined) { spec.setSquelch(sql); if (audio) audio.squelchDb = sql; }
```

**`index.html`:**
```diff
 #sig.fault { height: 1.6em; background: rgba(224,80,80,0.18); border: 1px solid var(--red); }
 #sig.fault #sigFill, #sig.fault #sigPeak { display: none; }
+/* Squelch closed is not a fault. Amber, steady, and the meter STAYS VISIBLE so you can
+   watch a signal climbing towards the threshold. */
+#sig.squelched { border: 1px solid var(--amber); }
 #sigFault {
   display: none; position: absolute; inset: 0; align-items: center; justify-content: center;
   color: var(--red); font-size: 0.75em; letter-spacing: 0.12em; white-space: nowrap;
   animation: recPulse 1.6s ease-in-out infinite;
 }
+#sigFault.info { color: var(--amber); animation: none; opacity: 0.85; }
 #sigFault.show { display: flex; }
```

### Notes
- `_squelchDb` is a **cache of a setting**, not live gate state — the client cannot know
  whether the server's squelch is open or shut right now, only that it is armed. That is
  sufficient here. **`// TODO`:** if a server→client `squelch-open` / `squelch-closed` event
  is ever added to the VibeServer protocol, the 5-second timeout can be dropped entirely and
  this becomes instantaneous. Worth considering alongside the airband scanner work, since
  the scanner needs squelch-open events anyway.
- **Check the native RN client for the same bug.** If the "no audio" health logic was ported
  across, it will have the identical false positive — and squelch is *far* more likely to be
  on in the native app than in the web one.

---

## 12. Out of scope (explicitly)

- **ADS-B / aircraft correlation.** Showing *which* aircraft is transmitting would be
  wonderful and is a natural v11 follow-on (1090 MHz decode is well within VibeDSP's reach
  on an RTL-SDR; OpenSky has a free API). Not in this brief. Ship the frequencies and the
  scanner first.
- **ACARS / VDL2 / HFDL decode.** Separate work.
- **openAIP airspace data.** Rejected on licence grounds.
- **UHF military airband (225–400 MHz).** Data largely unpublished; legality varies. Not now.

---

## 13. Open questions for Stuart

1. **Singapore first.** It is one FIR and about four fields — the fastest national list to
   complete, and it will be the most *complete* one in the app. Good proof of the schema
   before tackling Brazil's five FIRs.
2. **Default radius.** 25 nm feels right in the abstract, but you have Sywell on the
   doorstep *and* Cranfield, Cambridge, Wyton and Conington inside ~20 nm. Try 25 and see
   whether it reads as rich or crowded.
3. **Activity log persistence.** Across sessions, or reset per scan? Persisting turns it
   into a genuine multi-week local band survey, which is a lovely thing to own — but it
   needs a clear-down control.
4. **Legality** (§9). Worth reading before this becomes a headline feature, given where your
   users now are.
