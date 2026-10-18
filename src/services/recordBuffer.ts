/**
 * recordBuffer.ts — the RECORD BUFFER's rules (V12): how long, and when the buffered audio stops
 * belonging to what you are listening to.
 *
 * ★★ WHAT IT IS (Stuart, 2026-10-18): "a 30 second record buffer would be useful". With it on, the
 *    native audio path keeps the last 30 or 60 seconds of exactly what a recording would capture, and
 *    ⏺ REC starts the file WITH that audio already in it — the signal you only noticed once it had
 *    started is in the recording. The ring itself is native (VibePowerModule.swift RecordBufferRing,
 *    VibeStreamService.kt RecordBufferRing) and in the web client (web/client/src/audio.ts); THIS file
 *    owns the one rule they all need from the app: when to throw the buffered audio away.
 *
 * ★★★ THE RETUNE RULE, IN ONE PLACE. "Clear the buffer on retune a large amount but keep it when a
 *     user is fine tuning on a signal. Bigger allowance based on demodulator — 50 kHz on FM is nothing
 *     but in SSB it's a 1/4 of a band." So a move is judged against the MODE's allowance, and judged
 *     from an ANCHOR — the dial at the moment the buffer's current contents began — never from the
 *     last step. Creeping across a band 500 Hz at a time therefore still clears it: each step is
 *     small, the distance from where the oldest buffered audio was heard is not.
 *  ★ "When the contents began" moves with time: the ring only holds the last N seconds, so once N
 *    seconds have passed the oldest audio in it was heard wherever the dial was N seconds ago. The
 *    anchor is that position — which is why a slow, patient retune over minutes is NOT a clear when
 *    nothing in the buffer predates it.
 *  ★ A change of MODE, of SOURCE (server, receiver on a multi-radio server, local SDR, a DAB service
 *    or ensemble, a different stream) always clears: the audio in the ring is not this audio.
 *  ★ Volume, mute, squelch, filter width, NR, notch, binaural… never clear. They are not inputs here.
 *
 * ★ Pure and synchronous — scripts/test_recordBuffer.ts drives it with a fake clock.
 */

/** The choices the Audio sheet offers. 0 = Off (nothing is allocated or run). */
export type RecordBufferSeconds = 0 | 30 | 60;
export const RECORD_BUFFER_CHOICES: readonly RecordBufferSeconds[] = [0, 30, 60];
/** ★★ ON BY DEFAULT, 30 s (Stuart, 2026-10-18) — until then it was Off until you chose. Someone who has turned it
 *  off has "0" stored, and that stays Off; only a missing (never chosen) or unreadable value takes the default. */
export const RECORD_BUFFER_DEFAULT: RecordBufferSeconds = 30;
/** App-wide, one key for every server — the buffer is a property of THIS app, not of a receiver. */
export const RECORD_BUFFER_KEY = 'lsv_record_buffer';

export function parseRecordBufferSeconds(v: string | null | undefined): RecordBufferSeconds {
  // ★ Number('') and Number(null) are 0 — so "nothing stored" must be caught before it reads as an explicit Off.
  if (v == null || String(v).trim() === '') return RECORD_BUFFER_DEFAULT;
  const n = Number(v);
  return n === 0 || n === 30 || n === 60 ? n : RECORD_BUFFER_DEFAULT;
}

/**
 * How far the dial may move from the anchor before the buffered audio is someone else's, in Hz.
 *  - CW 1 kHz — a CW signal is a few hundred Hz wide; 1 kHz is "still zeroing in".
 *  - USB/LSB 3 kHz — one voice channel.
 *  - AM/SAM 10 kHz — one broadcast channel (9/10 kHz raster): nudging for the cleaner sideband stays.
 *  - NFM / 'fm' 15 kHz — one or two 12.5 kHz channels. ★ 'fm' in this app is NARROW FM (UberSDR's name,
 *    ±6 kHz in MODE_BANDWIDTHS); broadcast FM is 'wfm'.
 *  - WFM 150 kHz — less than one 200 kHz broadcast channel spacing, so the next station clears it.
 *  - DRM 10 kHz — an AM-raster channel. DAB 0 — the ensemble is fixed; ANY dial change is a new one
 *    (the service is part of the source key, see SDRScreen).
 *  - Anything else (IQ, data modes, a mode a server invents) 5 kHz — between SSB and AM: a narrow
 *    digital signal keeps through small corrections, a band hop does not.
 */
export function retuneAllowanceHz(mode: string): number {
  switch ((mode || '').toLowerCase()) {
    case 'cw': case 'cwu': case 'cwl': return 1_000;
    case 'usb': case 'lsb': return 3_000;
    case 'am': case 'sam': case 'sau': case 'sal': case 'sas': case 'drm': return 10_000;
    case 'nfm': case 'fm': return 15_000;
    case 'wfm': case 'wbfm': return 150_000;
    case 'dab': return 0;
    default: return 5_000;
  }
}

/** Where the listener is: the dial, the mode, and WHICH audio it is (server · receiver · DAB service). */
export interface RecordBufferDial {
  freqHz: number;
  mode: string;
  /** Anything whose change means "different audio": the server URL, the radio, the DAB service… */
  source: string;
}

/**
 * Decides, on each change of dial, mode or source, whether the buffered audio must be cleared.
 * `update` returns true when it must — and re-anchors at the new position.
 */
export class RecordBufferAnchor {
  /** Dial positions since the last clear, oldest first: the dial was `hz` from `t` until the next entry. */
  private hist: { t: number; hz: number }[] = [];
  private mode = '';
  private source = '';
  private started = false;

  private windowMs: number;
  constructor(windowMs: number) { this.windowMs = windowMs; }

  /** The buffer's length changed (30 ↔ 60). Contents are kept natively, so the history is too. */
  setWindow(seconds: number): void { this.windowMs = Math.max(0, seconds) * 1000; }

  /** The buffer was emptied for some other reason (turned on, a recording path reset): start afresh here. */
  reset(d: RecordBufferDial, nowMs: number): void {
    this.hist = [{ t: nowMs, hz: d.freqHz }];
    this.mode = (d.mode || '').toLowerCase();
    this.source = d.source;
    this.started = true;
  }

  /** The dial at the moment the oldest audio still in the buffer was heard. */
  anchorHz(nowMs: number): number {
    const edge = nowMs - this.windowMs;
    // The last position that was already current at the window's edge; before the edge, older history is
    // no longer in the ring and is dropped.
    let i = 0;
    while (i + 1 < this.hist.length && this.hist[i + 1].t <= edge) i++;
    if (i > 0) this.hist.splice(0, i);
    return this.hist.length ? this.hist[0].hz : NaN;
  }

  update(d: RecordBufferDial, nowMs: number): boolean {
    const mode = (d.mode || '').toLowerCase();
    if (!this.started || mode !== this.mode || d.source !== this.source) {
      // The first call is a clear too: whatever a previous screen left in the native ring is not this audio.
      this.reset(d, nowMs);
      return true;
    }
    const anchor = this.anchorHz(nowMs);
    if (Math.abs(d.freqHz - anchor) > retuneAllowanceHz(mode)) {
      this.reset(d, nowMs);
      return true;
    }
    const last = this.hist[this.hist.length - 1];
    if (!last || last.hz !== d.freqHz) {
      this.hist.push({ t: nowMs, hz: d.freqHz });
      // Bounded: a slider dragged for minutes inside the allowance would otherwise grow this forever.
      // Dropping the middle keeps the anchor (index 0) and the newest, which are the two that matter.
      if (this.hist.length > 512) this.hist.splice(1, this.hist.length - 256);
    }
    return false;
  }
}

/** Seconds → the Audio sheet's label for the choice. */
export function recordBufferLabel(s: RecordBufferSeconds): string {
  return s === 0 ? 'OFF' : `${s} S`;
}
