/**
 * Converter transform tests — the maths, the derived range, and the decorator's boundary.
 *
 * ★★ THE POINT OF TESTING THIS AT ALL is that neither of us owns an up-converter. The transform
 *   has exploitable fixed points, so everything except "is the RF genuinely where the profile
 *   claims" — which is not our code — can be pinned here rather than guessed at on air.
 *
 * Run: npx tsx scripts/test-converter.ts
 */
import {
  CONVERTER_PRESETS, NO_CONVERTER, type ConverterProfile,
  active, displayRange, fromPreset, isIdentity, outputRange, presetById, toDisplay, toHardware,
} from '../src/services/converter';
import { wrapWithConverter } from '../src/services/ConverterBackend';

let fails = 0;
const eq = (name: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) { fails++; console.log(`  ✗ ${name}\n      got  ${JSON.stringify(got)}\n      want ${JSON.stringify(want)}`); }
  else console.log(`  ✓ ${name}`);
};

const prof = (o: Partial<ConverterProfile>): ConverterProfile =>
  ({ ...NO_CONVERTER, engaged: true, id: 'test', ...o });

console.log('\nTransform — the four cases in the design table');
{
  // Ham It Up: hardware 128 MHz is display 3 MHz. Sebastian's actual setup.
  const up125 = prof({ offsetHz: -125_000_000 });
  eq('125 MHz up: hw 128 MHz → display 3 MHz', toDisplay(128_000_000, up125), 3_000_000);
  eq('125 MHz up: display 3 MHz → hw 128 MHz', toHardware(3_000_000, up125), 128_000_000);

  const up120 = prof({ offsetHz: -120_000_000 });
  eq('120 MHz up (SpyVerter): display 7.1 MHz → hw 127.1 MHz',
     toHardware(7_100_000, up120), 127_100_000);

  // Down-converter / LNB: the model carries it even though no preset ships yet.
  const lnb = prof({ offsetHz: 9_750_000_000 });
  eq('9750 LNB: hw 739.5 MHz → display 10489.5 MHz', toDisplay(739_500_000, lnb), 10_489_500_000);

  const high = prof({ offsetHz: 10_500_000_000, inverted: true });
  eq('high-side inverted: hw 11 MHz → display 10489 MHz', toDisplay(11_000_000, high), 10_489_000_000);
}

console.log('\nRound trip — every preset, to the Hz');
for (const p of CONVERTER_PRESETS) {
  const c = fromPreset(p, undefined);
  if (isIdentity(c)) continue;
  const f = 7_150_000;
  eq(`${p.label} round-trips ${f} Hz`, toDisplay(toHardware(f, { ...c, engaged: true }), { ...c, engaged: true }), f);
}

console.log('\nDerived display range — the user never enters one');
{
  const up125 = prof({ offsetHz: -125_000_000 });
  // An RTL-SDR's 24–1766 MHz behind a 125 MHz up-converter, declaring no passband: shift only.
  eq('125 MHz up shifts the dongle range down',
     displayRange([24_000_000, 1_766_000_000], up125), [-101_000_000, 1_641_000_000]);
  // ★ Inversion swaps the ends — a [hi, lo] pair handed to a clamp forbids everything.
  const inv = prof({ offsetHz: 10_500_000_000, inverted: true });
  const [lo, hi] = displayRange([24_000_000, 1_766_000_000], inv);
  eq('inverted range comes back ordered', lo < hi, true);
  eq('inverted range low end is offset − hw high', lo, 10_500_000_000 - 1_766_000_000);
}

console.log('\nThe passband clamp — what the converter actually converts');
{
  // ★★★ THE CASE THAT PROMPTED THIS. An up-converter carries an input low-pass filter, so while
  //   it is inline HF is all the receiver can hear — the aerial is behind it. Derived from the
  //   dongle alone the VFO would range to 1641 MHz, all of it silent. It must not go there.
  const hamItUp = fromPreset(presetById('up-125')!, undefined);
  const [lo, hi] = displayRange([24_000_000, 1_766_000_000], hamItUp);
  eq('range is clamped to what the converter passes', [lo, hi], [100_000, 30_000_000]);
  eq('the VFO cannot reach 125 MHz while engaged', hi < 125_000_000, true);
  eq('and cannot go negative', lo >= 0, true);

  // The dongle is the binding constraint at the bottom when it starts inside the passband.
  eq('the tighter of the two ends wins',
     displayRange([135_000_000, 1_766_000_000], hamItUp), [10_000_000, 30_000_000]);

  // ★★★ AN EMPTY INTERSECTION MUST NOT COME BACK INVERTED. A 200 MHz-and-up dongle behind a
  //   125 MHz up-converter overlaps nowhere; [75 MHz, 30 MHz] handed to a clamp forbids
  //   everything and freezes the VFO with nothing on screen to explain it.
  const [elo, ehi] = displayRange([200_000_000, 1_766_000_000], hamItUp);
  eq('an impossible pairing still returns an ordered range', elo < ehi, true);
  eq('and falls back to the shifted range rather than nothing', [elo, ehi], [75_000_000, 1_641_000_000]);

  // ★ A profile that declares no passband is taken at its word — old stored blobs keep working.
  const noPb = prof({ offsetHz: -125_000_000, inputLoHz: 0, inputHiHz: 0 });
  eq('no declared passband means shift only',
     displayRange([24_000_000, 1_766_000_000], noPb), [-101_000_000, 1_641_000_000]);

  // ★ Bypassed: the converter is out of the path, so the dongle's own full range is back —
  //   which is the whole point of the toggle (you unplugged it to get VHF).
  eq('bypassing restores the dongle range',
     displayRange([24_000_000, 1_766_000_000], active({ ...hamItUp, engaged: false })),
     [24_000_000, 1_766_000_000]);
}

console.log('\nEngage toggle — a bypassed converter is the identity');
{
  const c = prof({ offsetHz: -125_000_000, engaged: false });
  eq('bypassed resolves to identity', isIdentity(active(c)), true);
  eq('bypassed does not shift', toDisplay(128_000_000, active(c)), 128_000_000);
  eq('engaged does', toDisplay(128_000_000, active({ ...c, engaged: true })), 3_000_000);
  eq('id "none" is inert even if engaged is somehow true',
     isIdentity(active({ ...NO_CONVERTER, engaged: true })), true);
}

console.log('\nTrim is separate from the offset (and from PPM)');
{
  const c = prof({ offsetHz: -125_000_000, trimHz: 2_000 });
  eq('trim shifts the display with the LO', toDisplay(128_000_000, c), 3_002_000);
  eq('and round-trips', toHardware(toDisplay(128_000_000, c), c), 128_000_000);
}

console.log('\nThe decorator — one place in each direction');
{
  const calls: Record<string, unknown[]> = {};
  const rec = (k: string) => (...a: unknown[]) => { calls[k] = a; };
  const innerStatus = { frequency: 128_000_000, mode: 'usb', bandwidthLow: -3000, bandwidthHigh: 3000,
                        binCount: 1024, binBandwidth: 100, centerHz: 128_000_000, bwHz: 102_400,
                        trueCenterHz: 128_000_000 };
  const inner: any = {
    kind: 'ubersdr', uuid: 'u',
    caps: { profiles: false, serverSideZoom: true, smeter: 'header', freqRange: [24_000_000, 1_766_000_000],
            chat: false, serverNR: false, maxBandwidth: { default: 6000 } },
    connect: rec('connect'), destroy: rec('destroy'), tune: rec('tune'),
    syncFrequency: rec('sync'), setFollowMode: rec('follow'),
    panSpan: () => ({ loHz: 100_000_000, hiHz: 150_000_000, movable: true }),
    setMode: rec('mode'), setBandwidth: rec('bw'), zoom: rec('zoom'), pan: rec('pan'),
    resetView: rec('reset'), setRate: rec('rate'), pauseSpectrum: rec('pause'),
    resumeSpectrum: rec('resume'), getStatus: () => innerStatus, getView: () => innerStatus,
    // NOTE: deliberately no watchSpectrumUrl / forceResubscribe / getProfiles.
  };
  let p: ConverterProfile = prof({ offsetHz: -125_000_000 });
  const w = wrapWithConverter(inner, () => p);

  w.tune(3_000_000);
  eq('tune() sends hardware Hz down', calls.tune?.[0], 128_000_000);
  w.zoom(3_000_000, 100);
  eq('zoom() anchor is converted', calls.zoom?.[0], 128_000_000);
  eq('zoom() bin width is NOT converted (it is a width)', calls.zoom?.[1], 100);
  w.pan(3_000_000);
  eq('pan() is converted', calls.pan?.[0], 128_000_000);

  eq('getStatus() frequency comes up as display Hz', w.getStatus().frequency, 3_000_000);
  eq('getStatus() centerHz too', w.getStatus().centerHz, 3_000_000);
  eq('getStatus() trueCenterHz too (the watch crop reads it)', w.getStatus().trueCenterHz, 3_000_000);
  eq('caps.freqRange is the derived display range', w.caps.freqRange, [-101_000_000, 1_641_000_000]);
  eq('panSpan() is converted', w.panSpan(), { loHz: -25_000_000, hiHz: 25_000_000, movable: true });

  // ★★ The profile is read LIVE — the engage toggle must work on a running session.
  p = { ...p, engaged: false };
  eq('bypassing mid-session stops the shift', w.getStatus().frequency, 128_000_000);
  eq('and restores the raw caps range', w.caps.freqRange, [24_000_000, 1_766_000_000]);
  w.tune(128_000_000);
  eq('and tune() passes straight through', calls.tune?.[0], 128_000_000);

  // ★★★ An optional the inner backend lacks must stay ABSENT, or `if (c.watchSpectrumUrl)` lies.
  eq('watchSpectrumUrl absent when inner has none', !!w.watchSpectrumUrl, false);
  eq('forceResubscribe absent when inner has none', !!w.forceResubscribe, false);
  eq('getProfiles absent when inner has none', !!w.getProfiles, false);
  const w2 = wrapWithConverter({ ...inner, forceResubscribe: rec('resub') }, () => p);
  eq('forceResubscribe present when inner has one', !!w2.forceResubscribe, true);
}

console.log('\nThe manual editor derives the band the tuner must cover');
{
  // ★ Stuart's own example: "for a Ham it up it would be 100KHz - 30MHz mapped to 125MHz".
  const hamItUp = fromPreset(presetById('up-125')!, undefined);
  const out = outputRange(hamItUp)!;
  eq('a Ham It Up needs the radio to cover 125.1-155 MHz', out, [125_100_000, 155_000_000]);
  // ★★ AND THE BOTTOM IS 125.1, NOT 125 — the LO is what is printed on the box, not where the
  //    bottom of the band comes out. Asking the question the other way puts every entry 100 kHz
  //    out, which is the trap in "0.1-30 MHz mapped to 125".
  eq('the LO is not the output start', out[0] !== 125_000_000, true);
  eq('a profile with no declared range has no output range',
     outputRange({ ...hamItUp, inputLoHz: 0, inputHiHz: 0 }), null);
}

console.log('\nPresets are keyed on LO, and nothing inverting ships');
{
  eq('Ham It Up is 125 MHz and negative', presetById('up-125')?.offsetHz, -125_000_000);
  /* ★★★ NOTHING SHIPS INVERTED until the IQ stream is conjugated — a preset that needed it would
   *   mis-tune and break every decoder. This is the gate on that, not a description of today. */
  eq('no preset is inverted', CONVERTER_PRESETS.every(p => !p.inverted), true);
  eq('a down-converter preset exists now', !!presetById('dn-9750'), true);
  eq('QO-100 through a 9750 LNB lands at 739.5 MHz',
     toHardware(10_489_500_000, fromPreset(presetById('dn-9750')!, undefined)), 739_500_000);
  /* ★★★ AND THE LNB PRESETS DECLARE NO INPUT RANGE ON PURPOSE. A universal Ku LNB is specified
   *   as 10.7-11.7 GHz on its low band; QO-100 sits at 10489.5, BELOW that. Clamping to the
   *   datasheet would refuse the one frequency people buy the hardware for. */
  eq('the 9750 LNB does not clamp QO-100 out',
     displayRange([24_000_000, 1_766_000_000],
                  fromPreset(presetById('dn-9750')!, undefined))[0] <= 10_489_500_000, true);
  /* ★★★ THE uint32 RULE IS ABOUT WHAT REACHES THE RADIO, NOT ABOUT THE PRESET. An LNB's LO is
   *   9.75 GHz and its display frequencies are above 10 GHz — both far past 2^32 — and that is
   *   FINE, because neither ever crosses the native bridge: what crosses is the HARDWARE
   *   frequency, and this asserts that every preset leaves one inside a uint32 for a receiver
   *   covering up to 2 GHz. That is the property the bridge actually depends on. */
  for (const pr of CONVERTER_PRESETS) {
    if (pr.id === 'none' || pr.id === 'custom') continue;
    const c = fromPreset(pr, undefined);
    const [dlo, dhi] = displayRange([0, 2_000_000_000], { ...c, engaged: true });
    const hwLo = toHardware(dlo, c), hwHi = toHardware(dhi, c);
    eq(`${pr.label}: hardware stays inside a uint32`,
       Math.abs(hwLo) < 2 ** 32 && Math.abs(hwHi) < 2 ** 32, true);
  }
  eq('choosing None disengages', fromPreset(presetById('none')!, prof({ engaged: true })).engaged, false);
  eq('choosing a real preset engages it', fromPreset(presetById('up-125')!, undefined).engaged, true);
  eq('changing preset keeps the trim', fromPreset(presetById('up-120')!, prof({ trimHz: 500 })).trimHz, 500);
}

console.log(fails ? `\n${fails} FAILED\n` : '\nAll converter tests passed\n');
process.exit(fails ? 1 : 0);
