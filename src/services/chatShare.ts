/**
 * chatShare — "share a station" in the canned chat: what a client SENDS, and how it DRAWS what the
 * server relays. One file for the app (ChatDrawer / SDRScreen) and the browser (web/client chat.ts),
 * so an app user and a web user share with each other in the same shape.
 *
 * ★★★ THE SENDER'S LABEL NEVER LEAVES THE DEVICE (Stuart, 2026-10-01). A bookmark's name is whatever
 *     its owner typed, and the canned chat exists precisely so that nothing anybody types reaches the
 *     room. `shareFromBookmark` reads the frequency, the mode and the passband — and nothing else;
 *     scripts/test_chat_share.ts proves a label, group or comment cannot reach the payload.
 * ★★★ THE NAME THE ROOM SEES IS THE SERVER'S. The server (vibe_chat_share.h) validates the share,
 *     names it from what THAT RECEIVER knows — RDS-learned stations, the owner's bookmarks, the EiBi
 *     schedule on air now, the DAB services it has decoded — and relays it. `parseShared` reads only
 *     what the server sent; there is no client name field to trust.
 * ★★ THE WIRE (id "check_out", the existing "Hey, check out" phrase — so older browsers still draw it):
 *      out: {type:'say', id:'check_out', kind:'bookmark', hz, mode?, bwLo?, bwHi?}
 *           {type:'say', id:'check_out', kind:'dab', hz:<block centre>, mode:'dab', block:'12B', sid?, eid?}
 *      in:  {type:'said', from, admin?, id:'check_out', hz, mode, kind, bwLo?, bwHi?, block?, sid?, eid?,
 *            name?, ensemble?, nameSrc?, text}
 *    `hz`/`mode` on a DAB share are for servers and clients from before this: an old server relays
 *    "check out 225.648 MHz DAB", an old browser draws it. `text` is the server's own sentence for
 *    any client that cannot draw the structured share.
 * ★ [TUNE] is a USER action on the receiving side and goes through the ordinary deck path —
 *   receiving a share never tunes anybody. On a shared dial with somebody else on it, `shareTuneStep`
 *   says ASK first, exactly as the SHARED TUNER banner does.
 *
 * Pure: no React Native, no DOM — scripts/test_chat_share.ts runs it under node.
 */
import { DAB_BLOCKS } from './dabBlocks';
import type { DialState } from './dialChat';

export type ShareKind = 'bookmark' | 'dab';

/** The frame a client sends. ★ No string in it is free text: `kind`, `mode` and `block` are ids. */
export interface ShareOut {
  type: 'say'; id: 'check_out'; kind: ShareKind;
  hz: number; mode?: string; bwLo?: number; bwHi?: number;
  block?: string; sid?: number; eid?: number;
}

/** A share as the server relayed it. `name`/`ensemble`/`text` are the SERVER'S words. */
export interface SharedStation {
  kind: ShareKind;
  hz: number; mode?: string; bwLo?: number; bwHi?: number;
  block?: string; sid?: number; eid?: number;
  name?: string; ensemble?: string;
  /** The server's own sentence ("shared 96.600 MHz WFM — Heart"), when it sent one. */
  text?: string;
}

/** The labels every client draws for a mode — the same words as the server's modeLabel(). */
export const SHARE_MODE_LABEL: Record<string, string> = {
  wfm: 'WFM', nfm: 'NFM', am: 'AM', sam: 'SAM', usb: 'USB', lsb: 'LSB', cw: 'CW', cwu: 'CW-U', cwl: 'CW-L',
  iq: 'IQ', dab: 'DAB', rds: 'Advanced RDS', rtty: 'RTTY', navtex: 'NAVTEX', wefax: 'WEFAX',
  sstv: 'SSTV', ft8: 'FT8 / FT4', time: 'Time signal',
};
/** The closed list the server accepts (vibe_chat_share.h kModes). A mode outside it is not sent. */
const SHAREABLE_MODES = new Set(Object.keys(SHARE_MODE_LABEL).filter((m) => m !== 'dab'));

/** "96.600 MHz" at and above 1 MHz, "198 kHz" below — the server's freqText, digit for digit. */
export function shareFreqText(hz: number): string {
  const h = Math.round(hz);
  if (h >= 1_000_000) return `${Math.floor(h / 1_000_000)}.${String(Math.floor((h % 1_000_000) / 1000)).padStart(3, '0')} MHz`;
  if (h % 1000 === 0) return `${h / 1000} kHz`;
  return `${(h / 1000).toFixed(1)} kHz`;
}

/** The DAB block a frequency is, within the 50 kHz both clients' dabGoTo already allow. */
export function dabBlockAt(hz: number): { name: string; hz: number } | null {
  let best: { name: string; hz: number } | null = null;
  let bestD = Infinity;
  for (const b of DAB_BLOCKS) {
    const d = Math.abs(b.hz - hz);
    if (d < bestD) { bestD = d; best = b; }
  }
  return best && bestD <= 50_000 ? best : null;
}

/** What a bookmark may contribute. ★ `name`, `group`, `comment` are deliberately NOT in this type:
 *  the function cannot read what it is not given, and callers pass the bookmark straight in. */
type BookmarkLike = {
  frequency: number; mode?: string | null;
  bandwidth_low?: number | null; bandwidth_high?: number | null;
  sid?: number | null; eid?: number | null;
};

/** ★★★ A bookmark -> a share, WITHOUT ITS LABEL. Only the fields listed in ShareOut are ever
 *  written; the object is built field by field, never spread from the bookmark. Null when there is
 *  nothing shareable (no frequency, or a DAB bookmark that is not on a block). */
export function shareFromBookmark(b: BookmarkLike): ShareOut | null {
  const hz = Math.round(Number(b?.frequency) || 0);
  if (!(hz > 0)) return null;
  const mode = String(b.mode || '').toLowerCase();
  if (mode === 'dab') {
    const blk = dabBlockAt(hz);
    if (!blk) return null;
    return shareDab(blk.name, typeof b.sid === 'number' && b.sid > 0 ? b.sid : undefined,
                    typeof b.eid === 'number' && b.eid >= 0 ? b.eid : undefined);
  }
  const out: ShareOut = { type: 'say', id: 'check_out', kind: 'bookmark', hz };
  if (SHAREABLE_MODES.has(mode)) out.mode = mode;
  const lo = b.bandwidth_low, hi = b.bandwidth_high;
  if (typeof lo === 'number' && typeof hi === 'number' && Number.isFinite(lo) && Number.isFinite(hi) && hi > lo) {
    out.bwLo = Math.round(lo); out.bwHi = Math.round(hi);
  }
  return out;
}

/** A DAB service (or, with no sid, the multiplex) -> a share. Null for a block name that is not one. */
export function shareDab(block: string, sid?: number, eid?: number): ShareOut | null {
  const blk = DAB_BLOCKS.find((b) => b.name.toUpperCase() === String(block || '').toUpperCase());
  if (!blk) return null;
  const out: ShareOut = { type: 'say', id: 'check_out', kind: 'dab', hz: blk.hz, mode: 'dab', block: blk.name };
  if (typeof sid === 'number' && sid > 0) out.sid = Math.round(sid);
  if (typeof eid === 'number' && eid >= 0 && eid <= 0xffff) out.eid = Math.round(eid);
  return out;
}

/** What the receiver is playing now -> a share: the DAB service when DAB is on, else the dial. */
export function shareFromTuned(
  dial: { frequency: number; mode: string; bandwidthLow?: number; bandwidthHigh?: number },
  dab?: { channel?: string; sid?: number; eid?: number } | null,
): ShareOut | null {
  if (dab && dab.channel) return shareDab(dab.channel, dab.sid, dab.eid);
  return shareFromBookmark({
    frequency: dial.frequency, mode: dial.mode,
    bandwidth_low: dial.bandwidthLow, bandwidth_high: dial.bandwidthHigh,
  });
}

/** How a share is drawn in a picker row on the SENDER's own screen (their label is fine there —
 *  it never leaves the device). "96.600 MHz WFM" / "DAB 12B · C0D2". */
export function shareSummary(s: ShareOut | SharedStation): string {
  if (s.kind === 'dab') {
    const blk = DAB_BLOCKS.find((b) => b.name === s.block);
    const where = `${s.block ?? '?'}${blk ? ` (${shareFreqText(blk.hz)})` : ''}`;
    return `DAB ${where}${s.sid ? ` · ${s.sid.toString(16).toUpperCase()}` : ''}`;
  }
  const m = s.mode ? SHARE_MODE_LABEL[s.mode] : '';
  return `${shareFreqText(s.hz)}${m ? ` ${m}` : ''}`;
}

const num = (v: unknown): number | undefined => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
const str = (v: unknown, max = 80): string | undefined =>
  typeof v === 'string' && v.length > 0 ? v.slice(0, max) : undefined;

/** ★ The server's relayed share -> what this client draws, or null when it is not drawable.
 *  Accepts the pre-2026-10-01 line too (hz + mode, no kind): that is an analogue share with no name. */
export function parseShared(msg: Record<string, unknown> | null | undefined): SharedStation | null {
  if (!msg) return null;
  const hz = num(msg.hz) ?? 0;
  const mode = typeof msg.mode === 'string' ? msg.mode.toLowerCase() : undefined;
  const kind: ShareKind = msg.kind === 'dab' || mode === 'dab' ? 'dab' : 'bookmark';
  const out: SharedStation = { kind, hz };
  if (kind === 'dab') {
    const blk = (typeof msg.block === 'string' && DAB_BLOCKS.find((b) => b.name === msg.block)) || dabBlockAt(hz);
    if (!blk) return null;
    out.block = blk.name; out.hz = blk.hz; out.mode = 'dab';
    const sid = num(msg.sid); if (sid && sid > 0) out.sid = sid;
    const eid = num(msg.eid); if (eid !== undefined && eid >= 0) out.eid = eid;
    const ens = str(msg.ensemble); if (ens) out.ensemble = ens;
  } else {
    if (!(hz > 0)) return null;
    if (mode) out.mode = mode;
    const lo = num(msg.bwLo), hi = num(msg.bwHi);
    if (lo !== undefined && hi !== undefined && hi > lo) { out.bwLo = lo; out.bwHi = hi; }
  }
  const name = str(msg.name); if (name) out.name = name;
  const text = str(msg.text, 160); if (text) out.text = text;
  return out;
}

/** The station, in words, from the SERVER'S facts:
 *    "96.600 MHz WFM — Heart" · "96.600 MHz WFM" · "DAB Heart — 12B (225.648 MHz)" · "DAB 12B (225.648 MHz) — D1 National" */
export function sharedStationText(s: SharedStation): string {
  if (s.kind === 'dab') {
    const where = `${s.block} (${shareFreqText(s.hz)})`;
    if (s.name) return `DAB ${s.name} — ${where}`;
    if (s.ensemble) return `DAB ${where} — ${s.ensemble}`;
    return `DAB ${where}`;
  }
  const m = s.mode ? (SHARE_MODE_LABEL[s.mode] ?? '') : '';
  return `${shareFreqText(s.hz)}${m ? ` ${m}` : ''}${s.name ? ` — ${s.name}` : ''}`;
}

/** The whole chat line body (the speaker is drawn beside it): "📻 shared 96.600 MHz WFM — Heart". */
export function sharedLineText(s: SharedStation): string {
  return `📻 shared ${sharedStationText(s)}`;
}

/** What tapping [TUNE] on a share should do on this receiver right now.
 *    'tune'    — go: an ordinary receiver, you alone on a shared dial, or you tuned it last
 *    'ask'     — a shared dial somebody else is on: offer "Can I tune?" first, exactly as the
 *                SHARED TUNER banner says ASK TO TUNE; tuning anyway stays the user's choice
 *    'refused' — a listen-only (spectator) dial and you are not its unlocked admin
 *    'no-dab'  — a DAB share on a receiver that cannot play DAB (the server would refuse the share,
 *                so this is a stale line from before a setting changed) */
export type ShareTuneStep = 'tune' | 'ask' | 'refused' | 'no-dab';
export function shareTuneStep(
  s: SharedStation, dial: Pick<DialState, 'mode' | 'listeners' | 'mine'> | null,
  opts: { admin?: boolean; dabCapable?: boolean } = {},
): ShareTuneStep {
  if (s.kind === 'dab' && opts.dabCapable === false) return 'no-dab';
  if (!dial || dial.mode === 'exclusive') return 'tune';
  if (dial.mode === 'spectator' && !opts.admin) return 'refused';
  if (dial.listeners > 1 && !dial.mine) return 'ask';
  return 'tune';
}
