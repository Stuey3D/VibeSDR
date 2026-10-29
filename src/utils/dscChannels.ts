/**
 * ★★ THE DSC CHANNELS — distress AND routine — and WHERE TO TUNE FOR EACH (Stuart, 2026-10-29: he typed 4208.5, heard
 * nothing, and "most people tune DSC wrong"). ONE pure module, read by the app (src/services/dsc.ts) and the web client
 * (web/client/src/dsc.ts); scripts/test_dsc_channels.ts holds it to the tables below.
 *
 *  ★★★ A LISTED DSC FREQUENCY IS THE ASSIGNED (CARRIER) FREQUENCY, NOT THE DIAL. DSC is 170 Hz FSK centred on it, so a
 *      receiver in USB puts its dial 1.7 kHz BELOW (4206.8 USB for 4208.5) and the tones land at 1615 / 1785 Hz audio.
 *      Typed straight in (4208.5 USB) the tones are at 0 Hz — under the passband, silence. Channel 70 (156.525 MHz) is
 *      NFM on the channel itself.
 *  ★ Distress and safety (ITU Radio Regulations Appendix 15): 2187.5, 4207.5, 6312, 8414.5, 12577, 16804.5 kHz + ch 70.
 *  ★ Routine ship-to-shore / ship-to-ship (ITU-R M.541, the frequencies Stuart listed): 2177, 2189.5; 4208, 4208.5, 4209;
 *    6312.5, 6313, 6313.5; 8415, 8415.5, 8416; 12577.5, 12578, 12578.5; 16805, 16805.5, 16806; 18898.5, 18899, 18899.5;
 *    22374.5, 22375, 22375.5; 25208.5, 25209, 25209.5 kHz.
 *  ★ AGENTS.md (no control that works on one radio only): nothing here knows a radio — the callers draw only the channels
 *    THIS radio can be tuned to (dscReachable), on every driver alike.
 */

export type DscKind = 'distress' | 'routine';
/** A DSC band: the MF band ('2'), an HF band by its MHz ('4' … '25'), or VHF channel 70. */
export type DscBand = '2' | '4' | '6' | '8' | '12' | '16' | '18' | '22' | '25' | 'vhf';

export interface DscChannel { hz: number; label: string; vhf: boolean; kind: DscKind; band: DscBand }

const hf = (khz: number, kind: DscKind, band: DscBand): DscChannel =>
  ({ hz: Math.round(khz * 1000), label: String(khz), vhf: false, kind, band });

/** Distress and safety (RR Appendix 15) — the set the panel always drew. */
export const DSC_DISTRESS: readonly DscChannel[] = [
  hf(2187.5, 'distress', '2'), hf(4207.5, 'distress', '4'), hf(6312, 'distress', '6'), hf(8414.5, 'distress', '8'),
  hf(12577, 'distress', '12'), hf(16804.5, 'distress', '16'),
  { hz: 156525000, label: 'Ch 70', vhf: true, kind: 'distress', band: 'vhf' },
];

/** Routine calling (ITU-R M.541). */
export const DSC_ROUTINE: readonly DscChannel[] = [
  hf(2177, 'routine', '2'), hf(2189.5, 'routine', '2'),
  hf(4208, 'routine', '4'), hf(4208.5, 'routine', '4'), hf(4209, 'routine', '4'),
  hf(6312.5, 'routine', '6'), hf(6313, 'routine', '6'), hf(6313.5, 'routine', '6'),
  hf(8415, 'routine', '8'), hf(8415.5, 'routine', '8'), hf(8416, 'routine', '8'),
  hf(12577.5, 'routine', '12'), hf(12578, 'routine', '12'), hf(12578.5, 'routine', '12'),
  hf(16805, 'routine', '16'), hf(16805.5, 'routine', '16'), hf(16806, 'routine', '16'),
  hf(18898.5, 'routine', '18'), hf(18899, 'routine', '18'), hf(18899.5, 'routine', '18'),
  hf(22374.5, 'routine', '22'), hf(22375, 'routine', '22'), hf(22375.5, 'routine', '22'),
  hf(25208.5, 'routine', '25'), hf(25209, 'routine', '25'), hf(25209.5, 'routine', '25'),
];

/** Every DSC channel, lowest first. */
export const DSC_ALL: readonly DscChannel[] = [...DSC_DISTRESS, ...DSC_ROUTINE].sort((a, b) => a.hz - b.hz);

export const DSC_BANDS: readonly DscBand[] = ['2', '4', '6', '8', '12', '16', '18', '22', '25', 'vhf'];
/** "MF 2 MHz" · "4 MHz" · "VHF CH 70" — a band's name in a heading. */
export function dscBandLabel(b: DscBand): string {
  return b === 'vhf' ? 'VHF' : b === '2' ? 'MF 2 MHz' : `${b} MHz`;
}

/** The audio tone a USB receiver puts a DSC channel on: the dial 1.7 kHz below. */
export const DSC_TONE = 1700;

/** Where to tune for a DSC channel: USB 1.7 kHz below, or channel 70 in NFM. */
export function dscTuneFor(ch: { hz: number; vhf: boolean }): { hz: number; mode: 'usb' | 'nfm' } {
  return ch.vhf ? { hz: ch.hz, mode: 'nfm' } : { hz: ch.hz - DSC_TONE, mode: 'usb' };
}

/** "4206.8" — a tuned dial in kHz (one decimal), or "156.525" MHz for VHF. */
export function dscDialText(hz: number): string {
  return hz >= 30e6 ? (hz / 1e6).toFixed(3) : (hz / 1e3).toFixed(1);
}

/** A channel key's tooltip / accessibility line. */
export function dscKeyTitle(ch: DscChannel): string {
  if (ch.vhf) return 'VHF channel 70, 156.525 MHz — tuned in NFM';
  return `${ch.label} kHz (${ch.kind}) — tuned ${dscDialText(dscTuneFor(ch).hz)} kHz USB`;
}

/** The band a DISPLAYED dial is in — within 20 kHz of one of its channels (or the marine VHF band), else null. */
export function dscBandOf(dialHz: number): DscBand | null {
  if (!(dialHz > 0)) return null;
  if (dialHz >= 156e6 && dialHz <= 162.1e6) return 'vhf';
  let best: DscBand | null = null;
  let bestD = 20000;
  for (const c of DSC_ALL) {
    if (c.vhf) continue;
    const d = Math.abs(dialHz - c.hz);
    if (d <= bestD) { bestD = d; best = c.band; }
  }
  return best;
}

/** A band's channels, lowest first (distress among them). */
export function dscBandChannels(b: DscBand): DscChannel[] {
  return DSC_ALL.filter((c) => c.band === b);
}

/** The channels this radio can reach (where each would be TUNED) — the rest are not drawn. */
export function dscReachable(chs: readonly DscChannel[], reach: (hz: number) => boolean): DscChannel[] {
  return chs.filter((ch) => reach(dscTuneFor(ch).hz));
}

/**
 * ★ The channel keys for the dial: the band it is in (its distress channel and its routine ones), or — off every DSC band
 * — the distress set, so the box always has a way onto DSC. Only channels this radio reaches.
 */
export function dscChannelsForDial(dialHz: number, reach: (hz: number) => boolean): { band: DscBand | null; channels: DscChannel[] } {
  const b = dscBandOf(dialHz);
  const own = b ? dscReachable(dscBandChannels(b), reach) : [];
  if (own.length) return { band: b, channels: own };
  return { band: null, channels: dscReachable(DSC_DISTRESS, reach) };
}

/** The key lit for this dial: tuned exactly as that channel wants (±150 Hz, the right mode). */
export function dscChannelOn(ch: { hz: number; vhf: boolean }, dialHz: number, mode: string): boolean {
  const t = dscTuneFor(ch);
  const m = (mode || '').toLowerCase();
  const modeOk = ch.vhf ? (m === 'nfm' || m === 'fm') : m === 'usb';
  return modeOk && Math.abs(dialHz - t.hz) <= 150;
}

/**
 * The decoder's audio centre for the listener's dial and mode, from the NEAREST DSC channel to 1700 Hz — any channel,
 * distress or routine, so a dial a little off (2186.0 USB) or on a routine channel (4206.8 for 4208.5) decodes. In LSB the
 * tones are above the dial. Anywhere else, 1700 Hz.
 */
export function dscCentreFor(dialHz: number, mode: string): number {
  const m = (mode || '').toLowerCase();
  let centre = DSC_TONE;
  let best = Infinity;
  for (const c of DSC_ALL) {
    if (c.vhf) continue;
    const a = m === 'lsb' ? dialHz - c.hz : c.hz - dialHz;
    if (a >= 300 && a <= 3000 && Math.abs(a - DSC_TONE) < best) { best = Math.abs(a - DSC_TONE); centre = a; }
  }
  return Math.round(centre);
}

/**
 * ★★ "TUNE 4206.8 USB FOR THIS CHANNEL" — the one-tap fix for the commonest DSC mistake. The dial is ON a channel's
 * assigned frequency (±200 Hz: the channels are 500 Hz apart), where its tones fall at 0 Hz and nothing can decode
 * them; or at the right USB dial in the wrong mode; or near channel 70 in a mode other than NFM. null = the dial is
 * already right (or nowhere near a DSC channel).
 */
export function dscTuneOffer(dialHz: number, mode: string): { ch: DscChannel; hz: number; mode: 'usb' | 'nfm'; text: string } | null {
  if (!(dialHz > 0)) return null;
  const m = (mode || '').toLowerCase();
  // ★ Already right for SOME channel: no offer. (4207.3 USB is 4209's dial — and within 200 Hz of 4207.5, which is the
  //   assigned frequency of another; channels 1.5 kHz apart make that overlap on every HF band.)
  if (DSC_ALL.some((c) => dscChannelOn(c, dialHz, m))) return null;
  for (const c of DSC_ALL) {
    const t = dscTuneFor(c);
    if (c.vhf) {
      if (Math.abs(dialHz - c.hz) <= 12500 && m !== 'nfm' && m !== 'fm') {
        return { ch: c, hz: t.hz, mode: t.mode, text: `TUNE ${dscDialText(t.hz)} NFM for channel 70` };
      }
      continue;
    }
    const onAssigned = Math.abs(dialHz - c.hz) <= 200;
    const rightDialWrongMode = Math.abs(dialHz - t.hz) <= 150 && m !== 'usb';
    if (onAssigned || rightDialWrongMode) {
      return { ch: c, hz: t.hz, mode: t.mode, text: `TUNE ${dscDialText(t.hz)} USB for ${c.label} kHz` };
    }
  }
  return null;
}
