/* test_stored_layers.ts — ★★★ beta96 maps2: STORED SONDES AND VOR STATIONS on the AIR & SEA map (src/services/storedLayers.ts,
 * docs/v12/ONE-MAP.md). The server writes a day card per kind (vibeserver vibe_posgal.h, pinned by vibeserver/test-posgal.cpp);
 * this is the clients' half: the card's table read back (the C++ card's own text — set POSGAL_SAMPLE to a file test-posgal
 * wrote to read that instead of the copy below), the cards picked per kind and window, merged, bounded, the loader's one-at-a-
 * time and cache, the items faded by age (fadeOf) and never live, the rows, and both pages wired (web asRender, app airSeaTrk).
 *   npx tsx scripts/test_stored_layers.ts
 */
import { readFileSync, existsSync } from 'node:fs';
import { galPosParse, storedCardPick, storedMerge, storedLoader, storedSondeItems, storedVorItems, storedSondeRows, storedVorRows, storedStatus,
         STORED_ROWS_MAX, type GalPos } from '../src/services/storedLayers';

let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { if (c) pass++; else { fail++; console.log(`FAIL ${what}`); } };
const eq = (a: unknown, b: unknown, what: string) => ok(JSON.stringify(a) === JSON.stringify(b), `${what}: ${JSON.stringify(a)} ≠ ${JSON.stringify(b)}`);

// ★ test-posgal.cpp's card, exactly as the server wrote it (POSGAL_SAMPLE=<file> reads a fresh one)
const SAMPLE_COPY = `Radiosondes heard · 2025-11-26 (UTC)
Heard by this server's SONDES mode.
Updated 10:12 UTC — at most every 5 minutes while SONDES runs. The AIR & SEA map shows them, faded by age, when SONDES is not running.

Each, most recent first (last heard · first heard · id · name · type · position · altitude m · highest m · MHz · country):
10:12:00 · 09:50:00 · S1234567 · - · RS41-SGP · 51.7000, -1.1000 · 2000 · 9000 · 403.000 · -
10:05:20 · 10:05:20 · T7654321 · - · RS41-SGP · 52.1000, 0.1500 · 300 · 300 · 403.000 · -
`;
const SAMPLE = process.env.POSGAL_SAMPLE && existsSync(process.env.POSGAL_SAMPLE) ? readFileSync(process.env.POSGAL_SAMPLE, 'utf8') : SAMPLE_COPY;
const DAY = 20418 * 86400;   // 2025-11-26 00:00 UTC (the C++ test's day)

const runAll = async () => {
  /* ── the card's table ── */
  {
    const rows = galPosParse(SAMPLE, DAY, DAY + 11 * 3600);
    eq(rows.map((r) => r.id), ['S1234567', 'T7654321'], 'the server\'s card read back: both sondes, newest first');
    eq([rows[0].last - DAY, rows[0].first - DAY], [10 * 3600 + 12 * 60, 9 * 3600 + 50 * 60], 'last and first heard on the card\'s day');
    eq([rows[0].lat, rows[0].lon, rows[0].alt, rows[0].top, rows[0].mhz, rows[0].type, rows[0].name, rows[0].cc], [51.7, -1.1, 2000, 9000, 403, 'RS41-SGP', '', ''], 'the fields; "-" is empty');
    const vor = `x\nEach, most recent first (…):\n08:00:00 · 07:00:00 · DTY/116400 · DAVENTRY · vor · 52.1800, -1.1140 · - · - · 116.400 · GB\n`;
    const v = galPosParse(vor, DAY, DAY + 9 * 3600);
    eq(v.map((r) => [r.id, r.name, r.type, r.alt, r.top, r.mhz, r.cc]), [['DTY/116400', 'DAVENTRY', 'vor', null, null, 116.4, 'GB']], 'a VOR row');
    const bad = `Each, most recent first\n`
      + `25:00:00 · 07:00:00 · A · - · t · 50.0, 1.0 · - · - · - · -\n`            // a bad time
      + `08:00:00 · 07:00:00 · B · - · t · 95.0000, 1.0000 · - · - · - · -\n`      // off the globe
      + `08:00:00 · 07:00:00 · C · - · t · 0.0000, 0.0000 · - · - · - · -\n`       // (0, 0)
      + `08:00:00 · 07:00:00 · D · - · t · 50.0000, 1.0000 · - · -\n`              // fields missing
      + `08:00:00 · 07:00:00 · <b>E · - · t · 50.0000, 1.0000 · - · - · - · -\n`   // markup in the id: cleaned
      + `08:00:00 · 07:00:00 · - · - · t · 50.0000, 1.0000 · - · - · - · -\n`      // no id
      + `23:59:00 · 23:00:00 · F · - · t · 50.0000, 1.0000 · - · - · - · -\n`;     // later than now (a clock ahead)
    eq(galPosParse(bad, DAY, DAY + 9 * 3600).map((r) => r.id), ['bE'], 'damaged rows cost only themselves; markup is cleaned');
    eq(galPosParse('no table here', DAY, DAY), [], 'no table, no rows');
    const big = 'Each, most recent first\n' + Array.from({ length: 400 }, (_, i) => `08:00:00 · 07:00:00 · X${i} · - · t · 50.0000, 1.0000 · - · - · - · -`).join('\n');
    eq(galPosParse(big, DAY, DAY + 9 * 3600).length, STORED_ROWS_MAX, 'at most 300 rows a card');
  }

  /* ── which cards ── */
  {
    const now = DAY + 12 * 3600;
    const c = (s: string, d: number) => ({ s, t: DAY - d * 86400, f: `${s}.txt` });
    const items = [c('sondes-2025-11-26', 0), c('sondes-2025-11-25', 1), c('sondes-2025-11-24', 2), c('vor-2025-11-26', 0), c('vor-2025-11-20', 6), c('vor-2025-11-10', 16),
                   { s: 'ismnodes-868-2025-11-26', t: DAY, f: 'ism.txt' }, { t: DAY, f: 'noslot.txt' } as any];
    eq(storedCardPick(items, 'sondes', now).map((x) => x.s), ['sondes-2025-11-26', 'sondes-2025-11-25'], 'sondes: today\'s and yesterday\'s cards (24 h)');
    eq(storedCardPick(items, 'vormap', now).map((x) => x.s), ['vor-2025-11-26', 'vor-2025-11-20'], 'VOR: the last 7 days\' cards');
    const many = Array.from({ length: 20 }, (_, i) => c(`vor-2025-11-${String(26 - (i % 7)).padStart(2, '0')}`, i % 7));
    ok(storedCardPick(many, 'vormap', now).length === 8, 'VOR: at most 8 cards');
  }

  /* ── merged across cards ── */
  {
    const now = DAY + 12 * 3600;
    const r = (id: string, last: number, first: number, top: number | null, lat = 50): GalPos => ({ id, name: '', type: 't', cc: '', lat, lon: 1, alt: 100, top, mhz: 403, first, last });
    const m = storedMerge([[r('S1', now - 60, now - 3000, 9000, 51)], [r('S1', now - 86000, now - 86300, 31000, 49), r('S2', now - 90000, now - 90100, 100)]], 'sondes', now);
    eq(m.map((x) => [x.id, x.lat, x.first, x.top]), [['S1', 51, now - 86300, 31000]], 'one row each: the newest position, the earliest first heard, the highest; one older than 24 h gone');
    const lots = Array.from({ length: 500 }, (_, i) => r(`V${i}`, now - i, now - i, null));
    eq(storedMerge([lots], 'vormap', now).length, STORED_ROWS_MAX, 'at most 300 in all');
  }

  /* ── the loader: one at a time, past days kept, today's and yesterday's fetched again ── */
  {
    const now = DAY + 12 * 3600;
    const cards = [0, 1, 2, 3].map((d) => ({ s: `vor-2025-11-${26 - d}`, t: DAY - d * 86400, f: `v${d}.txt` }));
    const body = (d: number) => `Each, most recent first\n08:00:00 · 07:00:00 · ID${d} · N · vor · 52.0000, -1.0000 · - · - · 116.000 · GB\n`;
    const fetched: string[] = [];
    let inFlight = 0, most = 0;
    const load = storedLoader('vormap', {
      list: async (k) => (k === 'vormap' ? cards : []),
      text: async (f) => { inFlight++; most = Math.max(most, inFlight); fetched.push(f); await new Promise((res) => setTimeout(res, 1)); inFlight--; return body(Number(f[1])); },
    });
    const a = await load(now);
    eq(a?.map((x) => x.id), ['ID0', 'ID1', 'ID2', 'ID3'], 'every card\'s stations, newest first');
    eq(most, 1, 'one card at a time');
    fetched.length = 0;
    await load(now + 120);
    eq(fetched, ['v0.txt', 'v1.txt'], 'again: today\'s and yesterday\'s fetched, the older days kept');
    const p = load(now), again = await load(now);
    eq(again, null, 'a load while one runs: null (keep what is drawn)');
    await p;
    const none = storedLoader('sondes', { list: async () => null, text: async () => null });
    eq(await none(now), null, 'no gallery: null');
  }

  /* ── drawn: faded by age, never live; rows; status ── */
  {
    const now = DAY + 12 * 3600;
    const fresh: GalPos = { id: 'S1', name: '', type: 'RS41-SGP', cc: '', lat: 51.7, lon: -1.1, alt: 12345, top: 30001, mhz: 403, first: now - 3000, last: now - 120 };
    const old: GalPos = { ...fresh, id: 'S2', last: now - 8 * 3600 };
    const it = storedSondeItems([fresh, old], now);
    eq(it.map((x) => x.key), ['sds:S1', 'sds:S2'], 'sonde items keyed sds:');
    ok(!/opacity/.test(it[0]._ic.html) && /opacity:0\.4/.test(it[1]._ic.html), 'a fresh one full, an 8-hour-old one faded to 0.4 (fadeOf)');
    ok(/Remembered by this server/.test(it[0]._cd) && /12,345 m/.test(it[0]._cd) && /highest 30,001 m/.test(it[0]._cd) && /SONDES shows them live/.test(it[0]._cd), 'the card says remembered, the altitude and the highest');
    ok(it.every((x) => (x._rk ?? 0) === 0), 'never ranked as live');
    const xss = storedVorItems([{ ...fresh, id: 'X/1', name: '<img src=x onerror=1>' }], now)[0];
    ok(!/<img/.test(xss._cd) && /&lt;img/.test(xss._cd), 'a name is escaped in the card');
    eq(xss.key, 'vos:X/1', 'VOR items keyed vos:');
    const rx = { lat: 51.5, lon: -1.0 };
    const sr = storedSondeRows([fresh], now, rx)[0];
    eq([sr.name, sr.second, sr.sp, sr.age], ['S1', 'RS41-SGP · remembered · 12,345 m', '403.000', '2 min'], 'a sonde row');
    ok(/NM$/.test(sr.range) && !!sr.brg, 'its range in NM and bearing from the receiver');
    const vr = storedVorRows([{ ...fresh, id: 'DTY/116400', name: 'DAVENTRY', type: 'vor', mhz: 116.4 }], now, null)[0];
    eq([vr.key, vr.name, vr.second, vr.range], ['vos:DTY/116400', 'DTY', '116.40 VOR · DAVENTRY · remembered', ''], 'a VOR row (no receiver: no range)');
    eq([storedStatus('sondes', 3), storedStatus('vormap', 1), storedStatus('sondes', 0)], ['3 sondes remembered (24 h)', '1 VOR station remembered (7 days)', ''], 'the status words');
  }

  /* ── both pages wired ── */
  {
    const main = readFileSync('web/client/src/main.ts', 'utf8');
    ok(/vor: vmOn \? \(vmReport \? vmReport\.heard : 0\) : vmStored\.length, sondes: sdOn \? \(sdReport \? sdReport\.sondes\.length : 0\) : sdStored\.length,/.test(main), 'web: the layers are present from the store while the mode is off');
    ok(/if \(!vmOn && on\('vor'\)\) vorItems\.push\(\.\.\.storedVorItems\(vmStored, nowStored\)\);/.test(main) && /if \(!sdOn && on\('sondes'\)\) sondeIts\.push\(\.\.\.storedSondeItems\(sdStored, nowStored\)\);/.test(main), 'web: drawn when the mode is off and the chip on');
    ok(/asStoredStart\(\);/.test(main) && /setInterval\(\(\) => \{ void asStoredPoll\(\); \}, 120_000\)/.test(main) && /document\.visibilityState !== 'visible'/.test(main.slice(main.indexOf('async function asStoredPoll'))), 'web: read on opening and every 2 min while visible');
    ok(/if \(asStoredT\) \{ clearInterval\(asStoredT\); asStoredT = null; \}/.test(main), 'web: the poll stops with the map');
    const sdr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
    ok(/if \(vmMem\) have\.vor = vmStored\.length;/.test(sdr) && /if \(sdMem\) have\.sondes = sdStored\.length;/.test(sdr), 'app: the layers are present from the store while the mode is off');
    ok(/storedVorItems\(vmStored, nowStored\)/.test(sdr) && /storedSondeItems\(sdStored, nowStored\)/.test(sdr) && /storedVorRows\(vmStored, nowStored, trkRx\)/.test(sdr), 'app: items and rows');
    ok(/if \(!airSeaUp \|\| !storedLoads\) return;/.test(sdr) && /setInterval\(\(\) => \{ void go\(\); \}, 120_000\)/.test(sdr), 'app: read while the map is up, every 2 min');
    const mo = readFileSync('src/components/MapOverlay.tsx', 'utf8');
    ok(/legShow\('vos',hvs\);legShow\('sds',hss\);/.test(mo), 'app: the KEY shows the remembered rows when they are on the map');
    const shim = readFileSync('android/app/src/main/cpp/local_sdr_shim.cpp', 'utf8');
    ok(/posGalTick\(\);/.test(shim) && /posGalStop\(\);/.test(shim) && /g_gallerySaveLive\.load\(\)/.test(shim.slice(shim.indexOf('static void posGalTick'))), 'server: the cards are fed while the mode runs, written at a clean stop, the owner\'s gallery rules');
    const gal = readFileSync('src/components/GalleryOverlay.tsx', 'utf8'), html = readFileSync('web/client/index.html', 'utf8');
    ok(/\{ id: 'sondes', label: 'SONDES' \}, \{ id: 'vormap', label: 'VOR' \}/.test(gal) && /data-kind="sondes">SONDES/.test(html) && /data-kind="vormap">VOR/.test(html), 'the gallery filters the new kinds (app and web)');
  }
};

void runAll().catch((e) => { fail++; console.log('FAIL threw', e); }).then(() => {
  console.log(`test_stored_layers: ${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
});
