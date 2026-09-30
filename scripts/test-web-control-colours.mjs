/**
 * test-web-control-colours.mjs — the web client's CONTROLS colours and transparency (controlColours.ts).
 *
 *   node scripts/test-web-control-colours.mjs
 *
 * Pure logic, no browser (and so silent). Checks, as data:
 *   1. THE DEFAULT IS TODAY: an untouched look sets NO variable at all, and every var() the page reads
 *      falls back to exactly the literal the table says is today's.
 *   2. NOTHING UNREADABLE: every swatch × swatch × swatch at every transparency step draws its button
 *      text, dial glyphs and status line at ≥ 3:1 — and a clash is reported to the listener.
 *   3. SOLID IS SOLID: at 100 every panel is opaque, and no backdrop-filter exists anywhere in the client.
 *   4. MEANING COLOURS DO NOT FOLLOW THE FONT (REC, mute, squelch, "on").
 *   5. IT TRAVELS: the four keys are VIEW_KEYS, a stored '' beats a portable master, JSON round-trips.
 *   6. THE BROWSER'S DEFAULT: each signal that starts an older device solid, and a stored choice wins.
 * ★ Node strips the TypeScript itself, so this imports the real module and needs nothing installed.
 */
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  readLook, lookVars, isDefaultLook, contrast, parseHex, toHex, over, alphaFor, readableOn, solidByDefault,
  TODAY, OVERLAYS, ROOT_VARS, BG_SWATCHES, BTN_SWATCHES, FONT_SWATCHES, CTL_KEYS, DEFAULT_LOOK, MIN_CONTRAST,
} from '../web/client/src/controlColours.ts';
import { VIEW_KEYS } from '../web/client/src/portable.ts';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => readFileSync(path.join(root, p), 'utf8');
const html = read('web/client/index.html');
const mainTs = read('web/client/src/main.ts');
const tutTs = read('web/client/src/tutorial.ts');
const all = html + '\n' + mainTs + '\n' + tutTs;

let fails = 0, oks = 0;
const ok = (c, m) => { if (c) oks++; else { fails++; console.log('  FAIL ' + m); } };

// ── 1. the default is today ────────────────────────────────────────────────────────────────────
{
  const d = readLook({});
  ok(isDefaultLook(d) && JSON.stringify(d) === JSON.stringify(DEFAULT_LOOK), 'empty prefs read as the default look');
  const v = lookVars(d);
  ok(!Object.keys(v.card).length && !Object.keys(v.root).length && v.warn === '', 'default look sets no variable at all');
  ok(isDefaultLook(readLook({ ctlBg: 'navy', ctlBtn: 12, ctlFont: '#12', ctlSolid: 'x' })), 'garbage prefs read as the default');
  ok(readLook({ ctlSolid: 250 }).solid === 100 && readLook({ ctlSolid: -4 }).solid === 0, 'transparency is clamped 0..100');
  ok(readLook({ ctlBg: '#ABC' }).bg === '#aabbcc', '#rgb is normalised');
  ok(html.includes('background: var(--ctl-card-bg, rgba(10, 10, 10, 0.92))'), '#mcard falls back to rgba(10,10,10,0.92)');
  ok(TODAY.card.rgb.join() === '10,10,10' && TODAY.card.a === 0.92, 'TODAY.card matches the literal');
  ok(/--btn-bg:\s*rgba\(20,10,0,0\.75\)/.test(html) && TODAY.btn.a === 0.75, 'TODAY.btn matches :root --btn-bg');
  ok(/--btn-text:\s*#ffb833/.test(html) && TODAY.font === '#ffb833', 'TODAY.font matches :root --btn-text');
  ok(html.includes('color: var(--ctl-status, #c9922e)') && TODAY.status === '#c9922e', 'status line falls back to #c9922e');
  ok(html.includes('color: var(--ctl-glyph, var(--btn-text))'), 'dial glyphs fall back to --btn-text');
  // every overlay variable is read somewhere, and every fallback is exactly the table's colour
  for (const o of OVERLAYS) {
    const re = new RegExp(`var\\(${o.v},\\s*rgba\\(([^)]*)\\)\\)`, 'g');
    const uses = [...all.matchAll(re)];
    ok(uses.length > 0, `${o.v} is used (${o.what})`);
    for (const u of uses) {
      const n = u[1].split(',').map(Number);
      ok(n.slice(0, 3).join() === o.rgb.join() && n[3] === o.a, `${o.v} fallback rgba(${u[1]}) = table ${o.rgb}/${o.a}`);
    }
  }
  // and no --ov- variable is read that the table does not set
  for (const m of all.matchAll(/var\((--ov-[a-z0-9-]+)/g)) ok(ROOT_VARS.includes(m[1]), `${m[1]} is set by lookVars (ROOT_VARS)`);
  // ★ the panel dim: opaque and UNBLURRED at fully solid, today's value at 0 (unset), dimmer-to-opaque between
  {
    const s100 = lookVars({ bg: '', btn: '', font: '', solid: 100 }).root['--ov-panel-shadow'];
    ok(s100 === 'none', `solid 100: no dim, no blurred shadow — the waterfall stays visible around the panel (${s100})`);
    ok(lookVars({ bg: '', btn: '', font: '', solid: 0 }).root['--ov-panel-shadow'] === undefined, 'solid 0: panel dim left to the CSS fallback (today)');
    ok(/100vmax rgba\(0, ?0, ?0, ?0\.[0-9]+\)/.test(lookVars({ bg: '', btn: '', font: '', solid: 50 }).root['--ov-panel-shadow'] || ''), 'solid 50: panel dim fading out');
  }
}

// ── 2. nothing unreadable ──────────────────────────────────────────────────────────────────────
{
  ok(Math.abs(contrast([255, 255, 255], [0, 0, 0]) - 21) < 1e-9, 'contrast(white, black) = 21');
  const same = readableOn([20, 20, 20], [20, 20, 20]);
  ok(same.nudged && contrast(same.rgb, [20, 20, 20]) >= MIN_CONTRAST, 'a font equal to its background is nudged to ≥ 3:1');
  ok(!readableOn([255, 184, 51], [10, 10, 10]).nudged, "today's amber on today's card is left alone");
  let combos = 0, worst = 99;
  for (const bg of BG_SWATCHES) for (const btn of BTN_SWATCHES) for (const font of FONT_SWATCHES) for (const solid of [0, 35, 70, 100]) {
    const l = { bg: bg.hex, btn: btn.hex, font: font.hex, solid };
    const v = lookVars(l);
    if (isDefaultLook(l)) continue;
    combos++;
    const cardRgb = parseHex(bg.hex) ?? TODAY.card.rgb, btnRgb = parseHex(btn.hex) ?? TODAY.btn.rgb;
    const cardSeen = over(cardRgb, alphaFor(TODAY.card.a, solid), [0, 0, 0]);
    const btnSeen = over(btnRgb, alphaFor(TODAY.btn.a, solid), cardSeen);
    const textOnBtn = parseHex(v.card['--btn-text'] ?? TODAY.font);
    const glyph = parseHex(v.card['--ctl-glyph'] ?? TODAY.font);
    const status = parseHex(v.card['--ctl-status'] ?? TODAY.status);
    const c = Math.min(contrast(textOnBtn, btnSeen), contrast(glyph, cardSeen), contrast(status, cardSeen));
    worst = Math.min(worst, c);
    if (c < MIN_CONTRAST) ok(false, `unreadable: ${JSON.stringify(l)} → ${c.toFixed(2)}:1`);
    // a font the listener chose that had to move is SAID
    if (font.hex && toHex(textOnBtn) !== font.hex) ok(v.warn !== '', `adjusted font is reported: ${JSON.stringify(l)}`);
  }
  ok(combos > 1000, `every swatch combination checked (${combos}, worst ${worst.toFixed(2)}:1)`);
  // a custom picker colour that matches the button exactly
  const clash = lookVars({ bg: '', btn: '#336699', font: '#336699', solid: 100 });
  ok(clash.warn.length > 0 && contrast(parseHex(clash.card['--btn-text']), [0x33, 0x66, 0x99]) >= 3, 'custom clash is nudged and reported');
}

// ── 3. solid is solid ──────────────────────────────────────────────────────────────────────────
{
  const v = lookVars({ ...DEFAULT_LOOK, solid: 100 });
  ok(v.card['--ctl-card-bg'] === 'rgb(10,10,10)' && v.card['--btn-bg'] === 'rgb(20,10,0)', 'solid card and buttons are opaque, same colours');
  for (const o of OVERLAYS) ok(v.root[o.v] === `rgb(${o.rgb.join(',')})`, `solid ${o.v} is opaque`);
  ok(!('--btn-text' in v.card), 'transparency alone leaves the font untouched');
  const half = lookVars({ ...DEFAULT_LOOK, solid: 50 });
  ok(half.card['--ctl-card-bg'] === 'rgba(10,10,10,0.96)', 'half-way is half-way to opaque');
  const code = all.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '').replace(/<!--[\s\S]*?-->/g, '');
  ok(!/backdrop-filter\s*:/.test(code) && !/backdropFilter/.test(code), 'no backdrop-filter anywhere in the web client');
  ok(!/color-mix\(/.test(JSON.stringify(v)) && !/calc\(/.test(JSON.stringify(v)), 'no color-mix()/calc() handed to old WebViews');
}

// ── 4. meaning colours do not follow the font ─────────────────────────────────────────────────
{
  ok(/#mAudio\.muted, #mPanelMute\.muted \{ color: #ff3b30; border-color: #ff3b30; \}/.test(html), 'mute stays red');
  ok(/\.mBtn\.on \{ border-color: var\(--phosphor\); color: var\(--phosphor\); \}/.test(html), '"on" stays phosphor');
  ok(/#mRecTime \{[^}]*color: #ff3b30/.test(html), 'recording timer stays red');
  ok(/#mSnr\.sql \{ color: #ff3b30/.test(html), 'closed squelch stays red');
  const v = lookVars({ bg: '#000000', btn: '#000000', font: '#ffffff', solid: 0 });
  ok(!Object.keys(v.card).some((k) => /phosphor|red|green|amber/.test(k)) && !Object.keys(v.root).some((k) => /phosphor|red|green|amber/.test(k)),
     'the look never sets a meaning colour');
  for (const s of FONT_SWATCHES) {
    const c = parseHex(s.hex); if (!c) continue;
    const [r, g, b] = c;
    ok(!(r > 180 && g < 110 && b < 110) && !(g > 180 && r < 130 && b < 170), `font swatch ${s.name} is neither red nor green`);
  }
}

// ── 5. it travels ──────────────────────────────────────────────────────────────────────────────
{
  for (const k of CTL_KEYS) ok(VIEW_KEYS.includes(k), `${k} is a VIEW_KEY (travels with SAVE FOR ALL)`);
  const master = { ctlBg: '#0a1628', ctlFont: '#ffffff', ctlSolid: 60 };
  const local = { ctlBg: '', ctlSolid: null };
  const merged = JSON.parse(JSON.stringify({ ...master, ...local }));   // prefs(): master under this server
  const l = readLook(merged, 100);
  ok(l.bg === '' && l.font === '#ffffff', "a stored '' (Default here) beats the master's colour; master fills the rest");
  ok(l.solid === 100, 'a stored null transparency (RESET) means the browser default, not the master');
  const rt = readLook(JSON.parse(JSON.stringify({ ctlBg: '#1e1e1e', ctlBtn: '#2c2c2c', ctlFont: '#8fd3ff', ctlSolid: 45 })));
  ok(JSON.stringify(rt) === JSON.stringify({ bg: '#1e1e1e', btn: '#2c2c2c', font: '#8fd3ff', solid: 45 }), 'JSON round trip');
}

// ── 6. the browser's default ───────────────────────────────────────────────────────────────────
{
  const modern = { reducedTransparency: false, backdropFilter: true, deviceMemory: 8, cores: 8,
    ua: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.5 Mobile/15E148 Safari/604.1' };
  ok(!solidByDefault(modern), 'a modern iPhone keeps glass');
  ok(!solidByDefault({ reducedTransparency: false, backdropFilter: true, ua: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15' }),
     'unknown memory/cores is NOT a reason to go solid');
  ok(solidByDefault({ ...modern, reducedTransparency: true }), 'prefers-reduced-transparency → solid');
  ok(solidByDefault({ ...modern, backdropFilter: false }), 'no backdrop-filter support → solid');
  ok(solidByDefault({ ...modern, deviceMemory: 2 }) && !solidByDefault({ ...modern, deviceMemory: 4 }), 'deviceMemory ≤ 2 → solid, 4 → glass');
  ok(solidByDefault({ ...modern, cores: 2 }) && !solidByDefault({ ...modern, cores: 4 }), 'hardwareConcurrency ≤ 2 → solid, 4 → glass');
  const ua = (s) => ({ ...modern, ua: s });
  ok(solidByDefault(ua('Mozilla/5.0 (iPhone; CPU iPhone OS 14_8 like Mac OS X) AppleWebKit/605.1.15')), 'iOS 14 → solid');
  ok(!solidByDefault(ua('Mozilla/5.0 (iPhone; CPU iPhone OS 15_0 like Mac OS X) AppleWebKit/605.1.15')), 'iOS 15 → glass');
  ok(solidByDefault(ua('Mozilla/5.0 (iPad; CPU OS 12_5_7 like Mac OS X) AppleWebKit/605.1.15')), 'iPadOS 12 → solid');
  ok(solidByDefault(ua('Mozilla/5.0 (Linux; Android 8.1.0; SM-J530F) AppleWebKit/537.36 Chrome/88.0 Mobile Safari/537.36')), 'Android 8.1 → solid');
  ok(!solidByDefault(ua('Mozilla/5.0 (Linux; Android 9; Pixel 3) AppleWebKit/537.36 Chrome/120.0 Mobile Safari/537.36')), 'Android 9 → glass');
  ok(!solidByDefault(ua('Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 Chrome/131.0 Mobile Safari/537.36')), "Chrome's reduced 'Android 10; K' → glass");
  ok(!solidByDefault(ua('Mozilla/5.0 (Linux; Android 16) AppleWebKit/537.36 Chrome/140.0 Mobile Safari/537.36')), 'Android 16 is not read as Android 1 → glass');
  // ★ the stored choice always wins
  ok(readLook({}, 100).solid === 100 && readLook({}, 0).solid === 0, 'no stored choice → the browser default');
  ok(readLook({ ctlSolid: 0 }, 100).solid === 0, 'a stored GLASS beats a solid browser default');
  ok(readLook({ ctlSolid: 100 }, 0).solid === 100, 'a stored SOLID beats a glass browser default');
  ok(readLook({ ctlSolid: 40 }, 100).solid === 40, 'a stored 40% beats a solid browser default');
}

console.log(`${fails ? '✗' : '✓'} control colours: ${oks} checks passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
