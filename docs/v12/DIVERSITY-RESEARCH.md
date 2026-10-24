# Diversity reception with two receivers (V12 research)

**Status:** research and design only, 2026-10-24. Nothing is built, and no device, server or LAN host was touched.
The code findings were checked against `v12-adsb` at the time of writing. Everything else is a proposal.
**Asked by Stuart:** *"Diversity tuning using 2 SDR's same as how the RSP Duo does it"*. The headline case is
*"I could connect one to my front garden wire and one to my back garden wire and get the best of both worlds."*
**Related:** `RADIO-ROLES.md` (hiding member radios, stable ids), `DECODER-RESEARCH.md` (principle 2: build our
own from published papers, and treat GPL programs as behaviour references only), `BACKGROUND-DECODING.md`.

**Marking.** ⚠ marks a claim I could not confirm from a primary source, which must be checked before anyone
relies on it. 📐 marks a number I worked out from standard theory, not one measured on our code or hardware.

---

## 1. Executive summary

**What two garden wires can give you on HF.** There are two separate benefits, and they need different
machinery.

1. **Fading diversity.** Skywave signals fade at each antenna, and the two antennas do not fade at exactly the
   same moments. Picking or blending the better one removes the deep fades. In textbook Rayleigh fading with
   two fully independent branches, this is worth **about 10 dB at the 1 % outage level for plain selection,
   and about 11.5 dB for maximal-ratio combining (MRC)**. MRC also adds up to 3 dB of average SNR 📐. Real
   gains are lower, because front and back garden wires a few tens of metres apart are only partly
   decorrelated at 40 m and below. Wires that run in different directions also give *polarisation*
   diversity, which helps. We should **measure the correlation on Stuart's wires before promising a number**.
2. **Cancelling noise from the house.** If one interference source dominates (a switch-mode supply, a plasma
   TV, a solar inverter), both wires hear it with a fixed amplitude ratio and a fixed phase difference.
   Subtracting a scaled, phase-rotated copy of one wire from the other puts a *null* on that source. This is
   what the MFJ-1026, the ANC-4 and the DXE NCC-1 phasers do in analogue, and it is SDRuno's "manual"
   diversity on the RSPduo. It typically gives 20 to 40 dB on a single source ⚠ (practitioner reports, not a
   controlled measurement). Two antennas give **one null**.

**Stuart's actual motivation.** *"My front garden antenna is great on 80 m/40 m but my back garden one is
better on 160 m/20 m."* The simplest win needs no diversity DSP at all. It is **automatic per-band antenna
choice, learned from the SNR each wire actually measures** (§3.5). It works with one radio and the existing
antenna switch, or with a pair. With a pair it becomes the starting point that the live combiner refines.

**No reference is needed to avoid an echo** (§3.4). Stuart asked whether a reference-free version would work
"but with a slight echo". It works, and without the echo. *Time* alignment (cross-correlate the two streams
to remove the USB and buffer lag, then follow the slow crystal drift with fine resampling) needs no reference
and removes the echo completely. *Phase* alignment is only needed to add the two signals *before*
demodulation. Get the phase wrong there and you hear flanging or cancellation, not an echo.

**What works with two independent receivers (no shared clock).**

- **Non-coherent diversity needs no reference at all, and works today with any two of Stuart's radios.**
  Both radios tune the same frequency. We measure the SNR of each channel, then either *select* (with a short
  crossfade) or *weight and sum after detection* (FM, AM envelope). For data modes we **decode both and merge
  the results**. This catches most of the fading benefit and none of the noise-cancelling one.
- **Coherent combining (full MRC and null steering) is possible with independent radios, but only if we
  track the drift between the two crystals continuously.** The design in §4 splits the job in two:
  - **The relative clock rate (ppm).** It does not depend on frequency and changes slowly with temperature.
    It is measured once on any good carrier, *carried across retunes*, and refined whenever a lock is held.
  - **The relative phase.** It is lost at every hardware retune and every gain step. It is found again at the
    new frequency **from the wanted signal itself**, in milliseconds on a readable signal and a second or two
    on a weak one 📐. When nothing is strong enough, the system falls back to non-coherent diversity.

  With this split, the "reference station" Stuart suggested (Radio 5 Live on MW) only has to calibrate the
  ppm. It does not have to stay inside the capture. That answers *"soon as you tune away it falls apart."*
- **Null depth with independent radios is limited by their relative phase noise above the tracking
  bandwidth.** A null of 30 dB needs about 2° rms of residual phase error, and 20 dB needs about 6° 📐. With
  TCXO radios (RTL V4, RSP1A/1B, HF+), a 20 to 30 dB null looks reachable ⚠, but **we need to measure it on
  two real radios first**. With plain-crystal RTLs it will be worse.

**Which hardware is best.**

- **RSPduo.** It is coherent by design, uses one ADC chip and one clock, and SDRuno and SDRconnect already
  offer diversity on it. The cost is that dual-tuner mode caps each tuner at about 2 MHz of bandwidth, forces
  low-IF and gives up the Hi-Z port. Nobody here owns one, and our driver hard-codes single-tuner mode
  (`sdrplay_source.cpp` `applyDuoChoice`).
- **Two RX888 MkII (§6).** This pair suits Stuart's idea best for coherent HF work. The RX888 samples
  0 to 30 MHz directly, so MSF, DCF77 and Radio 5 are always inside both captures, and tuning is digital, so
  the phase between the two receivers never jumps on a retune. Feed both from one 27 MHz clock and they are
  coherent across all of HF. However, VibeServer has no RX888 driver: the RX888 belongs to UberSDR's world.
- **Two of the radios Stuart already owns (RSP1A + RSP1B is the best pair, then RTL V4 × 2, then mixed
  pairs).** These are the realistic first target. Non-coherent diversity works on them at once. Tracked
  coherent combining is the experimental second step.

**Recommended first step (phase 1, §7).** It is entirely reference-free. Ship two things:

- **Per-band learned antenna selection** (§3.5).
- **Time-aligned post-detection diversity**: selection with a short crossfade, or SNR-weighted combining of
  the demodulated audio or envelope.

Coherent combining with a tracked reference comes second. That covers MRC, null steering, and the "use one
SDR to cancel the other's noise" canceller (§4.7).

The first step adds a **"diversity pair" radio** to VibeServer: two member radios
tuned together, one process owning both, and one stable `/r/<id>`. The first version should:

- run **non-coherent** combining: selection with a crossfade for SSB/CW, SNR-weighted post-detection sums for
  AM/FM/NFM, and decode-both-and-merge for FT8/NAVTEX/RTTY;
- show a **"which antenna is winning" meter**;
- ship a **built-in measurement**: the envelope correlation of the two branches, their SNRs, and their
  relative drift. This tells us whether Stuart's wires are worth the coherent work before we build it.

**The hard parts, stated plainly.**

- Each radio currently runs in **its own process**, so the two IQ streams never meet.
- RTL and HF+ streams carry **no sample counter**, so a dropped USB transfer silently shifts one stream. We
  have to detect the jump and re-acquire.
- **Independent AGCs and gain steps rotate the phase**. In coherent mode both gains must be held fixed, or
  every step must be reported to the combiner.
- Two radios at 2 MS/s is **beyond a comfortable load for a Pi 2**. It is fine on a Pi 4 or 5 and on x86.

---

## 2. How existing products do it

### 2.1 SDRplay RSPduo (SDRuno, SDRconnect)

- **Hardware.** There are two tuners in one box. In dual-tuner mode, each tuner's low-IF output goes to its own
  ADC path, and both paths share one sample clock (the ADC rate is common; the first tuner configured is the
  "master" and sets it). The API (`vendor/openrsp/include/sdrplay_api_rspDuo.h`) has four modes:
  `Single_Tuner`, `Dual_Tuner`, `Master` and `Slave`. It also has IF types `IF_1_620` / `IF_2_048`
  (`sdrplay_api_tuner.h`) and a `rspDuoSampleFreq` for master/slave.
- **Limits.** Dual operation is **only possible in low-IF mode**. It allows up to 2 MHz of bandwidth per tuner
  (versus up to 10 MHz for a single tuner), with a flat low-IF passband of about 1.536 MHz. An early design
  document gives only 6 or 8 MHz ADC rates in low-IF ⚠ (pre-release document; check against the current API
  specification). The **Hi-Z port is not available** in diversity mode; only the two 50 Ω ports are.
- **Diversity in SDRuno (since v1.32).** Both tuners sit on the same frequency, and their gains can be linked
  (the default) or set separately. A controller window applies **automatic** amplitude and phase weights,
  which it recalculates continuously; SDRplay describes the method as MRC. Pressing "APPLY AUTO" switches to
  **manual** mode, where you drag a point to choose phase and amplitude, and either control can be locked.
  **For noise reduction SDRplay's own advice is to use manual mode**, setting the weights so the noise
  subtracts. One user found that auto mode *maximised* the noise, which is what an MRC that does not know the
  noise is correlated will do. That is a lesson for our design: §4.5 uses max-SINR weights rather than plain
  MRC. The combined output is a single IQ stream, so everything downstream still works. SDRconnect has the
  same mode with more amplitude and phase control (W1VLF video, 2024). SDRplay quotes "up to 3 dB" of SNR
  gain plus better behaviour on fading signals.
- **Antenna spacing.** SDRplay's demos suggest about a quarter wavelength between the antennas.

### 2.2 Perseus

The single Perseus is one channel and has no diversity of its own. Owners use an external phaser, such as the
DXE NCC-1, ahead of it. The **Perseus 22** has two synchronously sampled channels and lists diversity as a
feature. I found no documented way to synchronise two separate Perseus units.

### 2.3 KrakenSDR / KerberosSDR (coherent RTL arrays)

- The KrakenSDR has five RTL-SDR channels driven from **one shared 28.8 MHz 1 ppm TCXO**, plus an
  **internal noise source switched onto all inputs** for calibration.
- A shared clock makes the sample rates and LO frequencies match, but not the sample timing or the LO phase.
  Both are **re-randomised at every initialisation and every retune**. The DAQ software therefore re-measures
  the delay (by cross-correlation) and the phase against the noise source whenever coherence is lost.
- rtl_coherent (tejeez) documents the same method for home-modified dongles: crystals removed and one clock
  fed through small capacitors.
- This is the clearest public confirmation that **R82xx tuners do not keep phase across a retune**.

### 2.4 Analogue phasers (MFJ-1026, ANC-4, DXE NCC-1)

These take a main antenna and a "noise" antenna. They apply gain and a phase shift to the noise antenna (the
NCC-1 also adds delay) and sum the result into one receiver. This is two-element null steering done in RF.
It works because both antennas are sampled by the *same* receiver after the combiner, so no clock issue
arises. Our digital version is the same equation applied after two receivers. The equation is easy; the
clocks are the hard part.

### 2.5 KiwiSDR and GPS

Each KiwiSDR uses GPS to timestamp samples for TDoA direction finding. That is time alignment to about a
microsecond, not phase coherence at HF (7 MHz needs about 10 ns for 25°). I found no coherent-combining
feature for two Kiwis ⚠.

### 2.6 Car FM "phase diversity"

Mainstream FM car-radio tuners combine two or more antennas coherently inside one chip that shares a clock ⚠
(general industry knowledge, no primary source gathered here). VHF coherent diversity is ordinary when the
clock is shared.

---

## 3. What our radios allow

| Radio (driver) | Shares a clock? | Notes for diversity |
|---|---|---|
| **RTL-SDR Blog V4** (`rtlsdr`) | ⚠ No documented external-clock input on the V4. Clock-sharing mods (desolder one crystal, feed the other's clock) are documented for older dongles, and one report says a V4 stopped enumerating after the attempt. | 1 ppm TCXO, so it drifts little. HF arrives through the built-in upconverter, which I believe is clocked from the same 28.8 MHz ⚠. Capture is up to about 2.4 MS/s. The R828D relocks at a random phase on every retune (§2.3). **No sample counter** in the stream. |
| Older RTLs | Crystal-sharing mod only | Plain crystal: tens of ppm, drifting several ppm during warm-up ⚠. Poor for coherent work. |
| **SDRplay RSP1A / RSP1B** (`sdrplay`) | No reference input or output (the API exposes `extRefOutputEn` only on the RSP2 and the RSPduo). | 0.5 ppm TCXO ⚠. Reaches LF (MSF and DCF are receivable). Our driver caps the rate at 2 MS/s; the hardware does up to 10 MHz. **The callback carries `firstSampleNum` and `grChanged`**, which we already read (`noteSampleNum`, `noteGainChangePacket`). Those are exactly what drop detection and phase-step detection need. |
| **SDRplay RSPdx** | ⚠ Not in the API ext-ref fields. Treat as independent. | As for the RSP1A/1B. |
| **SDRplay RSP2 / RSPduo** | Reference output in the API (`sdrplay_api_Update_Rsp2_ExtRefControl` / `_RspDuo_ExtRefControl`). Hardware reference input ⚠. | The RSPduo is natively coherent (§2.1) and needs `Dual_Tuner` support in our driver. Untested here: nobody owns one. |
| **Airspy HF+ / Discovery / Dual Port** (`airspyhf`) | ⚠ No external reference input that I could find. The "Dual Port" has separate HF and VHF inputs on **one** tuner, not two channels. | Up to about 912 kHz of capture. Excellent HF dynamic range. No diversity mode of its own. No sample counter. |
| **Airspy R2 / Mini** (`airspy`) | The R2 has a clock input ⚠ (expansion header). The Mini does not ⚠. | VHF/UHF only without an upconverter. |
| **HackRF** (`hackrf`) | **Yes**: CLKIN and CLKOUT (10 MHz). Two HackRFs can share a clock. | 8-bit and weak on HF. Shared clock means no drift, though phase and delay are still random per retune. |
| **Icom** (`icom`) | No | Audio and scope only. No IQ path suitable for combining. |
| **RX888 MkII** (not a VibeServer driver) | Has an external 27 MHz clock connector ⚠ (documented for the original RX-888 by the TAPR clock kit; MkII to be confirmed). | Direct sampling, no tuner on HF. See §6. |

**Gain model, following the CLAUDE.md rule.** The three families do not share a gain model, so a pair's gain
UI must branch on each member's `radio.driver`, as `LocalHardwarePanel` does. There is no single "pair gain"
slider unless both members are the same driver. RSP1A + RSP1B is the cleanest pair: same driver, same gain
model, same LF reach, and sample counters.

### 3.1 (a) Coherent combining: what it needs and gives

- **Needs:** both streams at the same rate and aligned to a fraction of a sample at the channel rate, plus
  their relative phase known well enough. That means either a shared clock with a calibration step (Kraken,
  RSPduo, HackRF, a modified RTL, RX888 with a shared reference) or continuous tracking (§4).
- **Gives:**
  - **MRC:** the fading gain in §3.3 plus up to 3 dB of average SNR when the noise is independent.
  - **Null steering:** one null for two antennas, 20 to 40 dB on a single local source ⚠.
  - **Max-SINR combining:** both at once (§4.5).
  - **Direction finding:** two antennas only resolve a cone and are ambiguous; it needs three or more and a
    known geometry, so it is out of scope.

### 3.2 (b) Non-coherent combining: works with any two dongles

| Mode | Method | Needs |
|---|---|---|
| FM broadcast, NFM, airband AM | Per-branch channel SNR; **SNR-weighted sum of the demodulated audio** (the classic post-detection or ratio-squared combiner), or selection with a 10 to 20 ms crossfade. | Audio-level time alignment only (a fraction of a millisecond). |
| AM broadcast | SNR-weighted envelope sum or selection. Selective fading on AM (carrier fading separately from the sidebands) is *frequency*-selective, so coherent combining or SAM helps more. | As above. |
| SSB / CW | **Selection with crossfade**, not summing. Two SSB audios with a few Hz of relative LO offset beat and "flange" when summed. Summing becomes safe once the relative frequency is corrected (§4), and at that point we are close to the coherent path anyway. | Relative frequency within about 0.5 Hz to sum. |
| FT8 / FT4 / WSPR / NAVTEX / RTTY / SSTV / WEFAX | **Decode both and merge.** FT8 deduplicates naturally (keep the higher-SNR copy). NAVTEX and RTTY keep the better character stream per message. Images take the better line. | Nothing new. Each branch is an existing decoder. |

This catches most of the anti-fading benefit (selection is within about 1.5 dB of MRC at 1 % outage 📐) and
none of the noise cancelling.

### 3.3 What fading gains to expect

📐 These are standard Rayleigh results for two independent branches with equal mean SNR (Jakes; Goldsmith
ch. 7; Proakis). For a single branch, P(γ < x·γ̄) ≈ x.

| Outage | 1 branch needs | Selection (≈ x²) | MRC (≈ x²/2) | Gain (SC / MRC) |
|---|---|---|---|---|
| 1 % | −20 dB | −10 dB | −8.5 dB | **10 / 11.5 dB** |
| 0.1 % | −30 dB | −15 dB | −13.5 dB | **15 / 16.5 dB** |

Equal-gain combining is within about 0.5 to 1 dB of MRC.

**Correlation erodes the gain.** As a rough guide, an envelope correlation of 0.5 still keeps most of it, and
0.9 keeps little ⚠.

**How far apart is far enough on HF.** The classic HF space-diversity rules call for several wavelengths
between antennas ⚠. Some measurements show useful decorrelation at about one wavelength ⚠. At 7 MHz
(λ ≈ 43 m), front and back garden wires 20 to 40 m apart are about 0.5 to 1 λ apart. That is borderline for
space diversity alone. Wires that run in different directions add polarisation diversity, which decorrelates
well on skywave because Faraday rotation varies ⚠. At 14 to 28 MHz the same spacing counts for more
wavelengths. **Hence the phase-1 correlation meter: measure, then decide.**

**What each approach gives against HF selective fading** 📐 (Rayleigh, two independent branches, 1 % outage;
less as correlation rises):

| Approach | Reference? | Flat fading gain | Frequency-selective fading (e.g. AM carrier fading out of step with its sidebands) |
|---|---|---|---|
| Per-band learned antenna choice (§3.5) | No | None against fading. It gains the *mean* SNR difference between the wires, often 5 to 10 dB on the band where one wire is simply better ⚠ (antenna-dependent) | None |
| Post-detection selection (crossfade) | No | ≈ 10 dB | Partial: switches to the less-distorted branch as a whole |
| Post-detection SNR-weighted sum (FM, AM envelope) | No | ≈ 10.5 to 11 dB, close to equal-gain combining | Partial |
| Coherent MRC / max-SINR, one weight per channel | Phase tracking | ≈ 11.5 dB, plus up to 3 dB mean SNR | Partial |
| Coherent per-bin combining (phase 3) | Phase tracking | as above | **Good**: each sub-band takes its own better branch, so AM selective-fade distortion is repaired |

Post-detection combining captures nearly all of the flat-fade benefit. Coherence adds the 3 dB array gain,
the repair of in-band selective fading, and noise nulling.

### 3.4 Time alignment versus phase alignment ("will it echo?")

- **The echo comes from time misalignment.** USB transfer sizes and driver buffering make one stream lag the
  other by milliseconds, and the two crystals then slowly change that lag (1 ppm is 1 ms per 1000 s). Mixing
  audio that is 5 ms apart sounds like a comb filter or echo; at 30 ms or more it is a true echo.
- **Time alignment needs no reference.**
  1. Cross-correlate about 50 ms of the two IQ streams (any common signals in the band give a sharp peak).
  2. Delay the earlier stream by the integer plus fractional offset.
  3. Follow the drift with a fine resampler whose ratio comes from the slowly moving correlation peak (a
     delay-locked loop).
  4. Re-run step 1 after any detected drop.
- **The required accuracy is modest.** About 1/10 of the audio period at the highest audio frequency
  (≈ 30 µs) is plenty for summing audio; only selection crossfades are more forgiving.
- **Phase alignment** is needed **only for pre-detection (IQ) combining**: coherent MRC, nulling, and the
  noise canceller. There, a relative LO phase that drifts makes the two copies add and cancel in turn. The
  result is flanging, or the wanted signal vanishing, not an echo. Post-detection combining is immune to
  this, because AM envelope, FM discriminator output and selection all discard the carrier phase.
- **One exception: SSB audio.** Demodulated SSB still carries the relative LO *frequency* error as an
  audio-pitch offset of a few Hz. Summing two such audios beats, so SSB post-detection uses selection with a
  crossfade unless the relative frequency has been corrected (§4.2).

### 3.5 Per-band learned antenna selection (Stuart's front/back wires)

- **What it is.** For each frequency bucket (for example 25 to 50 kHz wide, finer within the amateur bands),
  keep a long-running record of each antenna's **noise floor and SNR on signals**, split by day and night.
  The bucket's "best wire" (or weight pair) is chosen from the record.
  - With a pair, both wires are measured all the time, so the record fills itself.
  - With **one radio plus the existing antenna switch** (`vibe_antswitch.h`, `ANTENNA-SWITCH-ROTATOR.md`), it
    fills during idle sweeps or whenever the user flips antennas. That gives the win without a second radio.
- **How it composes with the combiner.**
  - On a retune, the map supplies the **starting weights instantly** (no settling, no first-second dropout).
  - The live post-detection combiner then **refines** them from the current SNRs.
  - On 160/20 m the back wire starts favoured, but a fade on it still hands over to the front wire.
  - With one radio, the map simply picks the antenna; there is nothing to refine.
- **Cost.** A few kB per radio and a periodic average, so it is trivial even on a Pi 2.
- **UI.** "Antenna: auto (Back, learned for 20 m)". A small per-band table in the setup page shows the learned
  winners and lets the owner pin a band to one wire.

---

## 4. DSP design for two independent clocks

### 4.1 The physics, in one line per radio

For receiver k with fractional clock error εₖ (LO and ADC derived from one crystal, which holds for the RTL
(one 28.8 MHz shared by tuner and demodulator), the RSP (one 24 MHz) and probably the HF+ ⚠):

- the LO is off by **εₖ·f_RF** plus a fixed synthesiser rounding offset, set per tune;
- the sample clock is off by **εₖ·fs**;
- the LO phase at each relock is a **random constant**;
- the stream start time differs between the two radios (USB start skew is milliseconds and unknown);
- dropped transfers add sudden time jumps.

So the relative state between receivers A and B is:

- **Δε** — one number, independent of frequency, drifting slowly with temperature;
- **θ** — the relative phase, re-randomised by each hardware retune or gain step;
- **τ** — the time skew, a large unknown at start plus slow drift Δε·t plus sudden jumps on drops.

**Moving the channel inside the capture (a DDC retune) does not relock any PLL, so θ is preserved.** Only a
hardware LO move loses it. With a wide capture, clicking around the waterfall stays coherent.

Phase continuity on a hardware retune ⚠: confirmed lost for R82xx (KrakenSDR documentation). I found no
documentation that the RSP or the HF+ keep phase on a retune. **Assume all of them lose it.**

### 4.2 What a reference provides, split into two jobs (answering *"soon as you tune away it falls apart"*)

**Job 1: the relative clock rate Δε (ppm).**

- **Where it comes from.** It is measured from the rate of change of the relative phase of *any* carrier both
  radios hear: Δf = Δε·f_carrier, so Δε = (dθ/dt)/(2π f_carrier). It is the same at every frequency, so it is
  **carried across retunes** and stored per pair.
- **How it is refreshed.** Opportunistically: every time the phase loop in Job 2 is locked, its frequency
  term refines Δε. **The wanted signal maintains the ppm for free.**
- **Precision it must reach.** Δε only has to bring the residual frequency within the phase loop's pull-in
  range of about ±1 Hz. That is 0.14 ppm at 7 MHz, 0.007 ppm at 145 MHz, and 0.001 ppm at 1 GHz 📐. It also
  sets the resampling ratio for B. A residual of 0.01 ppm walks timing by 10 µs in 1000 s, which is
  negligible for a 10 kHz channel.
- **How long an estimate stays good** 📐 ⚠ (spec-sheet reasoning, to be measured):
  - **TCXO (RTL V4 1 ppm, RSP 0.5 ppm):** about ±0.5 to 1 ppm over the full temperature range, an average
    slope of a few hundredths of a ppm per °C, with local slopes up to about 0.1 ppm/°C. Indoors, with ±1 °C
    over ten minutes, the drift is about 0.02 to 0.1 ppm per ten minutes.
    - **HF:** good for tens of minutes to hours.
    - **2 m:** marginal over minutes. Refresh every minute, or let the FLL pull in (±0.1 ppm is ±15 Hz at
      145 MHz, a quick search).
  - **Plain-crystal RTL:** self-heating moves it several to tens of ppm in the first 10 to 20 minutes ⚠, so
    the estimate must be refreshed continuously during warm-up. After that it lasts minutes at HF.
  - **Both radios warm up independently**, so Δε itself drifts most in the first quarter-hour. The UI should
    say "warming up".
- **Initial acquisition with no prior Δε.** Search ±(ε_spec_A + ε_spec_B)·f, for example ±14 Hz at 7 MHz
  for two 1 ppm TCXOs. Do it with an FFT of the product of the two channels' outputs (x_A · conj x_B), or of
  a reference carrier. That takes a few hundred ms on a strong signal. The fixed rounding offset each
  synthesiser adds per tune is either computed from the driver's known step or swept up in the same search.

**Job 2: the relative phase θ, re-found at each new frequency.**

- **Method.** It comes from the **wanted signal itself**, by cross-correlation: r = E[x_A · conj x_B] over
  the channel bandwidth. This needs no carrier, so it works for SSB, data and noise alike. For AM and CW a
  carrier PLL is an equivalent, tighter alternative.
- **How fast** 📐. The variance of the phase estimate is σ² ≈ 1 / (2·T·C/N0_eff), where
  C/N0_eff = (C/N0_A · C/N0_B) / (C/N0_A + C/N0_B + B), roughly the weaker branch when one is weak. Time to
  reach 5° rms (T·C/N0 ≈ 66):

  | Signal (SNR in 2.4 kHz ≈ C/N0 − 33.8 dB) | C/N0_eff | Time to 5° |
  |---|---|---|
  | Good SSB, +10 dB | 44 dB-Hz | ~3 ms |
  | Readable, 0 dB | 34 dB-Hz | ~30 ms |
  | Weak, −10 dB | 24 dB-Hz | ~0.3 s |
  | FT8-weak, −20 dB | 14 dB-Hz | ~3 s |

- **The real limit is propagation, not estimation.** HF skywave changes the relative phase with a Doppler
  spread of about 0.1 to 1 Hz on quiet to moderate paths (ITU-R F.1487 / Watterson channel ⚠ for exact
  figures). The combiner must keep tracking at a few Hz anyway, so "re-acquire after a retune" and "track
  fading" are **one loop**.
- **When one branch is much weaker.** C/N0_eff collapses towards the weak branch, so θ is poorly known. In MRC
  that branch also carries almost no weight, so **the error lands exactly where it does not matter**. The
  combiner degrades gracefully into selection.
- **For noise nulling** the "signal" being tracked is the house noise. By definition it is strong in both
  wires, so phase lock is fast.
- **SSB speech pauses.** A voice-activity gate on channel power holds the weights during gaps.
- **Below about 14 dB-Hz** (nothing usable in the channel), **fall back to non-coherent** (§3.2) and say so.

### 4.3 Three ways to get the initial Δε, and the default

1. **A reference in the same capture** (Stuart's "known strong station", user-selectable). The server lists
   the **strongest steady carriers inside the current capture** and the owner picks one:
   - **MW: Radio 5 Live.** 909 kHz (Brookmans Park) and 693 kHz (Droitwich) are both likely strong ground
     wave at Moulton ⚠ (which one serves IO92 best: let the carrier scan decide).
     ⚠ **The BBC is closing 5 Live MW transmitters one by one. Redruth and Folkestone went in July 2026,
     Exeter and Brighton in September 2026, with the whole AM network to end by the end of 2027.** Never
     hard-code it: the list must be a live scan with a "this carrier has gone" state.
   - **HF:** the 41 m / 49 m / 22 m broadcasters by day and night. They sit within ±1 MHz of 40 m and 20 m,
     so they fit inside an RTL or RSP capture.
   - **LF:** MSF 60 kHz and DCF77 77.5 kHz, for RSPs and the HF+ only. The RTL V4 starts at about 500 kHz.
     MSF is keyed **off** for 100 to 500 ms each second, so it needs holdover across the gaps. DCF77 is
     continuous: amplitude drops to about 15 % and a ±15.6° spreading phase modulation is added.
   - **Any modulation of the reference is common to both antennas and cancels** in x_A · conj x_B. Only the
     SNR during gaps matters.
   - At MW the two wires are a small fraction of a wavelength apart (λ ≈ 330 m at 909 kHz), so the
     reference's *relative* phase is set by the antennas and feeders and is very steady, even when the field
     itself fades at night.
   - **Precision:** a strong MW carrier on a long wire is in the region of 100 dB-Hz ⚠, which gives
     sub-0.1° phase at 1 kHz update rates 📐. Δε comes out to 1e-10 in about a second.
   - **The catch** (Stuart's own *"that won't work"*): the reference must be **inside both captures**. MW
     plus 7 MHz needs a span above 6 MHz. That rules out RTL (2.4 MS/s) and HF+ (≤912 kHz). It is possible
     on an RSP in principle (10 MHz), but only at about 8 MS/s per radio. Two RSPs at that rate saturate the
     one 480 Mbit/s USB 2.0 path on a Pi 4 (its USB 2 ports share the VL805's hub) ⚠, and the RSP
     preselector band for 7 MHz attenuates MW ⚠ (the carrier is so strong it may survive). **So a fixed MW
     reference is a calibration source, used once to seed Δε, not a running one.** Job 1 then carries the
     ppm across the tune to HF.
2. **The wanted signal as its own reference** (§4.2, Job 2). It works on AM and CW carriers, on SSB (by
   correlation, no carrier needed), on data, and on house noise. It is weak only when the wanted signal is
   weak in both wires, which is also when diversity gains least. **This is the recommended default.**
3. **An injected reference** (KrakenSDR style). A cheap oscillator is coupled very weakly into both feeders
   near the radios. An Si5351 module on the Pi's I²C bus could be **retuned by the server** to sit just
   outside the wanted channel on any band. Alternatively, a switched noise source gives Kraken-style
   calibration of delay and phase.
   - It gives a reference that **never fades, is always in the capture, and has a fixed relative phase**. It
     is the right tool for deep nulls on weak signals and for VHF.
   - Its cost: a hardware accessory, a careful level (−100 dBm-ish at the feed, so it does not desensitise
     the receiver or radiate from the wires; ⚠ check the rules on unintentional radiation), and harmonics
     from the Si5351's square wave. It is a phase 3+ option.

**Default:** option 2 for phase, plus Job 1's carried ppm. Option 1 is the user-pickable way to seed or
refresh the ppm (the UI line "Diversity reference: Radio 5 Live 909 kHz — locked ●" applies while that
carrier is inside the capture). Option 3 is a "station accessory" for enthusiasts.

**No reference is needed at all** for non-coherent selection, SNR-weighted post-detection combining, or
decode-both-and-merge (§3.2). That is phase 1.

### 4.4 Alignment pipeline (per pair, in one process)

```
radio A IQ ─┬─ channel DDC (NCO + decimate → e.g. 48 kHz) ─────────────────┐
            └─ (optional) reference-carrier DDC (≈1 kHz) ──┐               │
radio B IQ ─ coarse delay buffer ─ Farrow resampler (ratio 1+Δε) ─┬─ channel DDC ┤
                                                    └─ ref DDC ┤               │
  sync: start-up cross-correlation → τ (integer + fractional) │               │
        x_A·conj x_B → FLL → PLL → Δε (carried), θ(t) ─────────┴─► e^{-jθ} on B │
                                                                               ▼
                                    combiner at channel rate (§4.5) → demodulator
```

- **Coarse τ at start, and after any detected drop.** Cross-correlate about 50 ms of wideband IQ from both
  radios. Every common signal in the band contributes, so the peak is sharp. This is a one-off FFT
  correlation and cheap.
- **Drop detection:**
  - RSP: from `firstSampleNum`.
  - RTL and HF+: from a **sudden jump in θ or in the correlation peak**, since they have no counter.
  - Either way, re-run the coarse τ.
- **Gain steps.** RSP `grChanged` (already read), and our own RTL and HF+ gain commands, mark a phase
  discontinuity. In coherent mode **gains are held fixed by default**. Any change forces a fast phase
  re-acquire. Never freeze the waterfall to hide it (memory: `feedback_never_freeze_waterfall_for_gain`).
- **Existing code to reuse:** `vibe_decfeed_resampler.h` and `vibe_dab_resample.h` (resamplers; check
  whether a Farrow fractional stage exists), plus the channel DDCs in `vibedsp/`. The PLL and FLL can follow
  the SAM/ECSS work already planned for V12 (`sam_ecss_cquam_plan`).

### 4.5 Weighting: one algorithm for both MRC and nulling

At the channel rate, form the 2×2 covariance over blocks of 10 to 50 ms: **R = E[x xᴴ]**, where
x = [x_A, x_B·e^{-jθ}].

- **Auto, plain MRC.** Use the principal eigenvector of R. This is blind maximum-SNR combining, correct when
  the noise in the two branches is independent and equal. Normalise each branch by its own noise floor first,
  because the radios differ.
- **Auto, max-SINR.** Estimate the **noise covariance Rₙ from bins beside the channel** (broadband house
  noise is there too, the signal is not). The weight is **w ∝ Rₙ⁻¹·h**, where h is the principal eigenvector
  of R − Rₙ. When the noise is uncorrelated this *is* MRC. When the noise is correlated (the house) it
  **automatically steers the null**.
  - This fixes the SDRuno failure where "auto maximised the noise".
  - Rₙ moves slowly, so update it at about 1 Hz.
- **Manual (the SDRuno equivalent).** The user sets the amplitude ratio and phase on a 2-D puck. Locking
  either axis is optional. It is useful when the wanted signal and the noise share a bin.
- **LMS canceller.** A "noise antenna" mode (the MFJ-1026 topology): x_A is main, x_B is noise, and B is
  adaptively scaled and subtracted. It is a single complex tap at the channel rate, or a few taps to handle
  a delay spread.
- **Cost:** a 2×2 complex eigen-solve per block, which is **trivial** on every platform.

### 4.6 CPU, USB and latency

**Combining is cheap: all of it runs at the channel rate.** The cost is running **two front ends**:

| Host | Two RTLs at 2.4 MS/s (u8, 4.8 MB/s each) | Two RSPs at 2 MS/s (16-bit IQ, ≈8 MB/s each) | Verdict ⚠ (none measured) |
|---|---|---|---|
| Pi 2 (minimum spec) | USB is shared with Ethernet (≈35 MB/s practical), so it fits on paper. CPU is the problem: memory notes say the Pi 2 is already heavy with one radio. | Tight | **Non-coherent narrow modes only, at reduced rates (1.024 MS/s RTL / 768 kS/s HF+ / 2 MS/s RSP with a small FFT). Never a listed feature.** |
| Pi 4 / Pi 5 | Fine | Fine at 2 MS/s. **Not** at 8 MS/s × 2 (USB 2 shared hub). | Coherent and non-coherent both realistic. |
| x86 / Mac | Fine | Fine | Everything, including wide RSP captures. |

- **Waterfall.** Phase 1 draws the waterfall from the primary member, with an A / B toggle. A combined
  *wideband* waterfall would mean per-FFT-bin combining (weights per bin from the same covariance method).
  That is affordable on a Pi 4 at 2 MS/s, but it is phase 3.
- **Latency.** The coarse-alignment buffer must cover the worst USB start skew plus jitter, about 50 to
  100 ms ⚠. That adds to the existing pipeline once. It does not affect the shared-dial contract.

### 4.7 "Use one SDR to cancel the noise on the other" (software MFJ-1026 / ANC-4)

- **Topology.**
  - Radio A is on the main wire. Radio B is on a noise-sense antenna: the wire nearer the house, or a short
    sense wire near the noise source.
  - Per channel, at the channel rate, an adaptive filter scales and rotates B so its copy of the local QRM
    matches A's, then subtracts it: y = x_A − w·x_B.
  - The adaptation is complex **NLMS** (normalised LMS, 1 tap; 2 to 4 taps if the delay difference between
    paths is significant compared with 1/channel bandwidth, which it rarely is at a 3 to 10 kHz channel) or a
    2×2 **RLS**.
  - The update is gated so it adapts on **noise-dominated periods** (SSB pauses, between CW elements) or on
    noise-only bins beside the channel. Otherwise it would learn to cancel the wanted signal.
- **It needs the coherent path.** The weight w is a phase and amplitude ratio between two receivers, so it
  only holds still if the relative LO phase holds still. That requires a shared clock (RSPduo, HackRF,
  RX888 with a shared reference) or the §4.2 tracking.
  - With tracking, the canceller can absorb the residual drift itself, because a rotating w is just a weight
    that keeps changing. NLMS follows a few Hz of rotation if its time constant is 10 to 50 ms 📐.
  - Without any tracking the residual rotation can reach ±14 Hz at 7 MHz. That is too fast to cancel deeply,
    because the misadjustment noise grows with the step size.
- **How fast it must adapt.** The noise path geometry is static, so the *true* weight changes only when:
  - the noise source changes character (a different supply loads up);
  - a gain step or retune happens (re-converge in under 100 ms);
  - after the sync loop, there is residual drift (sub-Hz).
  A 20 to 100 ms time constant is the sweet spot 📐.
- **How well it works** ⚠ (practitioner reports for analogue phasers, which have the same physics):

  | Noise | Expected |
  |---|---|
  | One dominant local source: switch-mode PSU, plasma TV, one LED driver, one inverter. Coherent hash, steady path. | **Good: 20 to 40 dB** on that source |
  | Broadband hash from one device | Good *per channel*. Cancellation holds across a few kHz easily. Across MHz it needs a per-bin weight, which is phase 3 for the waterfall. |
  | Several sources in different directions | Partial. Two antennas give one null; the remainder cancels poorly. |
  | Atmospheric / galactic / distant QRN arriving like the wanted signal (skywave, many directions) | **Little to none.** Cancelling it cancels the signal too. This is a local-noise tool, not a static remover. |
  | Wanted signal arriving from the same direction as the noise | The signal is nulled as well. A manual override is needed. |

- **CPU** 📐. A 1-tap complex NLMS is about 10 real operations per sample: at 48 kHz that is about
  0.5 Mop/s, **well under 1 % of one Pi 2 core per channel**. A 4-tap version or RLS is still under 2 %.
  The cost that matters is the second radio's front end (§4.6), not the canceller.
- **UI.**
  - **Noise canceller: Auto** (adaptive null) with a **manual phase/amplitude puck** override (SDRuno style;
    lock either axis).
  - A **"noise reduced by N dB"** meter, computed as the noise floor in the noise-only side bins before and
    after cancellation.
  - A "sense antenna: B" choice, and a hold button that freezes the weights once a good null is found.
  - It shows "needs lock" and greys out (not hidden; the pair can do it once locked) while the coherent sync
    is not locked.
  - On pairs whose hardware can never lock (no IQ path, e.g. Icom), it is **not shown at all**, following the
    CLAUDE.md "dead control" rule.

### 4.8 RSPduo dual-tuner constraints (for the native coherent path)

- Low-IF only (IF 1.620 / 2.048 MHz).
- At most about 2 MHz usable per tuner (output up to 2 MS/s).
- One shared ADC rate, with tuner A as master.
- Both tuners on one frequency for diversity (`Dual_Tuner`). Independent frequencies need master/slave.
- 50 Ω ports only.
- Our driver forces `RspDuoMode_Single_Tuner`, and a dual-tuner path needs a two-stream callback.
- ⚠ Whether `Dual_Tuner` forces identical rfHz, and the exact rate list, must be read from the API
  specification. OpenRSP implements the same ABI, so the enums are in `vendor/openrsp/include/`.

---

## 5. Stuart's front and back garden wires: recipes

| What you have | What to run | What you get |
|---|---|---|
| **RSP1A + RSP1B** (best pair) | Phase 1 non-coherent. Then phase 2 coherent with gains locked, ppm seeded on Radio 5 MW (or MSF, which RSPs reach) and carried to HF, phase from the wanted signal. | Fade removal now. Then MRC and house-noise nulling, with null depth to be measured. |
| **RTL V4 × 2** | Same. Seed the ppm from a 41 m broadcaster inside the 2.4 MHz capture. MSF and DCF are out of reach. | As above. TCXOs help. No sample counter, so drop handling relies on detecting jumps. |
| **RSP + HF+**, or any mixed pair | Non-coherent at once. Coherent is possible (both TCXO), but the gain controls stay per member. | As above. Mixed pairs are fine. |
| **Either pair + injected Si5351 tone** | Phase 3 option for deep, stable nulls. | A reference that never fades on any band. |
| **Two RX888 with a shared 27 MHz clock** | See §6. | True coherence across 0 to 30 MHz. |

**Practical notes.**

- **Put both wires on the same feeder type and length where possible.** It is not required, but it keeps
  the relative phase steady with frequency.
- **For nulling, the wire nearer the house is the "noise" antenna.**
- **Run the correlation meter for a day before judging space diversity.** If envelope correlation sits above
  about 0.8 on 40 m, the gain there will be small. Try 20 m, where the same spacing is twice as many
  wavelengths, or turn one wire to a different direction for polarisation.

---

## 6. Two RX888 MkII (the m9psy / UberSDR conversation)

**Why it fits Stuart's original idea best.**

- **No tuner PLL on HF.** The RX888 samples 0 to 30 MHz directly at about 64.8 MS/s; ka9q-radio typically
  runs it at 64.8 MS/s ⚠.
  - Tuning is a DDC, so **the phase between two RX888s never jumps on a retune**.
  - Their relative error is a single quantity: the relative sample-clock time Δt(t). The phase at any RF
    frequency f is 2π·f·Δt plus a fixed per-frequency antenna/feeder term.
- **MSF, DCF77, Radio 5 and every HF broadcaster are always inside both captures.** Stuart's "always-on
  reference" works literally: lock once, track forever, and combine coherently anywhere on HF.
- **Scaling caveat** 📐. A reference at f_ref predicts phase at f with its error multiplied by f/f_ref.
  - MSF to 7 MHz multiplies by about 117. At 80 dB-Hz with a 1 Hz loop, σ_ref ≈ 0.004°, which becomes about
    0.5° at 7 MHz. Fine, **provided the loop never cycle-slips**, and MSF's keyed gaps are a cycle-slip risk.
  - **DCF77 or a MW carrier is better: continuous, with a smaller scaling factor.** Better still, track
    several references and fit Δt(t).
  - The fixed per-frequency antenna term still has to be learned from the signal once per frequency, but it
    is slow.
- **Shared clock.** The original RX-888 has a small external 27 MHz reference connector (high impedance;
  must be AC-coupled; the board will not run without the clock once the TAPR kit is fitted). ⚠ Confirm the
  MkII's connector, level and revision differences from its schematic.
  - Feed both units from one 27 MHz TCXO or GPSDO-derived source. Then Δt is **constant**: one calibration
    (cross-correlation for the integer start skew, then the reference for the fractional part) and the pair
    is **coherent across all of HF with no tracking**.
  - The stock 27 MHz crystal oscillator plus Si5351 is reported as "far more drifty than other SDRs" ⚠. That
    is no problem with a shared clock, and only a tracking-loop design issue without one.
- **Host load** ⚠. 64.8 MS/s × 16 bit ≈ 130 MB/s per radio over USB 3, so two means about 260 MB/s. Use
  separate USB 3 controllers if possible. The front-end FFT for 64.8 MS/s is about a core of a modern x86 per
  radio, so two is a desktop or mini-PC job, not a Pi.
- **Two hosts (two UberSDR servers).** Shipping 130 MB/s between them is impractical. Each would instead send
  **narrow channel IQ plus a narrow reference-carrier stream**, both from the same sample clock, with sample
  timestamps (ka9q-radio's RTP timestamps are in sample units ⚠). A combiner then aligns them using the
  shared reference.
  - On one LAN with a ground-wave reference this is plausible.
  - Across the internet with skywave references it is not coherent. Non-coherent decode-both-and-merge
    works anywhere.
- **Where it would live.**
  - VibeServer has **no RX888 driver**: no FX3 firmware path, and a 64.8 MS/s DDC is beyond our Pi targets.
  - UberSDR is built on ka9q-radio (GPL-3.0). UberSDR's own licence ⚠: check its LICENSE; it is presumed
    GPL-family. By our principle, **we never copy that code**. Contributing a diversity feature **upstream
    to UberSDR** is fine; MadPsy accepts contributions (memory: `owrx_upstream_not_receptive`).
  - The cleanest split: UberSDR/radiod produces timestamped channel IQ from each RX888, and the combiner
    (our clean-room code in VibeServer, or theirs upstream) does §4.5. VibeServer already speaks UberSDR's
    protocol as a backend, so a VibeServer "pair" whose members are two UberSDR channel streams is
    conceivable. ⚠ It needs sample-accurate timestamps on those streams, which the public UberSDR client
    API may not expose.
- **Recommendation.** **For coherent HF diversity, two RX888 MkII with a shared 27 MHz clock is the best
  target technically.** It removes both of the hard problems (retune phase loss and drift). It is **not the
  best first step for VibeSDR**, because it needs a second RX888, an x86 host, and work that belongs mostly
  in UberSDR. Do phase 1 (non-coherent, every radio) in VibeServer first. Then offer the RX888 pair to MadPsy
  as an upstream collaboration, with our §4.5 combiner design as the contribution.

---

## 7. Phased plan

### Phase 0: measure (days, no UI)

Run a headless tool on a dev host, never a live server.

- **Setup:** two radios on the two wires.
- **It logs** per-branch SNR, envelope correlation, the relative frequency drift (Δε over time) and phase
  residuals after tracking.
- **Output:** the real numbers for §3.3 and §4.2 on Stuart's wires.

Tests stay silent.

### Phase 1: the diversity pair, non-coherent (about 2 to 3 weeks ⚠)

- **Model.**
  - A **pair** is a new radio entry in the server config with its own **stable id**, for example
    `div-<hash of both serials>`, served at `/r/<pairId>`.
  - While paired, the **members are hidden** from the radio list, the directory and command handling,
    following the Dedicated-role rule in `RADIO-ROLES.md` (enforced on the server; a command addressed to a
    member's old id is rejected or redirected).
  - **One process owns both members.** This is a new process kind for the supervisor; today it is one
    process per radio (`main.cpp` "this process runs exactly one of them").
  - Unpairing restores both radios with their old ids and settings.
- **Tuning.** One dial drives both members (same centre, same rate). Each member's gain control is drawn
  per `radio.driver`. Antenna-switch interplay is per member.
- **DSP.** Two channel DDCs, then **time alignment** (§3.4: cross-correlation, then a fine-resampler DLL; no
  reference), then per-branch SNR, then:
  - **per-band learned antenna map** (§3.5) supplying the starting weights. It also ships for single radios
    with an antenna switch;
  - selection with crossfade (SSB/CW);
  - SNR-weighted post-detection sums (AM/FM/NFM);
  - the background-decoding engine runs the decoder on **both** branches and merges (FT8 dedupe by message,
    keep the best SNR).
  - Waterfall from the primary member, with A / B toggle.
- **UI** (web client first, since it is the moat; app from hwinfo, no hardcoded settings):
  - **Pairing:** in the server setup page, "Make a diversity pair" lists two compatible free radios, plus a
    note on matching antennas.
  - **"Which antenna is winning" meter:** two thin bars (A and B SNR) and a share indicator showing who is
    selected or the weight share. It sits beside the S-meter using the standard font (not the display font).
  - **Combining-mode pill:** Off (A) / Off (B) / Select / Combine. In phase 2 it adds Null.
  - **Status line:** "Diversity: non-coherent (no lock)", "Diversity: coherent ● locked 3.2°", and
    "Diversity reference: Radio 5 Live 909 kHz — locked ●" when a picked reference is in the capture.
- **Shared-dial contract.** The pair's full state (mode, weights or meter, lock status) is sent by the
  server, and clients have no vote. Recovery and re-acquisition are server-side, never a client
  "user action".

### Phase 2: coherent with tracking (about 3 to 5 weeks ⚠, experimental flag)

- §4.2 to §4.5 for the RSP pair first, because it has sample counters and gain-change flags. Then the RTL V4
  pair.
- Auto max-SINR, the manual phase/amplitude puck, and the NLMS "use one SDR to cancel the other's noise"
  mode (§4.7) with its "noise reduced by N dB" meter.
- The reference picker: a live carrier scan inside the capture, the owner picks one, and the ppm is stored
  per pair.
- Gains locked by default in coherent mode.
- Clear automatic fallback to phase 1 behaviour.
- Tour and release-note copy updated (the CLAUDE.md grep list).

### Phase 3: options

- RSPduo `Dual_Tuner` native path (needs a Duo to test).
- Injected Si5351 reference on the Pi's I²C.
- Per-bin wideband combined waterfall.
- HackRF shared-clock pairs.
- The RX888 / UberSDR upstream collaboration (§6).

### Risks

- **Front-end load.** Two front ends double it, and the Pi 2 is already heavy.
- **Silent USB drops** on RTL and HF+ break alignment invisibly unless jump detection is solid.
- **Independent AGCs rotate phase.** This interacts with the RSP gain-step smoother (memory:
  `gainstep_found_minus_one_crash`, `rsp_gain_step_pop_kills_stereo`).
- **Wires too close.** If the front and back wires are highly correlated on 40/80 m, the fading gain may be
  small. Phase 0 exists to find this out before money and time are spent.
- **Null depth with independent TCXO synthesisers is unmeasured.** It may stall at 15 to 20 dB.
- **The reference station disappearing** (BBC MW closures by the end of 2027). The picker is a scan, never a
  constant.
- **UI weight.** A pair is a new concept in the radio list, the directory card, deep links and the tours.
- **RSPduo untested.** No one here owns one.

---

## 8. Sources

- SDRplay: RSPduo Introduction V3 (low-IF requirement, 1.536 MHz passband, 6/8 MHz rates):
  https://www.sdrplay.com/wp-content/uploads/2018/05/RSPduo-Introduction-V3.pdf
- RadCom RSPduo review: https://www.sdrplay.com/wp-content/uploads/2018/07/RadComRSPduoReview1.pdf
- RSPduo product page: https://sdrplay.com/rspduo/
- SDRplay, new diversity software for the RSPduo (SDRuno 1.32): https://www.sdrplay.com/2019/07/10/
- SDRplay, cancelling signals using SDRconnect and the RSPduo (W1VLF, 2024): https://www.sdrplay.com/2024/02/02/
- RTL-SDR.com, two-antenna diversity in SDRuno:
  https://www.rtl-sdr.com/two-antenna-diversity-for-the-sdrplay-rspduo-now-available-in-sdruno/
- RTL-SDR.com, RSPduo diversity cancelling local interference: https://www.rtl-sdr.com/?p=33668
- RTL-SDR.com, RSPduo diversity with a loop and a Miniwhip:
  https://www.rtl-sdr.com/sdrplay-rspduo-diversity-combing-a-magnetic-loop-and-miniwhip-antenna/
- HFUnderground thread: https://www.hfunderground.com/board/index.php/topic,55830.0.html
- KrakenSDR FAQ (shared clock, noise-source recalibration after a retune):
  https://www.crowdsupply.com/krakenrf/krakensdr/updates/frequently-asked-questions
- rtl_coherent (timing and phase re-randomised per initialisation; clock-sharing mod):
  https://github.com/tejeez/rtl_coherent
- RTL-SDR.com, clock distribution for phase-coherent RTL-SDRs: https://www.rtl-sdr.com/?p=66340
- RTL-SDR.com, coherent tag: https://www.rtl-sdr.com/tag/coherent/
- RigExpert TBR-AN-7 (external 28.8 MHz reference into an RTL):
  https://rigexpert.com/files/product_documentation/gpsdo/TBR-AN-7.pdf
- RTL-SDR Blog V4L product page (refers to the V4 user guide):
  https://www.rtl-sdr.com/product/rtl-sdr-blog-v4l-lite-r828s-rtl2832u-1ppm-tcxo-sma-software-defined-radio-dongle-only/
- Perseus diversity discussion (Fallows):
  https://play.fallows.ca/wp/radio/software-defined-radio/developing-my-new-sdr-diversity-recorder/ and
  https://play.fallows.ca/wp/radio/software-defined-radio/diversity-receivers-side-by-side
- Perseus 22: https://www.wimo.com/en/perseus22-sdr-receiver · Microtelecom: https://microtelecom.it
- TAPR RX888 clock kit (27 MHz reference connector, AC coupling):
  https://tapr.org/product/rx888-clock-kit-and-thermal-pad/
- RX-888 TCXO retrofit thread ("far more drifty"): https://forum.sdr-radio.com/viewtopic.php?p=6509
- AB9IL, RX-888 clocks and references: https://www.ab9il.net/software-defined-radio/RX-888ExternalClocksandFreqRefs.html
- AB9IL, WebSDRs on the RX-888 Mk2: https://www.ab9il.net/software-defined-radio/rx-888mk2-websdr.html
- UberSDR overview (built on KA9Q Radio): https://cq.sk/en/ubersdr-new-generation-of-internet-sdr-receivers-for-radio-amateurs/
  · MadPsy: https://github.com/madpsy · ka9q-radio (GPL-3.0): https://github.com/ka9q/ka9q-radio
- Si5351 datasheet (25/27 MHz crystal, Si5351C CLKIN): https://cdn.sparkfun.com/assets/3/e/a/9/a/Si5351-datasheet.pdf
- BBC 5 Live AM closures, 2026: https://radiotoday.co.uk/2026/06/bbc-starts-closing-bbc-radio-5-live-am-transmitters/
  and https://radiotoday.co.uk/2026/07/bbc-radio-5-live-to-close-more-am-transmitters/ ·
  https://en.wikipedia.org/wiki/BBC_Radio_5_Live
- Diversity-combining theory: W. C. Jakes, *Microwave Mobile Communications* (1974), ch. 5; A. Goldsmith,
  *Wireless Communications* (2005), ch. 7; J. Proakis, *Digital Communications*, the diversity chapter.
  Cooper Union thesis: https://ee.cooper.edu/~keene/assets/sam_thesis.pdf
- HF channel model: ITU-R F.1487 (Watterson model, Doppler spread figures) ⚠ figures quoted from memory.
- Repo: `vendor/openrsp/include/sdrplay_api_rspDuo.h`, `sdrplay_api_tuner.h`, `sdrplay_api.h`;
  `android/app/src/main/cpp/sdrplay_source.cpp` (`applyDuoChoice`, `noteSampleNum`, `grChanged`);
  `vibeserver/radios.h`; `vibeserver/main.cpp` (one process per radio, `/r/<id>` routing).

## Addendum (Stuart, 2026-10-24): FT8 as the reference on the amateur bands

The reference does not have to be constant or known — both receivers measure their DIFFERENCE on the same signal, so
any signal both hear well enough will do. FT8 is a good one: each transmission is a constant-envelope (8-FSK) 12.6 s
tone, the 20 m / 40 m FT8 segments usually carry dozens of stations at once, and the 2.4 s slot gaps are bridged by
carrying the ppm estimate (§4.2). Cross-correlating the two receivers' IQ of one FT8 signal gives the relative
frequency and phase directly, whatever is being sent. Constraint as for every reference: it must sit inside both
captures (on an RTL at 2.4 MS/s the whole of 20 m is one capture) and be strong enough on BOTH antennas.
