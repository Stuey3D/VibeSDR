/**
 * build-web.mjs — bundle the VibeSDR web client: one HTML shell plus its script files, all compiled
 * into the server so a phone with no filesystem can still serve every byte.
 *
 *   node scripts/build-web.mjs          -> web/dist/vibesdr.html + web/dist/vs/*.js (+ .gz, .br)
 *   node scripts/build-web.mjs --serve  -> also serve them on :8080 for dev
 *
 * ★★★ THE PAGE AND ITS SCRIPT ARE SEPARATE FILES (2026-09-30). The script used to travel INSIDE the
 *     HTML as base64 and was eval()d — 885 KB of every visit, uncacheable because the HTML must be
 *     no-store (the directory injects a host that changes). Now the HTML is a shell of ~120 KB that
 *     is fetched every time, and the script is `/vs/app-<hash>.js`, served with `immutable`: a
 *     listener's second visit costs the shell and nothing else. Every file is also pre-compressed
 *     here (gzip, and brotli for https) — the server has no compressor and needs none.
 * ★★ STILL NO FILES ON DISK AT RUN TIME. emitCppHeader() compiles every output into
 *    vibe_web_page.h, as before; the page simply makes more than one request of it now.
 */

import { build } from 'esbuild';
import { shrinkHtml } from './lib/shrink-html.mjs';
import { readFile, writeFile, mkdir, rm } from 'node:fs/promises';
import zlib from 'node:zlib';
import { createServer } from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// ★★★ CATCH THE ONE MISTAKE esbuild CANNOT. esbuild does not type-check, so an identifier used
//     OUTSIDE the scope it was declared in bundles perfectly and throws ReferenceError at run
//     time — which kills the rest of that function and nothing else. It shipped a receiver whose
//     audio and RDS worked while the waterfall and the frequency never appeared, and the page
//     looked "completely broken" for a reason no log mentioned (2026-08-08).
// ★★ Deliberately NARROW: only "Cannot find name" (TS2304/TS2552). The tree is not clean under a
//    full strict check, and a gate that always fails is a gate everyone learns to skip.
import { execFileSync } from 'node:child_process';
function typeCheck() {
  // ★ No file arguments and no -p: tsc then finds the project's own tsconfig by walking up, which
  //   is what resolves the DOM/es2022 libs. Passing a file instead makes tsc refuse outright
  //   ("tsconfig.json is present but will not be loaded if files are specified") — and the first
  //   version of this guard swallowed that refusal and reported success on code that was broken.
  let out = '';
  try {
    execFileSync('npx', ['tsc', '--noEmit'],
                 { stdio: ['ignore', 'pipe', 'pipe'],
                   cwd: new URL('../web/client', import.meta.url).pathname });
  } catch (e) {
    out = String(e.stdout || '') + String(e.stderr || '');
  }
  // ★★ NARROW ON PURPOSE: only "Cannot find name". The tree has pre-existing type complaints, and
  //    a gate that always fails is a gate everyone learns to skip. This one catches the mistake
  //    esbuild cannot see and that costs a whole screen of the app at run time.
  const bad = out.split('\n').filter((l) => /TS2304|TS2552/.test(l));
  if (bad.length) {
    console.error('\n✗ undefined identifier(s) — these throw at RUN time, and esbuild cannot see them:\n');
    for (const l of bad.slice(0, 10)) console.error('   ' + l);
    process.exit(1);
  }
}
typeCheck();

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SRC_HTML = path.join(root, 'web/client/index.html');
const ENTRY    = path.join(root, 'web/client/src/main.ts');
const OUT_DIR  = path.join(root, 'web/dist');
const OUT_HTML = path.join(OUT_DIR, 'vibesdr.html');

const TARGET = ['chrome110', 'safari16', 'firefox115'];

const DEV = process.argv.includes('--dev');
const OUT_JS = path.join(OUT_DIR, 'vs');
/** ★★ WHERE THE SCRIPTS LIVE: `/vs/`, ABSOLUTE — the same URL whichever page asked, so a listener
 *  who arrives at a multi-radio server's front door and then opens a radio (/r/<id>/) already has
 *  the code in cache, and so does one who hops between radios. Every process of a server is the
 *  same binary with the same build, and the door answers /vs/ itself (local_sdr_shim.cpp).
 *  ★ If it does not — an update restarts the door and its radios a few seconds apart, so for a
 *    moment they can carry different builds — the page's own radio has the same files under its
 *    prefix, and the loader falls back to that (BASE_JS). */
const JS_DIR = '/vs/';

/** The page's own copy of BASE_PATH (web/client/src/main.ts) — the SAME regex, on purpose: the
 *  fallback must reach the process that served the page. See JS_DIR. */
const BASE_JS = `(location.pathname.match(/^\\/r\\/[^/]+/)||[''])[0]`;

const gzip = (b) => zlib.gzipSync(b, { level: 9 });
const brotli = (b) => zlib.brotliCompressSync(b, { params: {
  [zlib.constants.BROTLI_PARAM_QUALITY]: 11,
  [zlib.constants.BROTLI_PARAM_MODE]: zlib.constants.BROTLI_MODE_TEXT,
  [zlib.constants.BROTLI_PARAM_SIZE_HINT]: b.length,
} });

async function bundle() {
  const res = await build({
    entryPoints: { app: ENTRY },
    bundle: true,
    // ★★ ESM + splitting, so a module nobody needs yet can be a separate file fetched on demand
    //    (import()) — chunk URLs resolve against the importing module's own URL, so they follow
    //    the page's /r/<id>/ prefix with no help. Every target browser runs modules.
    format: 'esm',
    splitting: true,
    outdir: OUT_JS,
    entryNames: '[name]-[hash]',
    chunkNames: 'c-[hash]',
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
    target: TARGET,
    minify: !DEV,
    sourcemap: false,
    write: false,
    metafile: true,
    legalComments: 'none',
  });
  const files = res.outputFiles.map((f) => ({ name: path.basename(f.path), bytes: Buffer.from(f.contents) }));
  const js = res.outputFiles.map((f) => f.text).join('\n');

  // ★★★ DO NOT REWRITE THE BUNDLE TEXT. The WASM Opus decoder embeds its module as a binary
  // string literal (simple-yenc) with a CRC32 over the decoded bytes, precisely so tampering
  // cannot pass silently — and it contains 206 raw NUL bytes. Escaping them to `\x00` looked
  // equivalent, built clean, and broke the decoder in the browser with `Decode failed crc32
  // validation`. The files must carry these bytes VERBATIM — which an EXTERNAL script does (the
  // NUL→U+FFFD rewrite is the HTML parser's, and applies only to script INSIDE the page), and the
  // header carries them as base64 with an explicit length.

  // A VibeServer is plain http:// on a LAN IP — NOT a secure context. Anything
  // gated on one is undefined there and throws at runtime, but works fine in dev
  // (localhost counts as secure), so it only ever fails on the real device.
  // Fail the build instead.
  // ★★★ WebCodecs belongs on this list and was missing for a week of debugging: AudioDecoder is
  // [SecureContext] too, so on http://vibeserver.local it is undefined — the client concluded the
  // browser could not do Opus, asked for uncompressed, and the server refused it. Silence, on
  // every real LAN listener, invisible from the dev Mac (loopback is a secure context AND exempt
  // from the codec policy). It is allowed ONLY behind a typeof guard, which is how audio.ts uses
  // it; the WASM decoder is the path that always works.
  const banned = [
    ['crypto.subtle', 'use src/services/vibeAuth.ts (pure-JS HMAC) instead'],
    ['randomUUID',    'use getRandomValues(); randomUUID is secure-context-only'],
  ];
  const guardedOnly = [
    ['AudioDecoder', 'WebCodecs is secure-context-only — guard with `typeof AudioDecoder === "undefined"` '
                   + 'and fall back to the WASM decoder (opus-decoder)'],
  ];
  // ★ Checked per FILE: with the bundle split, a chunk that mentions the API while its typeof
  //   guard lives in a different chunk would pass a check over the joined text.
  // ★ As a whole IDENTIFIER: the WASM decoder's own chunk is full of `WASMAudioDecoderCommon`,
  //   which contains the needle and is not WebCodecs at all.
  for (const [needle, hint] of guardedOnly) {
    // Every mention must sit next to a typeof test. Minified or not, esbuild keeps both tokens.
    const ident = new RegExp(`(?<![A-Za-z0-9_$])${needle}(?![A-Za-z0-9_$])`);
    for (const f of res.outputFiles) {
      if (ident.test(f.text) && !f.text.includes(`typeof ${needle}`)) {
        throw new Error(`unguarded secure-context-only API "${needle}" in ${path.basename(f.path)} — ${hint}`);
      }
    }
  }
  for (const [needle, hint] of banned) {
    if (js.includes(needle)) {
      throw new Error(`secure-context-only API "${needle}" in the bundle — ${hint}`);
    }
  }

  // The entry, and every chunk it imports STATICALLY — those are needed before anything runs, so
  // the page asks for them up front (modulepreload) instead of discovering them one round trip
  // at a time. Chunks reached only through import() are left for when they are wanted.
  const outKey = (name) => Object.keys(res.metafile.outputs).find((k) => path.basename(k) === name);
  const entry = files.find((f) => f.name.startsWith('app-') && f.name.endsWith('.js'));
  if (!entry) throw new Error('no app-*.js entry in the esbuild output');
  const eager = [];
  const walk = (name) => {
    if (eager.includes(name)) return;
    eager.push(name);
    for (const imp of res.metafile.outputs[outKey(name)].imports) {
      if (imp.kind === 'import-statement') walk(path.basename(imp.path));
    }
  };
  walk(entry.name);

  // ★★ THE SHIPPED PAGE CARRIES NO COMMENTS AND NO INDENTATION — see scripts/lib/shrink-html.mjs.
  //    The ★ notes stay in index.html, where they are read; a listener's link never carried them
  //    to anybody. `--dev` keeps the page as written, so a dev build can still be read in devtools.
  const src = await readFile(SRC_HTML, 'utf8');
  const html0 = DEV ? src : await shrinkHtml(src, { target: TARGET });
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

  // ★★★ THE LOADER. The first half is plain markup in the head — modulepreload for the entry and
  //     every chunk it imports statically — so the browser starts the download while the rest of
  //     the HTML is still arriving (a slow link's whole problem is time). The second half, where
  //     the bundle used to be, runs it: everything it touches has been parsed by then, as before.
  //  ★★ A MODULE THAT WILL NOT LOAD TRIES THE PAGE'S OWN RADIO NEXT (see JS_DIR), then says so. On
  //     a link that drops, a page that silently never starts is the worst outcome; a sentence and
  //     a refresh is the cure. onerror fires only for a fetch or parse failure of the module graph,
  //     never after it has run, so a fallback can never run the client twice.
  //  ★ Replacer FUNCTIONS, not strings: in a replacement string "$&" means "the matched text".
  const preload = eager.map((n) => `<link rel="modulepreload" href="${JS_DIR}${n}">`).join('');
  const run = `<script>(function(){var b=${BASE_JS},d=[${JSON.stringify(JS_DIR)}];if(b)d.push(b+${JSON.stringify(JS_DIR)});`
            + `function go(i){var s=document.createElement('script');s.type='module';s.src=d[i]+${JSON.stringify(entry.name)};`
            + `s.onerror=function(){s.remove();if(i+1<d.length)return go(i+1);var e=document.createElement('div');`
            + `e.textContent='This page did not finish loading. Refresh to try again.';`
            + `e.style.cssText='position:fixed;left:0;right:0;bottom:0;padding:12px;background:#300;color:#ffb833;font:14px monospace;text-align:center;z-index:99999';`
            + `document.body.appendChild(e)};document.body.appendChild(s)}go(0)})()</script>`;
  let out = html.replace(/<\/title>/, (m) => m + preload);
  if (out === html) throw new Error('no </title> in index.html — where does the preload go?');
  const out2 = out.replace(/<script type="module" src="\.\/src\/main\.ts"><\/script>\s*$/, () => run + '\n');
  if (out2 === out) throw new Error('script tag not found in index.html — did the tag change?');
  out = out2;

  // ★★★ THE FRONT DOOR'S COPY, STAMPED AT BUILD TIME. The door serves the same page as a radio but
  //     must hide the single-radio controls before first paint (html[data-frontdoor] in
  //     index.html; see the note at the page route in local_sdr_shim.cpp). The server used to
  //     insert this at request time — it cannot any more, because what it serves is compressed.
  //  ★ After the <meta charset>, never before: a byte ahead of it can change how the rest decodes.
  const kMeta = '<meta charset="utf-8">';
  if (!out.startsWith(kMeta)) throw new Error('the page must start with <meta charset="utf-8"> (the door stamp goes after it)');
  const door = kMeta + "<script>document.documentElement.setAttribute('data-frontdoor','1')</script>" + out.slice(kMeta.length);

  await rm(OUT_JS, { recursive: true, force: true });
  await mkdir(OUT_JS, { recursive: true });
  await writeFile(OUT_HTML, out);
  const assets = [
    { path: '', type: 'text/html; charset=utf-8', raw: Buffer.from(out, 'utf8') },
    { path: '', type: 'text/html; charset=utf-8', raw: Buffer.from(door, 'utf8') },
  ];
  for (const f of files) {
    assets.push({ path: JS_DIR + f.name, type: 'text/javascript; charset=utf-8', raw: f.bytes,
                  eager: eager.includes(f.name) });
  }
  for (const a of assets) {
    a.gz = gzip(a.raw);
    a.br = brotli(a.raw);
    if (a.gz.length >= a.raw.length) a.gz = null;
    if (a.br.length >= (a.gz ?? a.raw).length) a.br = null;
    if (a.path) {
      const p = path.join(OUT_DIR, a.path);
      await writeFile(p, a.raw);
      if (a.gz) await writeFile(p + '.gz', a.gz);
      if (a.br) await writeFile(p + '.br', a.br);
    }
  }
  const kb = (n) => (n / 1024).toFixed(1);
  for (const a of assets.filter((x) => x !== assets[1])) {
    console.log(`built ${(a.path || path.relative(root, OUT_HTML)).padEnd(32)} ${kb(a.raw.length).padStart(7)} KB`
              + `  gz ${a.gz ? kb(a.gz.length) : '-'}  br ${a.br ? kb(a.br.length) : '-'}`
              + (a.path && !a.eager ? '  (on demand)' : ''));
  }

  await emitCppHeader(assets);
  return assets;
}

/**
 * Emit the page and its scripts as a C++ header the shim compiles in, so a phone with no
 * filesystem to serve from can still hand out the whole client.
 */
async function emitCppHeader(assets) {
  // ★★★ BASE64, NOT A C STRING. The page used to be emitted as a raw string literal and served
  // with `std::string kPage(kVibeWebPage)` — i.e. strlen. The WASM Opus decoder's embedded module
  // contains NUL bytes, so the Pi served exactly 233,787 bytes of a 488,109-byte page and stopped:
  // no error at build time, none at run time, just a page that ends mid-script. Escaping the NULs
  // in the JS was the wrong end to fix it (it fails the decoder's own CRC32 — see bundle()).
  // Base64 is pure ASCII, cannot collide with a raw-string delimiter, and carries its own length,
  // so the bytes reach the browser exactly as built whatever they contain.
  // Chunked: MSVC caps string literals at 64 KB and a single 650 KB line is unreadable in a diff.
  // Adjacent string literals concatenate, so this is one literal to the compiler.
  const B64_LINE = 120;
  const literal = (buf) => {
    const b64 = buf.toString('base64');
    const lines = [];
    for (let i = 0; i < b64.length; i += B64_LINE) lines.push(`  "${b64.slice(i, i + B64_LINE)}"`);
    return lines.join('\n');
  };
  const decls = [];
  const rows = [];
  assets.forEach((a, i) => {
    const names = [];
    const lens = [];
    for (const [k, buf] of [['raw', a.raw], ['gz', a.gz], ['br', a.br]]) {
      if (!buf) { names.push('nullptr'); lens.push(0); continue; }
      const n = `kVibeWebA${i}_${k}`;
      decls.push(`static const char ${n}[] =\n${literal(buf)};`);
      names.push(n);
      lens.push(buf.length);
    }
    rows.push(`  { ${JSON.stringify(a.path)}, ${JSON.stringify(a.type)}, { ${names.join(', ')} }, { ${lens.join(', ')} } },`);
  });

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

  // PWA install needs an icon of at least 192px; 512 covers every surface (install prompt, dock,
  // task switcher) from one file, and browsers scale down happily.
  const iconBytes = await readFile(path.join(root, 'assets/vibeserver-art.png'));
  const iconArr = Array.from(iconBytes).map(b => '0x' + b.toString(16).padStart(2, '0'));
  const iconLines = [];
  for (let i = 0; i < iconArr.length; i += 16) {
    iconLines.push('  ' + iconArr.slice(i, i + 16).join(', ') + ',');
  }
  const iconCpp = `static const unsigned char kVibeIcon512[] = {\n${iconLines.join('\n')}\n};\n` +
                  `static const unsigned int kVibeIcon512Len = ${iconBytes.length};\n`;

  const N = assets.length;
  const header = `// GENERATED by scripts/build-web.mjs — DO NOT EDIT.
//
// The VibeSDR web client, compiled into the shim so the server can hand out the whole thing from a
// phone with no filesystem. Rebuild with:  node scripts/build-web.mjs
//
// Source: web/client/  — the page (${(assets[0].raw.length / 1024).toFixed(1)} KB) and ${N - 2} script file(s).
//
// ★★★ THE PAGE AND ITS SCRIPTS ARE SEPARATE ASSETS. Asset 0 is the page, asset 1 the same page
//     stamped for the front door, and the rest are the scripts it loads from /vs/ — hashed names,
//     so they are served \`immutable\` and a returning listener fetches only the page.
// ★★ EACH IN UP TO THREE ENCODINGS, compressed at build time: identity, gzip, brotli. The server
//    has no compressor; it picks the smallest one the client says it accepts.
// ★★★ Base64, because the scripts CONTAIN NUL BYTES (the WASM Opus decoder embeds its module as a
//     binary string). As a C string the page was served with strlen() and silently truncated to
//     the first one — 233 KB of 488 KB, with no error anywhere. Each body is decoded once, on first
//     use, by vibeWebAssetBody(), and knows its own length.
#pragma once

#include <mutex>
#include <string>

enum VibeWebEnc { kVibeEncIdentity = 0, kVibeEncGzip = 1, kVibeEncBrotli = 2 };

struct VibeWebAsset {
  const char* path;        // URL path below the page's base ("/vs/app-….js"); "" for the two pages
  const char* type;        // Content-Type
  const char* b64[3];      // by VibeWebEnc; nullptr = not carried in that encoding
  unsigned int len[3];     // decoded length of each
};

${decls.join('\n')}

static const VibeWebAsset kVibeWebAssets[] = {
${rows.join('\n')}
};
static const int kVibeWebAssetCount = ${N};
static const int kVibeWebPageIdx = 0;       // the receiver page
static const int kVibeWebDoorPageIdx = 1;   // the same page, stamped data-frontdoor

/** Asset \`idx\` in encoding \`enc\`, decoded once on first use and kept. Empty if not carried. */
inline const std::string& vibeWebAssetBody(int idx, int enc) {
  static std::once_flag once[${N}][3];
  static std::string body[${N}][3];
  std::call_once(once[idx][enc], [idx, enc] {
    const char* p = kVibeWebAssets[idx].b64[enc];
    if (!p) return;
    static const char kT[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    signed char rev[256];
    for (int i = 0; i < 256; i++) rev[i] = -1;
    for (int i = 0; i < 64; i++) rev[(unsigned char)kT[i]] = (signed char)i;
    std::string& out = body[idx][enc];
    out.reserve(kVibeWebAssets[idx].len[enc]);
    unsigned int acc = 0;
    int bits = 0;
    for (; *p; ++p) {
      const signed char v = rev[(unsigned char)*p];
      if (v < 0) continue;                       // '=' padding and any stray whitespace
      acc = (acc << 6) | (unsigned int)v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back((char)((acc >> bits) & 0xFF));
      }
    }
  });
  return body[idx][enc];
}

/** The asset served at \`path\` (below the page's base), or -1. The pages have no path. */
inline int vibeWebAssetFind(const std::string& path) {
  if (path.empty()) return -1;
  for (int i = 0; i < kVibeWebAssetCount; i++)
    if (path == kVibeWebAssets[i].path) return i;
  return -1;
}

/** The smallest encoding of asset \`idx\` this client accepts. Identity is always carried. */
inline int vibeWebPickEnc(int idx, bool acceptsBr, bool acceptsGzip) {
  if (acceptsBr && kVibeWebAssets[idx].b64[kVibeEncBrotli]) return kVibeEncBrotli;
  if (acceptsGzip && kVibeWebAssets[idx].b64[kVibeEncGzip]) return kVibeEncGzip;
  return kVibeEncIdentity;
}

/** The receiver page, uncompressed — exactly ${assets[0].raw.length} bytes. */
inline const std::string& vibeWebPage() { return vibeWebAssetBody(kVibeWebPageIdx, kVibeEncIdentity); }

${favCpp}
${iconCpp}
`;
  const dst = path.join(root, 'android/app/src/main/cpp/vibe_web_page.h');
  await writeFile(dst, header);
  console.log(`wrote  ${path.relative(root, dst)}  (${(Buffer.byteLength(header) / 1024).toFixed(0)} KB of source)`);
}

let built = await bundle();

if (process.argv.includes('--serve')) {
  const port = 8080;
  createServer(async (req, res) => {
    if (req.url === '/rebuild') {
      built = await bundle();
      res.writeHead(204).end();
      return;
    }
    // ★ A script: served from THIS build, whatever prefix the page was opened under.
    const m = /^(?:\/r\/[^/]+)?(\/vs\/[^?#]+)/.exec(req.url || '');
    if (m) {
      const a = built.find((x) => x.path === m[1]);
      if (!a) { res.writeHead(404).end(); return; }
      res.writeHead(200, { 'Content-Type': a.type, 'Cache-Control': 'no-store' });
      res.end(a.raw);
      return;
    }
    // ALWAYS re-bundle on load, not just with --dev. A dev server that quietly
    // serves a stale build is worse than none — you end up debugging a page that
    // no longer exists.
    built = await bundle();
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(built[0].raw);
  }).listen(port, () => {
    console.log(`dev server:  http://localhost:${port}`);
    console.log('(the page asks for the VibeServer host:port + PIN on its splash)');
  });
}
