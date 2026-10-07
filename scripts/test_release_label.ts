/**
 * RELEASE_LABEL (src/constants/version.ts) against the newest VERSION_HISTORY entry in AboutOverlay
 * (2026-10-07). The diagnostics header prints "VibeSDR 11.0 RC26 (554)" so a report from a build without
 * crash capture can be told from a current one — which only works while the label is TRUE. Two hand-kept
 * copies of one fact drift (version.ts has drifted three times); this makes the drift a failure.
 *
 * Run: node --no-warnings scripts/test_release_label.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';

const about = readFileSync(new URL('../src/components/AboutOverlay.tsx', import.meta.url), 'utf8');
const ver = readFileSync(new URL('../src/constants/version.ts', import.meta.url), 'utf8');
const label = /export const RELEASE_LABEL = '([^']*)'/.exec(ver)?.[1];
const app = /export const APP_VERSION = '([^']*)'/.exec(ver)?.[1];
// ★ The first `{ v: '…'` after the declaration — comments sit between the bracket and the first entry.
const hist = about.indexOf('const VERSION_HISTORY');
const first = hist < 0 ? undefined : /\{\s*v:\s*'([^']+)'/.exec(about.slice(hist))?.[1];

let fails = 0;
const ok = (what: string, c: boolean) => { if (!c) { fails++; console.log('  FAIL ' + what); } };
ok('RELEASE_LABEL found in version.ts', label !== undefined);
ok('APP_VERSION found in version.ts', !!app);
ok('newest VERSION_HISTORY entry found', !!first);
if (label !== undefined && first && app) {
  // 'V11 RC26' ⇒ major 11, label 'RC26'; a final release ('V11.0') carries no label.
  const m = /^V(\d+)(?:\.\d+)*(?:\s+(.+))?$/.exec(first);
  ok(`newest entry "${first}" has the expected shape`, !!m);
  if (m) {
    ok(`RELEASE_LABEL '${label}' matches the newest entry "${first}"`, (m[2] ?? '') === label);
    ok(`APP_VERSION '${app}' is the same major as "${first}"`, app.split('.')[0] === m[1]);
  }
}
console.log(fails ? `test_release_label: ${fails} FAIL` : 'test_release_label: OK');
process.exit(fails ? 1 : 0);
