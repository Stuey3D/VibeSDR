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

## 3. ★★★ THE RADIO MODEL IS NOT A NICE-TO-HAVE
Stuart: *"maybe even store the radio model used too."* ★ **This is what makes the numbers mean
anything.**

dBFS is relative to the receiver's own full scale. −60 dBFS on an Airspy HF+ with a real aerial and
−60 dBFS on an RTL dongle on a whip are not the same signal, and SNR varies with the backend's
bandwidth choices too. Without provenance a log spanning receivers produces numbers that look
comparable and are not — [[feedback_no_inferred_hardware_readouts]] in its most seductive form,
because these ARE measured, just not on a common scale.

**Record with every entry:** radio driver + model (`hwinfo` already carries both) · sample rate ·
server URL / "local" · antenna (a free-text field the user sets per receiver) · app version.
★ Scope §1 means these rarely vary, which is exactly why the few times they DO vary must be visible.

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

## 7. LOCATION — DELIBERATELY HELD BACK
Time and place is what turns a log into a catch record, and *"out and about"* makes it more valuable
still. **But:** `ACCESS_FINE_LOCATION` was **removed** from the Android manifest for the v10 Play
submission — it was declared, never used, and contradicted the Play declaration; best explanation for
9.0.2 sitting in review a week.

★★ Re-adding location, even coarse, means a new Play data-safety declaration and an iOS purpose
string. Everything else in this brief is local-only and needs **no declaration at all** (same
reasoning as the deferred Diagnostics row).

**So:** build the logbook without location, leave the field in the schema, and treat location as a
separate opt-in decision. It must not ride in on this feature's coat-tails and re-open a review
problem that was just closed.

---

## 8. ★★ THE VIBESERVER HALF: LOGGING WHEN NOBODY IS WATCHING
The thing only VibeServer can do. It runs 24/7 on a Pi, a Mac or an Android, it has the aerial, and
it is already decoding RDS continuously. It could record every PI it sees, all day, **unattended** —
so a sporadic-E opening at 11am on a Tuesday is in the log when you get home instead of missed. No
phone app can do this: the phone is not on the aerial and is not awake.

★ That turns VibeServer from *"a receiver you connect to"* into *"a receiver that has been listening
for you"* — a larger product claim. It also gives the multi-radio pool ([[vibeserver_multiradio]]) a
purpose beyond capacity: one radio parked on a band monitoring while another is being listened to.
★ Cost is known and measured — see the Pi benchmark: ADV RDS as shipped is ~115% of a core at
1.024 MSPS on a 32-bit Pi, so an unattended monitor is one core, not a spare machine.

---

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
