#!/usr/bin/env node
// build-website-demo.mjs — the live Buzzer on vibesdr.net (web/website-demo/demo.ts → website/assets/demo/).
//
// ★ One small bundle, no splitting: the web client's Waterfall + SignalProcessor + palette tables and the
//   demo's own FFT. The IQ loop and its metadata are copied beside it from demo/, so the site never drifts
//   from the committed capture (707c29c7).
// ★ Run after any change to the demo OR to the renderer it borrows (web/client/src/waterfall.ts, wfgl.ts,
//   src/assets/signalProcessor.ts, colormaps) — the bundle is committed, because the website is static.
import { build } from 'esbuild';
import { copyFileSync, mkdirSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT = path.join(ROOT, 'website/assets/demo');
mkdirSync(OUT, { recursive: true });

await build({
  entryPoints: [path.join(ROOT, 'web/website-demo/demo.ts')],
  bundle: true,
  format: 'iife',
  minify: true,
  target: ['es2020', 'safari15'],
  platform: 'browser',
  outfile: path.join(OUT, 'demo.js'),
  legalComments: 'none',
  logLevel: 'warning',
});
for (const f of ['buzzer.cs8', 'buzzer.json']) copyFileSync(path.join(ROOT, 'demo', f), path.join(OUT, f));
for (const f of ['demo.js', 'buzzer.cs8', 'buzzer.json']) console.log(`${f}  ${(statSync(path.join(OUT, f)).size / 1024).toFixed(1)} kB`);
