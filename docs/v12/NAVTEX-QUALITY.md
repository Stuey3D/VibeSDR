# NAVTEX decode quality + SNR timeline (beta96, branch b96-navfec)

Stuart 11-26: "yes add the error correction stats in", then: each message's SNR, an SNR timeline per message, the strip
down the LEFT of the text (one colour per rendered line, the worst slice on it), and the community feed's quality
figures so the map can pick the best copy ("I hear it with a large red part in the middle, NickB hears it all green →
use NickB's").

**DISPLAY ONLY.** Nothing here changes what the decoder prints, its lock, or what counts as a message. The split is read
from the same soft bits *after* `decodeSoft` has decided; the SNR reads the front end's tone magnitudes. test-navtex-fec
(old counts unchanged) and test-navtex-snr ("clean 10 dB decode is exact with the meter running") check that.

## RESUME HERE
- All built and committed on `b96-navfec`; nothing pushed or merged.
- Not verified on real air: every figure is from synthetic SITOR-B. A live 518/490 kHz session (the coordinator's) should
  confirm the SNR reads sensibly (a strong Niton/Cullercoats copy: green, 20–35 dB in 100 Hz; a weak far station: amber).
- The app's strip (React Native `onTextLayout`) is tested as logic (`lineRangesOf` at two layouts) but not on a device.
- R36S: only the 0x07 split line (`dec_state.h`); not built here (needs the arm64 image) — no SNR on the handheld.

## Definitions
- **clean** — both copies (DX and RX, 350 ms apart) valid 4-of-7 words and the same word.
- **FEC** — printed from one copy (the other invalid, missing, or overruled by the vote) or from the soft tiers / ML.
  = `clean + repaired − agreed` of NavtexRx::Counts.
- **lost** — neither copy usable ('_'), = `failed`.
- **copies disagreed** — both valid but different words (shown as its own %; those characters sit in clean-or-FEC by
  what was printed — e.g. RX valid but wrong and the vote took DX: FEC; DX hit and RX printed: still `clean` to the old
  counts, FEC in the split).
- **SNR** — in the **bit bandwidth (100 Hz at 100 baud)**, as the SITOR-A status reports. At the centre of each bit
  that sits in the middle of three alike (so no transition leaks keying sidebands into the other tone), the chosen
  tone's |m|² is signal + noise and the other tone's is noise alone; noise per channel = 2·N0·Σh²·fs of the raised
  cosine; noise in 100 Hz = N0·baud. Signal / noise also as dBFS (int16 full-scale sine = 0 dBFS). Colours: < 5 dB red,
  5–10 amber, > 10 green (`navtexQuality.ts`).
- **weak %** (`redPct`) — the share of the message's non-space characters decoded during a slice under 5 dB.
- **Slices** — one per second of the bit clock (locked or not, so a fade shows), ≤ 120 a message; longer messages merge
  pairs at equal length (an odd last slice becomes half of the next). Each slice records the character index where it
  ENDED in the cleaned text (`tc`), so the strip maps slices → characters → wrapped lines at render time.

## Wire / record
- op **0x07** (after the NNNN): `u32 clean | u32 repaired | u32 lost` + **beta96** `| u32 agreed | u32 disagreed`
  (21 bytes; every client still reads the first 13).
- op **0x08** (new, a second, sent BEFORE the same chunk's text): `i16 BE signal | i16 BE noise`, dBFS × 10.
- Gallery index line (only what was measured): `nqc nqf nql nqd` (counts), `snr sdb ndb smin red ts tl[] tc[]`.
  The file's head gains "Decode quality: … (n characters; copies disagreed …)" and "SNR x dB avg · lowest · y % weak
  (signal, noise dBFS in 100 Hz)". The title is unchanged (vibe_navtex_map.h parses it).
- Community feed `nx` rows: + `clean %, FEC %, lost %` (14 members) and + `SNR dB, lowest dB, weak %` (17); older
  records keep 11; never the slices. A server's own copies of one id are ranked weak % → clean % → lowest → mean SNR
  when both are measured, else the old fewest-lost rule. (Page side: the coordinator.)

## Where
- Decoder: `android/app/src/main/cpp/decoders/fsk_decoder.{h,cpp}` (Counts.agreed/disagreed/on/off/bits, measureBit,
  snrOf, onSlice).
- Host: `vibe_decoder_host.h` (0x08, 21-byte 0x07). Gallery: `vibe_gallery.h` (NavtexAsm::slice/quality,
  navtexQualityJson/Line, navtexSnrLine), `vibe_bgdecode.h` (tap reads 0x07/0x08). Community: `vibe_community.h`,
  `local_sdr_shim.cpp`.
- Shared client rules: `src/utils/navtexQuality.ts`; assembler `src/utils/navtex.ts` (slice(), split, SAVE lines).
- Web: `web/client/src/navtexSnrDom.ts`, `decoders.ts`, `main.ts` (live card, gallery rows + viewer), `index.html`.
- App: `src/components/NavtexQuality.tsx`, `DecoderPanel.tsx`, `GalleryOverlay.tsx`, `SDRScreen.tsx`,
  `services/DecoderClient.ts`. R36S: `handheld-ui/src/dec_state.h`.

## Tests (how)
- `vibeserver/test-navtex-fec.cpp` §7 — exact bit streams: clean → all agreed; one RX + one DX copy lost → 2 FEC; both
  lost → 1 lost; RX valid-but-wrong → 1 disagreed + FEC; DX valid-but-wrong → 1 disagreed, RX printed; old counts same.
- `vibeserver/test-navtex-snr.cpp` — phase-continuous FSK + white noise at true power SNRs −8…+60 dB in 3 kHz: read
  within 1 dB of SNR + 14.8 (bit bandwidth) from 6.8 to 74.8 dB; signal dBFS exact; 1 s slices; a 6 s 25 dB fade reads
  < 5 dB inside and ≥ 10 outside; the biquad front end reports none.
  ★ Found: `tools/rtty-bench/synth_navtex.py`'s "snr" sets noise from the amplitude, so its label is 3 dB above the power
  SNR (its "+15 dB" is 12 dB). Left alone — the bench's CER thresholds were measured on it.
- `vibeserver/test-navtex-quality.cpp` — the gallery assembler: slices tied to the cleaned text, redPct/minSnr, all
  green = 0 %, the cap (300 s → 75 slices of 4 s, the fade still red), a lock-loss fade's slices on the gap marker, the
  tap end to end (21-byte 0x07 → nqc 38 / nqf 4 / nql 1 / nqd 1 and the file lines; 13-byte → nothing invented).
- `vibeserver/test-community.cpp` §8 — quality read from an index line, the all-green copy beats a red-middle one with a
  higher mean, row members, no slices, old rows unchanged.
- `scripts/test_navtex.ts` — the client assembler: slices grow LIVE, fade lines red at two widths (wrapped every 80 and
  every 6–8 characters, the web rule and the app's `lineRangesOf`), worst per line, split from 0x07, SAVE lines, an old
  server, cap, gap marker, gallery records old/new/malformed.
- `scripts/test-web-navtex-snr.mjs` — headless Edge (muted), the live card's markup + index.html's CSS: at 640 px and
  170 px every rendered line's strip colour = the worst slice of its own characters (read back per character from the
  layout), the faded lines red and nothing else; 10 px gutter; timeline red in the middle; quality bar; old server: none.

## CPU
Not measured separately (estimate): measureBit adds two multiplies and a ring write per front sample (2 kHz), a few adds
per bit (100/s), one `pow` pair a second in each assembler — against the raised cosine's ≈ 240 taps × 4 channels per
front sample it is well under 1 % of NavtexRx.
