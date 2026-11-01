/**
 * navtex — the decoder's character stream cut into NAVTEX MESSAGES (app + web; client-side — the server adds only
 *          each message's FEC count, op 0x07, see setFec).
 *
 * ★★★ WHY (Stuart, 2026-10-05): "navtex I imagine to be a hybrid of RTTY & WEFAX as navtex is transmitted in
 *     designated message blocks with a start and end … message arrives and is displayed with the option to save it
 *     … next message arrives previous message gets moved in the background and user can alternate between live
 *     receive or previous message like WEFAX." The server still sends NAVTEX as a plain text stream, exactly as RTTY
 *     (DecoderClient 0x01 frames) — the blocks are found HERE, so the web client and the app agree by construction.
 *
 * A message is   ZCZC B1B2B3B4 … NNNN      (ITU-R M.540, IMO NAVTEX Manual)
 *   B1 = transmitter letter A–Z · B2 = subject letter · B3B4 = serial 00–99.
 *
 * ★★ BUILT FOR A RADIO, NOT A FILE. The decoder prints '_' for a character it could not recover (fsk_decoder.cpp,
 *    ITU-R M.476's error symbol), so a header or trailer is often damaged:
 *    • ZCZC is found with ≥ 3 of its 4 letters in place (the miss must be '_' or a letter, never a space or a line
 *      break), followed by the 4-character id. An id is accepted with up to two lost characters.
 *    • NNNN is found with ≥ 3 N's, the 4th lost ('_') — never another letter, so a word cannot end a message.
 *    • No NNNN at all: the message is closed "[end lost]" when the NEXT header arrives, when the server's decoder
 *      LOSES ITS LOCK (lockState() — VibeServer only), or after END_LOST_MS of silence (tick()).
 *    • Text before any header (the box was opened mid-message) is a message of its own, "[start lost]" — but only
 *      once it holds real text (ORPHAN_MIN_ALNUM letters/digits), so a few characters of noise between broadcasts
 *      are not dressed up as a message.
 * ★ Stateful and incremental (push / tick) because a message's TIME is part of it — the save file is named by the
 *   minute it began — and a stateless re-parse of a capped scrollback cannot tell one GB98 from its retransmission
 *   four hours later. Only the live message and ONE previous are kept, as WEFAX's PREV keeps one picture.
 * ★ No station NAMES: B1 letters are allocated per NAVAREA and reused across the world on 518 kHz (the same letter
 *   is a different transmitter in each area), so a letter-keyed name table would confidently misname every station
 *   outside the area it was written for. "Station G" is always true.
 */

/** ★ 75 s of nothing ends a message whose NNNN was lost. A NAVTEX message never pauses mid-text (100 baud, FEC
 *  repeats every character), so a minute of silence means the transmission has ended, not a gap in it — and it is
 *  short enough that a reader is not left looking at "receiving" long after the station has gone quiet. */
import { navtexLabelOf, navtexResolve, navtexStartLostLabel, navtexStartLostResolve, type NavtexResolution } from './navtexStations.generated.ts';   // ★ .ts: Node runs this file directly (scripts/test_navtex.ts)
export const NAVTEX_END_LOST_MS = 75_000;
/** Letters/digits an orphan fragment (no header seen) must hold before it is shown as a "[start lost]" message. */
export const ORPHAN_MIN_ALNUM = 16;
/** ★★ A LOCK LOST FOR LESS THAN THIS IS A BLIP, NOT AN END (2026-10-22). NickB's NAVTEX overnight on 490 kHz came out as
 *  one message cut into "[end lost]" + a run of "[start lost]" pieces, each a few lines, on a clean signal: the decoder
 *  resyncs on a weak patch and relocks within about a second (NavtexRx: ≤ ~110 bits), and every relock had closed the
 *  message. A co-channel TAKEOVER (NickB, 2026-10-12) still splits: its lock stays lost for longer, or the new station's
 *  own ZCZC arrives — either way the message ends where the lock was lost, and the text after that point (the takeover,
 *  or the decoder's garbage while hunting) is not glued onto it. */
export const LOCK_BLIP_MS = 1500;
/** ★★★ A FADE IS NOT AN END EITHER (Lenovo, 12.0.0~beta3, 2026-10-22: 517.5 kHz lost lock for 5–15 s at a time — real
 *  fades — and each one still split a message). A lock loss never closes a message now: it closes only on its NNNN, a
 *  new ZCZC (the co-channel takeover protection, unchanged) or NAVTEX_END_LOST_MS of silence. A gap longer than
 *  LOCK_BLIP_MS is SHOWN in the text where it happened, and counted in the title ("gaps: 2 (27 s)"); the decoder's
 *  hunting garbage between the loss and the relock is dropped. */
export function navtexGapMarker(ms: number): string { return `[\u2026 signal lost ${Math.max(1, Math.round(ms / 1000))} s \u2026]`; }

export interface NavtexMessage {
  /** B1B2B3B4 as received ('_' where lost), or null when the start was lost. */
  id: string | null;
  /** B1 — transmitter letter, or null if lost / unknown. */
  station: string | null;
  /** B2 — subject letter, or null if lost / unknown. */
  subject: string | null;
  /** B3B4 — '98', or null if either digit was lost. */
  serial: string | null;
  /** The message text between the header and NNNN, line breaks normalised. */
  text: string;
  startLost: boolean;
  endLost: boolean;
  /** NNNN seen, or closed as end-lost — nothing more will be added. */
  done: boolean;
  /** When the header (or the first orphan character) arrived, ms since epoch. */
  startedAt: number;
  /** Characters lost ('_') and characters received (letters, digits, '_') in the text. */
  lost: number;
  chars: number;
  /** ★ Fades the message survived (see navtexGapMarker): how many, and their total length in ms. */
  gaps?: number;
  gapMs?: number;
  /** ★ The decoder's OWN per-message FEC count, when the server sends one (VibeServer op 0x07, 2026-10-05) — absent
   *  from an older server, UberSDR or OpenWebRX, which leave the '_'-derived figures above as the only measure. */
  fec?: NavtexFec;
}

/** clean = the RX copy read, repaired = rebuilt from the DX copy or the soft tiers, lost = printed as '_'. */
export interface NavtexFec { clean: number; repaired: number; lost: number }

/** ITU-R M.540 / IMO NAVTEX Manual subject indicators (B2), short enough for a decoder-box title. */
const SUBJECTS: Record<string, string> = {
  A: 'Nav warning',
  B: 'Met warning',
  C: 'Ice report',
  D: 'SAR / piracy',
  E: 'Weather forecast',
  F: 'Pilot / VTS',
  G: 'AIS',
  H: 'LORAN',
  I: 'Not used',
  J: 'SATNAV',
  K: 'Other navaid',
  L: 'Nav warning (more)',
  T: 'Test',
  V: 'Special service', W: 'Special service', X: 'Special service', Y: 'Special service',
  Z: 'No messages on hand',
};

export function navtexSubject(b2: string | null): string | null {
  if (!b2) return null;
  return SUBJECTS[b2] ?? `Subject ${b2}`;
}

/** Where a message was heard: the dial and the RECEIVING SERVER's position (NaN when it has none) — what names its
 *  station (navtexStation, from NGA Pub. 117). */
export interface NavtexPlace { dialHz: number; lat: number; lon: number }

/** "Station G · Met warning · #98", or "[start lost]" for a message joined part way through. With the server's FEC
 *  count: "Station G · Met warning · #98 · 2 repaired · 1 lost" — or "· no errors" when every character read clean.
 *  ★ With `place`, the station by name: "B · Oostende, Belgium 🇧🇪 · Met warning · #10" — the server's gallery titles
 *    use the same table and rule (vibe_navtex_stations.h). */
/** ★ 2026-11-01: the station's resolution for this message — with its start time and header, so a corrupted header letter
 *  reads "Suspected <station>" on that station's letter; a start-lost message only from its own text. null = none. */
export function navtexStationOf(m: NavtexMessage, place?: NavtexPlace | null): NavtexResolution | null {
  if (!place) return null;
  const utc = m.startedAt > 0 ? Math.floor(m.startedAt / 1000) : NaN;
  if (m.startLost) return navtexStartLostResolve(place.dialHz, place.lat, place.lon, m.text, utc);
  return navtexResolve(m.station, place.dialHz, place.lat, place.lon, m.text, utc, m.id ?? '');
}
/** Why that station, in plain English — this message's evidence (empty when none was named). */
export function navtexStationWhy(m: NavtexMessage, place?: NavtexPlace | null): string[] {
  return navtexStationOf(m, place)?.why ?? [];
}

export function navtexTitle(m: NavtexMessage, place?: NavtexPlace | null): string {
  if (m.startLost) {
    const l = place ? navtexStartLostLabel(place.dialHz, place.lat, place.lon, m.text, m.startedAt > 0 ? Math.floor(m.startedAt / 1000) : NaN) : null;
    return l ? `Start lost · ${l}` : 'Start lost';
  }
  const parts: string[] = [];
  parts.push(place ? navtexLabelOf(m.station, navtexStationOf(m, place))
                   : m.station ? `Station ${m.station}` : 'Station ?');
  parts.push(navtexSubject(m.subject) ?? 'Subject ?');
  parts.push(m.serial ? `#${m.serial}` : '#??');
  if (m.gaps) parts.push(`gaps: ${m.gaps} (${Math.round((m.gapMs ?? 0) / 1000)} s)`);
  if (m.fec) {
    // ★ Only what happened: a repaired count of 0 is not news. A REPAIRED character prints exactly like a clean one,
    //   so this is the only place a reader can learn the FEC worked for it (the '_' count cannot see it).
    if (m.fec.repaired > 0) parts.push(`${m.fec.repaired} repaired`);
    if (m.fec.lost > 0) parts.push(`${m.fec.lost} lost`);
    if (m.fec.repaired === 0 && m.fec.lost === 0) parts.push('no errors');
  }
  return parts.join(' · ');
}

/** Share of characters lost, 0–100 (whole per cent), or null with nothing to judge yet.
 *  ★ null too once the server's own count is in the title (m.fec) — "1 lost" and "2% lost" side by side would be two
 *    answers to one question, and the '_' count is the weaker of them. */
export function navtexLostPct(m: NavtexMessage): number | null {
  if (m.fec) return null;
  if (m.chars <= 0) return null;
  return Math.round((100 * m.lost) / m.chars);
}

/** The message as it was sent, with the gaps said out loud: what the reader sees and what SAVE writes. */
export function navtexBody(m: NavtexMessage): string {
  const out: string[] = [];
  out.push(m.startLost ? '[start lost]' : `ZCZC ${m.id ?? '????'}`);
  if (m.text) out.push(m.text);
  if (m.endLost) out.push('[end lost]');
  else if (m.done) out.push('NNNN');
  return out.join('\n');
}

const pad2 = (n: number) => String(n).padStart(2, '0');
/** "2026-10-05T1601Z" — the minute the message began, UTC (filenames cannot hold ':' everywhere). */
function stamp(ms: number): string {
  const d = new Date(ms);
  return `${d.getUTCFullYear()}-${pad2(d.getUTCMonth() + 1)}-${pad2(d.getUTCDate())}T${pad2(d.getUTCHours())}${pad2(d.getUTCMinutes())}Z`;
}

/** navtex_GB98_2026-10-05T1601Z.txt — '_' in a lost id becomes 'x' so the name stays readable. */
export function navtexFileName(m: NavtexMessage): string {
  const id = m.id ? m.id.replace(/[^A-Z0-9]/g, 'x') : 'partial';
  return `navtex_${id}_${stamp(m.startedAt)}.txt`;
}

/** The saved file: two lines saying what and when, then the message exactly as shown. */
export function navtexFileText(m: NavtexMessage, place?: NavtexPlace | null): string {
  const d = new Date(m.startedAt);
  const when = `${d.getUTCFullYear()}-${pad2(d.getUTCMonth() + 1)}-${pad2(d.getUTCDate())} `
             + `${pad2(d.getUTCHours())}:${pad2(d.getUTCMinutes())} UTC`;
  const pct = navtexLostPct(m);
  const why = navtexStationWhy(m, place);
  const head = `NAVTEX · ${navtexTitle(m, place)}\nReceived ${when}${pct != null ? ` · ${pct}% of characters lost` : ''}`
             + (why.length ? `\nWhy: ${why.join('; ')}` : '');
  return `${head}\n\n${navtexBody(m)}\n`;
}

// ── Matching ───────────────────────────────────────────────────────────────────────────────────

const isSpaceCh = (c: string) => c === ' ' || c === '\t' || c === '\r' || c === '\n';
const isIdLetter = (c: string) => (c >= 'A' && c <= 'Z') || c === '_';
const isIdDigit = (c: string) => (c >= '0' && c <= '9') || c === '_';

/** ≥ 3 of "ZCZC" at i, the miss a lost or wrong LETTER (never a space / break: that is two words, not a header). */
function zczcAt(s: string, i: number): boolean {
  let hit = 0;
  for (let k = 0; k < 4; k++) {
    const c = s[i + k];
    if (c === 'ZCZC'[k]) hit++;
    else if (!(c === '_' || (c >= 'A' && c <= 'Z'))) return false;
  }
  // ★ "_CZC", "ZC_C", "ZCZ_" … but not e.g. "ZCZE" followed by a word — a wrong LETTER must still sit on an id.
  return hit >= 3;
}

type Hdr = { at: number; end: number; id: string } | 'wait' | null;

/** A header starting at i: ZCZC, 0–3 spaces, then the 4-character id. 'wait' if the stream ends inside it. */
function headerAt(s: string, i: number): Hdr {
  if (i + 4 > s.length) return null;
  if (!zczcAt(s, i)) return null;
  let j = i + 4;
  let sp = 0;
  while (j < s.length && (s[j] === ' ' || s[j] === '\t') && sp < 3) { j++; sp++; }
  if (j + 4 > s.length) {
    // ★ Still arriving: hold the decision rather than miss a header split across two chunks.
    for (let k = j; k < s.length; k++) if (!(k - j < 2 ? isIdLetter(s[k]) : isIdDigit(s[k]))) return null;
    return 'wait';
  }
  const id = s.slice(j, j + 4);
  if (!isIdLetter(id[0]) || !isIdLetter(id[1]) || !isIdDigit(id[2]) || !isIdDigit(id[3])) return null;
  let lostInId = 0;
  for (const c of id) if (c === '_') lostInId++;
  if (lostInId > 2) return null;
  // ★ A perfect ZCZC needs no separator check; a damaged one must not run straight on into a 5th id-like
  //   character (that is a word, not a header).
  if (s.slice(i, i + 4) !== 'ZCZC') {
    const after = s[j + 4];
    if (after === undefined) return 'wait';
    if (!isSpaceCh(after) && after !== '_') return null;
  }
  return { at: i, end: j + 4, id };
}

/** ≥ 3 N's in a 4-window, the 4th LOST — a lost character, never another letter. */
function nnnnAt(s: string, i: number): boolean {
  if (i + 4 > s.length) return false;
  let n = 0;
  for (let k = 0; k < 4; k++) {
    const c = s[i + k];
    if (c === 'N') n++;
    else if (c !== '_') return false;
  }
  return n >= 3;
}

function countLoss(text: string): { lost: number; chars: number } {
  let lost = 0, chars = 0;
  for (let k = 0; k < text.length; k++) {
    const c = text[k];
    if (c === '_') { lost++; chars++; }
    else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) chars++;
  }
  return { lost, chars };
}

function alnum(text: string): number {
  let n = 0;
  for (let k = 0; k < text.length; k++) {
    const c = text[k];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) n++;
  }
  return n;
}

/** CR CR LF (a teleprinter's return) and bare CRs → one line break; blank lines at either end trimmed. */
function cleanText(raw: string): string {
  return raw.replace(/\r+\n?/g, '\n').replace(/^[ \t]*\n+/, '').replace(/\s+$/, '');
}

// ── The assembler ──────────────────────────────────────────────────────────────────────────────

export class NavtexAssembler {
  /** The text since the last boundary: a message's body (inMsg) or the gap between messages. */
  private seg = '';
  private inMsg = false;
  private hdr: { id: string; startedAt: number } | null = null;
  /** When the gap's first character arrived — the start time of a "[start lost]" message. */
  private segStartedAt = 0;
  private lastTextAt = 0;
  /** Where the next boundary search starts in `seg` (each push only re-reads its own tail). */
  private scanFrom = 0;
  private finished: NavtexMessage[] = [];
  /** The server decoder's lock (op 0x03 state 3 = reading characters), as last reported; null = never reported. */
  private locked: boolean | null = null;
  /** ★ clearLive() ran and no message has finished since: LIVE is empty and the last finished message is PREV's. */
  private liveCleared = false;
  /** ★★ The lock was lost while text was arriving: where in `seg` (the cut) and when. Decided by what follows — a relock
   *  inside LOCK_BLIP_MS then NNNN = a blip, one message; a longer loss, or another ZCZC = the message ends at the cut. */
  private pendingCut: { at: number; atMs: number; relocked: boolean } | null = null;
  /** ★ The fades the message arriving has survived. */
  private gaps = 0;
  private gapMs = 0;
  /** Bumped on every visible change, so a UI can tell cheaply whether to redraw. */
  version = 0;

  reset(): void {
    this.seg = ''; this.inMsg = false; this.hdr = null; this.segStartedAt = 0; this.lastTextAt = 0;
    this.scanFrom = 0; this.finished = []; this.liveCleared = false; this.locked = null; this.pendingCut = null; this.gaps = 0; this.gapMs = 0; this.version++;
  }

  /** ★★ CLR, and a RETUNE (NickB, 2026-10-12: "changing from 518 to 490 when it's mid way through a message so it gets
   *  assigned to the previous message id from 518"; Stuart: "navtex needs to clear on retune"). The text after a
   *  retune is another transmission, so the message arriving is DROPPED — not closed under its id, which would put
   *  490's text under 518's ZCZC — and the stream starts again: "[start lost]" until the next ZCZC. LIVE is left
   *  empty (standing by); the messages already FINISHED are untouched, the last of them under PREV, as WEFAX's CLR
   *  sends the picture to PREV. reset() is the other thing — a fresh box (open / close / switch), nothing kept. */
  clearLive(): void {
    this.seg = ''; this.inMsg = false; this.hdr = null; this.segStartedAt = 0; this.lastTextAt = 0;
    this.scanFrom = 0; this.liveCleared = this.finished.length > 0; this.locked = null; this.pendingCut = null; this.gaps = 0; this.gapMs = 0; this.version++;
  }

  /** Feed decoded characters as they arrive. */
  push(chunk: string, nowMs: number): void {
    if (!chunk) return;
    if (!this.seg) this.segStartedAt = nowMs;
    this.seg += chunk;
    this.lastTextAt = nowMs;
    this.scan(nowMs);
    this.version++;
  }

  /** Close a message whose NNNN never came, once the station has been quiet for NAVTEX_END_LOST_MS. */
  tick(nowMs: number): boolean {
    if (!this.lastTextAt || nowMs - this.lastTextAt < NAVTEX_END_LOST_MS) return false;
    // ★ Silent for good while the lock was lost: the message ends where it was lost.
    if (this.pendingCut && !this.pendingCut.relocked) return this.closeAtCut(nowMs);
    const cur = this.current();
    if (!cur) return false;
    this.finish({ ...cur, endLost: true, done: true });
    this.seg = ''; this.inMsg = false; this.hdr = null; this.scanFrom = 0;
    this.version++;
    return true;
  }

  /** ★★ THE SERVER DECODER'S LOCK (VibeServer op 0x03: 0 no signal, 1 hunting, 3 reading — NavtexRx::State). A lock LOST
   *  while a message is open ends that message "[end lost]" under its own id, and what follows is "[start lost]"
   *  until a ZCZC (NickB, 2026-10-12, on 490: "Station E transmitting … then C come up on the top with a short message
   *  and drops, then it picks up decoding E again … the remainder of E got assigned to C until the next E message").
   *  Two transmitters on one channel do not share a character clock, so a takeover — and the fade back — costs the
   *  lock; one transmission does not lose it: 0 resyncs in 250 s of real 518 kHz (six messages, weak Baltic) and on
   *  every bench (−3 dB, 20 dB selective fading, 20 impulse bursts a minute), against 2–3 per co-channel takeover
   *  (synthetic, measured 2026-10-12). A takeover that happens to land in step with the first station's clock keeps
   *  the lock — that one is still ended by C's own ZCZC. Only a 3 → below-3 change counts: NoSignal ↔ hunting while
   *  unlocked is the level gate, not an event. Returns true when a message was closed (the caller redraws). */
  lockState(state: number, nowMs: number): boolean {
    const was = this.locked;
    this.locked = state >= 3;
    if (was && !this.locked) {
      // ★★ Not closed yet — see LOCK_BLIP_MS. A few characters of noise with no message open are dropped at once.
      // ★ A later loss after a blip moves the cut: the blip's text is the message's.
      if (this.inMsg || alnum(this.seg) >= ORPHAN_MIN_ALNUM) { if (!this.pendingCut || this.pendingCut.relocked) this.pendingCut = { at: this.seg.length, atMs: nowMs, relocked: false }; return false; }
      this.seg = ''; this.inMsg = false; this.hdr = null; this.segStartedAt = nowMs; this.scanFrom = 0;
      this.version++;
      return false;
    }
    // Relocked: a blip carries on invisibly; a FADE carries on too, with the gap shown where it happened (the hunting
    // garbage after the cut dropped). Either way the cut is kept until the next boundary: a ZCZC still ends it there.
    if (!was && this.locked && this.pendingCut && !this.pendingCut.relocked) {
      const lost = nowMs - this.pendingCut.atMs;
      if (lost >= LOCK_BLIP_MS) {
        const at = this.pendingCut.at;
        this.seg = this.seg.slice(0, at) + '\n' + navtexGapMarker(lost) + '\n';
        this.gaps++; this.gapMs += lost;
        this.scanFrom = Math.min(this.scanFrom, at);
        this.pendingCut = { at, atMs: this.pendingCut.atMs, relocked: true };
        this.version++;
        return false;
      }
      this.pendingCut.relocked = true;
    }
    return false;
  }

  /** The text up to the lock loss is one transmission's: a message closes [end lost], an orphan of real text becomes its
   *  own "[start lost] … [end lost]" fragment; what came after the cut (hunting garbage) is dropped. */
  private closeAtCut(nowMs: number): boolean {
    const cut = this.pendingCut;
    this.pendingCut = null;
    if (!cut) return false;
    const body = this.seg.slice(0, cut.at);
    let closed = false;
    if (this.inMsg || alnum(body) >= ORPHAN_MIN_ALNUM) { this.finish(this.build(body, true, true)); closed = true; }
    this.seg = ''; this.inMsg = false; this.hdr = null; this.segStartedAt = nowMs; this.scanFrom = 0;
    this.version++;
    return closed;
  }

  /** True while a message is arriving (header seen or real orphan text, no end yet). */
  get receiving(): boolean { return !!this.current(); }

  /** What LIVE shows: the message arriving, else the last one finished. */
  get live(): NavtexMessage | null {
    return this.current() ?? (this.liveCleared ? null : this.finished[this.finished.length - 1] ?? null);
  }

  /** What PREV shows: the message before LIVE's — one only, as WEFAX keeps one picture. */
  get prev(): NavtexMessage | null {
    const f = this.finished;
    return this.current() || this.liveCleared ? (f[f.length - 1] ?? null) : (f[f.length - 2] ?? null);
  }

  /** ★ The server's per-message FEC count (VibeServer op 0x07, 2026-10-05). It is sent BEHIND the text carrying its
   *  NNNN, on the same socket, so the message it belongs to is the one just closed by an NNNN. Attached only there:
   *  never to a message closed "[end lost]" (the server saw no NNNN either, so this is not its count), never to a
   *  "[start lost]" fragment (the server counted from ITS last boundary, which a joiner did not see), never twice.
   *  Returns true when it was attached — the caller redraws. */
  setFec(c: NavtexFec): boolean {
    const m = this.finished[this.finished.length - 1];
    if (!m || !m.done || m.endLost || m.startLost || m.fec) return false;
    m.fec = { clean: c.clean, repaired: c.repaired, lost: c.lost };
    this.version++;
    return true;
  }

  private finish(m: NavtexMessage): void {
    this.gaps = 0; this.gapMs = 0;         // ★ the next message starts with none
    this.finished.push(m);
    this.liveCleared = false;
    if (this.finished.length > 2) this.finished.splice(0, this.finished.length - 2);
  }

  private build(body: string, done: boolean, endLost: boolean): NavtexMessage {
    const text = cleanText(body);
    const { lost, chars } = countLoss(text);
    if (this.inMsg && this.hdr) {
      const id = this.hdr.id;
      const L = (c: string) => (c >= 'A' && c <= 'Z' ? c : null);
      return {
        id, station: L(id[0]), subject: L(id[1]),
        serial: /^[0-9]{2}$/.test(id.slice(2)) ? id.slice(2) : null,
        text, startLost: false, endLost, done, startedAt: this.hdr.startedAt, lost, chars,
        ...(this.gaps ? { gaps: this.gaps, gapMs: this.gapMs } : {}),
      };
    }
    return { id: null, station: null, subject: null, serial: null, text, startLost: true, endLost, done,
             startedAt: this.segStartedAt, lost, chars, ...(this.gaps ? { gaps: this.gaps, gapMs: this.gapMs } : {}) };
  }

  private cur: { v: number; m: NavtexMessage | null } = { v: -1, m: null };
  /** The message still arriving, if any — built once per change, however often the UI asks. */
  private current(): NavtexMessage | null {
    if (this.cur.v === this.version) return this.cur.m;
    const m = this.inMsg || alnum(this.seg) >= ORPHAN_MIN_ALNUM ? this.build(this.seg, false, false) : null;
    this.cur = { v: this.version, m };
    return m;
  }

  private scan(nowMs: number): void {
    for (;;) {
      let found: { kind: 'hdr'; at: number; end: number; id: string } | { kind: 'end'; at: number; end: number } | null = null;
      let waitAt = -1;
      for (let i = Math.max(0, this.scanFrom); i <= this.seg.length - 4; i++) {
        const h = headerAt(this.seg, i);
        if (h === 'wait') { waitAt = i; break; }
        if (h) { found = { kind: 'hdr', at: h.at, end: h.end, id: h.id }; break; }
        if (nnnnAt(this.seg, i)) { found = { kind: 'end', at: i, end: i + 4 }; break; }
      }
      if (!found) {
        // ★ Re-read the last few characters next time: a ZCZC / NNNN can straddle two chunks.
        this.scanFrom = waitAt >= 0 ? waitAt : Math.max(0, this.seg.length - 3);
        return;
      }
      // ★ A lock lost earlier in this segment: a NEW HEADER means it was a takeover — the message ends at the cut, and the
      //   text between the cut and this header is dropped. NNNN means it was a blip — one message, carried on.
      const cutAt = this.pendingCut && found.kind === 'hdr' ? this.pendingCut.at : -1;
      this.pendingCut = null;
      const before = this.seg.slice(0, cutAt >= 0 ? Math.min(cutAt, found.at) : found.at);
      if (found.kind === 'end') {
        // NNNN: the message (or a "[start lost]" fragment, if it holds real text) is complete.
        if (this.inMsg || alnum(before) >= ORPHAN_MIN_ALNUM) this.finish(this.build(before, true, false));
        this.inMsg = false; this.hdr = null;
      } else {
        // A new header: whatever was arriving ends here, its NNNN lost.
        if (this.inMsg || alnum(before) >= ORPHAN_MIN_ALNUM) this.finish(this.build(before, true, true));
        this.inMsg = true;
        this.hdr = { id: found.id, startedAt: nowMs };
        this.gaps = 0; this.gapMs = 0;
      }
      this.seg = this.seg.slice(found.end);
      this.segStartedAt = nowMs;
      this.scanFrom = 0;
    }
  }
}
