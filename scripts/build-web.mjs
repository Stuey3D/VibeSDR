/**
 * build-web.mjs — bundle the VibeSDR web client into ONE self-contained file.
 *
 *   node scripts/build-web.mjs          -> web/dist/vibesdr.html
 *   node scripts/build-web.mjs --serve  -> also serve it on :8080 for dev
 *
 * The output has to be a single file with no external requests: the shim serves
 * it from a phone with no filesystem to speak of, so there is nowhere to put
 * assets and no second request to make. esbuild inlines the TS (including the
 * modules imported straight out of src/ — colormaps, SignalProcessor, ADPCM),
 * and the <script> tag is replaced with the bundle.
 */

import { build } from 'esbuild';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { createServer } from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SRC_HTML = path.join(root, 'web/client/index.html');
const ENTRY    = path.join(root, 'web/client/src/main.ts');
const OUT_DIR  = path.join(root, 'web/dist');
const OUT_HTML = path.join(OUT_DIR, 'vibesdr.html');

async function bundle() {
  const res = await build({
    entryPoints: [ENTRY],
    bundle: true,
    format: 'iife',
    platform: 'browser',
    // Let the web client import the APP's modules verbatim. userBookmarks.ts is
    // pure logic (YAML/JSON parsers, kHz heuristic, merge, UberSDR-compatible
    // export) apart from its AsyncStorage load/save — so we swap that one import
    // for a localStorage shim rather than forking the file and letting the two
    // drift. Nothing else React-Native is reachable from the web entry point.
    alias: {
      '@react-native-async-storage/async-storage':
        path.join(root, 'web/client/src/shims/asyncStorage.ts'),
    },
    target: ['chrome110', 'safari16', 'firefox115'],
    minify: !process.argv.includes('--dev'),
    sourcemap: false,
    write: false,
    legalComments: 'none',
  });
  const js = res.outputFiles[0].text;

  // A VibeServer is plain http:// on a LAN IP — NOT a secure context. Anything
  // gated on one is undefined there and throws at runtime, but works fine in dev
  // (localhost counts as secure), so it only ever fails on the real device.
  // Fail the build instead.
  const banned = [
    ['crypto.subtle', 'use src/services/vibeAuth.ts (pure-JS HMAC) instead'],
    ['randomUUID',    'use getRandomValues(); randomUUID is secure-context-only'],
  ];
  for (const [needle, hint] of banned) {
    if (js.includes(needle)) {
      throw new Error(`secure-context-only API "${needle}" in the bundle — ${hint}`);
    }
  }

  const html0 = await readFile(SRC_HTML, 'utf8');
  // Inline the RDS mark as a data URI — the page must stay self-contained (the
  // shim serves it from a phone; there is nowhere to fetch an asset FROM).
  // All inlined as data URIs — the page must stay self-contained (the shim serves
  // it from a phone; there is nowhere to fetch an asset FROM).
  const dataUri = async (rel) =>
    `data:image/png;base64,${(await readFile(path.join(root, rel))).toString('base64')}`;
  // replaceAll, not replace: __FAVICON__ appears twice (icon + apple-touch-icon)
  // and replace() would leave the second one as a literal placeholder.
  const html = html0
    .replaceAll('__RDS_LOGO__', await dataUri('assets/rds-logo.png'))
    // ★ The VibeServer mark, not VibeSDR's. This page is served BY VibeServer — on a Mac, a phone
    // or a Pi — so the tab icon and the Now Playing artwork should say which thing you are
    // listening to. It already carries the family radio glyph, so it still reads as ours.
    .replaceAll('__FAVICON__', await dataUri('assets/vibeserver-favicon.png'))
    // Album art for the OS media controls. The VibeServer icon is enough on its own — it already
    // has the triangle-node inset, so the old base+inset compositing (the phone's
    // VibeStreamService.refreshArtwork recipe) has nothing left to add here, and one image cannot
    // half-load the way two could. The RDS station logo still overrides it when one is known.
    .replaceAll('__ARTWORK_BASE__',  await dataUri('assets/vibeserver-art.png'))
    .replaceAll('__ARTWORK_INSET__', await dataUri('assets/vibeserver-art.png'));
  // Replacer FUNCTION, not a string: in a replacement string "$&" means "the
  // matched text", and minified JS is full of `$` sigils — a stray `$&` spliced
  // the original <script src=...> tag back into the middle of the bundle and
  // broke the whole page. A function replacement disables all $-substitution.
  const out = html.replace(
    /<script type="module" src="\.\/src\/main\.ts"><\/script>\s*$/,
    () => `<script>\n${js}\n</script>\n`,
  );
  if (out === html) throw new Error('script tag not found in index.html — did the tag change?');

  await mkdir(OUT_DIR, { recursive: true });
  await writeFile(OUT_HTML, out);
  const kb = (Buffer.byteLength(out) / 1024).toFixed(1);
  console.log(`built ${path.relative(root, OUT_HTML)}  (${kb} KB)`);

  await emitCppHeader(out);
  return out;
}

/**
 * Emit the page as a C++ header the shim compiles in, so a phone with no
 * filesystem to serve from can still hand out the whole client from GET /.
 *
 * A C++ raw string literal is used (no escaping), so the only thing that can
 * break it is the delimiter appearing in the page — we assert it doesn't.
 */
async function emitCppHeader(html) {
  const DELIM = 'VIBEWEB';
  if (html.includes(`)${DELIM}"`)) {
    throw new Error('raw-string delimiter collides with page content');
  }
  // Safari will NOT use a data: URI favicon — it silently falls back to its default
  // arrow. So the icon is also emitted as raw bytes and served from a real URL
  // (GET /favicon.png). Tiny (~1 KB), and it keeps the page self-contained.
  const favBytes = await readFile(path.join(root, 'assets/vibeserver-favicon.png'));
  const favArr = Array.from(favBytes).map(b => '0x' + b.toString(16).padStart(2, '0'));
  const favLines = [];
  for (let i = 0; i < favArr.length; i += 16) {
    favLines.push('  ' + favArr.slice(i, i + 16).join(', ') + ',');
  }
  const favCpp = `static const unsigned char kVibeFavicon[] = {\n${favLines.join('\n')}\n};\n` +
                 `static const unsigned int kVibeFaviconLen = ${favBytes.length};\n`;

  const header = `// GENERATED by scripts/build-web.mjs — DO NOT EDIT.
//
// The VibeSDR web client, compiled into the shim so \`GET /\` can serve the whole
// thing from a phone. Rebuild with:  node scripts/build-web.mjs
//
// Source: web/client/  (${(Buffer.byteLength(html) / 1024).toFixed(1)} KB)
#pragma once

static const char* const kVibeWebPage = R"${DELIM}(${html})${DELIM}";

${favCpp}
`;
  const dst = path.join(root, 'android/app/src/main/cpp/vibe_web_page.h');
  await writeFile(dst, header);
  console.log(`wrote  ${path.relative(root, dst)}`);
}

let page = await bundle();

if (process.argv.includes('--serve')) {
  const port = 8080;
  createServer(async (req, res) => {
    if (req.url === '/rebuild') {
      page = await bundle();
      res.writeHead(204).end();
      return;
    }
    // ALWAYS re-bundle on load, not just with --dev. A dev server that quietly
    // serves a stale build is worse than none — you end up debugging a page that
    // no longer exists.
    page = await bundle();
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(page);
  }).listen(port, () => {
    console.log(`dev server:  http://localhost:${port}`);
    console.log('(the page asks for the VibeServer host:port + PIN on its splash)');
  });
}
