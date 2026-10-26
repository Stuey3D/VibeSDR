# V12 — station features (parked 2026-10-07)

Stuart, 2026-10-07: "this is now a V12 feature when we build the decoders into the app and server."

| Brief | What |
|---|---|
| [SATELLITE-ENGINE.md](SATELLITE-ENGINE.md) | SGP4 + `sats.json`, Doppler lock (ISS first), time sources, display |
| [DECODERS-GALLERY.md](DECODERS-GALLERY.md) | SSTV/APRS/Meteor LRPT on the engine, the Gallery, client-assisted decode |
| [RADIO-ROLES.md](RADIO-ROLES.md) | Public / Private / Dedicated radios, task scheduler, live governor |
| [ANTENNA-SWITCH-ROTATOR.md](ANTENNA-SWITCH-ROTATOR.md) | UberSDR switch + rotator in the clients (API read from M9PSY), VibeServer's own external switch and rotctld rotator, dummy-hardware tests |
| [ISM-DECODER.md](ISM-DECODER.md) | ISM sensors as a whole-radio mode (branch `v12-ism`, 2026-10-25): readable weather/sensor/doorbell/TPMS lines, clean-room decoders, the privacy rule, the arrows step bands |
| [CW-SKIMMER.md](CW-SKIMMER.md) | CW (branch `v12-cw`, 2026-10-25): one SIMD filterbank, a listener's decoder and a skimmer, human-fist timing, CER/CPU measured |
| [FTX-SENSITIVITY.md](FTX-SENSITIVITY.md) | FT8/FT4/FT2/JS8 sensitivity (branch `v12-ftx-sensitivity`, 2026-10-26): the second look (matched filter, multi-symbol likelihoods), a-priori decoding for a receiver, OSD pairs, the guards; decodes and CPU against jt9 on the same audio |
| [DIGITAL-SPOTS.md](DIGITAL-SPOTS.md) | Digital spots (branch `v12-spots`, 2026-10-26): Single mode and Band (a band's IQ slice, every mode on its own dial), the mode registry other mode engines plug into, band stepping, the map coloured by mode |

Each ends with a **Review against the codebase** section: what already exists (corrected clock, admin HMAC auth, stable radio
IDs, benchmark, health chip, crash reporter), what this week's fixes add (exact-rate audio, decoders keep the chain alive,
shared-dial contract for the tracker, WEFAX lessons) and corrections. Read those first.

Other V12 items: [VIRTUAL-AUDIO-CABLE-PLAN.md](../VIRTUAL-AUDIO-CABLE-PLAN.md); the iPhone Duo half-open layout (memory).
