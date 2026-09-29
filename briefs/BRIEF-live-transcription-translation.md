# BRIEF — live transcription and translation, on the device, free

Stuart, 2026-09-29, on Echo SDR Pro charging £4.99/month for on-device transcription + translation:
*"We can one up him by adding it for FREE."* Browser: *"doesn't matter too much"* — **app first**.
Target: the first update after V11 ships (11.1). Not before the store submission.

## The promise
Live captions of what the radio is saying, optionally translated, **on the device, free, private — nothing
leaves the phone**. Sold as *follow along*, not a perfect transcript: clean broadcast AM/FM transcribes
well; noisy SSB, fading shortwave and airband will be patchy, and the UI must say so honestly.

## Platforms and engines (all on-device, no server, no account, no cost)
| Platform | Speech-to-text | Translation |
|---|---|---|
| iPhone / iPad / Mac | Apple **SpeechAnalyzer / SpeechTranscriber** (iOS/macOS 26, long-form, on-device); fall back to `SFSpeechRecognizer` with `requiresOnDeviceRecognition` on older OS | Apple **Translation** framework (on-device language packs) |
| Android | on-device `SpeechRecognizer` (check `isOnDeviceRecognitionAvailable`, API 31+) | **ML Kit Translation** (free, on-device, downloadable models) |
| Web client | not in scope (Chrome-only APIs; revisit later) | — |
| Jr (watch) | not in scope — could show the phone's captions via the existing link later | — |
★ Never silently fall back to a server-based recogniser: if on-device isn't available for that language, say so.

## Audio source
The DEMODULATED audio the listener hears (post-demod, post-NR, 16 kHz mono resample for the recogniser),
tapped AFTER the audio pipeline — never touching playback timing. Thread priority rule applies
(Network > Audio > Spectrum > Decoders): transcription is a decoder — it drops, playback never waits.
Works for every backend (VibeServer, UberSDR, Kiwi, OWRX, FM-DX, local dongle) because it listens to what
we play. Off by default; costs nothing when off.

## UI (to design in the app's existing style — AGENTS.md: fix any tour/About copy)
- A CAPTIONS control where the audio tools live (speaker button sheet), with source language (auto if the
  engine supports detection, else chosen) and "translate to" (default: device language).
- A caption strip that scrolls the last few lines, legible over the waterfall, pausable; a full transcript
  view; copy/share.
- Save the transcript alongside a recording (same name, .txt), so recordings get text for free.
- First use: download the language pack with a plain explanation and size; nothing downloads silently.
- An honest "signal too noisy to transcribe" state rather than garbage text.

## Questions to settle when building
- Language auto-detection availability per platform; which broadcast languages have on-device packs.
- Battery/CPU on older phones (the 1 GB Android floor must not be made worse — feature simply unavailable
  where the engine isn't).
- Whether DAB/RDS text or bookmark names help choose the language automatically (e.g. RDS country).

## Test plan
Captured air audio: BBC R4 (clean speech), a foreign-language SW broadcast, AM MW at night, SSB, airband.
Measure word accuracy roughly per case; set user expectations from that, not from the demo case.
