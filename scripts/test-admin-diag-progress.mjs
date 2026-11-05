// test-admin-diag-progress.mjs — the admin page's diagnostics PROGRESS BAR parser (diagProgress in web/client/src/admin.ts) on
// a real-shaped maintenance log (Stuart, 2026-11-05: the button looked broken while the file was being built). SILENT.
import fs from 'node:fs';
const src = fs.readFileSync(new URL('../web/client/src/admin.ts', import.meta.url), 'utf8');
const m = src.match(/export function diagProgress[\s\S]*?\n}\n/)[0].replace('export ', '').replace(/: \{ pct: number; what: string \} \| null/g, '').replace('(log: string)', '(log)').replace('let best: { pct: number; what: string } | null = null;', 'let best = null;');
const diagProgress = new Function(m + '; return diagProgress;')();
const log = 'building the diagnostics file\ndiagnostics: 2% — started\ndiagnostics: 7% — versions\ndiagnostics: 40% — reading the last 24 h of the journal (the slow part on a small board)\n';
const a = diagProgress(log), b = diagProgress('nothing yet\n'), c = diagProgress(log + 'diagnostics: 100% — ready\ndiagnostics ready (812 KB)\n');
console.log(JSON.stringify(a), JSON.stringify(b), JSON.stringify(c));
process.exit(a?.pct === 40 && /^Reading/.test(a.what) && b === null && c?.pct === 100 ? 0 : 1);
