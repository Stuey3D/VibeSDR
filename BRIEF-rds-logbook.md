# BRIEF: the Advanced RDS logbook

**Status:** not started, designed 2026-07-31 with Stuart. Not scheduled — after the 10.0.1 list.
Started from an FM-DXer's suggestion about bookmark ordering and turned into something larger.

## The origin
An FM-DXer, on the bookmark list:
> *"How about we order them in order of confidence score? Heart and BBC Radio Northampton would be
> ~100%, BBC Radio 1/2/3/4 lower as they are weaker."*

★ The request reads as a sort option. It is not. **He is asking the bookmark list to be a logbook** —
"what I heard, and how well" rather than "take me back here". Stuart had independently been thinking
about logging for VibeServer; the two arrived at the same object from opposite directions.

Stuart: *"rather than simple bookmarks — we keep them — we add a logbook mode in the Advanced RDS
window. Any time Advanced RDS is in use we store the entire data stack and extras like the SNR and
S-reading dBFS, and then they can view the logbook on device or export it too."*

★★ **BOOKMARKS DO NOT CHANGE.** Hand-curated shortcuts and an automatic catch record are different
things; merging them gives a list that is half favourites and half catches, sorted together, and
spoils both. `userBookmarks.ts` stays exactly as it is.

---

## 1. ★★★ SCOPE: OUR OWN HARDWARE AND VIBESERVER ONLY
Stuart, and this is the decision that makes the whole feature coherent:
> *"This advanced logbook is for our local hardware and VibeServer only. A user isn't going to want
> to log other people's radios — they'd rather log their own, and do it out and about."*

★★★ **A LOGBOOK IS A RECORD OF YOUR OWN STATION.** An entry reading "heard on a Kiwi in Holland"
says nothing about your receiver, your aerial or where you were standing. It is not your catch.
That is the convention of the hobby, not merely a scoping convenience.

★★ **THE RULE IS "YOUR STATION", NOT "OUR DSP".** "Local hardware and VibeServer" is today's
enumeration of it, not the definition — see §1b. What is excluded is other people's REMOTE
receivers, not other people's silicon.

Consequences, all good:
- **No FM-DX / TunerScreen split.** An earlier draft worried that gating on Advanced RDS would
  exclude the FM-DX backend, *"where the FM DXers are"*. Out of scope by definition now — that
  backend receives finished text from someone else's tuner. See [[BRIEF-fmdx-backend-adapter]] for
  why that rule normally applies and why it does not here.
- The confirmation gate is **always available**, because ADV RDS is always available on these two.
- The radio and antenna are known and stable, so the dBFS comparability problem (§3) largely
  evaporates — it is your own gear across every entry.

### ★★ PLATFORM: THE iPHONE LOGS THROUGH VIBESERVER
`feedback_ios_rtlsdr_dext` — the dext was declined, local USB is **Android-only**, so an iPhone has
no local hardware to log. Stuart, asked whether that was a problem: *"nope, but we do it with
VibeServer."*

★ So the mobile story on the primary platform is **a VibeServer in the bag** — an Android phone or a
Mac with a dongle — and that is the intended shape, not a workaround. Do not design as though the
iPhone can log alone, and do not treat the server path as a fallback: on iOS it is the ONLY path,
which makes §8 (the VibeServer half) load-bearing rather than a bonus.

### ★★★ THE FIELD KIT: A PI ZERO 2 W ON ITS OWN HOTSPOT
Stuart: *"it's another use of the Pi Zero 2 W and radio, which then broadcasts a captive hotspot —
VibeServer is almost like plugging that radio directly into the phone."*

★ **That is the pitch.** Not "a server" (which sounds like infrastructure) but *a radio with no
wire*. And it is exactly what closes the iOS gap above: no dext, no USB, but a dongle on your belt
and the phone on its hotspot is functionally the same product on the platform where it is otherwise
impossible.

Already specced — `BRIEF-vibeserver-pi-iso.md` has the first-boot flow (no network on boot → open
the `VibeServer` hotspot → captive config page), and [[vibeserver_pi_two_products]] holds the
boundary: **the AP belongs to the APPLIANCE layer, never to VibeServer's config schema.**

★ **The Zero 2 W should BEAT the benchmark numbers.** It is a quad A53 and **64-bit capable**, so
Raspberry Pi OS 64-bit gets NEON compiled in — whereas every figure we have was measured on a
32-bit `armv7l` PiAware host with **no NEON**. Those numbers are the floor. WFM + ADV RDS as shipped
was 115% of one core at 1.024 MSPS; the Zero 2 W has four, and runs for hours off a USB power bank.

### ★★★ THEREFORE THE LOGBOOK MUST WORK WITH NO INTERNET AT ALL
On the Pi's own hotspot the phone has **no route out** — no iCloud, no station logos, no server
directory, no maps. **State it as a requirement**, or someone builds this assuming iCloud is
reachable at write time and it fails in precisely the field scenario it exists for.

- Every write is **local and complete**; sync is a later, separate step when the phone rejoins a
  real network. ★ The append-only shape (§5) makes that easy — union with de-dup, no conflicts.
- Anything the entry needs from the network (a logo, a transmitter database lookup, a map tile) is
  **decoration resolved later**, never part of the record.
- ★ Also expect the transport glyph to be wrong here — `UberClient.transportFor` has already been
  wrong in both directions, and "Wi-Fi that is actually a Pi in your pocket" is a new case for it.

---

## 1b. ★★★ THE TEF6686 BELONGS IN SCOPE — AND THE LOGBOOK IS WHY IT IS WORTH BUILDING
Stuart: *"this is where the TEF connection could really be useful for us, as it turns VibeSDR into
an FM-DX logging tool too."*

A TEF6686 on your desk **is your own station** — the §1 rule admits it. Only remote receivers you do
not own are excluded.

★★★ **AND IT INVERTS THE CASE FOR THE TEF INTEGRATION.** The logbook was listed as one benefit among
several (Now Playing from RT+, RadioText history, deeper decoding). It is actually **the reason**:
the TEF's own screen fundamentally cannot log — no trustworthy clock, no GPS, no storage, no export
— and its users are the most log-motivated people in the hobby. Serious FM DX is done on TEF
portables, not RTL dongles. The phone is not a nicer display for the TEF; **it is the missing half of
the instrument.**

★★ It also changes what VibeSDR IS in that market: not "an SDR client that also talks to your TEF"
but **an FM-DX logging tool** — a category with an existing audience already keeping logs by hand or
in a spreadsheet.

**Both design consequences are already accommodated:**
- **Confirmation** (§4) — the TEF decodes RDS in silicon, so there is no BER from our DSP and no
  confirmed/raw pairs. But it publishes its OWN RSSI, SNR, multipath and RDS block-error flags: a
  legitimate measurement of your own station, made by different apparatus. §4's rule already covers
  it — **the gate grades an entry, it does not decide whether one exists** — and §3's provenance
  block records which apparatus measured it.
- **Gain** (§3) — the TEF has no gain control in our sense (AGC + attenuator), so it is a fourth row
  in that table, not a special case. Precisely why per-driver gain was the right call.

★ The TEF cannot supply **grid or antenna** — no GPS, and it does not know what is plugged into it.
The phone provides both, as it already does for local hardware.
★★ Integration shape, audio and the open firmware question: no audio streaming is needed (these
radios have their own speakers); the phone is a remote control and display. Not yet briefed
separately — the open question is whether any shipping firmware exposes a TCP control port, and
whether groups arrive RAW or pre-parsed. Note `FmdxAdapter.ts` already emits TEF chip commands
(`G` = EQ/IMS, the antenna switch, `bwSwitch`) in the XDR-GTK vocabulary, so part of the work exists.

---

## 2. THE RECORD IS AN ENCOUNTER, NOT A GROUP
RDS delivers ~11 groups a second. *"Store the entire data stack"* read literally is megabytes an
hour of near-identical rows.

**One row per ENCOUNTER.** You arrive on a station; we accumulate the best of everything seen and
write one entry when the PI changes, when you tune away, or after a gap. That is what a DXer would
write by hand, and it is what reads back as a logbook.

- New encounter when: PI changes · frequency changes · the station is re-heard after a gap of more
  than a few minutes (exact figure to pick).
- ★ An optional **raw trace** for a single session is worth having for debugging a marginal catch,
  but never the default.

### Fields
**Identity:** PI · PS · long PS · RT (best/last) · RT+ artist/title · PTY · PTYN · ECC → country
([[rdsCountry]]) · AF list · language.
**Signal:** best SNR · best S-reading dBFS · BER at best · `gtot` · pilot deviation · RDS deviation.
**Time:** first heard · last heard · best-ever (with its timestamp) · times heard.
**Provenance:** see §3.

### ★★ ONE NUMBER CANNOT BE A STATION'S SCORE
FM varies by orders of magnitude — time of day, tropo, season, whether the car moved. A score frozen
at bookmark time is a snapshot presented as a property of the station. Store **best-ever**,
**last-heard** and **times-heard** separately:
- **Best ever** is the DX number and the one to sort by — it is *the catch*.
- **Last heard** answers "is it there now" — the tuning number.
- **Times heard** separates a regular from a one-off opening.

★ Sorting by the wrong one of those will annoy the DXer more than today's arbitrary order does.

---

## 3. ★★★ PROVENANCE — THE STATION BLOCK
Stuart: *"we could log the entire RDS stack, the Maidenhead grid it was received at, the hardware
used (RTL-SDR v4, Airspy HF+ etc), the gain settings at the time and the signal quality"* — plus
*"an editable field so a user can describe the antenna they were using."*

★ **This is what makes the numbers mean anything.** dBFS is relative to the receiver's own full
scale: −60 dBFS on an Airspy HF+ with a real aerial and −60 dBFS on an RTL dongle on a whip are not
the same signal, and SNR varies with the backend's bandwidth choices too. Without provenance a log
produces numbers that look comparable and are not — [[feedback_no_inferred_hardware_readouts]] in
its most seductive form, because these ARE measured, just not on a common scale.

**Record with every entry:** radio driver + model (`hwinfo` carries both) · gain state (see below) ·
sample rate · antenna text · Maidenhead grid (§3b) · server URL or "local" · app version.

### ★★★ "GAIN SETTINGS" IS NOT ONE FIELD
Straight from the AGENTS.md rule — **the three radios do not share a gain model**:

| Radio | What "gain" is |
|---|---|
| RTL-SDR v4 | a gain list in dB, plus AGC on/off |
| Airspy HF+ | **no variable gain at all** — attenuator + preamp |
| SDRplay RSP | IF gain **reduction**, plus an LNA state |

★★ A single numeric `gain` column would be actively misleading: 28 on an RTL and 28 on an RSP mean
opposite things, and on an HF+ it means nothing at all. **Store the driver's own gain state**,
structured per driver, PLUS a human-readable string for display and export:
```
"RTL-SDR v4 · 28.0 dB, AGC off"   "Airspy HF+ · att 0 dB, preamp on"   "RSPdx · IF GR 40, LNA 3"
```
Export shows the string. Anyone analysing the CSV then gets something honest instead of a column
that silently means three different things. See [[one_radio_assumption_family]].

### ★★★ THE STATION BLOCK IS A SNAPSHOT, NEVER A POINTER
Hardware, antenna, grid and gain are constant across hundreds of entries, which makes it tempting
to store them once as a profile and reference it. **DO NOT.**

★ If entry #400 points at "my antenna" and in October the user puts up a better aerial and edits
that field, **every catch they ever made has just been rewritten.** A log entry is an immutable
record of what was true at that moment — that is the entire point of a log, and the one property
that cannot be recovered once lost.

**So:** keep an editable **station profile** for convenience (it pre-fills the fields, and it is
where the antenna text is maintained), but **copy the values into each entry at write time.** The
storage cost is a few dozen bytes against an append-only file. The de-duplication instinct is wrong
here.

### ★ WHERE THE ANTENNA FIELD LIVES — two cases, and they differ
- **Local hardware** — the antenna belongs to the phone's session; editable in the app.
- **VibeServer** — the antenna belongs to the **SERVER**, exactly as its location does today
  (published, opt-in, never assumed). The Pi in the loft knows what it is connected to; the phone
  does not. So it is a VibeServer config field alongside location, published to clients, and the
  log takes it from there.
  ★★ That is also the only version that works for unattended logging (§8), where no phone is
  present to ask.

★★ **FREE TEXT — DO NOT BUILD A PICKER.** Antennas are infinitely varied ("6-element yagi at 10m,
120°", "loft dipole", "wet string out the window"), so any dropdown we invent is wrong for someone
by the second week. It is also the one field we can never validate, so it **exports verbatim,
exactly as typed**. ★ Offer RECENT VALUES as suggestions — most people have two or three and retype
them constantly — but never constrain the input.

### ★ WHY THE MODEL AND THE GAIN MUST TRAVEL TOGETHER
Stuart: *"that's why we log the radio model — so we can put the exact gain settings for that unit;
the user-editable field then says the type of antenna used."* ★★ `28.0 dB` is meaningless alone and
unambiguous beside `RTL-SDR v4`; `att 0 dB, preamp on` only parses if you know it is an HF+. Logged
together the entry is **self-interpreting** — a year later "what did I have the HF+ set to when I
caught that?" is answerable from the file itself, with no outside knowledge required. Never write
one without the other.

## 3c. ★★ NOTES — TWO LEVELS, AND THE ONE MUTABLE FIELD
Stuart: *"a notes tab a user could — say — write down the weather conditions etc."*

**Two levels, not one.** Conditions apply to an OUTING, not to a catch. In a sporadic-E opening a
user may log forty stations in an hour and will not type the weather forty times:
- **Session note** — written once, applies to everything logged in that sitting. *"Sporadic-E, hot
  and still."* ★ This is where weather belongs.
- **Entry note** — the specific one. *"PS flickered, PI held steady"*, *"first time heard"*.

★ That is how people log by hand, and it is the difference between a notes field that gets used and
one ignored because it is too much typing.

### ★★★ THE LINE THAT KEEPS THE SYNC STORY SIMPLE
Notes are the **only** thing edited after the fact, which reintroduces mutability into a store §5
calls append-only. Resolve it as a rule, not an exception:

> ★★★ **Nothing the radio MEASURED may ever be edited. Anything the HUMAN WROTE may be.**

- Frozen: signal, gain, PI/PS/RT, grid, hardware, timestamps.
- Mutable: session note, entry note, antenna text — each with an `updatedAt` stamp, resolved exactly
  as `userBookmarks.ts` already does it.

★★ So the sync remains a conflict-free union everywhere except these fields, and a last-writer
conflict on a free-text note is genuinely harmless — unlike on a measurement, where it would be
falsification. See [[BRIEF-icloud-sync]].

★ **Do NOT fetch the weather automatically.** On the Pi's own hotspot there is no internet — which is
exactly when the user is in a field caring about conditions. Free text is the right answer, not a
degraded automatic one.

## 3b. ★★★ LOCATION = THE MAIDENHEAD SQUARE
Stuart: *"we detect coarse location, so in the logbook a reception location could simply be the
Maidenhead square."* ★★ **This is better than a privacy compromise — it is the hobby's NATIVE
UNIT.** DXers already exchange locators; lat/long would be the foreign format.

**The machinery exists:**
- `src/services/grid.ts`
- `vibeServer.ts:255` — *"Maidenhead: 2 letters, 2 digits, optionally 2 more letters. Decoded
  locally."*
- `ServerModeScreen.tsx:649` — *"A Maidenhead locator works **OFFLINE** — use it if this server has
  no internet."*

★★★ That last one is decisive given the Pi hotspot (§1): a town name needs a geocoder and therefore
the internet; **a locator needs neither.** It is the only location format that works in the exact
field scenario this feature exists for.

★★ **The privacy win is bigger than "coarse".** Logs get SHARED — that is what a logbook is for. A
6-character locator is ~5 × 2.5 km and a 4-character one ~111 × 70 km (`DecoderPanel.tsx:152`
already makes this point in our own code), so an exported log can be posted publicly without leaking
a home address. **The coarsening is in the FORMAT**, not in a policy someone has to remember to
apply on export. Far stronger than storing lat/lon and rounding later.

★ It also yields **distance and bearing to the transmitter** for free — *the* number in DX — and
plugs into the FT8 map, which already plots Maidenhead grids.

### ★★ SO §7's CAUTION LARGELY DISSOLVES — coarse location is ALREADY declared
```
AndroidManifest.xml:2   ACCESS_COARSE_LOCATION
Info.plist              NSLocationDefaultAccuracyReduced = true
                        NSLocationWhenInUseUsageDescription (sorting + map)
```
A grid square needs **no new permission**. ★ The iOS purpose string should gain the logging use — it
names only sorting and maps today, and Apple does care that it covers what the app actually does —
but that is an edit, not a new capability. See §7 for what is still deliberately out.

---

## 4. THE CONFIRMATION GATE — and its real limit today
Stuart: *"we apply all the confidence checks we do now and simply log when the data has been
confirmed."* Right rule. ★★★ **But check what "confirmed" currently covers.**

`UberSDRClient.ts` `RdsExt` — the confirmed/raw pairs are **block B only**:
```ts
pty, tp, ta, ms, di          // CONFIRMED BY REPETITION
ptyRaw, tpRaw, taRaw, msRaw, diRaw
```
★★★ **PI, PS, RadioText, RT+ and the AF list have NO confirmed/raw pair at all** — and those are
the fields a logbook is about. Nobody logs a catch because they were confident of the Traffic
Announcement flag. (This is the "RAW/CONFIRMED covers only block-B" item already on the open-bugs
list, meeting a feature that depends on it.)

### Two ways forward
**(a) Use the evidence we already have — client-side, do this first.**
`RdsExt` carries `ber` (block error rate %, −1 = unknown) and `gtot` (groups decoded, 0 = no block
sync). Combine those with **PI stability** — the same PI N times running without changing — and the
gate is defensible today. ★ It is close to what a DXer does by eye: *the PI stayed put and the
errors were low, so I'll count it.*

**(b) Extend confirmation properly — and note WHERE that is.**
```
★★ "NOTHING HERE IS DERIVED ON THE CLIENT — not the constellation, not the MPX curve,
    not the confirmations. The analyser runs beside the decoder on the server, where the
    baseband actually is."                                    — UberSDRClient.ts, RdsExt
```
So confirmation-by-repetition for **PI and PS is a VIBESERVER change, not an app change.** Better
long-term answer: it is where the block-B version already lives, and it reaches the web client and
Jr for free. Different repo, different release cycle — know that before scheduling it as an app task.
★ The existing reasoning carries over unchanged: the server sends both and never picks, because one
listener switching views must not alter what another listener on the same receiver sees.

### ★★ THE GATE GRADES AN ENTRY, IT DOES NOT CREATE ONE
Log the encounter either way; record **how well established** it was. A DXer knows the difference
between a confirmed catch and a glimpse and will want to see which is which — that column is
arguably the most valuable one in the book.

---

## 5. ★★★ STORAGE: AsyncStorage CANNOT HOLD THIS
```ts
saveUserBookmarks(list) → AsyncStorage.setItem(KEY, JSON.stringify(list))
```
It serialises and **rewrites the entire list on every change**. Fine for eighty bookmarks. A log
writing continuously for hours and accumulating thousands of entries would rewrite the whole blob
each time, getting slower as it grows.

We have `expo-file-system`; there is **no SQLite in `package.json`**. So either add `expo-sqlite` or
go append-only to a file — which suits a log, since entries are created and never edited.
★★ **Decide this before anyone has a log.** It is painful to change afterwards.

### ★ A log SYNCS more easily than favourites do
[[BRIEF-icloud-sync]]'s rule is that a merge must never be last-writer-wins, and
[[icloud_stale_tombstone_immortal]] is what happens when records mutate. A log is **append-only**:
the merge is a union with de-dup on (time, frequency, PI). No tombstones, no conflicts, no immortal
zombies. It is the easy case of the thing that has caused us the most trouble.

---

## 6. EXPORT
**CSV primary** — it opens in a spreadsheet, which is what people do with logs. JSON alongside, as
we already do for bookmarks.

★★★ **It hits the SAME BUG as the SSTV save.** Export must write a real file and share a `file://`
URL, never a `data:` URL — see [[BRIEF-decoder-panel-fixes]] §2, where a `data:` URL produces a blank
share sheet on macOS and an iPhone sheet missing Save to Photos / Save to Files. Do that fix once,
properly, so the third feature that needs it simply works.

---

## 7. LOCATION — COARSE ONLY, AND NOTHING MORE
★ Superseded in part by §3b: the **Maidenhead square** is the location field, coarse location is
already declared on both platforms, and no new permission is needed.

**What remains true and must be held:**
- ★★★ **NEVER store precise coordinates**, not even privately. The grid square IS the record. A
  lat/lon in the file is a home address waiting to be exported, and export is the point of a log.
- ★ Keep it **opt-in**, consistent with how VibeServer already publishes its own location — *"opt-in,
  never assumed"*. A log entry with no grid is perfectly valid.
- ★ Update the iOS `NSLocationWhenInUseUsageDescription`: it currently names only instance sorting
  and map alignment.
- ★ Play data-safety: local-only storage is not "collection", but the export path deserves a check
  before shipping rather than an assumption.

### ★★★ UNRELATED BUT FOUND HERE — `ACCESS_FINE_LOCATION` IS STILL IN THE TREE
Checked 2026-07-31: `AndroidManifest.xml:3` **and** `app.json:50` both still declare
`ACCESS_FINE_LOCATION`. The notes record it as REMOVED for the v10 Play submission (versionCode 86),
as the best explanation for 9.0.2 sitting in review for a week. But the manifest has not been
touched since **11 July** (`cfaf08c2`), and `git log -G ACCESS_FINE_LOCATION` finds only the
initial-release commit — **no commit ever removed it.**

Either it was an uncommitted working-tree edit that has since been lost, or it was never done and
the note recorded an intention as a fact. The AAB is no longer on the Desktop, so what actually
shipped could not be verified.
★★ **Resolve this independently of the logbook.** If the uploaded bundle still declares it, the
theory about the stalled review is untested; if it did not, the next Android build will silently put
it back.

---

## 8. ★★★ NOT A BACKGROUND FEATURE — IT LOGS WHILE YOU LISTEN
★ An earlier draft of this brief proposed unattended logging: a VibeServer left running for days,
recording every PI it saw so an opening at 11am on a Tuesday was in the log when you got home.
**Cut.** Stuart, 2026-07-31: *"RDS logging is not a background, server-not-in-use thing."*

★★ It follows from §1's own rule. A logbook is a record of **your own station** — and, by the same
token, of **your own listening**. Something the receiver caught while nobody was there is not a
catch in the sense the hobby means; you were not there to hear it.

**Consequences, all simplifying:**
- The logbook runs **only while a user is connected and listening**. No daemon, no scheduler, no
  "was anyone watching?" question.
- ★ It removes the conflict with the idle-power work: an idle radio is free to **park or duty-cycle**
  with nothing competing for it. See `BRIEF-idle-park-never-stops-the-radio.md` — its idle setting is
  a simple two-way (park · snapshot), NOT three.
- ★ What an idle server DOES produce is a **band-activity timelapse** — wake, snapshot a slice of
  spectrum, sleep, and let a spectrogram build up over time, exactly as UberSDR's own
  `band_activity.html` does. That is `BRIEF-band-activity-snapshots.md`, and it is a different
  feature with a different purpose. Do not merge the two.

## 9. ★ THE PAYOFF VIEW — a band-conditions display
Once the fields exist, the list can update **live** while connected: each entry showing what it is
doing *now*, not what it did once. The list stops being shortcuts and becomes a conditions display —
glance at it and see the usuals are normal but three distant ones have come up. **That is an
opening, visible without tuning to anything.**

---

## Open questions
1. `expo-sqlite` or append-only file? (§5 — decide first)
2. Encounter gap length before a re-hear becomes a new entry.
3. Which confidence the DXer actually meant — signal quality, or decode certainty? They are the same
   number for a local and come apart completely on a DX catch, and the answer shapes the default
   sort. ★ Worth asking him directly; he is the reason this exists.
4. Where the viewer lives. Stuart said "in the Advanced RDS window" — the *entry point* belongs
   there, but a scrollable log probably wants its own screen. ★ Relates to the BIG-button work in
   [[BRIEF-decoder-panel-fixes]] §1: panels in this app are height-capped on purpose.
