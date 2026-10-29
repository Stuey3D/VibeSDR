// test_dsc_channels.ts — V12 DSC channels, distress AND routine (src/utils/dscChannels.ts, Stuart 2026-10-29: he typed
// 4208.5, heard nothing — "most people tune DSC wrong"). The tables against ITU RR Appendix 15 / ITU-R M.541 as Stuart
// listed them, where each is tuned, the band a dial is in, the box's keys, the decoder's audio centre for a dial on ANY
// channel, and the one-tap "TUNE 4206.8 USB" offer — in the app's module and the web's alike.
// Run: npx tsx scripts/test_dsc_channels.ts
import * as S from '../src/utils/dscChannels';
import * as AD from '../src/services/dsc';
import * as WD from '../web/client/src/dsc';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const eq = (what: string, a: unknown, b: unknown) => ok(JSON.stringify(a) === JSON.stringify(b), `${what}${JSON.stringify(a) === JSON.stringify(b) ? '' : ` — ${JSON.stringify(a)} ≠ ${JSON.stringify(b)}`}`);

console.log('▶ the tables');
eq('distress (RR Appendix 15)', S.DSC_DISTRESS.map((c) => c.label), ['2187.5', '4207.5', '6312', '8414.5', '12577', '16804.5', 'Ch 70']);
eq('routine (ITU-R M.541, Stuart\'s list)', S.DSC_ROUTINE.map((c) => c.hz / 1000), [2177, 2189.5, 4208, 4208.5, 4209, 6312.5, 6313, 6313.5,
  8415, 8415.5, 8416, 12577.5, 12578, 12578.5, 16805, 16805.5, 16806, 18898.5, 18899, 18899.5, 22374.5, 22375, 22375.5,
  25208.5, 25209, 25209.5]);
ok(S.DSC_ALL.length === 33 && S.DSC_ALL.every((c, i, a) => !i || a[i - 1].hz < c.hz), 'all 33, lowest first, no two the same');
ok(S.DSC_ALL.every((c) => S.DSC_BANDS.includes(c.band)), 'every channel names one of the bands');
eq('the 4 MHz band', S.dscBandChannels('4').map((c) => `${c.label}${c.kind === 'distress' ? '*' : ''}`), ['4207.5*', '4208', '4208.5', '4209']);
eq('the 2 MHz band', S.dscBandChannels('2').map((c) => c.label), ['2177', '2187.5', '2189.5']);
eq('18, 22, 25 MHz have routine channels only', ['18', '22', '25'].map((b) => S.dscBandChannels(b as S.DscBand).every((c) => c.kind === 'routine')), [true, true, true]);
eq('the app keeps DSC_CHANNELS = the distress set', AD.DSC_CHANNELS, S.DSC_DISTRESS.map(({ hz, label, vhf }) => ({ hz, label, vhf })));
eq('the web too', WD.DSC_CHANNELS, AD.DSC_CHANNELS);

console.log('▶ where each is tuned — the ASSIGNED frequency is not the dial');
eq('4208.5 → 4206.8 USB', S.dscTuneFor(S.DSC_ROUTINE[3]), { hz: 4206800, mode: 'usb' });
eq('2187.5 → 2185.8 USB', S.dscTuneFor(S.DSC_DISTRESS[0]), { hz: 2185800, mode: 'usb' });
eq('Ch 70 → 156.525 NFM', S.dscTuneFor(S.DSC_DISTRESS[6]), { hz: 156525000, mode: 'nfm' });
for (const c of S.DSC_ALL) eq(`app = web = shared for ${c.label}`, AD.dscTuneFor(c), WD.dscTuneFor(c));
ok(S.dscKeyTitle(S.DSC_ROUTINE[3]) === '4208.5 kHz (routine) — tuned 4206.8 kHz USB', 'the key says both numbers');

console.log('▶ the band a dial is in');
eq('4206.8 (USB for 4208.5)', S.dscBandOf(4206800), '4');
eq('4208.5 typed straight in', S.dscBandOf(4208500), '4');
eq('8412.8', S.dscBandOf(8412800), '8');
eq('2185.8', S.dscBandOf(2185800), '2');
eq('156.525 MHz', S.dscBandOf(156525000), 'vhf');
eq('7.074 MHz — no DSC band', S.dscBandOf(7074000), null);
eq('an unknown dial', S.dscBandOf(NaN), null);

console.log('▶ the box\'s keys: the band\'s channels, else the distress set — only what the radio reaches');
const all = () => true;
eq('on 4206.8: the 4 MHz band', S.dscChannelsForDial(4206800, all).channels.map((c) => c.label), ['4207.5', '4208', '4208.5', '4209']);
eq('on 7.074: the distress set', S.dscChannelsForDial(7074000, all).channels.map((c) => c.label), ['2187.5', '4207.5', '6312', '8414.5', '12577', '16804.5', 'Ch 70']);
eq('an HF-only radio off band: no Ch 70', S.dscChannelsForDial(7074000, (hz) => hz < 30e6).channels.map((c) => c.label).includes('Ch 70'), false);
eq('a radio locked to 8.0–8.8 MHz on 4 MHz: its band is out of reach → what it CAN reach', S.dscChannelsForDial(4206800, (hz) => hz >= 8e6 && hz <= 8.8e6).channels.map((c) => c.label), ['8414.5']);
eq('the app\'s Mode panel options on 4206.8', (() => { const p = AD.dscPanelChannels(4206800, all); return [p.distress.length, p.band, p.routine.map((c) => c.label)]; })(), [7, '4', ['4208', '4208.5', '4209']]);
eq('…off band: no routine row', AD.dscPanelChannels(7074000, all).routine.length, 0);
ok(S.dscChannelOn(S.DSC_ROUTINE[3], 4206800, 'usb') && !S.dscChannelOn(S.DSC_ROUTINE[3], 4206800, 'lsb') && !S.dscChannelOn(S.DSC_ROUTINE[2], 4206800, 'usb'),
   'the lit key: 4208.5 on 4206.8 USB — not in LSB, and not its neighbour 4208');

console.log('▶ the decoder\'s audio centre recognises a dial near ANY channel');
eq('4206.8 USB (4208.5 routine) → 1700', AD.dscParams(4206800, 'usb'), { band: 'hf', center_frequency: 1700 });
eq('4206.3 USB (4208 routine) → 1700', AD.dscParams(4206300, 'usb'), { band: 'hf', center_frequency: 1700 });
eq('2186.0 USB (2187.5, a little off) → 1500', AD.dscParams(2186000, 'usb'), { band: 'hf', center_frequency: 1500 });
eq('18897.0 USB (18898.5 — routine only) → 1500', AD.dscParams(18897000, 'usb'), { band: 'hf', center_frequency: 1500 });
eq('LSB 4210.2 (4208.5 above the dial) → 1700', AD.dscParams(4210200, 'lsb'), { band: 'hf', center_frequency: 1700 });
eq('7.074 USB (nowhere near) → 1700', AD.dscParams(7074000, 'usb'), { band: 'hf', center_frequency: 1700 });
eq('channel 70 NFM → vhf', AD.dscParams(156525000, 'nfm'), { band: 'vhf' });
for (const [hz, m] of [[4206800, 'usb'], [2186000, 'usb'], [18897000, 'usb'], [4210200, 'lsb'], [156525000, 'nfm'], [8412800, 'usb'], [7074000, 'usb']] as const) {
  eq(`app = web for ${hz} ${m}`, AD.dscParams(hz, m), WD.dscParams(hz, m));
}

console.log('▶ "TUNE 4206.8 USB for this channel" — only when the dial is wrong');
eq('4208.5 typed in USB (Stuart\'s)', S.dscTuneOffer(4208500, 'usb')?.text, 'TUNE 4206.8 USB for 4208.5 kHz');
eq('…and it tunes there', (() => { const o = S.dscTuneOffer(4208500, 'usb'); return o && [o.hz, o.mode]; })(), [4206800, 'usb']);
eq('8414.5 typed in AM', S.dscTuneOffer(8414500, 'am')?.text, 'TUNE 8412.8 USB for 8414.5 kHz');
eq('2187.5 + 120 Hz', S.dscTuneOffer(2187620, 'usb')?.text, 'TUNE 2185.8 USB for 2187.5 kHz');
eq('the right dial in LSB', S.dscTuneOffer(4206800, 'lsb')?.text, 'TUNE 4206.8 USB for 4208.5 kHz');
eq('channel 70 in AM', S.dscTuneOffer(156525000, 'am')?.text, 'TUNE 156.525 NFM for channel 70');
eq('4206.8 USB — already right: no offer', S.dscTuneOffer(4206800, 'usb'), null);
eq('4207.3 USB is 4209\'s dial (and 200 Hz from 4207.5): no offer', S.dscTuneOffer(4207300, 'usb'), null);
eq('8414.3 USB is 8416\'s dial: no offer', S.dscTuneOffer(8414300, 'usb'), null);
eq('channel 70 in NFM: no offer', S.dscTuneOffer(156525000, 'nfm'), null);
eq('7.074 USB: no offer', S.dscTuneOffer(7074000, 'usb'), null);
eq('the web reads the same offer', WD.dscTuneOffer(4208500, 'usb')?.text, AD.dscTuneOffer(4208500, 'usb')?.text);

console.log('▶ the hint says where the dial goes');
ok(/ASSIGNED frequency, not the dial/.test(AD.DSC_HINT) && /4206\.8 for 4208\.5/.test(AD.DSC_HINT), 'the hint explains the 1.7 kHz with Stuart\'s 4208.5');

console.log(`test_dsc_channels: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
