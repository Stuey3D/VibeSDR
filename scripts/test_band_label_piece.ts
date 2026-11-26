// test_band_label_piece.ts — ★ V12 beta95 the band-plan strip's label room (src/utils/bandLabelPiece.ts; the web strip,
// web/client/src/main.ts drawBands — the app's WaterfallView rule, main 35459577a). Stuart's 225.648 MHz case.
//   npx tsx scripts/test_band_label_piece.ts
import { bandLabelPieces } from '../src/utils/bandLabelPiece';
let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
// widest first: a wide band 0..1000 (the UHF military airband) with a narrower one on it 300..500 (DAB Band III)
let p = bandLabelPieces([{ x0: 0, x1: 1000 }, { x0: 300, x1: 500 }]);
ok(JSON.stringify(p[0]) === '[500,1000]' && JSON.stringify(p[1]) === '[300,500]', `the wide band labels in its widest uncovered piece, the narrow one in its own (${JSON.stringify(p)})`);
p = bandLabelPieces([{ x0: 0, x1: 400 }, { x0: 0, x1: 400 }]);
ok(p[0][1] - p[0][0] === 0 && p[1][1] - p[1][0] === 400, 'a band wholly covered gets no room (no label), the one on top keeps it');
p = bandLabelPieces([{ x0: 0, x1: 900 }, { x0: 100, x1: 200 }, { x0: 600, x1: 700 }]);
ok(JSON.stringify(p[0]) === '[200,600]', `two bands on a wide one: the widest gap between them (${JSON.stringify(p[0])})`);
p = bandLabelPieces([{ x0: 0, x1: 300 }, { x0: 400, x1: 600 }]);
ok(JSON.stringify(p) === '[[0,300],[400,600]]', 'side by side: each its whole span');
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
