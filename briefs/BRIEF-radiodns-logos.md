# BRIEF — station logos from RadioDNS

**Status, 2026-08-11:** the lookup **works end to end and is proven against the BBC**; the HTTP
endpoint that exposes it does **not** dispatch yet. Not shipped. ▶ And there is a **licensing
decision for Stuart before it should be** — see §4.

## Why RadioDNS rather than a name search

Every other source guesses from the station's NAME, and the name is what RDS is worst at: the PS
field is EIGHT CHARACTERS, so "BBC Radio 2" is transmitted as `BBC R2` and no database matches it.
RadioDNS goes the other way — it identifies the service by **PI code, ECC and frequency**, which
is exactly what a receiver already knows.

The answer comes from the **broadcaster**, so it is their real artwork at known sizes rather than a
user-submitted favicon that may be a dead link (which is what put an empty box on Classic FM).

**Credit: RadioDNS — https://radiodns.org** — free to use, no patents, no licences, no account.

## 1. The lookup, verified

```
08810.c202.ce1.fm.radiodns.org          CNAME -> radiodns.api.bbci.co.uk
_radiospi._tcp.radiodns.api.bbci.co.uk  SRV   -> 0 100 443 radiospi.api.bbci.co.uk
https://radiospi.api.bbci.co.uk/radiodns/spi/3.1/SI.xml
  -> https://sounds.files.bbci.co.uk/.../bbc_radio_two/blocks-colour-black_1920x1080.png
```

★★★ **THE CNAME TARGET IS NOT A WEB SERVER.** It is a naming anchor with no address record at all
(`could not resolve host: radiodns.api.bbci.co.uk`), and skipping the **SRV** step is the mistake
that makes this look like "the station is not in RadioDNS". The SRV gives the host AND the port,
and the port is what says http or https.

★★ **The GCC is not the ECC.** It is the PI's country nibble followed by the ECC — `c` from PI
`C202` plus ECC `E1` = `ce1`.

★ Frequency is **MHz × 100**, five digits: 88.1 MHz → `08810`. My first cut divided by 100 kHz and
produced `00881`, which resolves nothing — while the comment beside it had the right answer.

★★ **Scope the logo to OUR bearer.** The SPI document lists every service the broadcaster runs, so
taking the first `<multimedia>` hands back a sibling's artwork — Radio 1's logo on Radio 4.
Verified separately: C202 → `bbc_radio_two`, C204 → `bbc_radio_fourfm`.

## 2. Why it belongs in the SERVER

A browser can do neither half: no raw DNS SRV query, and broadcaster SPI hosts send no CORS
headers. The daemon has neither problem — and doing it here fixes it **once for every client**
(browser, phone, watch) instead of three implementations of the same guessing game.

`vibeserver/radiodns.{h,cpp}` — DoH for CNAME and SRV, curl for the SPI, cached in memory: a day
for a hit, an hour for a miss, so a station that is not in RadioDNS does not cost two DNS queries
and a fetch every time somebody tunes past it.

## 3. ▶ WHAT IS LEFT

`GET /vibeserver/stationlogo?pi=&ecc=&freq=` returns **404**. The branch is present in the built
binary and sits in the same `else if` chain as `/vibeserver/radios`, which answers 200 — so the
chain is reached and the branch is skipped, and I have not found why. Everything else works: the
handler is registered, and `logoFor()` returns the correct BBC URLs when called directly.
★ Then: client falls back to the existing radio-browser search when this returns `{}`.

## 4. ★★★ THE LICENSING DECISION — READ BEFORE SHIPPING

RadioDNS itself is free and unlicensed. **The DATA is not** — it belongs to each broadcaster and
carries their terms. The BBC's are at https://radiospi.api.bbci.co.uk/terms.txt and include:

- **3.1.3 — "access to BBC Metadata is made available to all users … FREE OF CHARGE. The
  Manufacturer may not charge users to access any BBC Metadata."** VibeSDR is a **£2.99 app**. Is
  the user being charged to access the metadata, or is it incidental to a paid app? Genuinely
  arguable, and not my call.
- **5.4 — no press release or public announcement of use without prior written consent.** That
  touches release notes and the website.
- **2.1 / 3.1.4 — no adapting, editing or re-positioning the metadata**; display as provided.
- **4.3 — refresh at least monthly** (the 24-hour cache satisfies this).
- The BBC invites contact: `carriage.enquiries@bbc.co.uk`.

★ Other broadcasters serve their own SPI under their own terms; the BBC's are simply the ones we
hit first. ▶ **Recommendation: email the BBC before enabling it on a public server.** They ask to
be told, the answer costs nothing, and it converts an arguable reading of 3.1.3 into a known one.
