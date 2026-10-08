/**
 * The canned chat's phrase pad (ChatDrawer, shared dial) and the popup / decoder key sheen —
 * src/constants/chatPad.ts and src/constants/capSheen.ts.
 *
 * Proves:
 *   • THE PAD NEVER OVERFLOWS THE DRAWER on any phone: on every size from the SE in Display Zoom
 *     (320 pt) to a 17 Pro Max, portrait and landscape, Android nav bar or iOS home indicator, the
 *     room line + pad fit the drawer's body, the transcript keeps THREAD_MIN whenever the body has
 *     room for both, and the pad never drops below a row and a half (the half row says "scroll");
 *     before, 14 phrases (~300 pt of wrap) went straight into a 313 pt drawer and phrases 7–14 were
 *     laid out below its edge, untappable;
 *   • THE CAP SHEEN HAS NO PERCENTAGES: 45 / 20 / 35 flex shares in a layer pinned by its insets —
 *     Yoga resolved `height: '45%'` against a multi-line wrap's height and every chip drew a ghost
 *     slab over its neighbours (capSheen.ts); both shells spread the one definition;
 *   • source checks: the pad is a ScrollView capped at `padMaxH`, the phrase keys carry no hit slop
 *     (6 pt gaps — a 4 pt slop on both neighbours overlapped), and neither shell's key cap holds a
 *     percentage height again.
 *
 * Run: node --no-warnings scripts/test_chat_pad.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { phrasePadMaxHeight, PAD_MIN, PAD_ROW, THREAD_MIN, PAD_SHARE } from '../src/constants/chatPad.ts';
import { CAP_SHEEN, SHEEN_HI, SHEEN_MID, SHEEN_LO } from '../src/constants/capSheen.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
}

// ── The pad on every phone ───────────────────────────────────────────────────
// ChatDrawer: DRAWER_H = min(window height × 0.55, 480) at load; the body is that less the bottom
// inset + 8, the 32 pt handle and the header (36 on default, ~42 with metal dome keys).
const LINE = 18 + 6;    // the room line (one line) and the gap under it
const CHROME = 16;      // the pad's own padding (inputRow paddingVertical 8 × 2)
type Case = { name: string; w: number; h: number; bottom: number };
const CASES: Case[] = [
  { name: 'SE Display Zoom 320×568',          w: 320, h: 568, bottom: 0 },
  { name: 'bladeL8 320×569 + 48 pt nav bar',  w: 320, h: 569, bottom: 48 },
  { name: 'iPhone 17 Pro Max 440×956',        w: 440, h: 956, bottom: 34 },
  { name: 'iPhone 17 Pro Max landscape',      w: 956, h: 440, bottom: 21 },
  { name: 'SE landscape 568×320',             w: 568, h: 320, bottom: 0 },
  { name: 'Mac window 610×700',               w: 610, h: 700, bottom: 0 },
  { name: 'iPad 1024×1366',                   w: 1024, h: 1366, bottom: 20 },
];
for (const c of CASES) for (const header of [36, 42]) for (const line of [0, LINE]) {
  const drawer = Math.min(c.h * 0.55, 480);
  const body = drawer - c.bottom - 8 - 32 - header;
  const pad = phrasePadMaxHeight(body, line, CHROME);
  const room = body - line - CHROME;
  const thread = room - pad;
  const tag = `${c.name}, header ${header}, room line ${line ? 'on' : 'off'} (body ${body.toFixed(0)})`;
  ok(`${tag}: the pad is at least a row and a half`, pad >= PAD_MIN, `pad ${pad}`);
  if (room >= PAD_MIN + THREAD_MIN) {
    ok(`${tag}: the transcript keeps ≥ ${THREAD_MIN} pt`, thread >= THREAD_MIN, `thread ${thread}`);
    ok(`${tag}: line + pad fit the body`, line + CHROME + pad <= body, `${line} + ${CHROME} + ${pad} > ${body}`);
  }
  ok(`${tag}: the pad takes ≤ ${PAD_SHARE * 100} % of its room (or its floor)`,
     pad <= Math.max(PAD_MIN, Math.round(room * PAD_SHARE)), `pad ${pad} room ${room}`);
}
// The narrowest phone is the one that broke: the pad must be a SCROLLER there (14 phrases wrap to
// ~8 rows ≈ 300 pt at 320 pt), not a 300 pt block.
{
  const body = Math.min(568 * 0.55, 480) - 0 - 8 - 32 - 42;
  ok('SE: 14 phrases (~8 rows) do not fit uncapped — the cap is what keeps them on screen',
     8 * PAD_ROW > phrasePadMaxHeight(body, LINE, CHROME));
}
// Monotonic: a taller drawer never gets a smaller pad.
{
  let last = 0, mono = true;
  for (let b = 40; b <= 500; b += 5) { const p = phrasePadMaxHeight(b, LINE, CHROME); if (p < last) mono = false; last = p; }
  ok('the pad cap never shrinks as the body grows', mono);
}

// ── The sheen ────────────────────────────────────────────────────────────────
ok('sheen shares are 45 / 20 / 35 and fill the cap', SHEEN_HI === 45 && SHEEN_MID === 20 && SHEEN_LO === 35
   && SHEEN_HI + SHEEN_MID + SHEEN_LO === 100);
{
  const s = CAP_SHEEN(5);
  ok('the sheen layer is pinned by its four insets', s.capSheen.position === 'absolute' && s.capSheen.left === 0
     && s.capSheen.right === 0 && s.capSheen.top === 0 && s.capSheen.bottom === 0);
  ok('no sheen style holds a percentage', !/%/.test(JSON.stringify(s)), JSON.stringify(s));
  ok('the light keeps the top corners, the shade the bottom', s.capHi.borderTopLeftRadius === 5 && s.capLo.borderBottomRightRadius === 5);
}

// ── Source checks ────────────────────────────────────────────────────────────
const read = (p: string) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');
for (const f of ['src/components/PopupShell.tsx', 'src/components/DecoderShell.tsx']) {
  const src = read(f);
  ok(`${f}: spreads CAP_SHEEN`, /\.\.\.CAP_SHEEN\(\d\)/.test(src));
  ok(`${f}: no key cap layer has a percentage height`, !/cap\w*:\s*\{[^}]*height:\s*'\d+%'/.test(src),
     (src.match(/cap\w*:\s*\{[^}]*height:\s*'\d+%'[^}]*\}/) ?? [''])[0]);
  ok(`${f}: the sheen layer is drawn`, /style=\{\w+\.capSheen\}/.test(src));
}
{
  const src = read('src/components/ChatDrawer.tsx');
  ok('ChatDrawer: the phrases sit in a ScrollView capped at padMaxH',
     /<ScrollView[^>]*maxHeight:\s*padMaxH[\s\S]*?canned!\.map[\s\S]*?<\/ScrollView>/.test(src));
  ok('ChatDrawer: padMaxH comes from phrasePadMaxHeight', /padMaxH\s*=\s*phrasePadMaxHeight\(/.test(src));
  ok('ChatDrawer: phrase dome keys have no hit slop', /<PopupKey key=\{q\.id\}[^>]*hitSlop=\{0\}/.test(src));
  ok('ChatDrawer: the drawer steps in from the side insets', /paddingLeft:\s*insets\.left/.test(src) && /paddingRight:\s*insets\.right/.test(src));
}

console.log(`test_chat_pad: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
