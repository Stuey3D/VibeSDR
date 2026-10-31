# LoRa, LoRaWAN, Meshtastic and MeshCore in ISM mode (V12)

Stuart, 2026-10-27: *"can we incorporate LoRA/Meshtastic/Meshcore and any other 868 mesh stuff"* — the Pi 500's ISM 868
view showed "loads of pulses … but nothing decoded at all". Then: *"Yes show the public messages since they are
accessible by design and also add a map button to the ISM decoder to switch to a Map view for the nodes etc"*.

ISM mode (docs/v12/ISM-DECODER.md) now also runs a LoRa receiver over the same capture: every plan channel the capture
holds, every spreading factor the channel uses, on its own thread. What it hears joins the ISM device list as rows of
kind `meshtastic`, `meshcore`, `lorawan` and `lora`; the public messages are listed under MESSAGES; anything with a
position goes on the ISM map (the MAP key in the box header).

| File | What |
|---|---|
| `android/app/src/main/cpp/vibe_lora.h` | the PHY: codec, reference modulator, demodulator, channeliser |
| `vibe_lora_crypto.h` | AES-128, SHA-256, HMAC-SHA-256 (for the PUBLISHED keys only) |
| `vibe_lorawan.h`, `vibe_meshtastic.h`, `vibe_meshcore.h` | the three protocol parsers and their privacy rules |
| `vibe_lora_msg.h` | `Msg` — what a frame said, after the privacy rule; the protobuf wire reader |
| `vibe_lora_service.h` | the channel plans, the governor, the engine thread |
| `vibe_lora_track.h` | the list's rows (nodes, devices), the public messages, the gallery's events |
| `vibe_ism_service.h` | owns the engine; merges its rows into the report (`lora`, `msgs`) |
| `vibeserver/test-lora.cpp` | everything below that says "test-lora" |
| `scripts/test-server-lora.mjs`, `scripts/test-web-lora.mjs` | the real server, and the web client in headless Edge |
| `web/client/src/ism.ts`, `src/services/ism.ts`, `src/components/IsmPanel.tsx`, `MapOverlay.tsx` (`'ism'`) | the clients |

---

## 1. Sources — what came from where, facts vs assumptions

**Rule (Stuart):** published standards first; GPL code may be READ for understanding, never copied. Nothing here was
copied from gr-lora, gr-lora_sdr, rtl_433 or any Meshtastic / MeshCore firmware; the protobuf definitions were read
for field numbers only.

### 1.1 The LoRa physical layer

| Fact | Source | Status |
|---|---|---|
| Chirp spread spectrum; SF 7–12 (2^SF chips/symbol); BW 62.5/125/250/500 kHz; CR 4/5–4/8; explicit header; CRC; preamble, 2 sync symbols, 2.25 down-chirps (SFD) | Semtech AN1200.22 *LoRa Modulation Basics*; SX1272/76 and SX1261/2 datasheets | published |
| LDRO mandatory when a symbol lasts > 16 ms | SX127x datasheet | published |
| Sensitivity SNR per SF: SF7 −7.5, SF8 −10, SF9 −12.5, SF10 −15, SF11 −17.5, SF12 −20 dB | SX1276 datasheet table | published |
| Whitening LFSR (x^8+x^6+x^5+x^4+1, seed 0xFF) | Knight & Seeber, *Decoding LoRa* (GRCon 2016); Tapparel et al. (EPFL 2020) | literature; checked in test-lora (first bytes FF FE FC F8 F0 E1 C2 85 0B) |
| Hamming (8,4)/(7,4) and the 4/5, 4/6 parity codes; diagonal interleaver; Gray step; the header's 5-bit checksum; header and LDRO symbols carry SF−2 bits | Tapparel et al.; Robyns et al. (2018) | literature |
| Payload CRC = CRC-16/CCITT (init 0) of all but the last two bytes, XOR those two; sent low byte first | Tapparel et al. §III | literature |
| Sync symbols = each nibble of the sync word × 8 (0x34 → 24, 32) | Knight & Seeber; gr-lora_sdr's documentation | literature |
| **ASSUMED**: the exact bit order of the Hamming codeword (data bits LSB-first, then p0…p3), the header checksum's five masks (`0xF00, 0x8E1, 0x49A, 0x257, 0x12F` over n0·n1·n2), the +1 symbol offset and the reduced-rate symbol at 4·g+1 | reconstructed from the literature's descriptions | **confirmed on air** (§6: 31 real MeshCore frames pass header checksum and CRC) |

### 1.2 LoRaWAN

LoRaWAN 1.0.x / 1.1 Link Layer Specification and the Regional Parameters (LoRa Alliance, public): MHDR (MType, Major),
FHDR (DevAddr LE, FCtrl, FCnt LE, FOpts), FPort, MIC; join request = JoinEUI(8) DevEUI(8) DevNonce(2) MIC(4); uplinks
carry the PHY CRC, downlinks don't (and use inverted IQ); EU868's three default channels 868.1/868.3/868.5 and the RX2
869.525 MHz. The Things Network's DevAddr block 26000000/7 (NetID 0x000013) is from TTN's own addressing docs.

### 1.3 Meshtastic

From meshtastic.org: the on-air header (dest, sender, packet id — 4 bytes LE each; flags: hop limit bits 0-2, want-ack
3, via-MQTT 4, hop start 5-7; channel hash; next hop; relay node), sync word **0x2B**, preamble **16** below 1 GHz,
AES-CTR (16-byte key = AES-128), "the default primary channel uses the key AQ==, which is published", EU_868 869.40–
869.65 MHz with LongFast at 869.525, EU_433 433–434 MHz, the modem presets. The message layout (Data, User, Position,
Telemetry, PortNum, HardwareModel, Role) from the published protobufs (meshtastic/protobufs, GPL-3 — read for the field
numbers; the reader in `vibe_lora_msg.h` is ours).

**ASSUMED (documented firmware behaviour, not on the docs pages):** "AQ==" expands to the 16 default key bytes
`d4f1bb3a 20290759 f0bcffab cf4e6901`; the channel hash is XOR(name bytes) XOR XOR(key bytes); the CTR nonce is the
packet id as 64-bit LE, then the sender's node number LE, then zeros. Cross-checks that pass in test-lora: LongFast +
the default key gives channel hash **8** (the value the community quotes), and the djb2 slot hash puts LongFast on US
slot 20 (906.875 MHz) and EU_433's 433.875 MHz, as the radio-settings page says. Not yet seen on air (§6).

### 1.4 MeshCore

From MeshCore's own docs (github.com/meshcore-dev/MeshCore `docs/packet_format.md`, `payloads.md`,
`companion_protocol.md`, `faq.md`): the header byte (route bits 0-1, payload type 2-5, version 6-7), transport codes,
the path-length byte (hop count + hash size), ADVERT (public key 32, timestamp, signature 64, appdata: flags, lat/lon
×1e6, name), GRP_TXT (channel hash = first byte of SHA-256 of the key, 2-byte MAC, ciphertext; plaintext = timestamp,
flags, "sender: message"), and the built-in Public channel's key `8b3387e9c5cdea6ac9e5edbaa115cd72` ("messages should be
considered public"). Radio presets from MeshCore's preset list (api.meshcore.nz): **EU/UK (Narrow) 869.618 MHz, SF8,
BW 62.5, CR 4/8**; NL/PT/SK/HU 869.618 SF7; CZ 869.432 SF7; EU 433 Narrow 433.650 SF8.

**ASSUMED (the docs name a "cipher MAC" and blocks but not the algorithms):** AES-128-ECB over zero-padded blocks, MAC =
first two bytes of HMAC-SHA-256 keyed with the 16-byte secret zero-padded to 32 bytes; sync word 0x12 (the radio
library's private default). A Public message is shown **only if its MAC verifies**, so a wrong assumption shows
nothing, never garbage.

---

## 2. Privacy — decided, and what Stuart can change

| | Shown | Never shown |
|---|---|---|
| LoRaWAN | MType, DevAddr (and the network when published: TTN), FCnt, FPort, ADR/ACK, SF/BW/CR, SNR, frequency; join requests: DevEUI and JoinEUI (as a meter's serial is shown) | the payload — its **length** and the word "encrypted" only; FOpts are counted, not shown |
| Meshtastic | the clear header of every packet (from, to, hops, channel hash); under the **published default key** on a default-named channel: node info (long/short name, hardware, role), positions (as sent — often deliberately coarse), telemetry (battery, voltage, temperature …) and **broadcast** text | any packet that doesn't open with the published key ("private channel", header only); PKI direct messages (channel hash 0); a text addressed to one node even if it opens with the default key (it was addressed, not published) |
| MeshCore | adverts (name, kind, position — signed and broadcast in the clear); **Public** channel messages ("sender: text" — the sender's name is unverified by design) | every other group channel (incl. hashtag channels — we never guess names), direct messages, requests, responses — header only (type, route, hops) |

★★ **Decided by Stuart (2026-10-27): public messages ARE shown** ("accessible by design"). If he prefers node info and
positions only, the switch is `MeshTable::accept` (don't keep `m.text`) or the clients' MESSAGES chip; the owner can also
switch LoRa off per radio on the Setup page ("LoRa / Meshtastic / MeshCore").
★ No key is guessed, searched for, or accepted from a listener. The only keys in the code are the two the networks
publish. Tests: test-lora "the privacy rule, on what a client receives", test-server-lora "PRIVACY ON THE WIRE".

---

## 3. How it works

**Channeliser (one FFT for all channels).** Overlap-save: the capture is transformed once per block (2048 points at
2.4 MS/s, step 1536), each channel takes the bins around its centre, weights them with a Hamming-windowed sinc
(L/4+1 taps) and transforms them back at a small size (the decimation), the block's phase is corrected for where it
starts, an NCO removes the sub-bin residue, and an 8-tap windowed-sinc interpolator lands it on exactly its chip rate.

**Demodulator (one per channel × SF).** SEARCH dechirps every window and FFTs it; a preamble is K=4 of the last 5
windows agreeing to ±1 bin with a peak 7 dB over the mean. ALIGN moves the grid back by that bin (CFO folded into
timing), finds the SFD as **two** consecutive windows whose down-dechirp peaks agree and beat their up-dechirps,
reads the sync word from the two windows before, then separates CFO from timing **to a fraction**: on the corrected
grid the preamble's up-dechirp is CFO + δ and the SFD's down-dechirp CFO − δ.

★★★ **Fractional timing is not a frequency.** Inside each symbol the chirp wraps from +BW/2 to −BW/2; sampled δ off
the chip grid, that wrap is a phase jump of 2πδ, and the tone splits between two bins by an amount that depends on the
symbol's value. At one sample per chip with no filter (the first round-trip tests) it is invisible; through the
channeliser every SF11 frame with a few kHz of CFO lost symbols at 40 dB SNR. So δ is measured and each symbol's window
is **interpolated onto its exact start**; only the true CFO is removed as a frequency.
★★ **The chip clock drifts with the carrier:** a transmitter whose carrier is high by CFO runs its chips fast by the
same ppm — a 230-symbol SF11 packet at 20 ppm slides 9 samples. Symbol k starts at t0 + k·N·(1 − CFO/f), and the slide
between the preamble windows and the SFD is carried too (SF12 at 500 kHz lost every frame without it).

**Plans (`planFor`)** — only channels inside ±45 % of the capture run:

| band | tier 0 (always, while LoRa is on) | tier 1 / 2 (budget) |
|---|---|---|
| 868 | Meshtastic LongFast 869.525 (BW250 SF11, syncs 0x2B / 0x12), MeshCore EU/UK 869.618 (BW62.5 SF8), LoRaWAN 868.1 / .3 / .5 (BW125 SF7–12) | MeshCore SF7; Meshtastic other presets SF7–10; LoRaWAN 867.1–867.9; RX2 869.525 inverted (SF9, SF12); LongModerate 869.4625 / 869.5875; MeshCore CZ |
| 433 | Meshtastic 433.875, MeshCore 433.650 | LoRaWAN EU433 433.175/.375/.575; MeshCore 433 variants |
| 915 | LoRaWAN US915 914.3 / 914.5 (SF7–10) | 914.7 / 914.9 |

At 2.4 MS/s on 868.6 MHz, 13 of the 16 EU868 channels fit (867.1–867.5 do not). US Meshtastic (906.875) and US
MeshCore (910.525) are outside the 915 band's capture.

**Governor.** A detector's cost unit = its rate/125 kHz × (SF+3)/10. The benchmark row `dec_lora` times the 868 default
set through the threaded engine in real time; it is GRADED by its hottest thread (LoRa runs on `vibe-lora` / `lora2` /
`lora3` since 2026-10-31), and `budgetFromBench` reads the engine's whole work (the sum of those three threads in the row's
detail, `vsLoraBenchPct`) and adds tiers while all of LoRa would stay under 25 % of one core. No benchmark: everything on
64-bit, tier 0 only on 32-bit ARM. **The benchmark never hides the default set.**

**The owner's two switches, on the Setup page and the Android server screen** (the modes list, drawn as a tree since
2026-10-31: ISM › LoRa › every channel):
- **LoRa / Meshtastic / MeshCore** (`lora` in the blocked list when off). Under ISM: LoRa runs only inside ISM mode, so
  with ISM off it is greyed with the reason.
- **LoRa: listen on every channel** (`lora+all` in the list when ON — the "+" convention of `spotsband+wspr`), under LoRa,
  greyed while LoRa or ISM is off. **Unticked does NOT mean no LoRa**: the default channels above always decode while LoRa
  is on, plus as many tier 1/2 channels as the benchmark affords. Its hint says so, and how many run now
  (vibeserver.json `loraChannels`: per band `plan`, `fit` in the capture, `defaults`, `run`).
- **The benchmark's row `dec_lora_all`** ("Decoder: LoRa (868, every channel)": the same threaded engine on every channel
  the 868 capture holds, real time, scored by its hottest thread, the process total in the detail) sets its default on a
  fresh setup: GREEN ticks it on (with "it measured X % … so it was turned on"); anything else leaves it off with "measured
  X % … tick it to use it anyway". The advice block says "Can be enabled: LoRa on every channel" when green. ONE RULE, TWO
  READERS (`benchApplyDefaults`, `applyBench`). On the Pi 2 the row grades amber (two threads at ~77–84 %, keeping up),
  so it starts off there; the owner can tick it.

**Threads.** Network > Audio > Spectrum > Decoders: `Engine::feed` copies and returns; its own queue drops the oldest
block past half a second and counts it (`lora.dropped`). The ISM worker drains decoded frames each wake and merges them.

---

## 4. Measured

**Synthetic (test-lora), through the real channeliser at 4× the chip rate with random sub-sample timing, ±10 kHz CFO
and the matching chip-clock drift:** 96/96 frames byte-exact at every SF × BW × CR; a 60-byte frame at −3 dB at every SF.

**Sensitivity** (BW 125, CR 4/5, 20 bytes, 40 frames per point, packets decoded byte-exact):

| | Semtech | −3 dB | −2 dB | −1 dB | at Semtech's SNR | +1 dB |
|---|---|---|---|---|---|---|
| SF7 | −7.5 | 0 % | 12 % | 37 % | **82 %** | 90 % |
| SF8 | −10 | 2 % | 30 % | 40 % | **82 %** | 100 % |
| SF9 | −12.5 | 5 % | 30 % | 67 % | **100 %** | 95 % |
| SF10 | −15 | 2 % | 27 % | 75 % | **95 %** | 97 % |
| SF11 | −17.5 | 7 % | 45 % | 87 % | **97 %** | 97 % |
| SF12 | −20 | 12 % | 62 % | 95 % | **100 %** | 97 % |

So the 50 % point sits about 1–2 dB below the datasheet's figure. test-lora asserts ≥ 80 % at the datasheet SNR and
≥ 90 % 1 dB above.

**Whole band (test-lora / ism-offline):** Meshtastic ×5, MeshCore ×3 and LoRaWAN ×3 at once at +3 dB, all decoded; in the
looping clip, overlapping Meshtastic and MeshCore frames in the same 250 kHz decode both. 20 s of noise through every
channel → 0 frames.

**CPU (Mac, this machine, server build: PFFFT, -O3), 2.4 MS/s:** default set (5 channels, 20 detectors) **2.1 % of one
core**; everything (13 channels, 42 detectors) **4.0 %** (SF11/12 search every other window — no loss in the sweep). Header-only build (radix-2 FFT): 5.0 / 8.7 %.
**Pi 2 — not measured** (not to be touched): at the ~10–15× per-core ratio used for ISM and wM-Bus, the default set is
**~20–30 % of one Pi 2 core** on top of the ISM front end (10–20 %), so the governor keeps a Pi 2 at the default set.
`scripts/run-tests.sh` on the Pi 2 prints test-lora's CPU rows; the benchmark's `dec_lora` row measures it in place.

---

## 5. The clients (web and app, one rule)

- Chips: MESHTASTIC, MESHCORE, LORAWAN (with unnamed LoRa), MESSAGES. Lines are the server's (no unit choice).
- A tap on a node or device opens its detail: frequency, SF/BW/CR, SNR, hops; node id, hardware, role, channel, battery,
  voltage, channel use; position, altitude, **range and bearing from the receiver**, "deliberately approximate" when the
  node truncated it; its recent public messages. LoRaWAN: "Payload encrypted by the network — its length is shown, never
  its bytes."
- Status line: "… · LoRa 31 frames · 13 of 16 channels · …".
- **MAP** in the box header, drawn only while something listed has a position (the spots MAP's rule: no dead key). Web:
  the shared TrackView (APRS's map). App: MapOverlay kind `'ism'` (the shared track layer + TrackLiveList), not on Apple
  TV. Markers by network (T Meshtastic green, C MeshCore orange; hollow when not heard for 15 min), the receiver's house,
  a card with the detail, the list nearest first, "N on the map · furthest X km". CLOSE MAP returns to the list; leaving
  ISM closes it. Range rings: not drawn (the track layer has none) — the list gives range and bearing instead.
- Gallery: nodes, LoRaWAN devices and public messages on the band's day card (kind words in `vibe_ism_gallery.h`).
- Copy: the About V12 note and the receiver tour's mode card mention the networks, MESSAGES and MAP.

---

## 6. Real traffic — status

Recordings from Stuart's Pi 500, 2026-10-27 (session gain 33.8 dB, restored to AUTO; background ADS-B left with his OK
and it took the radio back afterwards):

- **48002** (V4 Lite #00000004), 868.8 and 867.5 MHz at 2.4 MS/s: pure noise — a max-hold never rose 8 dB above the
  median in 2 minutes, not even wM-Bus — and 0 preambles. That dongle does not hear 868 where it is.
- **48004** (V4 Lite #00000001, the one Stuart watches), 250 kS/s slices. ★ At 250 kS/s the raw IQ out on that
  single-listener radio is the listener's **passband**, not the capture: in NFM it carried ±7 kHz of signal inside
  250 kHz of nothing (`iqCb`, direct mode — the RxPipeline channel after the mode's filter; the listener path widens
  its channel for raw IQ, the direct path apparently does not). Recorded in **WFM** instead (±100 kHz).
  - **869.618 MHz, 5 min: 20 MeshCore frames, CRC good** (59 preambles, 35 SFDs, 3 bad CRCs, 0 bad headers): adverts
    (a repeater with a position), group messages on other channels (header only — none on the Public channel, whose
    hash is 0x11; these were 0x3D, 0x55, 0x84, 0x12, 0xD9), direct messages, requests, responses and path packets,
    1–42 hops, SNR −12.8 … 0 dB (below the noise, as LoRa should be), sync word read as **0x12**.
  - **869.525 MHz, 2 × 5 min:** 2 + 9 more MeshCore frames on the 869.618 channel at the passband's edge (incl. a room
    server's advert with a position); **no Meshtastic** frames (and its 250 kHz is wider than the WFM passband).
  - **868.3 / 868.1 MHz (LoRaWAN), 150 + 40 s:** no LoRaWAN frames; 868.3 is busy with other bursts (~9 % of the time).
- **What that proves:** the whole bit-level chain of §1.1 — Gray step and offset, reduced-rate header symbols, the
  interleaver, the Hamming bit order, the header checksum masks, the whitening and the CRC — is right on real chips:
  frames with CR 4/8 and lengths 25–135 bytes pass header and CRC. MeshCore's header / path / advert layout parses on
  real packets. **Still unconfirmed on air:** Meshtastic (no traffic heard), the MeshCore Public channel's MAC/cipher
  (no Public message heard in 10 min), LoRaWAN headers. `ism-offline <capture.u8> <rate> <centre>` shows every frame
  and the per-detector counts.

## 7. Not done / next

- Implicit-header frames (none of the three networks needs them).
- A per-hour node count and "furthest" on the gallery card (the service does not know the receiver's position).
- ~~A Setup-page control for `lora+all`.~~ Done 2026-10-31 (§3, "The owner's two switches").
- Soft-decision decoding for CR 4/5–4/6 (~1 dB more).
