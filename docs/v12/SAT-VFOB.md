# SATELLITE — OPEN IN VFO B (beta92)

Stuart, 2026-11-22:

> "we need a simple way to trigger the 2nd signal, so i guess if i am on the transponder the beacon could have an open in
> vfo b button. only needs one window as transponder is audio only and the orbit is the same as its the same satellite"
>
> "if one is an image and the other cw then when switching between the lines all it does is change what is shown in the
> decoder box"
>
> "switching is simply viewing the decoder when opened in vfo b, if a user wants to do one at a time it still works like
> today"

## RESUME HERE

Branch `b92-vfob` (off v12-experimental fe5b674f6). Built and tested end to end on synthetic signals: server, web page,
app. **Not yet heard on a real pass** — the first live check is RS-44 (transponder on A, OPEN IN VFO B on the beacon:
does the CW decoder read it?) and an ISS pass (145.800 on A, APRS 145.825 in B). Not on the R36S (it has no SATELLITE).

`vibe_web_page.h` was regenerated for the web test and **not committed** — the coordinator's build makes it. The iOS DSP
library needs `build_ios.sh` for the server-side code to reach the app's own engine (as for any shim change).

Open points for Stuart are listed at the end.

## What the listener gets

1. A downlink is picked in SATELLITE (one at a time — **exactly as before**). The server's `picked` answer now lists the
   other downlinks of that bird **this listener** may open in VFO B now (`"vfob":[tx,…]`): inside the radio's current
   window with the whole Doppler swing (2.7e-5·f + the 2.5 kHz trim clamp + half the passband), VFO B allowed for them
   (the owner's switch, not DRM / FM broadcast / a whole-radio mode — `watchWhyNotFor`), no owner band scan running, and a
   free place where VFO B takes a listener's (`maxUsers`). Nothing offered = no key (not a greyed one).
2. The box shows **OPEN IN VFO B · <downlink>** under the readout for each. Pressing it sends `{"type":"sat_vfob",norad,tx}`.
3. The server puts the listener's **ordinary VFO B** there (`handleWatch` — every VFO B rule; it never moves a shared dial),
   starts the decoder that downlink's `use` needs on **this listener's VFO B host** (`watchDecBKey` — the host a decoder
   socket's `"vfo":"B"` attach uses; the same gates as any decoder: the owner's switch, reach, the slots), and
   **Doppler-corrects B from the same pass**: the tracker (25 Hz) sets B's oscillator to nominal + A's shift × fB / fA
   (`WatchChannel::setShiftHz`). The dial B reports stays on the nominal, as A's does.
4. ONE box, one globe. It gains **B's line**: `VFO B · CW beacon · 435.600 26 MHz · CW decoder listening for the beacon`
   (its frequency with the Doppler, then what its decoder says). When **both** lines have a decoder view (a picture and
   CW, a picture and packets …), both lines become tappable and the lit one is what the decoder area shows — that switch
   exists only while B is open. Where A has nothing to decode (a transponder) the area simply shows B's.
5. B goes with **its usual off** (VFO A/B on the frequency card), a new pick, the drop (leaving SATELLITE) or its
   listener leaving. The listener is told (`sat_vfob` on:false + why), B's decoder stops quietly (no "start it again on
   VFO A" refusal), and the box is the one-downlink box again.
6. Fix in the same work: a CW beacon picked on A said "listening for packets". It now says **"CW decoder listening for the
   beacon"** and shows the decoded text (the last few lines), as CW does elsewhere. A pick's CW is always the one-signal
   decoder — never the skimmer the CW key may be set to (that attached a different decoder over the pick's).

Examples from the catalogue: RS-44 / FO-29 transponder ↔ CW beacon; AO-7 Mode B transponder ↔ Mode B beacon; AO-73
transponder ↔ FUNcube telemetry; ISS 145.800 ↔ 145.825 APRS; ISS 437.800 ↔ 437.550 SSTV events. ISS 145.x ↔ 437.x is
never offered (one window cannot hold both).

## Wire

Client → server (spectrum socket):

```
{"type":"sat_vfob","norad":N,"tx":i}     open downlink i of the PICKED bird in VFO B
{"type":"sat_vfob","off":true}            close VFO B (the same as {"type":"watch","id":1,"off":true})
```

Server → client:

```
{"type":"sat","state":"picked",…,"vfob":[1]}                               the offer, per listener (absent = none)
{"type":"sat_vfob","on":true,"norad","tx","hz","mode","use","label","decoder","params":{…},"shift":Hz[,"decoderWhy"]}
{"type":"sat_vfob","on":false,"refused":true,"norad","tx","why":"…"}        a refusal, in words
{"type":"sat_vfob","on":false,"norad","tx","why":"…"}                       B left the satellite
watch state chans[]: "shift":Hz                                             B's applied Doppler (0 for an ordinary B)
```

`params` are the exact attach parameters the server started B's decoder with: the client's own `"vfo":"B"` attach sends
them, so it **joins** that run (`DecoderHost::start` → Joined) instead of restarting it.

`sat_vfob` is **not** in ask-before-tuning's `kGated` (it moves nobody's dial — a VFO B is the listener's own); it **is**
in the scan lock's `kLocked`, as `watch` is. Old servers never send `vfob`: no key. Old clients never send `sat_vfob`.

## How it is built

| piece | where |
|---|---|
| B's oscillator at nominal + shift (feed, level, B's skimmer cut) | `vibe_watch.h` `WatchChannel::setShiftHz / tuneHz`; shim `feedWatchFromBins`, `watchFeedFull`, `watchOnFrame`, `watchSkimTapB` |
| the offer, the open, refusals | shim `satVfoBWhyNot`, `satVfoBOfferJson` (from `satSendPicked`), `satVfoBControl` |
| B's decoder on the B host | shim `satAttachB` (satAttach's twin), `satVfoBStopDecoder` |
| Doppler at 25 Hz | shim `satVfoBTick` (in the `vibe-sat-track` loop), `satVfoBShiftNow` = A's `Track.shiftHz × fB / Track.nominalHz` while TRACK/COAST, held otherwise (as A freezes) |
| B going | `watchRemove` → `satVfoBGone`; `satDetachPick` → `satVfoBCloseFor`; house thread `satVfoBSweep` |
| shared words | `src/services/satellites.ts`: `parseSatVfoB`, `satVfoBOffers`, `satVfoBHz`, `satDecView`, `satViewSwitch`, `satViewShown`, `satDecWord`, `satVfoBLine`, `satCwTail` |
| web | `web/client/src/satellites.ts` (`onVfoB`, `paintVfoB`, B's canvas / text / packets, the switch); `main.ts` `satVfoBFollow` / `satVfoBDecoder` (a second `DecoderClient`); `spectrum.ts` `satVfoB` |
| app | `SatellitePanel.tsx` (props `vfob`, `bText`, `bPackets`, `bStatus`, `bPicture`, `bImageRef`, `cwText`, `onVfoB`); `SDRScreen.tsx` (`satVfoBDecoderRef` — a second `DecoderClient`); `VibeServerClient.satVfoB` |

**Why a second decoder socket.** One decoder socket parses frames for ONE decoder (`0x01` is text for CW, a picture line
for SSTV), and a socket starting a decoder on one VFO stops its own on the other. So B's decoder is followed on its own
socket. It is **closed, never detached**: a detach on it stops whatever the B host runs now — after a re-aim that is the
next downlink's decoder, which the server has already started.

**Why A's shift × fB / fA.** A Doppler shift is −v·f/c, proportional to the frequency; so is a receiver's clock error,
which the loop's trim follows on an FM downlink. test-sat-engine §14 shows the scaled shift equals the model's Doppler at
B's own frequency to under 0.5 Hz over a whole RS-44 pass and an ISS pass.

## How it was tested (all synthetic, this Mac, 2026-11-23)

- **test-server-sat.mjs §10** (a real vibeserver, fake rtl_tcp at 2.4 MS/s, CelesTrak's 2026-11-19 elements moved to an
  hour ago, the server placed where RS-44 is ~36° up): RS-44 transponder picked → `vfob:[1]`; a plain pick makes no VFO B;
  OPEN IN VFO B → B on 435.605 CW, the CW decoder on the B host, VFO A's dial untouched; **B's shift = A's offset ×
  fB/fA over six samples, worst 0.0 Hz past the rate allowance, |shift| ~4.8 kHz**; a `"vfo":"B"` socket joins the
  decoder without a restart; refusals in words (the picked downlink, another bird, no pick); watch off → `sat_vfob`
  on:false, its decoder stopped, no refusal on the B socket, A's lock stands; a new pick and the drop close it; the beacon
  picked offers the transponder the other way (USB, nothing to decode); SO-50 (one downlink) offers nothing. ISS 145.800
  offers only 145.825 (437.x outside the window; asked anyway → refused) — APRS on B and SSTV on A both running. A radio
  with VFO B switched off: no key, refused in words. A locked window: 437.550 SSTV events offered beside 437.800, the
  open is that listener's alone. **160 / 0** (the whole file).
- **test-sat-engine §14**: A's shift × fB/fA = the model at B's frequency (RS-44 beacon over its pass from IO92ng; ISS
  APRS over a São Paulo pass), peak inside the offer's swing. **175 / 0**.
- **test-web-satellite-mode.mjs** (headless Edge, `--mute-audio`, the built page's CSS): no offer → no B part at all;
  the key and what it sends; B's line with its Doppler; one box, one globe; A a transponder → B's CW in the area, its
  text and its own words; 390 px fits; B off → the key back; a refusal says why; a CW beacon on A says "CW decoder
  listening for the beacon" and shows its text (no "no packets yet"); ISS SSTV on A + APRS on B → both lines tappable, B
  shown; a tap on A shows A's lent picture and hides B's packets; a tap on B the reverse; switching sends nothing; B
  closed → the switch ends and A's box is as before. **132 / 0**. Screenshots `sat-web-vfob-*.png`.
- **test_satellite_mode.ts** (the pure half both clients use) **211 / 0**; **test_satellites_app.ts** (the real server's
  `sat_vfob` / `picked` shapes, bounded parsing) all passed.
- `npx tsc --noEmit -p .`: no new errors (the 45 left are in `handheld-ui/test` and `scripts/wefax-world`, untouched).
  Needed a 4 GB heap (it runs out at 1 GB).

**CPU** (the whole vibeserver process, fake rtl_tcp 2.4 MS/s, this M5 Pro, shared with other agents' work): A on RS-44's
transponder with Doppler + B on its beacon with the CW decoder and Doppler 5.0 % of a core; A alone 7.7 % — the
difference is inside the noise of the measurement on a busy machine. What B adds is what VFO-AB.md measured for any VFO B
(~0.35 % of a core for one more narrow pipeline) plus a CW decoder (the same on B as on A); the Doppler tick is one map
lookup and one multiply per open at 25 Hz. Pi 2: as VFO-AB.md's estimate for a narrow VFO B (5–8 % of a core on a
personal receiver) plus the CW decoder.

## Not done / limits

- No real pass yet (above). A live RS-44 or ISS capture is the coordinator's to take.
- B's decoder does not count toward the satellite-pass gain profile (`satGainWatchTick` looks at A's pick); a picture
  decoder opened on B (SSTV events) gets no gain help unless A's is a picture decoder too.
- B's telemetry (AO-73 FUNcube opened on B) shows its text lines only — the values and CHARTS stay A's.
- The key is offered from the `picked` answer: a window that moves later (someone else moves a shared dial) is caught at
  the press (refused with the window in words) rather than the key vanishing at once.

## Open decisions for Stuart

- Should leaving SATELLITE keep VFO B (as an ordinary VFO B) instead of closing it? Built: it closes.
- A close key for B in the box as well as VFO A/B on the frequency card? Built: no (the box kept minimal).
- On opening B, which decoder does the box show first? Built: B's (the one just asked for); a tap on A's line goes back.
