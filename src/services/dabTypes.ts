// dabTypes.ts — what the server reports about a DAB multiplex, and the one gate every string
// from the air must pass through before it reaches a React tree.
//
// ★★★ THE TYPE IS A COPY OF THE WEB CLIENT'S ON PURPOSE (web/client/src/spectrum.ts, DabState).
//     Same server, same JSON, and Stuart's brief for the app DAB window is "a simple mirror of the
//     Webclient". Two readers of one message: when the server grows a field, BOTH files want it —
//     see AGENTS.md, "ONE RULE, TWO READERS". Keep them in step.
//
// ★★★ AND EVERY FIELD HERE IS MEASURED. Nothing in this interface is inferred from something else;
//     that is the standard the Advanced RDS panel set and the DAB panel keeps. A row we cannot
//     measure says so rather than showing a plausible number.

export interface DabState {
  channel: string; centreHz: number; label: string; eid: number;
  locked: boolean; nullDepthDb: number; offsetHz: number; offsetPpm: number;
  carrierShift: number; prs: number;
  fibOk: number; fibTotal: number; fibRate: number; frames: number;
  sid: number; bitrate: number; protection: string;
  services: { sid: number; label: string; codec: string; subch: number; short?: string; pty?: number;
              slides?: boolean; kbps?: number; prot?: string; cuStart?: number; cuSize?: number;
              scids?: number; ecc?: number; dls?: string; dlsAge?: number; logoAir?: boolean; logoSlide?: boolean; ptyDyn?: boolean;
    /** FIG 0/6 + 0/21: the FM stations that are this programme and the other DAB services carrying it. */
    pi?: number[]; fm?: number[]; linkSids?: number[]; linkHard?: boolean; linkActive?: boolean; piImplicit?: boolean }[];
  /** The playing service's codec as DECODED (super frame / Layer II header), not as promised. */
  /** True while the server is still measuring the decoder's real output rate (the start-up glide). */
  aacSettling?: boolean;
  codecDetail?: string; audioRateHz?: number; coreRateHz?: number; sbr?: boolean; ps?: boolean; audioCh?: number;
  ecc?: number; cif?: number; mci?: boolean; nsvc?: number;
  /** Transmitter Identification: which transmitters of the SFN the null symbol says we hear. */
  tii?: { main: number; sub: number; db: number; site?: string; area?: string; km?: number; lat?: number; lon?: number; ambiguous?: boolean }[];
  /** Signal analysis: MER (dB), raw MSC bit error rate, PRS impulse response (128 x uint8 dB
   *  bins, 4 samples each, 255 = peak), constellation (192 int8 x,y pairs, ideal radius 60). */
  mer?: number; mscBer?: number; irPeak?: number; ir?: number[]; iq?: number[];
  /* ★ The diagnostics the server has sent all along and the client never typed. `rfCentreHz` is
   *  what the RADIO is on, beside `centreHz` which is what was asked for — the one pair that told
   *  the 2026-09-04 bring-up apart from a dead decoder (see vibe_dab_service.h). */
  rfCentreHz?: number; rfRateHz?: number;
  prsRef?: number; prsRatio?: number; erased?: number; reacquires?: number; syncJumps?: number;
  rsFixed?: number; rsLost?: number; sfOk?: number; sfTried?: number; sfFireBad?: number;
  /** ★ The decoder's PCM output counter (cumulative, DAB+ and MP2 alike) — the digital flow. */
  pcmPushed?: number;
  mp2In?: number; mp2Bad?: number; mp2Concealed?: number; mp2NoSync?: number;
  dls?: string; dlsCrcOk?: number; dlsCrcFail?: number;
  /** ★ DL Plus (TS 102 980): the station's own division of the label into artist, title and the
   *  rest — DAB's RT+, keyed by name. `dlpRunning` false means the ITEM has ended (an ad break,
   *  the news) under a label the station has not cleared. */
  dlp?: Record<string, string>; dlpRunning?: boolean;
  /** ★ Announcements (FIG 0/18 and 0/19). `announce` is what is ON AIR now and relevant to the
   *  tuned service (plus any alarm, which is relevant to everyone); `announceSupport` is what this
   *  service can carry at all. Reported only — switching the audio would hijack a shared VFO. */
  announce?: { cluster: number; types: string[]; subChId: number; on: string; alarm: boolean }[];
  announceSupport?: string[];
  /** ★ The multiplex's own schedule (TS 102 371 Programme Information). `epgPi` counts the PI
   *  objects seen — zero everywhere in the UK, which is why the row says so rather than hiding. */
  epgPi?: number;
  epgNow?:  { name: string; desc: string; at: string; mins: number; lto: number };
  epgNext?: { name: string; desc: string; at: string; mins: number; lto: number };
  /** ★ Categorised slideshow (TS 101 499 5.3.5): a browsable gallery rather than one picture
   *  replacing the last. `slideAlert` 1 is an emergency warning (table 4). */
  slideCats?: { id: number; title: string; slides: { n: string; i: number }[] }[];
  slideAlert?: number; slideClick?: string;
  /** The RDS equivalents the ensemble broadcasts: clock (FIG 0/10), local offset, other blocks (0/21). */
  mjd?: number; utc?: string; lto?: number; altHz?: number[];
  /** Ofcom's licensed sites for this ensemble (nearest first when the receiver's position is known), and why the TII test failed. */
  licensed?: { site: string; area: string; code: string; km: number }[];
  /** The newest slideshow image off the air for the playing service; fetch /vibeserver/dabslide?seq=. */
  slide?: { seq: number; mime: string; bytes: number; name: string };
  motGroups?: number; motCrcFail?: number; motObjects?: number;
  spi?: { sid: number; packets: number; groups: number; crcFail: number; lost: number; dir: boolean; named: number; complete: number; logoSvcs: number };
  tiiDiag?: { comb: number; f4s: number; f45: number; frames: number };
  aacRateHz?: number; aacCh?: number; aacServerSide?: boolean;
  dropped?: number;
  /** Set (to the length snprintf wanted) when the server had to send a stub instead of the block. */
  truncated?: number;
  /** Client-side: the list is the last good one, held while the server reports none. */
  held?: boolean;
}

/**
 * ★★★ EVERY STRING OFF THE AIR GOES THROUGH HERE. THIS IS THE RDS FAULT, PRE-EMPTED.
 *
 * Stuart, 2026-09-07: "make sure we dont run into a glitch we had with RDS where broken packets
 * translated into text that broke the spectrum socket". A DAB label, a DLS segment, a DL Plus
 * tag, an EPG programme name and a slideshow category title are all attacker-adjacent in exactly
 * the same way RDS text was: they are bytes off the air, reassembled from packets that may be
 * corrupt, and a CRC that passes does not mean the CONTENT is sane. The server already bounds and
 * escapes them going into its JSON — this is the second reader, on the receiving side, because the
 * failure we are guarding against is precisely the one where a byte survived the first.
 *
 * Three things, all cheap:
 *   • C0/C1 CONTROL CHARACTERS OUT. DAB's own charsets use 0x0A/0x0B as segment breaks and 0x1F as
 *     a soft hyphen; the rest are noise, and a lone 0x00 or 0x1B in a React string is the shape of
 *     bug that took RDS down.
 *   • UNPAIRED SURROGATES OUT. A truncated UTF-8 sequence can decode to half a pair, which throws
 *     inside JSON.stringify — i.e. it breaks the NEXT message to leave this device, which is how
 *     the RDS fault reached the socket rather than staying on screen.
 *   • BOUNDED. 128 characters is longer than any legal DAB label or DLS line (16 / 128), so a
 *     length beyond it is corruption, not content, and it must not be allowed to grow a row.
 *
 * ★ Returns '' rather than throwing: a bad label costs its own row, never the panel.
 */
export function dabSafeText(v: unknown, max = 128): string {
  if (typeof v !== 'string' || !v) return '';
  let out = '';
  for (const ch of v) {                       // by CODE POINT — a surrogate pair stays whole
    const c = ch.codePointAt(0) ?? 0;
    if (c >= 0xd800 && c <= 0xdfff) continue;  // lone surrogate (a pair never yields one here)
    if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) { out += ' '; continue; }
    out += ch;
    if (out.length >= max) break;
  }
  return out.trim();
}

/** The same gate for a number the UI will render or do arithmetic on. NaN and Infinity survive
 *  JSON.parse (as null, or as a string the server built by hand) and then poison every layout
 *  they touch — see the `"a" + + "b"` ⇒ NaN trap in the style notes. */
export function dabSafeNum(v: unknown, fallback = 0): number {
  return typeof v === 'number' && Number.isFinite(v) ? v : fallback;
}

/**
 * Turn one `dab` message into a DabState the UI can render without checking anything again.
 *
 * ★★★ THE STRINGS ARE THE WHOLE POINT (see dabSafeText). Everything that came off the air —
 * ensemble label, service labels and short labels, DLS, every DL Plus tag VALUE, EPG programme
 * names and descriptions, slideshow category titles and filenames, TII site and area names — is
 * gated here, in one pass, at the door. The NUMBERS are passed through as the server sent them:
 * they are bounded by the parsers and the panel formats them, and rewriting a measurement would
 * break the "nothing inferred" rule this panel exists to keep.
 *
 * ★★ THE ARRAYS ARE BOUNDED TOO. `services`, `tii`, `announce`, `slideCats` and `licensed` are
 * built from repeated FIG records; a corrupt length field is exactly how a list becomes ten
 * thousand entries, and the cost of that lands on the render thread rather than the parser.
 * The caps are far above anything legal (a UK multiplex carries ~20 services, the biggest ~40).
 */
export function parseDabMessage(m: Record<string, unknown>): DabState {
  const o = { ...m } as unknown as DabState;
  const cap = <T,>(v: unknown, n: number): T[] => (Array.isArray(v) ? (v.slice(0, n) as T[]) : []);

  o.channel  = dabSafeText(m.channel, 8);
  o.label    = dabSafeText(m.label, 32);
  o.protection = dabSafeText(m.protection, 16);
  if (m.codecDetail !== undefined) o.codecDetail = dabSafeText(m.codecDetail, 64);
  if (m.dls   !== undefined) o.dls   = dabSafeText(m.dls);
  if (m.utc   !== undefined) o.utc   = dabSafeText(m.utc, 32);
  if (m.slideClick !== undefined) o.slideClick = dabSafeText(m.slideClick, 256);

  if (Array.isArray(m.services)) {
    o.services = cap<DabState['services'][number]>(m.services, 64).map(s => ({
      ...s,
      label: dabSafeText(s.label, 32),
      short: s.short !== undefined ? dabSafeText(s.short, 16) : undefined,
      codec: dabSafeText(s.codec, 24),
      prot:  s.prot !== undefined ? dabSafeText(s.prot, 16) : undefined,
      dls:   s.dls  !== undefined ? dabSafeText(s.dls)      : undefined,
    }));
  } else o.services = [];

  if (m.dlp && typeof m.dlp === 'object') {
    const out: Record<string, string> = {};
    // ★ The KEY is gated as well as the value: DL Plus content types are a fixed vocabulary from
    //   TS 102 980 table 3, but the name in the message is a string we did not write.
    for (const [k, v] of Object.entries(m.dlp as Record<string, unknown>).slice(0, 16)) {
      const key = dabSafeText(k, 24);
      if (key) out[key] = dabSafeText(v);
    }
    o.dlp = out;
  }

  if (Array.isArray(m.tii)) {
    o.tii = cap<NonNullable<DabState['tii']>[number]>(m.tii, 16).map(t => ({
      ...t,
      site: t.site !== undefined ? dabSafeText(t.site, 48) : undefined,
      area: t.area !== undefined ? dabSafeText(t.area, 48) : undefined,
    }));
  }
  if (Array.isArray(m.licensed)) {
    o.licensed = cap<NonNullable<DabState['licensed']>[number]>(m.licensed, 16).map(l => ({
      ...l, site: dabSafeText(l.site, 48), area: dabSafeText(l.area, 48), code: dabSafeText(l.code, 16),
    }));
  }
  if (Array.isArray(m.announce)) {
    o.announce = cap<NonNullable<DabState['announce']>[number]>(m.announce, 16).map(a => ({
      ...a,
      types: cap<string>(a.types, 12).map(t => dabSafeText(t, 32)).filter(Boolean),
      on:    dabSafeText(a.on, 32),
    }));
  }
  if (Array.isArray(m.announceSupport)) {
    o.announceSupport = cap<string>(m.announceSupport, 12).map(t => dabSafeText(t, 32)).filter(Boolean);
  }
  if (Array.isArray(m.slideCats)) {
    o.slideCats = cap<NonNullable<DabState['slideCats']>[number]>(m.slideCats, 16).map(c => ({
      ...c,
      title: dabSafeText(c.title, 48),
      slides: cap<{ n: string; i: number }>(c.slides, 64).map(sl => ({ ...sl, n: dabSafeText(sl.n, 64) })),
    }));
  }
  for (const k of ['epgNow', 'epgNext'] as const) {
    const e = m[k] as DabState['epgNow'] | undefined;
    if (e) o[k] = { ...e, name: dabSafeText(e.name, 96), desc: dabSafeText(e.desc, 256), at: dabSafeText(e.at, 32) };
  }
  if (m.slide && typeof m.slide === 'object') {
    const sl = m.slide as NonNullable<DabState['slide']>;
    o.slide = { ...sl, mime: dabSafeText(sl.mime, 48), name: dabSafeText(sl.name, 96) };
  }
  if (Array.isArray(m.ir)) o.ir = cap<number>(m.ir, 128);
  if (Array.isArray(m.iq)) o.iq = cap<number>(m.iq, 512);
  if (Array.isArray(m.altHz)) o.altHz = cap<number>(m.altHz, 32);
  return o;
}
