// test_decoder_bands.ts — V12 the decoder panel by band (src/utils/decoderBands.ts, Stuart 2026-10-28): the tab a dial
// opens on, the placement table, and which keys are DIMMED — Stuart's own example (7.074 MHz: FT8 / CW / PSK / RTTY
// bright, HFDL / NAVTEX dim) and the cases around it. Also: every id placed has a mode glyph (utils/modeIcons), and the
// time-signal table agrees with DecoderClient's timeStationFor (the decoder's own reading of the same stations).
import { DECODER_PLACES, RADIO_BANDS, WHOLE_RADIO_IDS, bandsOfDecoder, decoderDim, decoderGroupIcon, decoderNear, openingBand, radioBandOf,
         TIME_SIGNAL_HZ } from '../src/utils/decoderBands';
import { modeIconFor, MODE_ICONS, modeIconSvg } from '../src/utils/modeIcons';
import { timeStationFor } from '../src/services/DecoderClient';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

console.log('▶ the band a dial opens on');
ok(radioBandOf(7.074e6) === 'hf', '7.074 MHz → HF');
ok(radioBandOf(198e3) === 'hf', '198 kHz (LF) → HF');
ok(radioBandOf(29.999e6) === 'hf' && radioBandOf(30e6) === 'vhf', '30 MHz is the HF / VHF edge');
ok(radioBandOf(162e6) === 'vhf', '162 MHz → VHF');
ok(radioBandOf(299.9e6) === 'vhf' && radioBandOf(300e6) === 'uhf', '300 MHz is the VHF / UHF edge');
ok(radioBandOf(1090e6) === 'uhf', '1090 MHz → UHF');
ok(radioBandOf(NaN) === 'hf', 'an unknown dial → HF');
ok(openingBand(145e6, b => b === 'hf') === 'hf', 'a band with nothing in it falls back to one that has (HF-only server at 145 MHz)');
ok(openingBand(145e6, () => true) === 'vhf', '…and opens on the dial\'s own band when it has keys');

console.log('▶ placement');
const all = RADIO_BANDS.flatMap(b => DECODER_PLACES[b.id].flatMap(g => g.ids));
ok(bandsOfDecoder('dsc').join() === 'hf,vhf', 'DSC is in HF and VHF (MF/HF and channel 70)');
ok(bandsOfDecoder('hfdl').join() === 'hf' && bandsOfDecoder('time').join() === 'hf', 'HFDL and TIME HF');
console.log('▶ TUNES THE RADIO ITSELF — its own section, in no band tab, never dimmed (Stuart: "always lit up as they tune the SDR themselves")');
ok(WHOLE_RADIO_IDS.join() === 'adsb,ais,dab,ism,ndb,spotsband', 'ADS-B, AIS, DAB, ISM, NDB, DIGITAL SPOTS: BAND, in that order');
for (const id of WHOLE_RADIO_IDS) ok(bandsOfDecoder(id).length === 0, `${id} is in no band tab`);
for (const id of WHOLE_RADIO_IDS) for (const hz of [7.074e6, 145e6, 1090e6, 198e3, 130e6]) for (const b of RADIO_BANDS)
  ok(!decoderDim(id, hz, b.id), `${id} never dimmed (${hz / 1e6} MHz, ${b.label} tab)`);
for (const id of WHOLE_RADIO_IDS) ok(!!modeIconFor(id), `${id} has a glyph`);
ok(DECODER_PLACES.uhf.length === 0, 'UHF holds no band decoder now (ADS-B and ISM tune the radio) — no UHF tab is drawn');
ok(bandsOfDecoder('spots').join() === 'hf,vhf', 'DIGITAL SPOTS HF and VHF (6 m / 2 m)');
for (const b of RADIO_BANDS) {
  const ids = DECODER_PLACES[b.id].flatMap(g => g.ids);
  ok(new Set(ids).size === ids.length, `${b.label}: no decoder twice in one band`);
}
for (const id of new Set(all)) ok(!!modeIconFor(id), `${id} has a glyph`);
for (const b of RADIO_BANDS) for (const g of DECODER_PLACES[b.id]) ok(!!modeIconFor(decoderGroupIcon(g.group, b.id)), `${b.label} ${g.group} heading has a glyph`);
ok(modeIconFor('adsb') === 'aircraft' && modeIconFor('ais') === 'ship', 'ADS-B is an aircraft, AIS a ship (Stuart: "I thought AIS was aircraft")');
ok(modeIconFor('dsc') === 'ship' && modeIconFor('hfdl') === 'aircraft' && modeIconFor('navtex') === 'ship', 'DSC / NAVTEX ships, HFDL an aircraft');
ok(modeIconFor('rtty') !== modeIconFor('wefax'), 'RTTY (teleprinter) and WEFAX (fax) are different glyphs');
ok(modeIconFor('usb') === null && modeIconFor('scanner') === null, 'demodulators and tools have none');
for (const n of Object.keys(MODE_ICONS)) ok(/^<svg[^>]+viewBox="0 0 24 24"/.test(modeIconSvg(n as any)) && !/NaN|undefined/.test(modeIconSvg(n as any)), `${n} renders a clean <svg>`);

console.log('▶ dimming — Stuart\'s example, 7.074 MHz');
for (const id of ['spots', 'cw', 'psk', 'rtty', 'jtty', 'sstv']) ok(decoderNear(id, 7.074e6), `${id} bright`);
for (const id of ['hfdl', 'navtex', 'wefax', 'dsc', 'time']) ok(!decoderNear(id, 7.074e6), `${id} dim`);
console.log('▶ dimming — each where it lives');
ok(decoderNear('navtex', 518e3) && decoderNear('navtex', 490e3) && decoderNear('navtex', 4209.5e3), 'NAVTEX on 518 / 490 / 4209.5 kHz');
ok(decoderNear('dsc', 2187.5e3 - 1700) && decoderNear('dsc', 156.525e6), 'DSC on its USB dial for 2187.5 kHz and on channel 70');
ok(decoderNear('wefax', 4610e3 - 1900) && decoderNear('wefax', 8040e3), 'WEFAX on Northwood 4610 (USB dial) and 8040');
ok(decoderNear('hfdl', 8.9e6) && decoderNear('hfdl', 11.3e6), 'HFDL in the 8.9 and 11.3 MHz aero bands');
ok(decoderNear('time', 60e3) && decoderNear('time', 77.5e3) && decoderNear('time', 10e6), 'TIME on MSF/WWVB, DCF77, WWV');
ok(decoderNear('aprs', 144.8e6) && decoderNear('aprs', 10.1476e6), 'APRS on 144.800 and HF 10.1476');
ok(decoderNear('rds', 98.5e6) && !decoderNear('rds', 145e6), 'ADV RDS on FM broadcast');
ok(!decoderNear('cw', 9.6e6) && !decoderNear('rtty', 9.6e6), 'amateur modes dim on 31 m broadcast');
ok(decoderNear('cw', 144.05e6) && decoderNear('sstv', 145.8e6), 'CW and SSTV bright on 2 m (VHF tab)');
console.log('▶ never dimmed: tools and an unknown dial (dim is a HINT — every key stays tappable)');
ok(decoderNear('scanner', 7e6) && decoderNear('gallery', 7e6), 'tools are never dimmed');
ok(decoderNear('hfdl', NaN) && decoderNear('hfdl', 0), 'an unknown dial dims nothing');

console.log('▶ a tab that is not the dial\'s band is dimmed whole (a hint — the keys still work)');
ok(!decoderDim('cw', 7.074e6, 'hf') && decoderDim('cw', 7.074e6, 'vhf'), 'CW: bright on HF at 7.074, dim on the VHF tab');
ok(!decoderDim('cw', 144.05e6, 'vhf') && decoderDim('cw', 144.05e6, 'hf'), 'CW: bright on VHF at 144.05, dim on the HF tab');
ok(decoderDim('navtex', 7.074e6, 'hf'), 'NAVTEX dim at 7.074 on its own tab');
ok(!decoderDim('hfdl', NaN, 'vhf'), 'an unknown dial dims nothing');
console.log('▶ the time-signal table agrees with DecoderClient.timeStationFor');
for (const hz of TIME_SIGNAL_HZ) ok(timeStationFor(hz) !== null, `${hz} Hz is a station to the decoder`);

console.log(fail ? `\n✗ ${fail} failed` : '\n✓ decoder bands');
process.exit(fail ? 1 : 0);
