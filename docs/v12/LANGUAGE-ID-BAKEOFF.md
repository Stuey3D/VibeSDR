# TRANSCRIBE AUTO: language-ID bake-off (Apple only)

2026-10-17, measured on Stuart's Mac (M4, 10 cores, 16 GB, macOS 27.0.1) using real off-air audio. The question was
how V12's TRANSCRIBE "AUTO" should work out which language is being spoken.

Stuart's direction during the work:
- Avoid third-party components. Whisper and the hybrid approach were dropped before anything was installed.
- Brute-force it: run every installed model on every window.
- Slower devices need a fallback.
- Translation works better once it locks on, so lock fast and then stay locked.

So the only candidate measured here is Apple's Speech framework (SpeechTranscriber / DictationTranscriber), scored by
our own code with Apple's NaturalLanguage framework as a cross-check.

## The answer in one paragraph

**AUTO today picks the wrong language on English most of the time, because its only signal is mean word
confidence.** Apple's French, German, Spanish, Italian, Portuguese and Chinese models transcribe English audio as
plausible English text, and their confidence is often *higher* than the English model's, especially on SSB.
Measured on 10 s windows of speech, today's rule:
- named the wrong language in 34 % of windows, and in **60 % of English windows**;
- named the wrong language in 58 % of SSB windows.

The fix is two cheap checks, both Apple-only:
1. **Score each language by confidence × NaturalLanguage's probability that the model's own transcript is in that
   language.** A Spanish model writing English text gets a score of about zero.
2. **Let every model's transcript vote**, through NaturalLanguage, for the language it reads as.

A third check is needed for Chinese, Japanese and Korean: a model in a script no other language uses must produce
fluent-rate text (at least 1.5 tokens per second), otherwise it does not count.

With these checks (rule **H2**), 10 s windows were correct in 84 %, wrong in 1 % and gave no answer in 15 %. On 234
windows of noise, data and uninstalled languages it gave 0 false answers.

Running every installed model on a 10 s window costs about **0.18 s per language** on the M4 (about 0.10 s when two
run side by side). So brute force over the 13 installed languages takes about 1.6 s per 10 s window.

I also simulated the lock-then-hold rule (challenger must win 2 consecutive speech windows by 0.10) on the recorded
clips:
- **0 false switches** on 35 single-language clips;
- **0 false locks** on 25 noise or uninstalled-language clips;
- language changes caught with a median delay of 20 s, and never switched to the wrong language.

## Test set (built first, real off-air audio)

All clips are in `~/VibeSDR-test-audio/lang/<language>/`, 16 kHz mono, 90 to 120 s each, 197 MB in total.
`manifest.tsv` lists each clip's frequency, mode, UTC time, signal-to-noise ratio, label and the name the bench
results use. The tools are in `lang/tools/`.

**Recording.** Everything came from Stuart's UberSDR (RX888, 192.168.86.11:8080), under the standing permission to
record from it.
- AM broadcasts were recorded as `iq48` and demodulated by our own envelope detector (`am_demod.py`), which also
  measures the carrier-to-noise ratio in 9 kHz.
- Two 192 kHz band captures (40 m at 20:20 UTC and 80 m at 20:22 UTC) were split into individual SSB signals by
  `ssb_band.py`.
- The I/Q files were deleted once demodulated.
- Five English VOLMET clips (dial on frequency) came from the existing `~/VibeSDR-test-audio/ssb/offsets/` library.

**Labels.** Each clip's language comes from EiBi (which broadcasts were scheduled at the time), then checked
against the transcripts the matching models produced.
- **EiBi was wrong for several frequencies.** 9575 kHz was CRI Mandarin, not RRI French. 9730 kHz was CRI
  European Portuguese, not Voice of Vietnam German. 13720 kHz was Radio Martí Spanish. 3955 kHz carried a Korean song
  in KBS's German slot.
- So **EiBi can only be a prior. It must never decide the language on its own.**

**Signal quality.** For AM, strong means a carrier-to-noise ratio of at least 25 dB in 9 kHz, medium 10–25 dB and
weak below 10 dB. For SSB, it is the height of the 2.4 kHz signal above the noise floor.

| Label | Clips | What |
|---|---|---|
| en | 14 | BBC Radio 5 Live (909 kHz MW), CRI English, 5 VOLMET (SSB), 7 amateur QSOs / contest (40 m and 80 m LSB) |
| fr | 6 | CRI, Radio Vaticana ×2, Voice of Vietnam ×2, RFI (medium to weak) |
| de | 7 | HCJB (weak AM) and 6 German 80 m QSOs / contest (heavy English phonetics) |
| it | 2 | CRI Italian (strong) |
| es | 2 | Radio Martí (weak) |
| pt | 2 | CRI Portuguese (European), one Portuguese 40 m QSO |
| tr / zh | 1 / 1 | Voice of Turkey (strong), CRI Mandarin (strong) |
| other | 6 | Languages with **no installed model**: Hungarian (strong), Arabic ×2, Russian, Korean-slot and Arabic-like audio. The right answer is "no answer". |
| none | 19 | Off-air noise ×14, FT8 and FT4 data, broadcast-band edge, unintelligible weak AM. The right answer is "no answer". |
| skip | 11 | Songs, a music station with an English DJ, mixed-language contests, barely audible Mandarin. Kept as examples but left out of the scoring. |

**Languages scored.** One variant per language, using the app's exact module config
(`VibeTranscriber.module(for:)`):
- SpeechTranscriber: en-GB, de-DE, es-ES, fr-FR, it-IT, pt-BR, zh-CN, ja-JP, ko-KR, yue-CN.
- DictationTranscriber: nl-BE, pl-PL, tr-TR.

There are no Arabic, Russian or Hungarian models, so those clips test rejection. pt-BR scored European Portuguese
correctly.

## How it was measured

`lidbench.swift` runs each window through each language, one fresh analyzer per window, and records:
- words, mean and median word confidence, the fraction of words under 0.3, and words per second;
- NaturalLanguage's probability for the model's own language on its transcript, and NaturalLanguage's top language;
- timings.

The window sets were 5, 10, 15, 20 and 30 s, tiled across every clip. That came to 14,000 language × window results.

**Speech windows.** Accuracy is counted only on *speech windows*: windows where the true language's model heard at
least 1 word per second. Silence, music and station IDs inside a speech clip are not counted against a rule.
**Non-target windows** are every window of the none and other clips; any answer given on one is a false answer.

## Results

### Scoring rules compared

| Rule | Window | Speech windows | Correct | Wrong | No answer | Non-target windows | False answers |
|---|---|---|---|---|---|---|---|
| A: app today (mean conf, ≥10 words, 0.55) | 10 s | 184 | 58 % | **34 %** | 8 % | 234 | 1 |
| A: app today | 15 s | 116 | 57 % | **36 %** | 7 % | 149 | 1 |
| A′: app after its last round (best seen, no floor) | 10 s | 184 | 61 % | **39 %** | 0 % | 234 | **27 (12 %)** |
| B: conf × NL(own language) | 10 s | 184 | 78 % | 4 % | 17 % | 234 | 6 (3 %) |
| B: conf × NL(own language) | 15 s | 116 | 81 % | 2 % | 17 % | 149 | 3 (2 %) |
| **H2: conf × NL × cross-model vote, CJK ≥1.5 tok/s** | **10 s** | 184 | **84 %** | **1 %** | 15 % | 234 | **0** |
| **H2** | 15 s | 116 | 87 % | 2 % | 11 % | 149 | 0 |

Also tried:
- median confidence × NL × (1 − low fraction), and conf × NL × coverage (words per second);
- B with at least 6 words.

None beat H2. The coverage term mostly added "no answer". The full sweeps can be re-run with `lid_report.py`.

**H2, exactly.** For each language L, using the model's 10 s window result:
- **Too few words:** if the model produced fewer than 4 words, score = 0.
- **Base score:** s = mean word confidence × P_NL(L | the model's own transcript).
- **CJK speed gate:** if L is zh, yue, ja or ko and the model produced fewer than 1.5 tokens per second, score = 0.
- **Cross-model vote:** share = fraction of *the other models'* transcript words (only transcripts of at least 4
  words) that NaturalLanguage reads as L.
- **Final score:** s × (0.75 + 0.5 × share).
- **Decision:** the top language is the answer if its score is at least 0.40 and it leads the runner-up by at least
  0.05. Otherwise there is no answer.

Zh and yue count as one language family. The thresholds are not knife-edge. On 10 s windows:
- T = 0.40 with no margin gave 84 % correct, 2 % wrong, 0 false answers;
- T = 0.50 gave 76 % correct, 1 % wrong, 0 false answers.

**Why each part is there:**
- **Confidence alone is the bug.** Today's confusions on 10 s windows include English taken as Italian ×13,
  Chinese ×10, German ×9, Spanish ×9 and French ×8. Foreign Latin-script models write English *as English*, so the
  text looks fine and the confidence can be higher than en-GB's. This is also why AUTO's challenger lane (`overtake`
  0.08 on mean confidence) cannot be trusted to correct a wrong first choice: it compares the same flawed number.
- **The NL check** zeroes those foreign models, because their transcript reads as English, not as their own language.
- **The vote** helps where en-GB's own confidence is low. VOLMET and contest English are full of numbers and
  callsigns, so en-GB scores only 0.2–0.5 there, but six other models' transcripts all read as English.
- **The CJK gate** exists because NaturalLanguage cannot disagree with a kana, Hangul or Hanzi transcript. Japanese
  and Korean models write 8–18 tokens of kana or Hangul in 10 s on English SSB, Hungarian and noise, all with NL
  probability 1.0 and confidence around 0.5. Fluent Mandarin gave 2.3–3.2 tokens per second. The model's *own*
  transcript is left out of the vote for the same reason: it always "reads as" its own script.

### H2 by window length (how short can it be?)

| Window | Speech windows | Correct | Wrong | No answer | Non-target windows | False answers |
|---|---|---|---|---|---|---|
| 5 s | 413 | 71 % | 2 % | 27 % | 126 | 2 |
| **10 s** | 184 | **84 %** | 1 % | 15 % | 234 | 0 |
| 15 s | 116 | 87 % | 2 % | 11 % | 149 | 0 |
| 20 s | 89 | 90 % | 0 % | 10 % | 28 | 0 |
| 30 s | 45 | 84 % | 0 % | 16 % | 16 | 2 |

Ten seconds is the knee. Five seconds is usable for a tentative label, but it locked falsely on 2 of the 6 non-target
clips in the lock simulation. Longer windows add little, because a longer window is more likely to contain music or
an ID.

### By language, by quality, by kind (10 s windows: today's rule A against H2)

| Language | Speech windows | A correct | A wrong | H2 correct | H2 wrong |
|---|---|---|---|---|---|
| en | 88 | 30 % | **60 %** | 80 % | 1 % |
| fr | 40 | 85 % | 8 % | 90 % | 0 % |
| de | 17 | 65 % | 24 % | 65 % | 6 % |
| it | 14 | 93 % | 7 % | 100 % | 0 % |
| pt | 8 | 100 % | 0 % | 100 % | 0 % |
| tr | 7 | 86 % | 14 % | 100 % | 0 % |
| zh | 7 | 86 % | 0 % | 86 % | 0 % |
| es | 3 | 100 % | 0 % | 100 % | 0 % |
| none (174 windows) / other (60 windows) | — | false answers 0 / 1 | | false answers **0 / 0** | |

| Signal quality | Speech windows | A correct | A wrong | H2 correct | H2 wrong |
|---|---|---|---|---|---|
| strong | 39 | 85 % | 13 % | **97 %** | 0 % |
| medium | 121 | 49 % | 43 % | **84 %** | 1 % |
| weak | 24 | 62 % | 21 % | 62 % | 4 % |

| Kind | Speech windows | A correct | A wrong | H2 correct | H2 wrong |
|---|---|---|---|---|---|
| AM broadcast | 93 | 85 % | 10 % | 92 % | 0 % |
| SSB (VOLMET, QSOs) | 91 | 31 % | **58 %** | 76 % | 2 % |

The 10 s confusions left under H2 were English→Japanese ×1 and German→English ×1, the latter in a German contest QSO
made almost entirely of English phonetics. On songs, H2 gave no answer on two Korean and Chinese songs. On a music
station it answered English, which was correct: the DJ speaks English.

### Lock fast, then stay locked (simulated on consecutive 10 s windows)

**Rule:**
- **Unlocked:** lock on the first window whose H2 best is at least 0.40 with a margin of at least 0.10.
- **Locked on X:** a challenger Y must beat X by at least 0.10 (and score at least 0.40) in **2 consecutive speech
  windows**.
- A window that X wins resets the count. A window with no speech (best score below 0.40) leaves the count alone.

**Switch tests.** Two single-language clips joined end to end. Windows are scored independently, so joining them is
exact. Only pairs whose first clip locked correctly on its own are judged.

| Setting | First lock correct | First lock wrong → corrected | Never locked (weak or garbled) | False switches | False locks on none/other | Switch tests: caught / missed / wrong | Median switch delay |
|---|---|---|---|---|---|---|---|
| **10 s, N = 2** | 28 / 35 | 2 (both later switched to the right one) | 5 | **0** | **0 / 25** | 651 / 142 / **0** | 20 s |
| 10 s, N = 3 | 28 / 35 | 2 | 5 | 0 | 0 / 25 | 582 / 183 / 0 | 30 s |
| 15 s, N = 2 | 29 / 35 | 3 | 3 | 0 | 0 / 25 | 674 / 119 / 0 | 30 s |
| 5 s, N = 4 | 28 / 35 | 4 | 3 | 0 | 2 / 6 | 527 / 231 / 0 | 30 s |
| B (no vote, no CJK gate), 10 s, N = 2 | 28 / 35 | 6 | 1 | 1 | 2 / 25 | 616 / 142 / **19** | 20 s |

The median time to lock was the **first 10 s window of speech**. The longest was 50 s, on a clip that opened with
music and IDs. The two clips that locked wrong first were German 80 m contacts that opened with English callsign
phonetics. Both switched to German after 60–80 s, which is correct behaviour. The 5 that never locked were weak or
garbled (Radio Martí, RFI weak, a poor RAF VOLMET, a phonetics-only contest exchange and a weak Portuguese QSO), and
saying nothing was the right call.

### Speech vs Dictation agreement (asked for as a signal)

This was run for en-GB, the only locale with both models installed throughout the test, on 10 s windows. On English,
the two transcripts overlapped by a median word-Jaccard of 0.32. Confidence was 0.59 for Speech and 0.51 for
Dictation.

On foreign audio, **DictationTranscriber nearly refuses**: a median of 1 word against SpeechTranscriber's 11.
DictationTranscriber is therefore its own good rejector, and the Dictation-only languages (tr, nl, pl, and ru/ar when
installed) behave well under H2: Turkish was 100 %. A second model per language for agreement would double the cost
and is **not needed**.

## Cost: brute force, scaling, and the Neural Engine question

The scaling run used one clip (CRI Italian, nine 10 s windows) with a fresh analyzer per window, which is how the app
would run it.

| Languages | Sequential, total | Per language per 10 s window | 2 at a time, total | Per language per 10 s window (2 at a time) |
|---|---|---|---|---|
| 1 | 1.6 s | 0.18 s | — | — |
| 8 | 13.8 s | 0.19 s | — | — |
| 13 (all installed, one variant each) | 21.6 s | 0.18 s | 13.1 s | **0.11 s** |
| 30 (every installed variant, en-AU … zh-TW) | 43.4 s | 0.16 s | 23.7 s | **0.09 s** |

- **Per model, per 10 s window** (median, model preparation plus analysis): SpeechTranscriber 0.09–0.18 s (Korean
  fastest, Chinese up to 0.25 s for yue-CN); DictationTranscriber 0.18–0.23 s. The first load in a process is
  0.2–0.8 s.
- **Two at a time is 1.64–1.83× faster.** The earlier "parallel is no faster" finding was for 7 at once. Running more
  than 2 is not worth it.
- **Keeping analyzers warm saves nothing.** Preparing a new analyzer costs 0.03–0.13 s once the system has the model
  resident (it keeps it between analyzers and even between processes). A warm analyzer fed window after window costs
  the same per window.
  - API trap found here: `finalize(through:)` on an analyzer started with `start(inputSequence:)` **hung forever**.
    The pattern that works is `analyzeSequence(window)` followed by `finalize(through: lastSampleTime)`.
  - Recommendation: a fresh analyzer per language per window, or one per language kept in a pool, whichever is
    simpler. Do not engineer for warmth.
- **Continuous re-scoring is affordable on this Mac.** All 13 languages on every 10 s window is 1.6 s sequential or
  about 1.0 s two at a time, so a background check costs 10–16 % of one core.
- **The Neural Engine, honestly:** Apple does not say which compute unit Speech uses, and confirming ANE use needs
  `powermetrics` (sudo, not used). Here is what can be observed:
  - all the work happens in the `localspeechrecognition` XPC service;
  - during a sequential run it used **82–86 % of one CPU core**, and **150–178 %** two at a time;
  - our own process used about 5 %.

  That is CPU-bound behaviour. If inference were mostly on the ANE, the service's CPU would be low. So "brute force
  on the Neural Engine" is not what we get on macOS. The budget below assumes CPU cost.

**Download sizes.** The Mac's free-space deltas ran from 196 to 678 MB per downloaded locale. These readings are
noisy (one was negative), but "a few hundred MB per language" is the right order of magnitude.

### Slower devices: fallback by measured speed, not model name

Every SpeechAnalyzer device has a Neural Engine, but per-language cost varies widely: an A13 against an M4, thermal
state, and Low Power Mode. So measure it and size the pool from the measurement:

1. **Budget.** Detection may use up to **3 s of analysis per 10 s window**, a 30 % duty cycle. That keeps a
   decision within about 13 s of the first speech.
2. **Measure on the first window.** Time each language's analysis (we already get it per window) and take a running
   median per language. Languages per window = floor(3 s / median cost), doubled if running 2 at a time and the
   measured 2-wide speed-up is at least 1.5×.
3. **If the whole pool fits:** brute force every window while unlocked, then the background check while locked.
4. **If it does not fit:** a shortlist in this order: EiBi's languages for the frequency now, the receiver's main
   language, the user's preferred languages, the phone's languages. Widen to the rest of the pool only if nothing
   scores convincingly (H2 below 0.40 for 3 speech windows). This is today's pool order, minus the rounds of four.
5. **Re-evaluate** on `ProcessInfo.thermalStateDidChangeNotification` and on
   `NSProcessInfoPowerStateDidChange`/`isLowPowerModeEnabled`: shrink at `.serious` or in Low Power Mode, and suspend
   the background check at `.critical`.

**Measured on the M4:** 0.18 s per language sequential (0.11 s two at a time), so the 3 s budget covers 16 languages
sequential or about 27 two at a time. All 13 installed languages fit, with room to spare.

**Estimate only, not measured: an A13-class iPhone (11).** If the cost is CPU-bound as observed, an A13 core is about
2.5–3× slower than an M4 core, and a phone throttles sooner. That puts it at roughly 0.45–0.9 s per language per 10 s
window, so about 3–6 languages within the 3 s budget (4–8 two at a time on its 2 performance cores). That is a
shortlist device for detection, and the background check should run every 30 s, not every 10 s. Measure it on a real
iPhone 11 before shipping. The design above adapts either way.

**Pool size in practice: a hard limit found today.** `AssetInventory.maximumReservedLocales` is **5 per app**. In my
tests, reservations were keyed to the executable path. Locales released with `AssetInventory.release` were **purged
by the OS within minutes** (7 of 13 vanished almost at once). A typical phone therefore has its system languages plus
at most 5 that VibeSDR reserves, so "every installed model" is usually only 3–8 languages, and brute force is cheap
even on an A13. The app should choose its 5 reserved languages deliberately (receiver area plus EiBi plus the user's
preferences) and keep them reserved, not reserve and release.

## Recommendation

1. **Replace the scoring, which matters most.** Score with H2: confidence × NL-own-language, the cross-model NL vote
   with the model's own transcript excluded, and the CJK ≥1.5 tokens/s gate. Lock at **0.40 with a 0.10 margin**.
   `convincing = 0.55` on raw mean confidence and the 0.08 `overtake` should go: they are the source of the English
   errors.
2. **Brute force on 10 s buffered windows**, with no rounds of four. Run every language in the pool (up to the
   measured budget), 2 at a time, on the last 10 s of audio that had signal. Gate on audio RMS so pure silence is
   never scored. A 5 s window may show a tentative "Sounds like …" label, but never locks.
3. **Lock fast, then stay locked.** On lock, run one live SpeechTranscriber in the chosen language as today, so the
   translator gets whole sentences. In the background, every 10 s speech window (every 30 s on shortlist devices),
   score the pool on the buffered window with separate analyzers at background priority. Switch only when one
   challenger beats the locked language by **0.10 in 2 consecutive speech windows**; non-speech windows hold the
   count. The background check must never touch the live analyzer, the transcript or the translator. On a switch,
   rebuild the live lane and translator as `choose(_:)` does now.
4. **Rejection.** If nothing reaches 0.40, show "listening / no language recognised", not a guess. That was 0 false
   answers on 234 noise, data, music and uninstalled-language windows. This replaces today's "settle on the best
   heard anywhere", which gave 12 % false answers.
5. **Priors only break ties.** Use EiBi, the receiver's language and the user's preferences only when two languages
   are within the margin, and to order the shortlist. EiBi was wrong for at least 3 of the frequencies recorded today.

### What the app change looks like (VibeTranscriber.swift, outline)

- **New `LanguageScorer`** (q-only):
  - `score(window: AVAudioPCMBuffer, locales: [Locale]) async -> [Locale: Score]`. It runs fresh analyzers 2 at a
    time with `analyzeSequence` + `finalize(through:)`.
  - Collects finals and `ConfidenceAttribute` per run as `onResult` does now.
  - Runs `NLLanguageRecognizer` per transcript (`languageHypotheses(withMaximum: 5)`) and computes H2.
  - Records the analysis time per language for the budget.
- **A 10 s ring buffer** of the mono feed, in `feed(_:)`'s source format, with an RMS speech gate. Nothing about the
  live lane changes.
- **`TranscribeEngine` state:**
  - `detecting`: score every window, lock on H2.
  - `locked(lane)`: the live lane, plus a background `LanguageScorer` with a challenger counter.
  - These replace the rounds (`laneCap`, `round`, `rounds`, `bestSeen`, `openRound`), the probing lanes and the
    challenger lane. A single live model while locked also means less work than today's chosen + challenger lanes.
- **Budget:** `languagesPerWindow` from the measured median cost. Re-evaluate on thermal and Low Power Mode changes,
  and fall back to the shortlist order `begin()` already builds.
- **Asset handling:** reserve at most 5 locales, chosen deliberately. Never release a locale just after downloading
  it, or the OS purges it.
- The "language changed" status (`recentMean < 0.35`) is replaced by the background check, which also catches a
  change to a language the live model writes fluently.

## Risks and open items

- **No Japanese or Korean speech in the set.** They were off air or inaudible from the UK this evening. The CJK
  1.5 tokens/s gate was set on Mandarin (2.3–3.2 tokens/s) and on CJK junk (0.8–1.8 tokens/s). Real Japanese and
  Korean token rates must be checked; the gate could make them never lock. Record NHK World and KBS when they are
  audible.
- **Few clips for some languages.** Spanish and Portuguese have only 2 clips each, Turkish and Chinese 1 each.
  English, French and German carry most of the statistics. The thresholds were chosen on the same data. They are
  round numbers and not knife-edge (see above), but there is no held-out set: re-run `lid_tables.py` after adding
  clips.
- **Weak signals** stay at 62 % correct (abstaining, not wrong): the models hear nothing usable. Nothing in the
  scoring will fix that.
- **Mixed-language amateur radio** (German or Spanish operators with English phonetics and callsigns) is genuinely
  ambiguous. The hold rule makes it settle on the conversation's language after a minute, not on the callsigns.
- **Purged models:** if the OS purges a model mid-session, the pool shrinks silently. Check
  `installedLocales` before each detection pass.
- **Timings were taken while another agent's DSP test** held one core at 100 % for part of the main run. The scaling
  table was run after it stopped.
- **I downloaded 13 locales before Stuart's stop.** They were pt-BR, zh-CN, ja-JP, ko-KR, hi-IN (with the other
  Indian locales), ru-RU, ar-SA, tr-TR, ro-RO, nl-NL, id-ID and uk-UA, with pt-PT along with pt-BR. All reservations
  were then released. The OS purged hi, ru, ar, ro, nl-NL, id and uk within minutes. ja, ko, pt, zh and tr remained
  installed and were used.

## Reproduce

```
cd ~/VibeSDR-test-audio/lang/tools
swiftc -O -swift-version 5 lidbench.swift -o /tmp/lidbench           # bench (Speech + NaturalLanguage)
./runall.sh out.jsonl full  ../fr/*.wav                              # all 13 languages × 5/10/15/20/30 s windows
python3 lid_tables.py labels.json results/*.jsonl                    # the tables above (gunzip results first)
python3 lid_lock.py labels.json results/*.jsonl --H2                 # lock / hold simulation
./rec_batch.sh 90 <kHz>…                                             # record more AM off the UberSDR (≤ 8 at once)
node eibi-now.mjs 2030 S,F,D                                         # what EiBi lists on air (needs sked-a26.csv)
```

`labels.json` is keyed on the bench names. `manifest.tsv` maps each one to its file under `lang/<language>/`.
