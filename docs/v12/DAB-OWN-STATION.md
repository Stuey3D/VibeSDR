# DAB: each listener's own station on one multiplex (V12, Phase 1)

Branch `v12-dabstations`, off `v12-experimental`. Stuart's design is in memory `dab_per_listener_wideband.md`. This
document covers the FINAL RULE, which replaces the earlier slot design.

## 1. What changes for a listener

Today DAB is a whole-radio mode. One multiplex is decoded, one service plays, and every listener hears it. A
listener who picks another station switches it for everyone.

With **own station** on, a listener who picks a service inside the multiplex hears *that* service, and nobody else's
audio changes. Two listeners who pick the same service share one decode. The multiplex itself (the block) is still
the radio's: changing it is the same whole-radio action as today, under today's shared-dial rules (ask-before-tuning,
the owner's scan lock, the DX sweep).

The station list marks every service that is playing on this radio and shows how many listeners it has
(`▶ 2`), so joining a station that is already playing is obvious.

## 2. The rule (Stuart, 2026-11-15, final)

- The benchmark measures **N**, the number of stations this box can decode at once (§6).
- **N ≥ the radio's user limit**: own station is ON. Everyone may pick their own station. There are no slots, no
  asking and no eviction: the number of stations in use can never be more than the number of listeners, and that is
  never more than N.
- **N < the user limit**: own station is OFF. One station plays for everyone, exactly as today.
- The setup page tells the owner: *"This box can give up to N listeners their own DAB station. Lower the listener
  limit to N to turn it on."* The owner makes that choice. The server never lowers a limit itself.
- The owner can always switch it on or off whatever the benchmark said ("benchmark sets the default, never hides").
  Config key `dabOwnStation`, per radio: `-1` follow the rule (the default), `0` off, `1` on. When it is forced on
  above N, the box may run hot. Audio stays clean as long as the cores keep up, and the health chip shows the strain.
- A radio with a user limit of 1 has nobody to share with, so the feature has no meaning there. The rule reads
  "on", and nothing changes: one listener always drives the radio's own station (rule C below).

## 3. Data flow

```
 IQ ─► DabService (the radio's, g_dab) ── worker "vibe-dab": sync · FFT · demap · FIC · TII · quality
          │                                    │
          │                                    ├─► its own MSC (vibe-dab-msc): the PRIMARY station, as today
          │                                    │
          │                                    └─► MSC TAP (only while stations exist): one shared, refcounted copy
          │                                        of the frame's four CIFs (221 184 soft bits), plus a mux snapshot
          │                                        (stats; ensemble ~1/s; TII lines, scopes ~1/s)
          │                                              │
          │                     ┌────────────────────────┼────────────────────────┐
          │                     ▼                        ▼                        ▼
          │           station DabService "vibe-dab-st"  …                      (≤ N − 1 of them)
          │           (STATION MODE: no IQ; its own sub-channel time-deinterleave + depuncture + Viterbi +
          │            RS + FAAD2 / MP2 + PAD; its own PCM / ADTS queue)
          ▼                     │
   primary PCM ──► shim audio clock (20 ms) ──► ONE encode per station ──► that station's listener sockets
```

- **Shared per multiplex, once:** OFDM sync, FFT, demapping, frequency de-interleave, FIC and the service list, TII,
  quality, the PAD label scan and the SPI carousel. All of this stays in the radio's `DabService`, untouched.
- **Per station in use:** sub-channel extraction, the time de-interleaver, depuncturing, Viterbi, energy dispersal,
  the DAB+ super frame and RS, FAAD2 or MP2, PAD (DLS, slides), and one Opus encode for everyone on that station.
- **The primary station is the radio's own.** It is what plays today. It keeps the auto-select rule, the "last
  station on this multiplex" memory, resume (`g_dabWantSid`), DX and scan behaviour, and it serves every listener who
  has not picked something else. **With the feature OFF, or with one listener, the primary is the only station, the
  tap is empty, and the decode path is the current one, byte for byte.** The only addition to the hot path is a
  test of whether the tap is empty.
- **A station is a `DabService` in station mode.** Reusing the class means reusing all ~2 000 lines of audio chain
  (super frames, RS, FAAD2 timing, MP2 concealment, PAD, slides, the report builder) instead of copying them. A
  second copy would be the "two readers" fault this codebase keeps paying for. In station mode the object never sees
  IQ. Its worker takes MSC frames from a bounded queue, adopts the multiplex's snapshot, and runs the receiver's MSC
  half (`DabReceiver::pumpMscFrame`) for its one service.

### Station routing (the shim, `vibe_dab_own.h`, pure functions, unit tested)

A listener is a **session** (`sockSession`), so the spectrum and audio sockets of one client go together. When
listener X picks service S and the feature is ON:

- A. If S is the primary's service, X joins the primary.
- B. If a station already plays S, X joins it.
- C. If nobody else is on the primary, the primary itself retunes to S, and X stays on the primary. A lone listener
  therefore never creates a station, and that is today's path exactly.
- D. Otherwise a new station is opened for S. X's old station is released first if X was its last listener.
- E. If no station can be opened (the cap, which with N ≥ limit cannot happen, or the owner's forced on above N with
  a full house), X's choice switches the primary, as today, and a notice says why.

A station with no listeners left is closed at once and its thread joins on the control thread, never the audio
thread. With the feature OFF, `dab_service` is a whole-radio action, exactly as today.

### What stays whole-radio

The multiplex (`dab` with a block, `tune` while in DAB), DAB entry and exit, the band scan, the DX sweep, and the
owner's scan lock. **A block change closes every station**, because a station's sub-channel belongs to the old
ensemble. Each listener's choice is kept, and §5 applies it on the new block.

## 4. Threading

`Network > Audio > Spectrum > Decoders` (feedback_thread_priority_order):

- A station's worker, `vibe-dab-st`, runs at audio priority (`vibeAudioThread`). It is audio for its listeners, the
  same class as `vibe-dab-msc`. Each station is its own thread, so N stations spread across the cores
  (feedback_split_heavy_decoders_by_channel). FAAD2 runs on each station's own `vibe-faad2` thread (AacDecoderFaadAsync),
  as on the primary.
- **The tap never waits.** Each station's queue holds at most `kStationQueue` = 6 frames (0.58 s). When it is full the
  OLDEST frame is dropped and counted (`stDropped`). The radio's front end never slows down for a station. A dropped
  frame is a 96 ms hole in that one station's time de-interleaver, heard as a short burst of concealment on that
  station only, never on the primary.
- Lock order: the radio's `m_` → a station's queue lock. A station never takes the radio's lock. The report builder
  takes the radio's `m_` and then the station's `m_` (`stationParts`), and the station worker never holds its own `m_`
  while waiting on the radio. There is no cycle.
- The shim's 20 ms DAB audio clock takes each station's PCM and does one encode per station. Station PCM is taken
  with `takePcm`, which uses only the station's own `pm_`, so the clock never waits on a decoder.

## 5. Remember, resume and auto-select, per listener

- **Remember:** the shim keeps `session → wanted SId`. A reconnecting session (same `user_session_id`) re-adopts its
  station within the grace period (120 s after its last socket closed). The client does not re-assert anything,
  because recovery is not a user action (shared_dial_contract). After the grace period the entry is dropped and the
  listener gets the primary.
- **Block change (whole-radio):** every station closes, and listeners fall back to the primary, which applies the
  radio's rule (vibe_dab_last_service.h: same SId, else the last on that mux, else the first). Each listener who had
  their own station is then re-checked as soon as the new ensemble's list arrives. If **their** SId is on the new
  multiplex they get it back (rule 1, per listener). If not, they stay on the primary's choice, which is rules 2 and 3
  as the radio applies them. "Last on this mux" is kept per radio, not per session, because sessions are short-lived.
  This is the radio's memory of what people played there.
- The DX sweep and band scans never auto-select. They close the stations as a block change does, and they restore
  nothing: the server is moving the radio, not a listener.

## 6. The benchmark

- New row **`dec_dab_station`**: "DAB+: each extra station (own station)". It runs the bench multiplex through a
  `DabService` exactly as the `dab` row does, plus K = 3 stations in station mode on three other services (48 k DAB+,
  as most real stations are). The figure is the **mean of the stations' own hottest thread (`vibe-dab-st` + its
  `vibe-faad2`)**, in % of a core. That is what one more station costs. The row is skipped where `dab` is skipped (no
  Band III radio).
- `ruleFor("dec_dab_station")` = `{ "", false, false }`. It switches nothing off. It feeds N:

  `N = 1 + floor( max(0, 70 × cores − dabTotal − 70) / perStation )`, capped at 16. Here `dabTotal` is the sum of the
  `dab` row's threads (front end plus the primary station), and the second 70 keeps one core's green budget for
  network, spectrum and the audio clock. One core gives N = 1. A row that did not run gives N = 0, "not measured",
  which reads as OFF under `-1`.
- The result goes out as `"dabStations":{"n":N,"perStationPct":p,"frontPct":dabTotal,"memKB":k}` in benchmark.json.
  The shim reads N at runtime (`vsDabStationsN()`, like `vsLoraBenchPct`). The setup page reads the same block.
- **Memory:** each station is bounded. That covers the queue (6 × 221 KB shared frames, which are refcounted: the
  memory is the larger queue across stations, not the sum), the time de-interleaver (16 CIFs × the sub-channel's
  coded bits), the FAAD2 instance and the PCM queue (capped, as on the primary). The row measures RSS before and after
  opening the K stations, and `memKB` is per station. There is a hard cap of 16 stations per radio whatever N says.
  A 256 MB box at N ≤ 4 adds a few MB.

### 6.1 Measured (2026-11-16)

| Box | Front end (dab row, threads) | Per extra station | N | RSS per station | Notes |
|---|---|---|---|---|---|
| M5 Pro Mac, 15 cores | 1.9 % | **0.32 %** | 16 (cap) | ~5.5 MB | vibeserver-bench, FAAD2 |
| Pi 2, 4 × Cortex-A7 0.9 GHz | 107 % (vibe-dab 86 + msc 17 + faad2 3.5) | **12.3 – 13.5 %** (vibe-dab-st ~6.6 + its faad2 ~6.3) | **8 – 9** | 3 – 5 MB | dab-station-bench, armhf ARM mode + NEON, standalone at 0 listeners; K = 6 stations: 0 frames dropped |

The bench clip is deliberately impaired (RS repairing constantly), so these are worst-case figures. On the Pi 2,
the rule therefore turns own station on for a listener limit up to 8. Below 64 MB `MemAvailable`, no station is
opened (Linux). The pick then switches the radio's own station, as before the feature, and the listener is told why.

## 7. Protocol

Server → client (spectrum socket). Everything is in the report the client already reads, so each listener gets its
own full state:

- `dab` (legacy) and `dab_live` (v2): built **per station**. A listener on a station receives that station's report.
  It carries the multiplex fields (lock, MER, FIB, TII, the list) copied from the radio, and its own `sid`, DLS,
  slide, codec and counters. An old client therefore highlights the station it actually hears.
- New fields, in every DAB report:
  - `"own":{"on":true|false,"n":N,"limit":L}`. `on` means a pick is per listener. `n` and `limit` let a client
    explain an OFF.
  - `"playing":[{"sid":S,"n":K},…]`. Every service playing on this radio and how many listeners each has. The
    primary is included whenever it has listeners.
  - `"mine":S`. The service this listener hears (the same as `sid` in a per-station report, made explicit for
    clients that merge reports).
- The list (`dab_list`) and the all-services radio text (`dab_dls`) are per multiplex, sent once. They are the radio's.

Client → server: none new. `dab_service {sid}` is the pick. It is per listener when `own.on`, and whole-radio
otherwise. Under ask-before-tuning, `dab_service` is **not** gated while `own.on` is true, because it no longer
changes what anybody else hears. The scan lock still refuses it, so an owner's scan stays undisturbed.

Audio: unchanged formats. A station listener's audio socket simply carries that station's stream (Opus/PCM via
`sendAudioPcm`'s format, or DAB+ ADTS on a server without FAAD2).

## 8. Gates that must stay right

- **PAGE gate** (`vibe_paging_gate.h`): every station send goes through `pagingAudioOut`, as `sendAudioPcm` does.
  While paging runs, every station is silent.
- **Recording**: a client records the stream it hears, which is its own station. Server-side raw IQ is the radio's
  capture and is unaffected. `iqout` listeners have no DAB audio.
- **DX sweep and scans** step blocks: §3 (stations close; no auto-select).
- **DAB exit** closes every station. **The last listener leaving** leaves the radio as today (dab_persists_like_any_mode).
- **Squelch** (per session) applies to the station listener's stream, as on the shared one.
- **VFO B and mixing** are refused in DAB already (`watch` refuses dab). Nothing to do.

## 9. Clients

- **Web client** (`web/client/src`), **app** (`src/`): the station list draws `▶ n` beside every playing service
  (from `playing`). The box's footer line says "Your own station — others keep theirs" when `own.on` and more than
  one listener is present. Nothing is hardcoded: everything comes from the report.
- **R36S** (`handheld-ui/`): the station row gets the same `>n` marker inside its 40-character line.
- Copy (AGENTS.md): `sdrTour`'s DAB card, AboutOverlay's release notes and feature list, the web tutorial, and the
  setup page.

## 10. Tests

- `test-dab-own` (C++): station mode decodes a second service from the radio's own frames (the bench clip, three
  services, **distinct tone per service** checked by the dominant frequency of each station's PCM). It also checks
  that joiners share one decode, that the queue drops instead of waiting, that a block change closes stations, that
  the routing rules A to E hold, that the N rule holds, and the setup text.
- `scripts/test-server-dab-own-station.mjs` (e2e, fake rtl_tcp + the clean bench clip): three listeners on one shared
  dial each pick a service and each hear their own service's tone. A joiner shares. `playing` counts are right. With
  `dabOwnStation:0`, or N < limit, a pick switches everyone, as today. Silent: no audio is played, PCM is analysed in
  memory.
- **Crown jewel:** `dab-weak` ladders before and after on the real captures in `~/VibeSDR-test-audio/dab/tii`. The
  primary path is unchanged by construction, and the ladder proves it. The same ladder is then run with stations
  attached, to show that the primary is unaffected when the tap is busy.

## 11. Phase 2 (NOT this job): many multiplexes from one wide capture

### 11.0 HARD RULE: the standard DAB path does not move (Stuart, 2026-11-16)

The weak-multiplex lock is the crown jewel. Everything new about gain applies **only to the wide DAB modes**: a
sample rate of 4 MS/s or more, with a capture that holds more than one multiplex. That covers RF gain judged on
the blocks in use, slow gain behaviour, per-mux digital levelling and pager-burst immunity. **Below 4 MS/s, and in
today's single-multiplex DAB mode, the AGC, the gain path and the decode stay byte for byte as they are.** Phase 1
changes none of them either. `scripts/test-dab-standard-path-guard.mjs` holds this:
- the AGC and gain sources are unchanged;
- the shim adds and removes no gain or AGC line;
- the receiver gains only the empty tap;
- real weak captures decode identically before and after.

Phase 1 is shaped so that Phase 2 reuses it directly:
- **one multiplex decoder** (the radio's `DabService` front end: OFDM/FIC/TII) fed by its own cut of a wider capture,
- **per-station decoders** (station-mode `DabService`) hanging off whichever multiplex decoder they belong to, through
  the same MSC tap,
- **one PPM controller per radio** (auto_ppm_plan), never per multiplex.

### 11.1 Front end, to scale

- **Small N (2 to 4 muxes, e.g. RSP1A at 8 MS/s):** one `vibe_dab_ddc.h` DDC per multiplex in use, the path the DAB
  wide branch already built and proved (NCO mix → half-band ÷2 chain → rational L/M to 2.048). Each DDC costs one full-rate
  mix plus a first half-band stage at the input rate, roughly 10 to 14 flops per input sample. At 8 MS/s that is
  ~0.1 GFLOP/s per mux, trivial.
- **Large N (B210 at 56 MS/s, 10 to 15 Dutch muxes):** N independent DDCs cost N × 56 MS/s of mixing and first-stage
  filtering, ~0.7 GFLOP/s per mux and ~10 GFLOP/s at 15 muxes. Instead, use **one channeliser pass**. That is either
  an overlap-save FFT filter bank (one big forward FFT of the capture per block, then per mux a 1 536 + guard bin
  slice, a small inverse FFT to 2.048 MS/s and a rational step), or a polyphase filter bank on the Band III grid.
  Cost: the forward FFT is shared, ~5·log2(M) flops per input sample (M = 65 536 → ~80 flops/sample, ~4.5 GFLOP/s at
  56 MS/s, one AVX2 core-ish). Each mux then costs only its own small inverse FFT at 2.048 MS/s, which is nothing.
  ★ The crossover sits at about 4 to 6 muxes at 56 MS/s, and is never reached at 8 MS/s with 3 or 4 blocks. The
  server picks per-mux DDCs below it and the bank above it, and the benchmark measures both
  (vibe_benchmark_wide.h has the rows' shape).
- **Block frequencies come from the table** (`vibe_dab_channels.h` `kBandIII`), never from a spacing. Group
  boundaries differ: 10D → 11A is 1.856 MHz, not 1.712.
- **Lazy:** only multiplexes with a listener run OFDM/FIC, and only stations in use run their decode. A mux with no
  listener has no DDC slice computed. In the FFT bank, its bins are never inverse-transformed.

### 11.1a Gain in wide mode: the rule for EVERY wide radio

- **Settle once, then hold.** The RF/IF gain is chosen once for the whole capture. After that it moves only
  slowly, and only for clipping or headroom. Each multiplex's level is handled **digitally after the
  channeliser**: a per-mux scale on the DDC/bank output, so every block's OFDM sees full-scale soft bits whatever
  its neighbours do. A gain step can therefore never knock a multiplex off. This is the cure for the Almere B210,
  whose own AGC flaps in DAB and drops multiplexes ("with AGC off DAB locks perfectly").
- **Judge on the DAB blocks in use**, from the channeliser outputs, not on total ADC power. Out-of-band energy
  counts only as a clipping guard.
- **Ignore short bursts.** Use a fast attack only for real clipping (ADC peak near full scale). Otherwise decide
  slowly, over seconds, so a burst below clipping changes nothing. When a burst does clip, log it and ride it out
  briefly rather than stepping the gain down and back up. Stuart's diagnosis of the Almere flapping is the Dutch
  P2000 FLEX pagers at ~169.65 MHz: short, very strong bursts that a total-power AGC ducks on every one, so the
  multiplexes dip.
- **The real cure for actual overload is in hardware:** a Band III band-pass, or a 169 MHz notch. Say so to an
  owner whose capture clips.
- **Tests (Phase 2):**
  - A synthetic strong burst outside the DAB blocks (2-FSK, 0.5 to 2 s on, every few seconds), added to the
    Lenovo capture or to a synthetic one, must not change the RF gain or lose lock while it stays below clipping.
  - A forced RF gain step during the capture must not lose lock on any of the three Lenovo multiplexes.

### 11.1b Does the AGC keep 10D usable beside 10C and 11A? (explicit measurement)

One RF/IF gain serves all three blocks. The AGC is driven by total power, and 11A dominates that: in the capture,
11A sits ~36 dB above the floor against ~13 dB for 10D.

- **Compare:** 10D's MER, FIB error rate and MSC BER from the wide 8 MS/s capture against the single-mux live
  check (10D MER 9.2, FIB 12/12, BER 0.098; 10C 18.9; 11A 23.7). Do this both on the recorded file and live on
  the Lenovo.
- **Check:**
  - the per-mux digital stage rescales 10D to full-scale soft bits;
  - the AGC does not step on 11A's level in a way that drops 10D;
  - no intermod or ADC clipping from 11A lands on 10D (look at the spectrum under 10D for 11A products, and at
    the ADC peak histogram).
- **Record the RSP1A's native sample depth** on that path. That is the API's 16-bit samples, from a 12-bit ADC
  at this rate; the cu8 used over raw IQ is 8-bit.
- **If 10D comes out worse in wide mode,** report by how much, and whether the wide-mode AGC target or headroom
  should differ. Never trade the weak-signal crown jewel for it: the single-mux path is untouched by rule 11.0.

### 11.2 PPM

One controller per radio. Every mux's offset reading measures the same crystal, so they are combined
(**MER-weighted median**). Outliers are dropped and logged (a weak mux, or a transmitter off frequency). Each mux
keeps its own fine tracking, and only the shared controller moves the radio, always as a gentle glide, so no other
listener's mux is knocked off (test-afc "it glided").

### 11.3 Limits

- **USB:** the B210 at 56 MS/s with 16-bit IQ is 224 MB/s, which needs USB 3 with nothing else on the controller.
  At 8-bit (sc8) it is 112 MB/s. The RSP1A at 8 MS/s is ~32 MB/s (USB 2, near its ceiling).
- **The channeliser's own CPU:** ~1 core at 56 MS/s (above). **i7 estimate (11th gen, Almere):** channeliser ~1
  core. OFDM/FIC ~3 to 5 % of a core per mux, so 15 muxes ≈ 0.6 core. Per station ~1 to 2 %, so 50 stations ≈ 1
  core. Total ≈ 3 cores of 8. The limit is the uplink (~30 KB/s per listener → 12 to 15 Mbit/s for 50), not the CPU.

### 11.4 Test plan, RSP1A at 8 MS/s (Stuart's Lenovo; ONLY with his OK, at a time with no listeners)

- Blocks: **10C 213.360 MHz, 10D 215.072 MHz, 11A 216.928 MHz** (from the block table). 10C → 11A is 3.568 MHz
  centre to centre, plus 1.536 MHz of signal, so ~5.1 MHz. One capture centred near **215.14 MHz** holds all three
  with margin, inside the ~6.5 to 7 MHz usable at 8 MS/s. 11A starts a new group (10D → 11A = 1.856 MHz).
- **Expected ensembles** (Stuart's names, in block order): 10C *Northamptonshire*, 10D *Beds Bucks Herts*, 11A *SDL
  National*. Three different ensembles from one capture, e.g. three listeners each on one station from a different
  multiplex. **When it is run, confirm the mapping against the ensemble labels actually decoded, not against this list.**
- First, record a 30 s 8 MS/s capture (the owner's raw IQ, listeners 0, dial restored after), so everything after
  that is offline and repeatable with `dab-weak --wide`.
- **Capacity test** (Stuart estimates ~60 stations across the three): decode **every service on all three muxes at
  once** from the one capture on the Lenovo (i5, "lenovoi5"). Measure the CPU per multiplex front end and per station
  (thread times, as the benchmark rows do), the channeliser/DDC cost, and peak RSS. Report the resulting N for the
  own-station rule. Compare per-station against Phase 1's `dec_dab_station` figure for that machine, which checks the
  cost model against real numbers.
- **The headline end-to-end run (Stuart: "listen to all 65 stations at once"):**
  - 65 simultaneous listener sessions over loopback on the Lenovo (10C 16 + 10D 19 + 11A 30), each on a
    different service, every one getting continuous audio.
  - Report the CPU per multiplex front end, per station and in total on the i5, and the memory.
  - Report per-listener audio continuity (underruns and gaps, and how long it held), and the network side: kB/s
    per listener and the total. At ~30 kB/s each, 65 listeners is ~2 MB/s, ~16 Mbit/s of upload. That is the
    realistic limit over the internet, separate from what the CPU can do.
  - If 65 does not fit, report the largest N that does and which resource limits it. Never claim 65 unless it was
    measured.
- **First offline result (2026-11-17, Mac, `vibeserver/dab-wide-bench`, the recorded file, 30 s after 15 s warm-up):**
  one DabService per multiplex, each cutting its block from the same 8 MS/s stream with its own DDC. Every audio
  service was opened as a station: **65 services decoding at once** (3 multiplex decoders' own and 62 stations).
  0 station frames dropped, 0 wide samples dropped.
  - 10C *Northamptonshire*: 16 services, MER 18.2, FIB 1.000, BER 0.002, all decoding.
  - 11A *SDL National*: 30 MP2 services, MER 23.6, all decoding.
  - 10D *Herts Beds Bucks*: 19 services, MER 8.9, FIB 1.000, BER 0.107, every station ~63 % of super frames.
    The single-mux live check read MER 9.2 and BER 0.098, so wide mode reads ~0.3 dB lower on a different minute,
    from an 8-bit file. The live comparison on the RSP's native samples is still to do (§11.1b).
  - The ensemble labels decoded match Stuart's list.
  - **The same run on the Lenovo i5-11300H** (4 cores / 8 threads; standalone binary in a scratch dir, 0 listeners,
    the server untouched; 40 s after 15 s warm-up). All 65 services decoded with the same counts as on the Mac.
    **Total 75.5 % of ONE core:**
    - the three DDCs (vibe-dab-ddc) 22.4 %, ~7.5 % each at 8 MS/s;
    - the three OFDM/FIC front ends (vibe-dab) 14.3 %, ~4.8 % each;
    - 62 stations' own decode (vibe-dab-st) 22.1 %, plus FAAD2 14.6 % and MP2 2.1 %, so **~0.6 % per station**.
    - RSS 90 MB, ~1.3 MB per station.
    - Audio delivered per station: 2.18 to 2.19 M samples over the ~46 s each was open, i.e. real time with no
      shortfall. 0 frames dropped.
    - This is decode only, without the server's network side. At ~30 kB/s a listener, 65 listeners need ~2 MB/s
      (~16 Mbit/s) of upload, which is the realistic ceiling, not the CPU. **The end-to-end 65-listener run through
      the server is still to do:** it needs Phase 2's server integration (a listener's own multiplex).
- **Lenovo rules:**
  - 0 listeners first.
  - Back up `/etc/vibeserver/config.json`. When finished, it must hash again to the reference copy
    `/etc/vibeserver/config.json.stuart-2026-11-16` (sha256 4f6c3890…0522816), with the service running normally
    and the background decoders back (NAVTEX 489.5 and 517.5, the NDB skimmer, spots) on 648 kHz AM.
  - Run a test binary from a scratch directory with its own config, or stop the service and run it. Never
    apt-install a test build there, and never build there.
- Then scale up: the Almere B210 at 56 MS/s, most of Band III. That only happens with the owner's agreement (via
  Stuart). The box is public: never test on it unasked.
