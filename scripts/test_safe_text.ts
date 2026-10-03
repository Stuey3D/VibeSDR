// Untrusted station / bookmark text (src/utils/safeText) — run: node --no-warnings scripts/test_safe_text.ts
import { cleanText, cleanMode } from '../src/utils/safeText.ts';
let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) passes++; else { fails++; console.log(`✗ ${what}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); }
};
eq('plain', cleanText('BBC Radio 2'), 'BBC Radio 2');
eq('newline inside', cleanText('Heart\nFM'), 'Heart FM');
eq('NUL and tabs', cleanText('A\u0000B\tC'), 'A B C');
eq('C1 control', cleanText('X\u0085Y'), 'X Y');
eq('bidi override removed', cleanText('abc‮dcba'), 'abcdcba');
eq('zero-width removed', cleanText('Ra​dio﻿'), 'Radio');
eq('number', cleanText(42), '');
eq('object', cleanText({ toString: () => 'x' }), '');
eq('null', cleanText(null), '');
eq('only controls', cleanText('\n\r\t'), '');
eq('cap 64', cleanText('x'.repeat(500)).length, 64);
eq('no half surrogate', /[\ud800-\udbff]$/.test(cleanText('a'.repeat(63) + '😀')), false);
eq('RTL script kept', cleanText('راديو'), 'راديو');
eq('mode', cleanMode('WFM'), 'wfm');
eq('mode junk', cleanMode('am"><script>'), 'amscript');
eq('mode non-string', cleanMode(3), '');
console.log(`safe text: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
