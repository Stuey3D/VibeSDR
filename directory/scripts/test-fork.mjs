// Fork labelling (forkOf in src/index.js): what a fork says about itself is shown, checked like `flavour`.
//   node directory/scripts/test-fork.mjs
import assert from 'node:assert/strict';
const { forkOf } = await import(new URL('../src/index.js', import.meta.url));

// An official build sends nothing and is not a fork.
assert.deepEqual(forkOf({ flavour: 'VibeServer Lite', version: '11.0.0' }), {});
// A fork, in full.
assert.deepEqual(forkOf({ forkName: 'Lite+', forkVersion: '1.2~b3', forkUrl: 'https://example.org/liteplus' }),
  { forkName: 'Lite+', forkVersion: '1.2~b3', forkUrl: 'https://example.org/liteplus' });
// Non-Latin names are fine (OWRX servers carry Tibetan; forks may too).
assert.equal(forkOf({ forkName: 'བོད་ཡིག SDR' }).forkName, 'བོད་ཡིག SDR');
assert.equal(forkOf({ forkName: 'हिन्दी रेडियो' }).forkName, 'हिन्दी रेडियो');
assert.equal(forkOf({ forkName: 'Ραδιόφωνο' }).forkName, 'Ραδιόφωνο');
// An official name is not a fork name, in any case.
for (const n of ['VibeServer', 'vibeserver lite', 'VibeServer  inside  VibeSDR', 'VIBESDR']) assert.deepEqual(forkOf({ forkName: n }), {}, n);
// Markup, controls, length, empty: dropped.
for (const n of ['<b>x</b>', 'a"b', 'x', '', ' ', 'a‮b', 'a'.repeat(41), 42, null]) assert.deepEqual(forkOf({ forkName: n }), {}, String(n));
// A bad version or link is dropped, the name kept.
assert.deepEqual(forkOf({ forkName: 'Lite+', forkVersion: '1.2 <script>', forkUrl: 'javascript:alert(1)' }),
  { forkName: 'Lite+', forkVersion: '', forkUrl: '' });
assert.equal(forkOf({ forkName: 'Lite+', forkUrl: 'https://a.b/"onmouseover=x' }).forkUrl, '');
assert.equal(forkOf({ forkName: 'Lite+', forkUrl: 'https://' + 'a'.repeat(200) }).forkUrl, '');
// Version / link without a name mean nothing.
assert.deepEqual(forkOf({ forkVersion: '1.0', forkUrl: 'https://x.org' }), {});
console.log('fork labelling: ok');
