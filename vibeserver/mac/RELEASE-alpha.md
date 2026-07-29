## VibeServer for macOS — 0.4 Alpha

VibeServer turns a Mac and an SDR into a small, private SDR server. Plug in a radio,
click **Open in Browser**, and you have a full waterfall, tuning and audio in the browser — for
yourself, or for your phone, watch and friends on your network. Same C++ DSP core as the VibeSDR
app.

### New in 0.4 — SDRplay support, and an Advanced RDS decoder

**SDRplay RSP receivers are supported** alongside RTL-SDR dongles. Pick your radio in
Settings ▸ Radio. On an RSP you get its real controls rather than a dongle's: RF gain as an
LNA state, IF gain reduction, IF AGC with an adjustable target, the FM and DAB notch filters,
bias-T, sample rates to 8 MHz, and the receiver's own overload warning.

> ### ⚠️ SDRplay needs its API installed first
>
> VibeServer does not bundle it — SDRplay distribute it themselves, and it installs a
> background service that every application shares.
>
> **Download the API (v3.15 or later) from:** https://www.sdrplay.com/downloads/
>
> Install it, then plug in your RSP and press **Refresh** in Settings ▸ Radio. Without the
> API installed, RSPs simply will not appear — everything else works exactly as before, so
> there is no need to install it if you only use a dongle.
>
> ★ If a radio stops appearing after another SDR program has crashed, that program may have
> left the SDRplay service locked. Restart it with
> `sudo launchctl kickstart -k system/com.sdrplay.service`, then press Refresh. VibeServer
> will tell you when this has happened rather than hanging.

---

## 📻 ADV RDS — a broadcast-analyser view, for FM-DXers

Open **DECODERS ▸ ADV RDS**. Basic RDS (station name, RadioText, PI) is always on and needs
nothing switched on; this is the full picture, and it is built for people who care about what
a transmitter is actually doing.

**Everything the station sends**

| | |
|---|---|
| **PI** | hex and decimal, plus country, coverage area and reference number |
| **PS / Long PS** | the 8-character name, and the 32-character version (group 15A) |
| **RadioText / RT+** | including artist and title tagging where a station sends it |
| **PTY / PTYN** | programme type, and the station's own name for it |
| **TP · TA · MS · DI** | traffic programme, announcement, music/speech, decoder ID |
| **Clock** | date and time with UTC offset (group 4A) |
| **AF** | alternative frequencies — **tap one to tune there** — with a confidence score |
| **EON** | the other stations in the network, their frequencies and their TA flags |
| **ODA** | which Open Data Applications the station runs, named where we know them |
| **Group mix** | which group types it transmits and how often |
| **Language · ECC · PIN** | broadcast language, country code, programme item number |

**And what your receiver is making of it**

- **Block error rate** — errors before correction, over the last 12 groups.
- **Pilot and RDS injection in kHz deviation** — the same figures a broadcast analyser shows.
  A healthy pilot is 6.0–7.5 kHz; RDS is typically 2–4 kHz.
- **Constellation** — two tight lobes means healthy, a diffuse cloud means the subcarrier is
  buried, and a clean ring means the station's encoder is not locked to its pilot at all.
- **Symbol trace** — the "two lines" view: a clear gap through the middle means every bit is
  being decided with margin.
- **MPX spectrum** — L+R, the 19 kHz pilot, the L−R sidebands and RDS at 57 kHz, labelled.
- **★ RDS-to-pilot phase** — the measurement you would normally carry a Pira analyser to make.
  A correctly encoded transmitter sits near 0° or near 90°; anything between is worth knowing
  about. It reports how steady the reading is, and says so plainly when it cannot measure —
  a rotating constellation means the encoder is free-running, which is a finding in itself.

> **On the phase figure:** it is stable and repeatable — the same transmitter reads within a
> few degrees across different receivers and different antennas — but the absolute calibration
> has not yet been checked against a reference instrument. Treat differences between stations
> as real; treat the absolute number as provisional, and please tell us if you can compare it
> against an analyser.

Every field says what it means rather than assuming you already know: gain figures name their
spec band, the constellation carries a plain-English verdict, and the MPX is labelled.

---

### ⚠️ This is an early alpha — build in progress

- **One SDR at a time.** It serves a single receiver — an RTL-SDR (and its variants — Blog V4,
  Nooelec NESDR, etc.) or an SDRplay RSP. Multi-radio pools are designed but not built yet.
- **RSP spans below 2 MHz are not offered yet.** Zero-IF needs 2 MHz or more, and the
  decimation that would allow narrower spans is not built.
- **The advanced admin features aren't here yet** — link-management ceilings and the PIN are in,
  but the full multi-user, roles and hardware-profile wizard are still to come.
- **One listener at a time.** A second connection is politely turned away ("in use, try again
  later") until proper multi-client lands.

### Recommended use

- **Best on your local network** — share this Mac's radio with your own devices at home.
- **If you port-forward it to the internet, SET A PIN.** In Settings ▸ Access. A listener can
  change the dongle's hardware settings (gain, sample rate, bias-T on some models), so an open
  server on the public internet is handing that control to strangers. The PIN keeps it yours.

### Install

1. Download and unzip **VibeServer.zip**.
2. Move **VibeServer.app** to your Applications folder and open it — it lives in the menu bar
   (no Dock icon).
3. Plug in your RTL-SDR, then **left-click the menu-bar icon** to open the web client, or
   **right-click** for settings.

Notarised by Apple, so it opens without a Gatekeeper warning. Requires macOS 14 or later.

*Feedback welcome — this is being built in the open.*
