// test_adsb_feed.ts — V12 beta96 the app's server-screen copy of "Share ADS-B with flight trackers" (src/utils/adsbFeed.ts):
// the same answers as the Setup page's afRowsFrom / afBlob on the shared fixture (scripts/testdata/adsb-feed-cases.json,
// which scripts/test-setup-adsb-feed.mjs runs through the page), off by default, the gate, the status line, UUIDs.
import fs from 'node:fs';
import { ADSB_FEED_PRESETS, adsbFeedOffered, adsbFeedStatusLine, collectAdsbFeed, newUuid, parseAdsbFeed } from '../src/utils/adsbFeed';
let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const page = fs.readFileSync('android/app/src/main/cpp/vibe_setup_page.h', 'utf8');
const pagePresets = (page.match(/host: "(feed\.[a-z.]+)", port: (\d+)/g) || []).join('|');
ok(pagePresets === ADSB_FEED_PRESETS.map((p) => `host: "${p.host}", port: ${p.port}`).join('|'), 'the presets match the Setup page');
const m0 = parseAdsbFeed('');
ok(!m0.serve && !m0.push && !m0.open && m0.port === 30005 && m0.rows.every((r) => !r.on) && collectAdsbFeed(m0).blob === '', 'off by default; nothing stored');
const fx = JSON.parse(fs.readFileSync('scripts/testdata/adsb-feed-cases.json', 'utf8'));
for (const c of fx) {
  const m = parseAdsbFeed(c.stored || '');
  for (const [i, p] of Object.entries(c.edit || {})) Object.assign(m.rows[Number(i)], p as object);
  m.serve = c.serve; m.port = c.port; m.open = c.open; m.push = c.push;
  const got = collectAdsbFeed(m);
  ok(c.blob !== undefined ? got.blob === c.blob : (got.error || '').includes(c.error), `fixture "${c.name}": ${got.blob ?? got.error}`);
}
ok(adsbFeedOffered('rtl') && adsbFeedOffered('sdrplay') && !adsbFeedOffered('airspyhf') && !adsbFeedOffered('icom'), 'drawn only where the radio can run ADS-B');
let seed = 1; const rand = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647; };
const u = newUuid(rand);
ok(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u) && newUuid(rand) !== u, `a new UUID is a version-4 UUID: ${u}`);
ok(adsbFeedStatusLine({ on: false }) === '' && adsbFeedStatusLine({ on: true, line: 'feed.adsb.lol: connected, 12 messages', frames: 40 })
   === 'Now: feed.adsb.lol: connected, 12 messages · 40 messages decoded since the server started', 'the status line');
console.log(`test_adsb_feed: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
