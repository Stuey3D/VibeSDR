/**
 * sharedDecoders.ts — ★★★ A DECODER ON A SHARED DIAL IS THE ROOM'S (Stuart, 2026-11-24). One copy for the web page and the app.
 *
 *   "if the aprs decoder is open for user 1 then user 2 connecting it should be open for, same with all decoders. then if
 *    user 2 closes it then it closes for them only this is unless it is a tunes the sdr itself decoder like dab"
 *
 * On a shared dial (and a one-listener radio) the server runs ONE decoder host for the room and says what it runs on the
 * spectrum socket: {"type":"dec_shared","list":[{slot,ext,params,key,held,mine,n}]} (vibe_shared_decoders.h) — to everyone
 * when it changes, and to each listener as they arrive. A per-VFO radio never sends it (each listener has their own host).
 *
 * THE RULE (this file): a listener FOLLOWS the room's decoder — its box opens and it attaches with EXACTLY the server's
 * parameters, so it JOINS the run rather than restarting it under another key (the beta92 SATELLITE "params" lesson).
 *   · already holding it ("held") → nothing (it is theirs too);
 *   · they CLOSED that very run (its key) → it stays closed for them — until a different decoder starts in the room (a new
 *     key), the room's decoder stops, or they open one themselves;
 *   · a decoder they have just opened themselves wins for a moment (a broadcast already on its way is not a reason to undo a
 *     press) — the server's answer to their own attach follows at once;
 *   · their box busy with something that is not the room's (Advanced RDS, the scanner, TRANSCRIBE, SATELLITE, a decoder on VFO
 *     B) → not followed: that is theirs, and the room's decoder would replace it;
 *   · a decoder this client cannot show (an ext it does not know, PAGING) → not followed.
 * Closing the box sends the ordinary detach: the server lets go for THIS listener only and stops the decoder when the last
 * holder lets go. Opening a different decoder is the ordinary attach: on a shared dial it replaces the room's (one host), and
 * the others follow the new one — exactly as a tune moves the shared dial for everyone.
 * Whole-radio modes (DAB, ADS-B, ISM, AIS, NDB, radiosondes, the aviation datalink, SATELLITE, the scanner, the DX log) are not
 * on the room's decoder host and are never in the list: they keep the shared behaviour they always had.
 * The silent-decoder rule (silentDecoder.ts) follows from the box: a listener following APRS is muted exactly as one who
 * opened it.
 */

export type SharedSlot = 'decoder' | 'spots';

export interface SharedDecoder {
  slot: SharedSlot;
  /** The server's extension name (fsk, rttyskim, cwskim, msf, aprs …; 'spots' for the spots run). */
  ext: string;
  /** The attach parameters that JOIN the run (flat scalars, rebuilt by the server). Spots: {scope:'band'} or {}. */
  params: Record<string, number | string | boolean>;
  /** The run's identity — the same decoder with the same parameters. */
  key: string;
  /** This listener holds it (one of their sockets attached / joined it). */
  held: boolean;
  /** This listener started it. */
  mine: boolean;
  /** How many hold it. */
  n: number;
}

/** {"type":"dec_shared","list":[…]} → the list (bounded, every field checked), or null when it is not that message. */
export function parseDecShared(msg: unknown): SharedDecoder[] | null {
  const o = msg as { type?: unknown; list?: unknown } | null;
  if (!o || o.type !== 'dec_shared' || !Array.isArray(o.list)) return null;
  const out: SharedDecoder[] = [];
  for (const x of o.list.slice(0, 4) as unknown[]) {
    const e = x as Record<string, unknown> | null;
    if (!e || (e.slot !== 'decoder' && e.slot !== 'spots')) continue;
    const ext = typeof e.ext === 'string' && /^[a-z0-9]{1,16}$/.test(e.ext) ? e.ext : '';
    const key = typeof e.key === 'string' ? e.key.slice(0, 512) : '';
    if (!ext || !key) continue;
    const params: Record<string, number | string | boolean> = {};
    if (e.params && typeof e.params === 'object' && !Array.isArray(e.params)) {
      let n = 0;
      for (const [k, v] of Object.entries(e.params as Record<string, unknown>)) {
        if (++n > 24 || !/^[A-Za-z0-9_]{1,32}$/.test(k)) continue;
        if (typeof v === 'number' && Number.isFinite(v)) params[k] = v;
        else if (typeof v === 'boolean') params[k] = v;
        else if (typeof v === 'string' && v.length <= 64) params[k] = v;
      }
    }
    out.push({ slot: e.slot, ext, params, key, held: e.held === true, mine: e.mine === true,
               n: typeof e.n === 'number' && Number.isFinite(e.n) ? Math.max(0, Math.min(999, Math.round(e.n))) : 0 });
  }
  return out;
}

const TIME_EXTS = ['time', 'msf', 'dcf77', 'rwm', 'wwv', 'wwvb', 'jjy', 'tdf'];
const SAME_NAME = ['navtex', 'wefax', 'sstv', 'cw', 'psk', 'aprs', 'dsc', 'sitora', 'same', 'selcal', 'mfsk', 'hell', 'jtty', 'ncdxf',
                   'balloon', 'tones', 'vor', 'sarsat', 'hfdl', 'lrpt', 'ale', 'fctlm'];

/** The server's extension → the decoder key both clients use for its box (RTTY's one-signal decoder is 'fsk', its skimmer
 *  'rttyskim' — both the RTTY box; a time station is TIME's). null = not one a listener follows (PAGING runs only on the
 *  device's own radio; an ext this build does not know). */
export function sharedDecoderKey(ext: string): string | null {
  if (ext === 'fsk' || ext === 'rttyskim') return 'rtty';
  if (ext === 'cwskim') return 'cw';
  if (ext === 'pskskim') return 'psk';
  if (TIME_EXTS.includes(ext)) return 'time';
  if (SAME_NAME.includes(ext)) return ext;
  return null;
}

/** What the client should do now. */
export type SharedAction =
  | { kind: 'none' }
  | { kind: 'follow'; entry: SharedDecoder; box: string }
  | { kind: 'spots'; entry: SharedDecoder; scope: 'single' | 'band' };

export interface SharedNow {
  /** The decoder box this listener has open ('rtty', 'aprs' …), or null. */
  box: string | null;
  /** The box is busy with something that is not the room's decoder (Advanced RDS, the scanner, TRANSCRIBE, SATELLITE, a
   *  decoder on VFO B): the room's decoder is not followed into it. */
  busy: boolean;
  /** This listener's spots run is on (any scope). */
  spotsOn: boolean;
  /** Can this client show that decoder box here (its key offered on this receiver, not switched off)? */
  canShow: (box: string) => boolean;
  /** Can it run the spots here (Band needs the band skimmer offered)? */
  canSpots?: (scope: 'single' | 'band') => boolean;
  /** Milliseconds now (tests pass their own). */
  now: number;
}

/** How long a listener's own press wins over a broadcast already on its way. */
export const OWN_PRESS_MS = 2000;

/** ★ One per listener (the page / the screen): remembers what they closed and when they last pressed. */
export class SharedFollower {
  private last: SharedDecoder[] = [];
  private closedKey: Record<SharedSlot, string> = { decoder: '', spots: '' };
  private pressedAt: Record<SharedSlot, number> = { decoder: -1e15, spots: -1e15 };
  private followed: Record<SharedSlot, string> = { decoder: '', spots: '' };

  /** The room's list as last received. */
  get list(): readonly SharedDecoder[] { return this.last; }
  /** The room's entry for a slot, or null. */
  entry(slot: SharedSlot): SharedDecoder | null { return this.last.find((e) => e.slot === slot) ?? null; }
  /** The key this listener closed for the slot ('' = none) — for tests and the status line. */
  closed(slot: SharedSlot): string { return this.closedKey[slot]; }
  /** Was the box opened by following the room (not by a press)? The run's key, or ''. */
  following(slot: SharedSlot): string { return this.followed[slot]; }

  /** A new `dec_shared` list → what to do. A slot with no entry forgets its "closed": the run it named has ended, so the
   *  same decoder starting later is a new run. A different key in the slot forgets it too. */
  receive(list: SharedDecoder[], now: SharedNow): SharedAction[] {
    this.last = list;
    for (const slot of ['decoder', 'spots'] as SharedSlot[]) {
      const e = this.entry(slot);
      if (!e || e.key !== this.closedKey[slot]) this.closedKey[slot] = '';
      if (!e || !e.held) { if (this.followed[slot] && (!e || e.key !== this.followed[slot])) this.followed[slot] = ''; }
    }
    return this.decide(now);
  }

  /** What to do with the list as it stands (also after the listener's box frees up). */
  decide(now: SharedNow): SharedAction[] {
    const out: SharedAction[] = [];
    const d = this.entry('decoder');
    if (d && !d.held && this.closedKey.decoder !== d.key && now.now - this.pressedAt.decoder >= OWN_PRESS_MS && !now.busy) {
      const box = sharedDecoderKey(d.ext);
      if (box && now.canShow(box) && this.followed.decoder !== d.key) out.push({ kind: 'follow', entry: d, box });
    }
    const s = this.entry('spots');
    if (s && !s.held && this.closedKey.spots !== s.key && now.now - this.pressedAt.spots >= OWN_PRESS_MS && this.followed.spots !== s.key) {
      const scope = s.params.scope === 'band' ? 'band' : 'single';
      if (!now.canSpots || now.canSpots(scope)) out.push({ kind: 'spots', entry: s, scope });
    }
    return out;
  }

  /** The client acted on a follow (it attached with the entry's parameters / subscribed with its scope). */
  noteFollowed(slot: SharedSlot, key: string) { this.followed[slot] = key; }

  /** The listener pressed something themselves — opened a decoder (or the spots): their press wins for a moment, and what
   *  they had closed is forgotten (they are choosing again). */
  noteOwnOpen(slot: SharedSlot, now: number) {
    this.pressedAt[slot] = now;
    this.closedKey[slot] = '';
    this.followed[slot] = '';
  }

  /** The listener CLOSED the box (or the spots) — it stays closed for them while the room runs that very decoder. `box` = the
   *  box they closed: only when it is the room's decoder is there a run to remember. */
  noteClosed(slot: SharedSlot, box: string | null) {
    const e = this.entry(slot);
    this.followed[slot] = '';
    if (!e) return;
    if (slot === 'decoder' && (!box || sharedDecoderKey(e.ext) !== box)) return;
    this.closedKey[slot] = e.key;
  }

  /** A new server / radio: nothing carried over. */
  reset() {
    this.last = [];
    this.closedKey = { decoder: '', spots: '' };
    this.pressedAt = { decoder: -1e15, spots: -1e15 };
    this.followed = { decoder: '', spots: '' };
  }
}
