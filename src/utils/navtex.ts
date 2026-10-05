/**
 * navtex — the decoder's character stream cut into NAVTEX MESSAGES (app + web; client-side, no server change).
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
 *    • No NNNN at all: the message is closed "[end lost]" when the NEXT header arrives, or after END_LOST_MS of
 *      silence (tick()).
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
export const NAVTEX_END_LOST_MS = 75_000;
/** Letters/digits an orphan fragment (no header seen) must hold before it is shown as a "[start lost]" message. */
export const ORPHAN_MIN_ALNUM = 16;

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
}

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

/** "Station G · Met warning · #98", or "[start lost]" for a message joined part way through. */
export function navtexTitle(m: NavtexMessage): string {
  if (m.startLost) return 'Start lost';
  const parts: string[] = [];
  parts.push(m.station ? `Station ${m.station}` : 'Station ?');
  parts.push(navtexSubject(m.subject) ?? 'Subject ?');
  parts.push(m.serial ? `#${m.serial}` : '#??');
  return parts.join(' · ');
}

/** Share of characters lost, 0–100 (whole per cent), or null with nothing to judge yet. */
export function navtexLostPct(m: NavtexMessage): number | null {
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
export function navtexFileText(m: NavtexMessage): string {
  const d = new Date(m.startedAt);
  const when = `${d.getUTCFullYear()}-${pad2(d.getUTCMonth() + 1)}-${pad2(d.getUTCDate())} `
             + `${pad2(d.getUTCHours())}:${pad2(d.getUTCMinutes())} UTC`;
  const pct = navtexLostPct(m);
  const head = `NAVTEX · ${navtexTitle(m)}\nReceived ${when}${pct != null ? ` · ${pct}% of characters lost` : ''}`;
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
  /** Bumped on every visible change, so a UI can tell cheaply whether to redraw. */
  version = 0;

  reset(): void {
    this.seg = ''; this.inMsg = false; this.hdr = null; this.segStartedAt = 0; this.lastTextAt = 0;
    this.scanFrom = 0; this.finished = []; this.version++;
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
    const cur = this.current();
    if (!cur) return false;
    this.finish({ ...cur, endLost: true, done: true });
    this.seg = ''; this.inMsg = false; this.hdr = null; this.scanFrom = 0;
    this.version++;
    return true;
  }

  /** True while a message is arriving (header seen or real orphan text, no end yet). */
  get receiving(): boolean { return !!this.current(); }

  /** What LIVE shows: the message arriving, else the last one finished. */
  get live(): NavtexMessage | null {
    return this.current() ?? this.finished[this.finished.length - 1] ?? null;
  }

  /** What PREV shows: the message before LIVE's — one only, as WEFAX keeps one picture. */
  get prev(): NavtexMessage | null {
    const f = this.finished;
    return this.current() ? (f[f.length - 1] ?? null) : (f[f.length - 2] ?? null);
  }

  private finish(m: NavtexMessage): void {
    this.finished.push(m);
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
      };
    }
    return { id: null, station: null, subject: null, serial: null, text, startLost: true, endLost, done,
             startedAt: this.segStartedAt, lost, chars };
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
      const before = this.seg.slice(0, found.at);
      if (found.kind === 'end') {
        // NNNN: the message (or a "[start lost]" fragment, if it holds real text) is complete.
        if (this.inMsg || alnum(before) >= ORPHAN_MIN_ALNUM) this.finish(this.build(before, true, false));
        this.inMsg = false; this.hdr = null;
      } else {
        // A new header: whatever was arriving ends here, its NNNN lost.
        if (this.inMsg || alnum(before) >= ORPHAN_MIN_ALNUM) this.finish(this.build(before, true, true));
        this.inMsg = true;
        this.hdr = { id: found.id, startedAt: nowMs };
      }
      this.seg = this.seg.slice(found.end);
      this.segStartedAt = nowMs;
      this.scanFrom = 0;
    }
  }
}
