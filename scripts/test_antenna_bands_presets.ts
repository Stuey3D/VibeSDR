// ★ ONE LIST OF AERIAL / FILTER PRESETS, TWO COPIES (2026-10-09): the Linux setup page's AB_RANGE_PRESETS /
// AB_FILTER_PRESETS (vibe_setup_page.h, a raw-string page with no imports) and src/utils/antennaBands.ts (Lite's and the
// app's server screen). They must say the same thing, or an owner sees different presets on Linux and on Android.
import fs from 'node:fs';
import { ANTENNA_FILTER_PRESETS, ANTENNA_RANGE_PRESETS } from '../src/utils/antennaBands.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const page = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const grab = (name: string) => {
  const a = page.indexOf(`const ${name} = `);
  const b = page.indexOf('\n];', a);
  // The page's literals use double quotes and no trailing commas inside entries: JSON once the trailing comma is gone.
  return JSON.parse(page.slice(a + `const ${name} = `.length, b + 2).replace(/,\s*\]$/, ']').replace(/,(\s*)\]/g, '$1]'));
};
ok(JSON.stringify(grab('AB_RANGE_PRESETS')) === JSON.stringify(ANTENNA_RANGE_PRESETS), 'range presets: the setup page and the app agree');
ok(JSON.stringify(grab('AB_FILTER_PRESETS')) === JSON.stringify(ANTENNA_FILTER_PRESETS), 'filter presets: the setup page and the app agree');
console.log(`test_antenna_bands_presets: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
