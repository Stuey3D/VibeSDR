/**
 * ★★ CONTROL COLOURS — the web client's answer to the app's faceplates, kept deliberately small.
 *    Stuart, 2026-09-30: "I don't think porting all this visual work to the web client is worth it as we
 *    want the web client to be light and functional across all browsers when served from all server
 *    types. However so that the web client isn't left behind maybe we offer some simple colour choices
 *    for the controls split as follows: Background · Button colour · Font colour · Transparency so that a
 *    user can make the controls solid if they want. Saved in the same way as the display settings so it
 *    can travel between servers."
 *
 * ★★★ THE DEFAULT IS TODAY, TO THE PIXEL. Nothing here sets a CSS property until the listener changes
 *     something: every rule in index.html reads `var(--x, <today's literal>)`, and at the defaults this
 *     module REMOVES every variable, so the fallback literal is what renders. No colour arithmetic can
 *     drift the default look, because at the default no arithmetic reaches the page.
 * ★★ PURE, apart from applyControlLook(). Colour parsing, contrast and the variable map are plain data so
 *    scripts/test-web-control-colours.mjs can check them without a browser.
 * ★★ NO color-mix(), NO calc() inside rgba(): old Android WebViews and older Safari do not have them.
 *    Every value is computed here and handed to CSS as a finished colour string.
 * ★ Four prefs, all in VIEW_KEYS (portable.ts), so they travel between *.vibeserver.vibesdr.net servers
 *   exactly as the palette and the needle colour do: ctlBg, ctlBtn, ctlFont (a '#rrggbb' string, or ''
 *   for "today's"), ctlSolid (0 = today's glass … 100 = fully solid).
 */

export type RGB = [number, number, number];

/** Today's literals — the fallbacks in index.html, as data. Keep the two in step. */
export const TODAY = {
  card: { rgb: [10, 10, 10] as RGB, a: 0.92 },          // #mcard
  btn:  { rgb: [20, 10, 0] as RGB, a: 0.75 },           // --btn-bg
  font: '#ffb833',                                      // --btn-text (buttons, dial glyphs)
  status: '#c9922e',                                    // #mStatus
};

export interface Swatch { name: string; hex: string }
/** '' = today's colour. First in every list, so the default is always one tap away. */
export const BG_SWATCHES: Swatch[] = [
  { name: 'Default', hex: '' }, { name: 'Black', hex: '#000000' }, { name: 'Charcoal', hex: '#1e1e1e' },
  { name: 'Navy', hex: '#0a1628' }, { name: 'Forest', hex: '#0c1a10' }, { name: 'Slate', hex: '#28313b' },
  { name: 'Silver', hex: '#d8d8d8' },
];
export const BTN_SWATCHES: Swatch[] = [
  { name: 'Default', hex: '' }, { name: 'Black', hex: '#000000' }, { name: 'Graphite', hex: '#2c2c2c' },
  { name: 'Navy', hex: '#13294b' }, { name: 'Forest', hex: '#173a22' }, { name: 'Plum', hex: '#2e1433' },
  { name: 'Silver', hex: '#e6e6e6' },
];
/* ★★ NO RED AND NO GREEN. Red is REC, mute and a closed squelch; green is "on" and a good signal. A
 *    font in either would make those states unreadable at a glance. (The custom picker can still reach
 *    them — that is the listener's own choice, and the meaning colours do not follow the font anyway.) */
export const FONT_SWATCHES: Swatch[] = [
  { name: 'Default', hex: '' }, { name: 'White', hex: '#ffffff' }, { name: 'Silver', hex: '#c8c8c8' },
  { name: 'Ice', hex: '#8fd3ff' }, { name: 'Gold', hex: '#ffd24d' }, { name: 'Black', hex: '#111111' },
];

export const CTL_KEYS = ['ctlBg', 'ctlBtn', 'ctlFont', 'ctlSolid'] as const;

export interface ControlLook { bg: string; btn: string; font: string; solid: number }
export const DEFAULT_LOOK: ControlLook = { bg: '', btn: '', font: '', solid: 0 };

/** '#rgb' or '#rrggbb' (any case) → [r,g,b]; anything else → null. */
export function parseHex(s: unknown): RGB | null {
  if (typeof s !== 'string') return null;
  const m = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(s.trim());
  if (!m) return null;
  let h = m[1];
  if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
  return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
}
export function toHex(c: RGB): string {
  return '#' + c.map((v) => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0')).join('');
}
function normHex(s: unknown): string { const c = parseHex(s); return c ? toHex(c) : ''; }

/** What the page should show, from prefs(). Anything unreadable is today's value — a bad stored value
 *  must never be what decides whether the controls can be seen.
 *  ★★ `autoSolid` is used ONLY when the listener has never chosen a transparency (no number stored —
 *     null, from RESET, means the same). A stored choice always wins, whatever the browser looks like. */
export function readLook(p: Record<string, unknown>, autoSolid = 0): ControlLook {
  const s = p.ctlSolid;
  return {
    bg: normHex(p.ctlBg), btn: normHex(p.ctlBtn), font: normHex(p.ctlFont),
    solid: typeof s === 'number' && Number.isFinite(s) ? Math.max(0, Math.min(100, Math.round(s))) : autoSolid,
  };
}

/** What the browser says about itself — the inputs to solidByDefault(), gathered by browserSolidDefault(). */
export interface BrowserSignals {
  reducedTransparency: boolean;       // matchMedia('(prefers-reduced-transparency: reduce)')
  backdropFilter: boolean;            // CSS.supports backdrop-filter OR -webkit-backdrop-filter
  deviceMemory?: number;              // navigator.deviceMemory (GB; Chromium only)
  cores?: number;                     // navigator.hardwareConcurrency
  ua: string;
}
/**
 * ★★ AN OLD DEVICE STARTS SOLID (Stuart, 2026-09-30): with no choice stored, a browser that says it is
 *    small or old gets TRANSPARENCY EFFECTS OFF before anything see-through can slow it down. Anything we
 *    cannot read is NOT a reason to downgrade — every signal must say so positively.
 *  ★ Only the DEFAULT. It is never stored (see readLook): the listener's own choice, once made, wins.
 */
export function solidByDefault(b: BrowserSignals): boolean {
  if (b.reducedTransparency) return true;
  if (!b.backdropFilter) return true;
  if (typeof b.deviceMemory === 'number' && b.deviceMemory > 0 && b.deviceMemory <= 2) return true;
  if (typeof b.cores === 'number' && b.cores > 0 && b.cores <= 2) return true;
  // iOS / iPadOS below 15. (An iPad asking for the desktop site says "Macintosh" — unknown, so left alone.)
  const ios = /\b(?:iPhone|iPad|iPod)\b.*?\bOS (\d+)[_.]\d/.exec(b.ua);
  if (ios && Number(ios[1]) < 15) return true;
  // Android below 9. Chrome's reduced UA reports "Android 10; K" on everything newer, which reads as new.
  const and = /\bAndroid (\d+)(?:[.;)\s]|$)/.exec(b.ua);
  if (and && Number(and[1]) < 9) return true;
  return false;
}

let autoSolidCache: number | null = null;
/** 100 (solid) or 0 (today's glass), from this browser — read once per page. */
export function browserSolidDefault(): number {
  if (autoSolidCache !== null) return autoSolidCache;
  let solid = false;
  try {
    const nav = navigator as Navigator & { deviceMemory?: number };
    const sup = (p: string) => {
      try { return typeof CSS !== 'undefined' && !!CSS.supports && CSS.supports(p, 'blur(1px)'); } catch { return false; }
    };
    let reduced = false;
    try { reduced = !!window.matchMedia && window.matchMedia('(prefers-reduced-transparency: reduce)').matches; } catch { /* old engine */ }
    solid = solidByDefault({
      reducedTransparency: reduced,
      backdropFilter: sup('backdrop-filter') || sup('-webkit-backdrop-filter'),
      deviceMemory: typeof nav.deviceMemory === 'number' ? nav.deviceMemory : undefined,
      cores: typeof nav.hardwareConcurrency === 'number' ? nav.hardwareConcurrency : undefined,
      ua: nav.userAgent || '',
    });
  } catch { solid = false; }
  autoSolidCache = solid ? 100 : 0;
  return autoSolidCache;
}
export function isDefaultLook(l: ControlLook): boolean {
  return !l.bg && !l.btn && !l.font && l.solid === 0;
}

/** WCAG relative luminance. */
export function luminance(c: RGB): number {
  const f = (v: number) => { const s = v / 255; return s <= 0.03928 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4); };
  return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2]);
}
export function contrast(a: RGB, b: RGB): number {
  const la = luminance(a), lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}
/** `top` at `alpha` laid over an opaque `under`. */
export function over(top: RGB, alpha: number, under: RGB): RGB {
  return [0, 1, 2].map((i) => top[i] * alpha + under[i] * (1 - alpha)) as RGB;
}
/** 0 = today's alpha, 100 = opaque, straight line between. */
export function alphaFor(a0: number, solid: number): number {
  return a0 + (1 - a0) * Math.max(0, Math.min(100, solid)) / 100;
}
export function rgba(c: RGB, a: number): string {
  return a >= 1 ? `rgb(${c.join(',')})` : `rgba(${c.join(',')},${Math.round(a * 1000) / 1000})`;
}

/** WCAG's 3:1 — the floor for UI components and large text, and these are both. */
export const MIN_CONTRAST = 3;
/** The font colour to actually draw on `bg`: `fg` itself when it reads (≥ 3:1), otherwise the smallest
 *  step towards white or black that does. One of the two always gets there (≥ 4.58:1 on any colour), so
 *  the controls can never be drawn unreadable, whatever was picked. */
export function readableOn(fg: RGB, bg: RGB, min = MIN_CONTRAST): { rgb: RGB; nudged: boolean } {
  if (contrast(fg, bg) >= min) return { rgb: fg, nudged: false };
  const best = (to: RGB): { rgb: RGB; t: number } | null => {
    for (let t = 0.05; t <= 1.0001; t += 0.05) {
      const c = over(to, t, fg).map(Math.round) as RGB;
      if (contrast(c, bg) >= min) return { rgb: c, t };
    }
    return null;
  };
  const w = best([255, 255, 255]), k = best([0, 0, 0]);
  const pick = !w ? k : !k ? w : (w.t <= k.t ? w : k);
  return { rgb: pick ? pick.rgb : (luminance(bg) > 0.18 ? [0, 0, 0] : [255, 255, 255]), nudged: true };
}

/**
 * ★★★ EVERY PANEL OVER THE WATERFALL, not just the card (Stuart: "that should make every control
 *     solid"). Each one keeps its OWN colour and today's alpha; TRANSPARENCY only moves the alpha up to
 *     1. Colours stay with the card — the panels are full of amber text and meaning colours that were
 *     never designed against a user's background.
 *  ★ The var names are what index.html (and tutorial.ts / main.ts inline styles) read, with today's
 *    literal as the fallback. Add a panel here AND route its rule, or it simply stays glass.
 */
export const OVERLAYS: { v: string; rgb: RGB; a: number; what: string }[] = [
  { v: '--ov-panel98', rgb: [8, 6, 2],   a: 0.98,  what: 'menu, bookmarks, decoders, recordings, audio, frequency, chat panels; search results' },
  { v: '--ov-panel96', rgb: [8, 6, 2],   a: 0.96,  what: 'decoder / RDS / DAB box, tune-gap message, tutorial' },
  { v: '--ov-panel92', rgb: [8, 6, 2],   a: 0.92,  what: 'VTS station bar' },
  { v: '--ov-chip',    rgb: [10, 10, 10], a: 0.94, what: 'centre-on-VFO and gain-stuck chips' },
  { v: '--ov-admin',   rgb: [6, 5, 2],   a: 0.985, what: 'server admin page' },
  { v: '--ov-health',  rgb: [8, 12, 8],  a: 0.86,  what: 'server health pill' },
  { v: '--ov-vol',     rgb: [8, 12, 8],  a: 0.92,  what: 'wheel-volume popover' },
  { v: '--ov-warn',    rgb: [40, 10, 0], a: 0.94,  what: 'radio fault banner' },
  { v: '--ov-idle',    rgb: [8, 6, 1],   a: 0.9,   what: 'are-you-still-there screen' },
  { v: '--ov-busy',    rgb: [8, 6, 1],   a: 0.92,  what: 'server busy screen' },
];
/* ★ NO BACKDROP BLUR ANYWHERE. The one the client had (the START RADIO screen) is gone — that screen is
 *  opaque now and nothing is drawn behind it — so "solid" only has alphas to move. If a blur is ever
 *  added over the waterfall, route it through a variable here and set it to 'none' at 100. */

export interface LookVars {
  /** Set on #mcard — scoped, so nothing outside the card changes. */
  card: Record<string, string>;
  /** Set on <html> — the panels' alphas. */
  root: Record<string, string>;
  /** A user-facing note when a font colour had to be adjusted to stay readable, else ''. */
  warn: string;
}

/** Every variable, as data. At the default look both maps are EMPTY: the page renders its literals. */
export function lookVars(l: ControlLook): LookVars {
  const card: Record<string, string> = {};
  const root: Record<string, string> = {};
  if (isDefaultLook(l)) return { card, root, warn: '' };

  // The waterfall behind is mostly dark, so the glass is judged over black.
  const black: RGB = [0, 0, 0];
  const cardRgb = parseHex(l.bg) ?? TODAY.card.rgb;
  const btnRgb = parseHex(l.btn) ?? TODAY.btn.rgb;
  const cardA = alphaFor(TODAY.card.a, l.solid), btnA = alphaFor(TODAY.btn.a, l.solid);
  const cardSeen = over(cardRgb, cardA, black);
  const btnSeen = over(btnRgb, btnA, cardSeen);

  if (l.bg || l.solid) card['--ctl-card-bg'] = rgba(cardRgb, cardA);
  if (l.btn || l.solid) card['--btn-bg'] = rgba(btnRgb, btnA);

  let warn = '';
  if (l.font || l.bg || l.btn) {
    const f = parseHex(l.font || TODAY.font)!;
    const s = parseHex(l.font || TODAY.status)!;
    const onBtn = readableOn(f, btnSeen), onCard = readableOn(f, cardSeen), st = readableOn(s, cardSeen);
    card['--btn-text'] = toHex(onBtn.rgb);
    card['--ctl-glyph'] = toHex(onCard.rgb);
    card['--ctl-status'] = toHex(st.rgb);
    if (onBtn.nudged || onCard.nudged || st.nudged) {
      warn = l.font
        ? 'That font colour is too close to the background to read, so it has been adjusted.'
        : 'The default font would be hard to read on these colours, so it has been adjusted — pick a font colour to choose your own.';
    }
    if (l.font) {
      // Outlines follow the font at today's strengths (0.35 buttons, 0.22 card), or amber rims would
      // frame white text.
      const fr = onBtn.rgb;
      card['--btn-border'] = rgba(fr, 0.35);
      card['--bar-border'] = rgba(onCard.rgb, 0.22);
    }
  }

  if (l.solid) {
    for (const o of OVERLAYS) root[o.v] = rgba(o.rgb, alphaFor(o.a, l.solid));
  }
  return { card, root, warn };
}

/** Every variable either list may set — so a change can REMOVE what the last look set. */
export const CARD_VARS = ['--ctl-card-bg', '--btn-bg', '--btn-text', '--ctl-glyph', '--ctl-status', '--btn-border', '--bar-border'];
export const ROOT_VARS = OVERLAYS.map((o) => o.v);

/** Put a look on the page. Removes everything first, so the default look leaves no trace at all. */
export function applyControlLook(l: ControlLook, doc: Document = document): string {
  const vars = lookVars(l);
  const cardEl = doc.getElementById('mcard');
  const rootEl = doc.documentElement;
  for (const k of CARD_VARS) cardEl?.style.removeProperty(k);
  for (const k of ROOT_VARS) rootEl.style.removeProperty(k);
  for (const [k, v] of Object.entries(vars.card)) cardEl?.style.setProperty(k, v);
  for (const [k, v] of Object.entries(vars.root)) rootEl.style.setProperty(k, v);
  return vars.warn;
}
