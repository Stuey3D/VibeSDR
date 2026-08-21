// ★★★ A BOOKMARK MUST SURVIVE THE ROUTE YOU TOOK TO THE RECEIVER.
//
// Bookmarks are scoped per instance, and that scope used to be the URL. But one server reached two
// ways is two URLs — Stuart, explaining it on GitHub #21: "I can access my Raspberry Pi server via
// demo.vibesdr.net or 192.168.86.88:48000 … to VibeSDR these are 2 completely different servers".
// So a bookmark saved on the wrist over a hostname was simply absent in the app over the LAN, and
// it looked exactly like sync failing.
//
// They are now scoped to the server's OWN id (`instance` in /vibeserver.json), which no address
// changes, and existing ones are migrated onto it the first time each route is used.
//
// ★★ THIS IS A DATA MIGRATION, WHICH IS WHY IT IS TESTED. It rewrites a field in the user's saved
//    file; the failure mode is not a wrong pixel, it is somebody's bookmarks quietly becoming
//    invisible or, worse, one server's bookmarks being adopted by another. Both cases are below.
//
// ★ Pure functions, so no app and no device needed — the same reason the visit-grouping logic is
//   tested here rather than on the bench.

import assert from 'node:assert';
import { build } from 'esbuild';

// ★ STUBBED, NOT EXTERNAL. Marking the React Native deps external leaves bare specifiers in the
//   bundle, which a data: URL module cannot resolve. Nothing under test touches storage — these are
//   pure functions over an array — so an empty module is the honest stand-in.
const stubRN = {
  name: 'stub-rn',
  setup(b) {
    b.onResolve({ filter: /^(react-native|@react-native-async-storage\/.*)$/ },
                (a) => ({ path: a.path, namespace: 'stub' }));
    b.onLoad({ filter: /.*/, namespace: 'stub' },
             () => ({ contents: 'export default {}; export const NativeModules = {};', loader: 'js' }));
  },
};

const src = new URL('../src/services/userBookmarks.ts', import.meta.url);
const out = await build({
  entryPoints: [src.pathname], bundle: true, write: false,
  format: 'esm', platform: 'neutral', logLevel: 'silent', plugins: [stubRN],
});
const mod = await import(
  'data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));
const { bookmarksForInstance, adoptInstanceScope } = mod;

let pass = 0, fail = 0;
const ok = (name, fn) => {
  try { fn(); console.log(`  ok   ${name}`); pass++; }
  catch (e) { console.log(`  FAIL ${name}\n       ${e.message}`); fail++; }
};

const bm = (name, scope) => ({
  name, frequency: 96_100_000, mode: 'wfm', bandwidth_low: -75_000, bandwidth_high: 75_000,
  group: null, comment: null, extension: null, scope,
});

const LAN  = 'http://192.168.86.88:48000';
const WAN  = 'https://demo.vibesdr.net';
const VS   = 'vs:e460399135-1f3d';

// ── What you can SEE ─────────────────────────────────────────────────────────
ok('a global bookmark shows on every receiver', () => {
  const list = [bm('Global', '')];
  assert.equal(bookmarksForInstance(list, VS).length, 1);
  assert.equal(bookmarksForInstance(list, 'http://somewhere-else').length, 1);
});

ok("another server's bookmark stays hidden", () => {
  const list = [bm('Theirs', 'http://someone-elses-server:48000')];
  assert.equal(bookmarksForInstance(list, VS, [LAN, WAN]).length, 0);
});

ok('a bookmark saved under the old URL scope is still visible after the key change', () => {
  // ★ THE UPGRADE CASE. Without the legacy list this returns 0 — which is a user opening the app
  //   to find their bookmarks gone, caused entirely by our own improvement.
  const list = [bm('Saved before the upgrade', LAN)];
  assert.equal(bookmarksForInstance(list, VS, [LAN]).length, 1);
});

// ── What gets MIGRATED ───────────────────────────────────────────────────────
ok('a bookmark on the address we are connected by adopts the identity', () => {
  const next = adoptInstanceScope([bm('Radio 2', LAN)], LAN, VS);
  assert.equal(next[0].scope, VS);
});

ok('a global bookmark is NEVER migrated', () => {
  // ★ '' means "show me everywhere" — a deliberate choice, not a URL awaiting correction. Migrating
  //   it would silently confine it to one receiver.
  const list = [bm('Global', '')];
  assert.strictEqual(adoptInstanceScope(list, '', VS), list, 'must be untouched');
  assert.equal(adoptInstanceScope(list, LAN, VS)[0].scope, '');
});

ok('a DIFFERENT server\'s bookmarks are never adopted', () => {
  const other = 'http://someone-elses-server:48000';
  const next = adoptInstanceScope([bm('Theirs', other)], LAN, VS);
  assert.equal(next[0].scope, other, 'only the route we are connected by may be adopted');
});

ok('nothing to do returns the SAME array (no write, no re-render)', () => {
  // ★ This runs on every reconnect. Returning a new array each time would persist storage and
  //   re-render for no change at all.
  const list = [bm('Radio 2', VS)];
  assert.strictEqual(adoptInstanceScope(list, LAN, VS), list);
});

ok('two routes converge on one scope', () => {
  // ★★ THE POINT OF THE WHOLE EXERCISE. Saved over the LAN, saved over the hostname; after being
  //    connected by each, both are one receiver's bookmarks and each is visible from either route.
  let list = [bm('From the LAN', LAN), bm('From the hostname', WAN)];
  list = adoptInstanceScope(list, LAN, VS);
  list = adoptInstanceScope(list, WAN, VS);
  assert.deepEqual(list.map((b) => b.scope), [VS, VS]);
  assert.equal(bookmarksForInstance(list, VS).length, 2);
});

console.log(fail ? `\nFAILED ${fail}\n` : '\npassed\n');
process.exit(fail ? 1 : 0);
