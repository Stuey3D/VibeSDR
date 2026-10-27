# VFO A/B — two frequencies at once (V12)

GitHub discussion #57 (Gesiima): *"listen to two or more frequencies at the same time … monitoring a calling frequency
and a second channel without having to switch back and forth."* Stuart, 2026-10-27, chose the first step: dual
frequencies per radio, like VFO A/B on a rig. Two dongles at once is a later step and is not built.

## What the listener gets

- **Frequency card (tap the readout) → VFO A/B** (on/off; in the tuning-step sheet until V12 build 498). On creates VFO B at VFO A's frequency, mode and width; off removes it. Per
  session: a reconnect starts with one VFO.
- **Switching which VFO the controls tune**: the A/B key between the tune buttons (buttons mode); press-and-hold the
  radio trapezoid on the drum (drum mode); a pad/keyboard shortcut. Everything — frequency, mode, passband, the
  readout, the S meter — follows the selected VFO. The other is drawn as a second needle and passband, slightly dimmer.
- **Each VFO's own squelch row (round 3).** While B is on, the Audio menu (the speaker button; the web's AUDIO panel)
  has a SQUELCH row for each VFO, labelled with its letter in its needle colour: its own live bar (A's from `sig.chan`,
  B's from `sig.chanB`), its own threshold and its own AUTO SQUELCH, whichever VFO the controls are tuning. B's auto
  squelch is A's algorithm run on B's level (`src/utils/sqlAutoTrack.ts`, shared by the app and the web); the ABOVE
  NOISE margin is one slider for both. With B off the Audio menu is exactly as before.
- **Mute a VFO (round 3).** Each row has a MUTE. A muted VFO fades out over the same 400 ms ramp and the other glides to
  the middle — both ears; unmuting reverses it. A muted VFO keeps decoding and its meter keeps moving. Per session.
- **Decode on either VFO (2026-10-27).** Stuart: *"That seems like a weird limitations surely it doesnt matter which VFO
  is listen and which is decode?"* A decoder, Single or Band digital spots, or a skimmer runs on **the VFO selected when
  it is started**, fed that VFO's own audio or IQ before the mix — so the other VFO, or muting the decoding one, never
  reaches it. Selecting the other VFO afterwards moves nothing. Anything the decoder box tunes (a station, signal or spot
  tapped in it, a JTTY dial, a DSC channel, APRS's channel, Band's band) tunes **that** VFO, whichever the controls are
  on. In Band the arrows step bands, the readout names the band and the band's span is drawn only while the controls
  tune the VFO Band runs on; with B on, Band mutes **that VFO** in the mix rather than the speaker. The decoding VFO
  says **DEC** after its letter on its needle label and its squelch row. Switching B off (or B yielding to a listener,
  FM broadcast, a whole-radio mode) stops what was decoding on it, with the reason in words. (Round 3 had the decoders
  on A only and switched the controls to A to start one — a server without `decB` still gets that from the clients.)
- **Both are heard at once, each with its own squelch.** With **Stereo** on (the default, in the Audio menu beside
  Binaural) the **lower** frequency is in the **left** ear and the **higher** in the right — whichever is A or B, so the
  waterfall and the ears always agree. Stereo off, or a mono output: both VFOs in both ears.
- Every move is smooth: adding B pans A from the middle to its ear while B fades in; removing B fades it out and pans
  A home; tuning one across the other swaps the ears. 400 ms, raised-cosine, equal-power (no loudness dip, no click),
  100 Hz of hysteresis so a needle wobbling over the other does not flutter, within 5 Hz = both in both ears.
- **Where B may go (round 3).** On a radio that is the listener's alone — a personal receiver: one listener, no
  owner-locked centre — **the RF centre follows both VFOs** (Stuart: "in Dual VFO mode the RF centre should move
  accordingly so that both VFO's can be tuned as if they have the full 2.4MSPS window"). A B that no longer fits beside
  A, or an A tuned past the window, moves the radio to where both are inside (`vibewatch::fitCentre`: each VFO allows
  the centre within the usable half-span less the edge margin less half its passband — dongleForView's own figure — and
  the centre goes to the middle of the room both allow, never with the DC spike inside either passband); a pan cannot
  slide B out again (dongleForView keeps B as a second limit). Only VFOs further apart than one window holds — about
  1.91 MHz at 2.4 MS/s — are refused (`apart`), nothing moved. The state carries `"follow":true` there, so the web
  page drops its own wall for B. **Everywhere else** (a shared dial, a range-locked radio, an owner's locked centre)
  both VFOs must lie inside the radio's current window: a frequency outside it is refused with the window in the note
  — never honoured by moving the radio (B never moves a shared dial). If the radio moves and leaves B outside (the
  shared dial, the owner, or on a personal receiver an A tuned further from B than one window holds), B is parked
  silent with a note until it is back inside.
- **No dual VFO on FM broadcast (round 3).** VFO A on WFM (anywhere), or VFO A's dial inside 87.5–108 MHz in any mode,
  takes VFO B away — closed with the reason, and the toggle vanishes (`max` 0) until A leaves the band; VFO B may
  neither pick WFM nor be tuned into 87.5–108 MHz (refused `mode` / `band`). Japan's 76–95 MHz is covered by the WFM
  rule rather than a second band table.

## Owner

- **Setup page → "Let listeners use VFO A/B"** (config `vfoB`, per radio: 1 default, 0 off). Android: the same key in
  the server config (VibeServerBoot → `VibeLocalSDR.setVfoB`). Live: `{"type":"vfoB","on":0|1}` (admin).
- Off, or a radio/mode where it cannot run (an Icom — one physical receiver; a SpyServer source; DAB, ADS-B, ISM, AIS,
  NDB; a listener in DRM, on WFM or on the FM broadcast band), hwinfo says `"watch":0` and no client offers the toggle.
  A change of that while connected (a tune onto the FM band, a whole-radio mode) is re-told in the watch state's `max`
  within a second (`watchRecheckMax`), so the toggle vanishes and comes back without a reconnect. Older servers do not
  send `watch` at all: same thing.
- **Slots**: on a radio with a listener limit a VFO B takes a listener's place (it costs about one listener). A VFO B
  asked for on a full radio is refused; a *listener* arriving at a full radio closes the newest VFO B, with a note — a
  person always beats a second channel.

## Architecture (as built)

| piece | where |
|---|---|
| watch channel (own RxPipeline, AGC, squelch gate with open/closed + when, FIFO) | `vibe_watch.h` `WatchChannel`, `SquelchGate`, `PcmFifo` |
| the pan mixer (N sources ranked by frequency, ramps, equal-power, hysteresis, stereo/mono) | `vibe_watch.h` `PanMixer` |
| one listener's set (session-keyed: chans, fading, stereo, monoOut, mixer, private Opus) | shim `Impl::WatchSet` |
| range-locked radio: one more slice of the shared forward FFT, on the listener's own thread | shim `feedWatchFromBins` (from `feedOneClient`) |
| one-pipeline radio (personal receiver, shared dial): full-rate tune + decimate on its own AUDIO-priority thread; the DSP thread only copies | `vibe_watch.h` `FullRateFeeder`, shim `watchFeedFull` |
| mix into the listener's own stream (no second socket, no second Opus stream to decode) | shim `watchMix`; `onClientAudio` (range-locked); `watchDivertShared` → `watchSendPrivate` (one pipeline: the session leaves the shared encode while mixing and gets its own) |
| levels, squelch, window — off the same averaged wide row as the listener's own squelch and S meter | shim `watchOnFrame` (from `onSpectrum`) |
| control, refusal, slot accounting, housekeeping | shim `handleWatch`, `watchHousekeep` |
| who may have a VFO B (mode, DRM, WFM / FM band), the toggle re-told on a change | shim `watchWhyNotFor`, `watchMaxFor`, `watchRecheckMax` |
| mute (a source out of the mix, the other home to both ears) | `vibe_watch.h` `PanMixer::Src::muted`; shim `WatchSet::muteA`, `WatchChannel::setMuted` |
| the RF centre for two VFOs on a personal receiver | `vibe_watch.h` `fitCentre`; shim `watchCentreFollows`, `watchMoveCentre`, `retune` (A's recentre), `dongleForView` (B as a limit) |
| B's level for its squelch row | shim `sig` → `chanB` |
| decode on either VFO: B's own decoder host (`<session>#B` in the router), its dial and mode | shim `watchDecBKey`, `decoderEnv` (`onB`), `watchBOf`, the decoder socket's `takeHostB` / `syncPeerA` (`acceptDxcluster`) |
| …fed B's audio, before the mix | `vibe_watch.h` `WatchChannel::setAudioTap`; shim `handleWatch` (installed when B is made) |
| …fed B's IQ (skimmers, Band) | shim `feedWatchFromBins` → `feedSkimFromBins` + `WatchSet::skimB` (range-locked); `watchSkimTapB` + `WatchSet::bTap` (one pipeline) |
| …stopped, said in words, when B goes | shim `watchRemove` → `watchStopDecoderB` |

**Hand-back with no step.** The mixer's equal-power law is normalised so the centre is 1.0 in each ear — exactly what a
listener with one VFO hears today — and a hard-panned source is √2 (+3 dB) in one ear (the AGC's 0.25 setpoint leaves
9 dB of headroom). When B has faded and A is home, the mixer would output today's mono, and only then does the
ordinary path take over.

## Wire

Client → server (spectrum socket):

```
{"type":"watch","id":1,"on":true}                         create at VFO A's frequency/mode/width
{"type":"watch","id":1,"frequency":Hz,"mode":"usb","bandwidthLow":-..,"bandwidthHigh":..}   re-aim
{"type":"watch","id":1,"sql":-80,"sqlOn":true}            B's own squelch
{"type":"watch","stereo":false}                           the Stereo switch (any time, ramps)
{"type":"watch","monoOut":true}                           the client's output is mono
{"type":"watch","muteA":true}                             mute VFO A in the mix (round 3; only while B exists)
{"type":"watch","id":1,"mute":true}                       mute VFO B in the mix (round 3)
{"type":"watch","id":1,"off":true}                        remove (fades; A is unmuted as B goes)
```

Decoder socket (`/ws/dxcluster`), decode on either VFO:

```
{"type":"audio_extension_attach","extension_name":"wefax",…,"vfo":"B"}   the decoder runs on VFO B (absent = A, as ever)
{"type":"subscribe_digital_spots","scope":"band","vfo":"B"}              Single or Band spots on VFO B
{"type":"audio_extension_attached","vfo":"B"}                            the server's yes
{"type":"decoder_refused","what":"decoder"|"spots","reason":"vfo","vfo":"B","message":"…"}   no VFO B, or B went
```

Server → client: `{"type":"watch","max":1,"lo":Hz,"hi":Hz,"follow":bool,"decB":true,"stereo":true,"monoOut":false,"muteA":false,
"chans":[{"id":1,"freq":..,"mode":"am","bw":..,"sqlOn":..,"sql":..,"open":true,"since":<wall ms>,"lvl":dB,"parked":false,
"mute":false}]}` on every change and twice a second; plus `"refused":"outside|apart|band|full|off|mode|id|session",
"note":"…"` when it says no. hwinfo: `"watch":N`. `sig` gains `"chanB":dB` (VFO B's level, every frame) for a listener
with a VFO B.

Old clients never send `watch` and are untouched; a new client on an old server sees no `watch` in hwinfo and offers
nothing.

## Hooks for the scanner (not built — Stuart: "this all fits into the scanner work too")

- **The channel count is a parameter**, `vibewatch::kMaxPerListener` (1 today). The DSP, the mixer (N sources ranked by
  frequency and spread evenly between the ears) and the wire (`chans[]`, `id`) already take N; only the clients' A/B
  UI assumes one.
- **Priority watch**: each channel's `SquelchGate` carries `open` and `changedAt`, evaluated with hysteresis (2 dB) and
  a 250 ms tail so it does not chatter — the event a priority watch acts on, already on the wire per channel.
  Switching the audio *to* a channel when it opens is a mixer policy (gain per source), not new DSP.
- **Channel stepping**: re-aiming a channel is `WatchChannel::want()` — a frequency change moves the oscillator inside
  the same pipeline (no rebuild unless the width needs a wider slice), proven by test-watch §1 ("re-aimed, it hears the
  other carrier").
- **Sampling without listening**: a channel's level and squelch are measured off the shared wide row every spectrum
  frame whether or not its audio is mixed — a scanner can watch levels on channels that are not producing audio.

## Cost (measured on this Mac, 2.4 MS/s; `test-watch-cost`)

| mode | one-pipeline: A | +B | B/A | range-locked: A | +B | B/A |
|---|---|---|---|---|---|---|
| AM  | 0.35 % | 0.35 % | 1.00× | 0.10 % | 0.10 % | 1.01× |
| USB | 0.32 % | 0.33 % | 1.05× | 0.14 % | 0.16 % | 1.08× |
| NFM | 0.39 % | 0.38 % | 0.99× | 0.11 % | 0.11 % | 1.02× |
| WFM | 1.67 % | 1.66 % | 1.00× | 1.57 % | 1.56 % | 1.00× |

The mixer: 0.03 % of a core. The whole vibeserver process (fake rtl_tcp at 2.4 MS/s, one listener) read 2.4–3.6 % of a
core with and without B — the difference is inside the noise of the measurement. RAM: one more RxPipeline (a few
hundred kB) and a FIFO of at most 200 ms per VFO B; on a one-pipeline radio, up to 0.5 s of queued IQ (≈ 9.6 MB at
2.4 MS/s, cf32) while its thread is behind — normally a block or two.

**Pi 2 (estimate, not measured):** a 900 MHz Cortex-A7 is roughly 15–20× slower than this Mac per core for this work,
so a VFO B on AM/SSB/NFM costs about 5–8 % of one Pi 2 core on a personal receiver and 2–3 % on a range-locked one;
on WFM about 25–33 %. On the Pi 2 a WFM VFO B on top of WFM VFO A is the case to avoid; the narrow modes (the calling-
channel use Gesiima describes) are cheap. Nothing at all runs for a listener with VFO B off — the per-block gate is one
relaxed atomic read.

## Decode on either VFO (2026-10-27)

**How.** A decoder socket's attach or subscribe carrying `"vfo":"B"` goes to this listener's **VFO B host** — a
`DecoderHost` of its own in the router, keyed `<session>#B` — on **every** kind of radio, personal receiver, range-locked
and shared dial alike:

- **Audio**: B's `WatchChannel` hands its demodulated audio (mono, 48 kHz, before the FIFO and the mix) to the host
  through `setAudioTap` — exactly where VFO A's decoder is fed A's. B's mute and squelch never reach it; a parked B feeds
  nothing.
- **IQ** (a CW/PSK skimmer, Band spots): on a range-locked radio, B's slices of the shared forward FFT on the listener's
  own thread (`feedSkimFromBins`, the same cut A's skimmer gets, with B's own scratch); on a one-pipeline radio, B's own
  `WideTap` off the full-rate capture (`watchSkimTapB` — `skimTapFeed`'s cut, centred on B's dial).
- **Dial and mode**: `decoderEnv` reads B's for a `#B` host, so spots, CW/PSK RF and the gallery's cards are filed on
  B's RF.
- **Governance** is the box's, as for any decoder: the decoder slots and Band's benchmark budget, refused in words when
  full. A socket starting a decoder on one VFO stops its own decoder on the other (the same for spots), so a listener
  still holds one decoder and one spots run; a socket decoding B is a peer of its A host only while it also uses it.
  The admin status lists a B host's run with `"vfo":"B"`; the time limit and the orphan reaper count it as its
  listener's.

**Not a role swap.** The design discussed for a personal receiver was to swap which VFO sits on the main pipeline while
B decodes, to save CPU. It saves nothing: B's demodulator runs whether anything decodes it or not, so a decoder costs
the same on either VFO — while a swap would have had every reader of the shared dial (the config, `sig`, the S meter,
RDS, a recording, the band defaults, VibeAGC) translated back into A/B terms and the swap itself made glitch-free. One
path on all three radios instead.

**Cost, measured** (test-server-vfo-ab §6b; the whole vibeserver process, personal receiver, this Mac, 2.4 MS/s, A and
B both on): nothing decoding 3.0 % of a core · WEFAX on A 5.5 % · on B 5.2 % · Band (40 m) on A 6.3 % · on B 6.0 %. The
difference is inside the noise of the measurement: a decoder on B costs what it costs on A. Pi 2: the same as the
decoder on A (BACKGROUND-DECODING.md, DIGITAL-SPOTS.md).

## Known limits

- VFO A on stereo WFM is folded to mono while B is on (the ears are B's and A's now); it returns to stereo when B is
  removed.
- On a one-pipeline radio, the session's stream switches from the shared Opus encode to its own when B is added, and
  back when the pan has come home. The content at both moments is the same (A centre), so the change of encoder is
  the only discontinuity.
- The shared path trims AM by 6 dB and a listener's own channel does not (an older inconsistency); B takes whichever
  trim A has, so the two always match.
- **Band spots on VFO B**: the tuner is not moved to keep its DC spike out of B's band slices — `bandDcTick` places it
  for the one-pipeline host's Band, which is A's dial. The RF centre for two VFOs keeps the DC out of both passbands
  (`fitCentre`'s clearance), not out of a whole band's slice, so on a dongle with a DC spike it can sit inside one of B's
  mode windows. Not built: Stuart may want `bandDcTick` to follow a Band running on B as well.
