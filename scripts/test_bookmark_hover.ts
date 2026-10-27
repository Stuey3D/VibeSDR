/**
 * The bookmark under the pointer (src/utils/bookmarkHover.ts — GitHub #60; Stuart: "a little bookmark on the mouse
 * tooltip alongside the frequency … I do not want to crowd the waterfall with labels").
 *
 * Proves:
 *   • "over" = inside the bookmark's own saved passband, else its mode's default, else a ±2.5 kHz half-channel;
 *   • USB/LSB are one-sided (the passband is on one side of the carrier);
 *   • overlapping bookmarks: the nearest carrier wins, the rest are counted ("+N more");
 *   • the listener's own and the receiver's copy of the same bookmark are counted once;
 *   • a wide bookmark (a WFM station, a DAB block) does not hide a narrow one, and is found from its far edge;
 *   • junk (no name, NaN, zero) is ignored; an empty index and no index answer "none";
 *   • the lookup agrees with a brute-force scan over a large random list (the binary search is not missing anything);
 *   • the web's pointer label wires it (source checks).
 *
 * Run: node --no-warnings scripts/test_bookmark_hover.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { buildBookmarkHoverIndex, bookmarkAt, newBookmarkHit, hoverSpan, type HoverBookmarkIn } from '../src/utils/bookmarkHover.ts';

let fails = 0;
function ok(cond: boolean, what: string) {
  if (cond) console.log(`  ✓ ${what}`);
  else { fails++; console.log(`  ✗ ${what}`); }
}
const hit = newBookmarkHit();
const at = (idx: ReturnType<typeof buildBookmarkHoverIndex> | null, hz: number) => { bookmarkAt(idx, hz, hit); return { ...hit }; };

console.log('spans');
{
  ok(JSON.stringify(hoverSpan({ name: 'x', frequency: 1, mode: 'am', bandwidth_low: -3000, bandwidth_high: 3000 })) === '[-3000,3000]',
     'a saved passband wins');
  ok(JSON.stringify(hoverSpan({ name: 'x', frequency: 1, mode: 'am' })) === '[-5000,5000]', 'AM default ±5 kHz');
  ok(JSON.stringify(hoverSpan({ name: 'x', frequency: 1, mode: 'USB' })) === '[50,2700]', 'mode is case-blind; USB above the carrier');
  ok(JSON.stringify(hoverSpan({ name: 'x', frequency: 1, mode: 'lsb' })) === '[-2700,-50]', 'LSB below');
  ok(JSON.stringify(hoverSpan({ name: 'x', frequency: 1 })) === '[-2500,2500]', 'no mode: ±2.5 kHz');
  ok(JSON.stringify(hoverSpan({ name: 'x', frequency: 1, mode: 'am', bandwidth_low: 3000, bandwidth_high: -3000 })) === '[-5000,5000]',
     'a nonsense saved passband falls back to the mode');
}

console.log('the lookup');
{
  const user: HoverBookmarkIn[] = [
    { name: 'Radio 4 LW', frequency: 198_000, mode: 'am' },
    { name: 'FT8 40m', frequency: 7_074_000, mode: 'usb' },
    { name: 'Shannon VOLMET', frequency: 5_505_000, mode: 'usb' },
  ];
  const server: HoverBookmarkIn[] = [
    { name: 'Radio 4 LW', frequency: 198_000, mode: 'am' },          // the same one, kept on the receiver too
    { name: 'Droitwich test', frequency: 199_000, mode: 'am' },      // overlaps Radio 4 LW
    { name: 'Radio 2', frequency: 88_100_000, mode: 'wfm' },
    { name: 'Airband 118.0', frequency: 118_000_000, mode: 'am', bandwidth_low: -4500, bandwidth_high: 4500 },
    { name: 'Airband 118.0083', frequency: 118_008_333, mode: 'am', bandwidth_low: -4500, bandwidth_high: 4500 },
    { name: '', frequency: 3_000_000, mode: 'am' },
    { name: 'NaN', frequency: NaN, mode: 'am' },
    { name: 'zero', frequency: 0, mode: 'am' },
    { name: 'BBC National DAB', frequency: 225_648_000, mode: 'dab' },
    { name: 'Narrow inside DAB', frequency: 225_700_000, mode: 'nfm' },
  ];
  const idx = buildBookmarkHoverIndex(user, server);
  ok(idx.size === 9, 'junk ignored, the duplicate counted once (9 left)');
  let r = at(idx, 198_000);
  ok(r.name === 'Radio 4 LW' && r.more === 1, 'on the carrier: its name, "+1 more" for the overlapping one');
  r = at(idx, 198_900);
  ok(r.name === 'Droitwich test' && r.more === 1, 'nearer the other carrier: the other name');
  r = at(idx, 193_500);
  ok(r.name === 'Radio 4 LW' && r.more === 0, 'inside only one passband: no "+more"');
  ok(at(idx, 192_000).name === '', 'outside every passband: nothing');
  ok(at(idx, 7_075_500).name === 'FT8 40m', 'USB: inside, above the carrier');
  ok(at(idx, 7_073_000).name === '', 'USB: below the carrier is not inside');
  ok(at(idx, 88_190_000).name === 'Radio 2', 'WFM: ±100 kHz');
  r = at(idx, 118_004_200);
  ok(r.name === 'Airband 118.0083' && r.more === 1, 'airband 8.33 neighbours both cover the midpoint (nearest first)');
  ok(at(idx, 118_008_000).name === 'Airband 118.0083', 'airband: the nearer channel');
  r = at(idx, 225_700_000);
  ok(r.name === 'Narrow inside DAB' && r.more === 1, 'a narrow bookmark inside a DAB block is found, the block counted');
  ok(at(idx, 226_400_000).name === 'BBC National DAB', 'the DAB block from its far edge (its own tier)');
  ok(at(null, 198_000).name === '' && at(buildBookmarkHoverIndex([]), 198_000).name === '', 'no index / empty: nothing');
  ok(at(idx, NaN).name === '', 'NaN pointer: nothing');
}

console.log('agrees with brute force');
{
  let seed = 12345;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 2 ** 32);
  const modes = ['am', 'usb', 'lsb', 'cw', 'nfm', 'wfm', 'dab', ''];
  const list: HoverBookmarkIn[] = [];
  for (let i = 0; i < 3000; i++) {
    const m = modes[Math.floor(rnd() * modes.length)];
    const b: HoverBookmarkIn = { name: `b${i}`, frequency: Math.round(100_000 + rnd() * 30_000_000), mode: m };
    if (rnd() < 0.2) { const w = 500 + rnd() * 20000; b.bandwidth_low = -w * rnd(); b.bandwidth_high = w; }
    list.push(b);
  }
  const idx = buildBookmarkHoverIndex(list);
  let bad = 0;
  for (let k = 0; k < 4000; k++) {
    const hz = 100_000 + rnd() * 30_000_000;
    let best = '', bestD = Infinity, n = 0;
    for (const b of list) {
      const [l, h] = hoverSpan(b);
      if (hz < b.frequency + l || hz > b.frequency + h) continue;
      n++;
      const d = Math.abs(b.frequency - hz);
      if (d < bestD) { bestD = d; best = b.name; }
    }
    const r = at(idx, hz);
    const sameD = best === '' ? r.name === '' : Math.abs(r.frequency - hz) === bestD;
    if (!sameD || r.more !== Math.max(0, n - 1) || (!!best !== !!r.name)) bad++;
  }
  ok(bad === 0, `4000 random pointers over 3000 random bookmarks match a brute-force scan (${bad} differ)`);
}

console.log('the web pointer label');
{
  const main = readFileSync('web/client/src/main.ts', 'utf8');
  ok(/bookmarkAt\(bmIdx, t\.hz, bmHit\)/.test(main), 'looks up what a click there would tune to (clickTarget), as the label reads');
  ok(/if \(u !== bmUser \|\| sv !== bmServer\) \{ bmUser = u; bmServer = sv; bmIdx = buildBookmarkHoverIndex\(u, sv\); \}/.test(main),
     'the index is rebuilt only when a list is replaced — both lists, the listener\'s and the receiver\'s');
  ok(/if \(bmHit\.name !== shownBm \|\| bmHit\.more !== shownMore\)/.test(main), 'the DOM is touched only when the bookmark changes');
  const html = readFileSync('web/client/index.html', 'utf8');
  ok(/<div id="ptrFreq"[^>]*>.*<span class="bm">.*<span class="bn"><\/span><span class="bx"><\/span><\/span><\/div>/.test(html),
     'the label carries the glyph, the name and "+N more"');
  ok(/#ptrFreq\.hasBm \.bm \{ display: inline; \}/.test(html), 'shown only over a bookmark');
}

console.log(fails ? `\n✗ ${fails} failed` : '\n✓ all passed');
process.exit(fails ? 1 : 0);
