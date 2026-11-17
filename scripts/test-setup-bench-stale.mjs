// test-setup-bench-stale.mjs — the setup page says when the saved benchmark is older than the rows this version measures
// (Stuart, 2026-11-17: the Pi 2's run was from 20 September, before DAB own station had a row). The page's REAL code.
//   · BENCH_KNOWN_ROWS names every row id the benchmark can produce (read from the C++ sources — a new row must be added);
//   · an old run lists what it is missing, in words, under "Last run";
//   · a fresh run, and rows a radio here cannot use (notRun), say nothing;
//   · DAB's own rows are not called missing where DAB was not run.
import fs from 'node:fs';

const rd = (p) => fs.readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const src = rd('android/app/src/main/cpp/vibe_setup_page.h');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const a = src.indexOf('const BENCH_KNOWN_ROWS'), b = src.indexOf('function renderBench(');
if (a < 0 || b < a) { console.log('   FAIL the stale-benchmark check was not found in the page'); process.exit(1); }
const api = new Function(`${src.slice(a, b)}\nreturn { BENCH_KNOWN_ROWS, benchStaleRows, benchStaleText };`)();
const known = new Set(api.BENCH_KNOWN_ROWS.map(([id]) => id));

// ── every row id the C++ benchmark produces is known to the page ──
const bench = rd('android/app/src/main/cpp/vibe_benchmark.h'), more = rd('android/app/src/main/cpp/vibe_benchmark_more.h');
const dabH = rd('android/app/src/main/cpp/vibe_benchmark_dab.h');
const ids = new Set();
const rule = more.slice(more.indexOf('inline RowRule ruleFor'), more.indexOf('for (const auto& e : t)', more.indexOf('inline RowRule ruleFor')));
for (const m of rule.matchAll(/\{ "([a-z0-9_]+)",\s*\{/g)) ids.add(m[1]);
const gatedList = bench.slice(bench.indexOf('The decoder rows\' own ids'), bench.indexOf('spot_*'));
for (const m of gatedList.matchAll(/"([a-z0-9_]+)"/g)) ids.add(m[1]);
for (const m of more.matchAll(/run\("([a-z0-9_]+)"/g)) ids.add(m[1]);
for (const m of dabH.matchAll(/"(dab_[a-z0-9_]+|dec_dab_[a-z0-9_]+)"/g)) ids.add(m[1]);
ok(ids.size >= 40, `the benchmark's row ids were read from the sources (${ids.size})`);
ids.delete("spot_");   // ★ the digital-spots rows are spot_<mode>, one per mode registered — not a fixed id
const unknown = [...ids].filter((id) => !known.has(id));
ok(unknown.length === 0, `every benchmark row is in BENCH_KNOWN_ROWS${unknown.length ? ' — missing: ' + unknown.join(', ') : ''}`);

// ── a September run (before own station, FreeDV, M17, aviation…) ──
const sept = { v: 1, at: 1789910402, rows: ['wfm_2400000', 'dab', 'dab_scan', 'dec_rtty', 'dec_navtex', 'dec_wefax', 'dec_sstv', 'dec_time', 'dec_ft8', 'dec_lora']
  .map((id) => ({ id, pct: 20, grade: 'green' })) };
const s = api.benchStaleRows(sept);
ok(s.includes('DAB+ own station') && s.includes('FreeDV') && s.includes('M17') && s.includes('VDL2'), `an old run lists the rows added since (${s.length})`);
ok(!s.includes('RTTY') && !s.includes('DAB+') && !s.includes('DAB+ station label scan'), '…and not the rows it measured');
const t = api.benchStaleText(sept);
ok(t.startsWith(' Measured before this version added ' + s.length + ' rows: ') && /Run the benchmark again to measure them; until then they keep their defaults\.$/.test(t),
  'the words under "Last run" (several)');

// ── one row missing: singular ──
const allButOne = { v: 1, rows: api.BENCH_KNOWN_ROWS.filter(([id]) => id !== 'dec_dab_station').map(([id]) => ({ id, grade: 'green' })) };
ok(api.benchStaleText(allButOne) === ' Measured before this version added a row for DAB+ own station. Run the benchmark again to measure it; until then it keeps its default.',
  'the words for one row');

// ── a fresh run says nothing ──
const fresh = { v: 1, rows: api.BENCH_KNOWN_ROWS.map(([id]) => ({ id, grade: 'green' })) };
ok(api.benchStaleRows(fresh).length === 0 && api.benchStaleText(fresh) === '', 'a fresh run: no note');

// ── rows a radio here cannot use are not "missing" ──
const hfOnly = { v: 1, rows: api.BENCH_KNOWN_ROWS.filter(([id]) => !/^(dab|dab_scan|dec_dab_station|adsb|dec_ais)$/.test(id)).map(([id]) => ({ id })),
  notRun: [{ id: 'dab', why: 'no radio here reaches Band III' }, { id: 'adsb', why: '…' }, { id: 'dec_ais', why: '…' }] };
ok(api.benchStaleRows(hfOnly).length === 0, 'an HF-only box: DAB, its own station and ADS-B not run are not called missing');
ok(api.benchStaleRows(null).length === 0 && api.benchStaleText(null) === '', 'no run at all: no note (the page says "Never run")');

// ── wired in, UK English ──
ok(/when\.textContent = "Last run " .* \+ benchStaleText\(BENCH\);/.test(src), 'the note follows "Last run"');
ok(!/\b(color|center|analyze)\b/i.test(src.slice(a, b)), 'UK English');

console.log(fail ? `FAIL  setup bench stale: ${fail} failed` : 'ok  setup bench stale');
process.exit(fail ? 1 : 0);
