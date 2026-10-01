/**
 * test-web-no-desktop-bar.mjs — the retired DESKTOP CONTROL BAR stays gone from the BUILT page.
 *
 *   node scripts/build-web.mjs && node scripts/test-web-no-desktop-bar.mjs
 *
 * ★★★ WHY. The compact control card (#mcard) is the web layout at EVERY width. The old desktop bar
 *     (#bar: VOL / MUTE / AUDIO / REC, its own pill, arrows, meter…) was display:none everywhere for
 *     months and still shipped: still found by getElementById, still wired, still styled — and twice a
 *     fix or a feature landed on the hidden copy and reached nobody (the DAB button, the web sweep
 *     pacing). Every visitor also paid for its bytes. It was deleted on 2026-10-01; this keeps it so.
 * Checks, on web/dist (what the server actually embeds and serves — never the source):
 *   1. no removed id is an element id in the shell HTML, nor a CSS selector there;
 *   2. no removed id that is distinctive enough to grep is a string literal in any built script;
 *   3. the things that USED TO LIVE in the bar and are still real are present where they live now:
 *      #linkStats in the card's status row, #searchWrap in the frequency panel, the bandwidth row in
 *      its parking space (#bwHome), the volume / mute / REC / lock / centre / fit / bookmarks controls.
 * ★ Pure file reads, no browser — silent.
 */
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dist = path.join(root, 'web/dist');
if (!existsSync(path.join(dist, 'vibesdr.html'))) {
  console.log('  FAIL web/dist/vibesdr.html missing — run node scripts/build-web.mjs first');
  process.exit(1);
}
const html = readFileSync(path.join(dist, 'vibesdr.html'), 'utf8');
const scripts = readdirSync(path.join(dist, 'vs')).filter((f) => f.endsWith('.js'))
  .map((f) => ({ f, t: readFileSync(path.join(dist, 'vs', f), 'utf8') }));

let fails = 0, oks = 0;
const ok = (c, m) => { if (c) oks++; else { fails++; console.log('  FAIL ' + m); } };

// Every id the bar carried that has no life outside it. (#linkStats and its chips, #searchWrap and
// the bandwidth row were never the bar's: they are kept, and checked for in 3.)
const REMOVED = ['bar', 'leftCol', 'tuner', 'pill', 'freq', 'freqUnit', 'freqChan', 'stereo', 'vfo', 'tuneDown',
  'stepBtn', 'tuneUp', 'demod', 'decodersBtn', 'midBlock', 'cluster', 'zoomOut', 'zoomIn', 'zoomReset', 'lockBtn',
  'centreBtn', 'menuBtn', 'chatBtn', 'chatUnread', 'vol', 'muteBtn', 'audioBtn', 'recordingsBtn', 'recBtn', 'recTime',
  'rightCol', 'sigWrap', 'sig', 'sigFill', 'sigSql', 'sigPeak', 'sigFault', 'sigLine', 'sigLabel', 'findRow',
  'bookmarksBtn'];
// Short ones ('freq', 'vol', 'sig', 'stereo', 'bar', 'pill'…) are also message fields, URL params and
// class names in the scripts, and 'freqUnit' is a saved preference's key — so only the distinctive
// ones are looked for as script literals.
const DISTINCTIVE = REMOVED.filter((id) => id.length >= 6 && !['stereo', 'freqUnit'].includes(id));
const CLASSES = ['barRow', 'mutedRed'];

// ── 1. the shell HTML ──────────────────────────────────────────────────────────────────────────
for (const id of REMOVED) {
  ok(!new RegExp(`\\bid=["']?${id}["'\\s>]`).test(html), `#${id} is still an element in the built page`);
  ok(!new RegExp(`#${id}(?![\\w-])`).test(html), `#${id} is still a CSS selector in the built page`);
}
for (const c of CLASSES) ok(!new RegExp(`\\b${c}\\b`).test(html), `.${c} is still in the built page`);

// ── 2. the built scripts ──────────────────────────────────────────────────────────────────────
for (const id of [...DISTINCTIVE, ...CLASSES]) {
  const hit = scripts.find(({ t }) => new RegExp(`["'\`#]${id}["'\`\\s.:,)\\]]`).test(t));
  ok(!hit, `"${id}" is still referenced by ${hit?.f}`);
}

// ── 3. what the bar's leftovers became ─────────────────────────────────────────────────────────
const has = (re, what) => ok(re.test(html), what);
has(/id="?mLinkHost"?><span id="?linkStats"?>/, '#linkStats is written inside the card\'s #mLinkHost');
has(/id="?mChipHost"?>(<!--[\s\S]*?-->)?<span id="?initChip"?/, '#initChip is written inside #mChipHost');
has(/id="?mSearchHost"?>\s*<div id="?searchWrap"?>/, '#searchWrap is written inside #mSearchHost (frequency panel)');
has(/id="?bwHome"? hidden>\s*<div class="?bwRow"?>/, 'the bandwidth row is parked in a hidden #bwHome');
for (const id of ['mcard', 'mAudio', 'mStep', 'mVfoUp', 'mFreq', 'mStereo', 'mPanelVol', 'mPanelMute', 'mPanelRec',
                  'mPanelRecs', 'mPanelLock', 'mPanelCentre', 'mPanelFit', 'mBookmarks', 'mCentreFloat', 'status',
                  'bwLo', 'bwHi', 'bwSync', 'search']) {
  ok(new RegExp(`\\bid=["']?${id}["'\\s>]`).test(html), `#${id} exists`);
}

console.log(fails ? `\n${fails} FAILED (${oks} ok)` : `  ok   the desktop bar is gone from the built page (${oks} checks)`);
process.exit(fails ? 1 : 0);
