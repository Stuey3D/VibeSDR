# V12 — station features (parked 2026-10-07)

Stuart, 2026-10-07: "this is now a V12 feature when we build the decoders into the app and server."

| Brief | What |
|---|---|
| [SATELLITE-ENGINE.md](SATELLITE-ENGINE.md) | SGP4 + `sats.json`, Doppler lock (ISS first), time sources, display |
| [DECODERS-GALLERY.md](DECODERS-GALLERY.md) | SSTV/APRS/Meteor LRPT on the engine, the Gallery, client-assisted decode |
| [RADIO-ROLES.md](RADIO-ROLES.md) | Public / Private / Dedicated radios, task scheduler, live governor |

Each ends with a **Review against the codebase** section: what already exists (corrected clock, admin HMAC auth, stable radio
IDs, benchmark, health chip, crash reporter), what this week's fixes add (exact-rate audio, decoders keep the chain alive,
shared-dial contract for the tracker, WEFAX lessons) and corrections. Read those first.

Other V12 items: [VIRTUAL-AUDIO-CABLE-PLAN.md](../VIRTUAL-AUDIO-CABLE-PLAN.md); the iPhone Duo half-open layout (memory).
