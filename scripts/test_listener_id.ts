/**
 * The app's listener id (src/services/listenerIdCore.ts) — the web client's `bid`, sent the same way.
 *
 * ★★★ Stuart, 2026-10-27: "2 radios one server is allowed unless server owner blocks the same connection using both
 *     radios exactly how the web client handles it now". The server keys its radios-per-listener rule by `bid` when a
 *     client sends one; the app sent none and was judged by its address. The real-server half is
 *     scripts/test-server-listener-id.mjs; this pins the client half:
 *       1. one id per install — made once, saved, read back on the next launch, never remade while it is valid;
 *       2. the shape the server accepts (letters, digits, '-', ≤ 64) — anything else is replaced, never sent;
 *       3. storage failing still gives this run an id (the web's own fallback);
 *       4. `&bid=` only once loaded — before that NOTHING, never an empty `bid=`;
 *       5. a factory reset forgets it, so the next connection makes a fresh one;
 *       6. SOURCE: every VibeServer socket the app opens carries it (both spectrum URLs and the audio socket), the
 *          spectrum client awaits it before connecting, and the parameter is the web's own name.
 *
 * Run: node --no-warnings scripts/test_listener_id.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { ListenerIdCore, LISTENER_ID_KEY, validListenerId } from '../src/services/listenerIdCore.ts';

let fails = 0, passes = 0;
const ok = (what: string, cond: boolean) => { if (cond) { passes++; return; } fails++; console.error(`FAIL ${what}`); };

const memStore = (init: Record<string, string> = {}) => {
  const m = { ...init };
  let sets = 0;
  return { m, sets: () => sets,
           get: async (k: string) => (k in m ? m[k] : null),
           set: async (k: string, v: string) => { sets++; m[k] = v; } };
};
let n = 0;
const make = () => `aaaaaaaa-bbbb-4ccc-8ddd-${String(++n).padStart(12, '0')}`;

// 1. Made once, saved, read back.
{
  const st = memStore();
  const a = new ListenerIdCore(st, make);
  ok('nothing before load', a.now() === '' && a.query() === '');
  const [x, y] = await Promise.all([a.load(), a.load()]);
  ok('single flight: concurrent loads agree', x === y && st.sets() === 1);
  ok('saved under its key', st.m[LISTENER_ID_KEY] === x);
  ok('query is the web parameter', a.query() === `&bid=${x}`);
  const b = new ListenerIdCore(st, make);   // the next launch
  ok('next launch reads the same id back', (await b.load()) === x && st.sets() === 1);
}
// 2. A stored value the server would discard is replaced, never sent.
for (const bad of ['', 'has space', 'x'.repeat(65), 'semi;colon', 'a&bid=b']) {
  const st = memStore({ [LISTENER_ID_KEY]: bad });
  const id = await new ListenerIdCore(st, make).load();
  ok(`bad stored value ${JSON.stringify(bad.slice(0, 12))} replaced`, id !== bad && validListenerId(id) && st.m[LISTENER_ID_KEY] === id);
}
ok('a UUID is the accepted shape', validListenerId('3f2504e0-4f89-41d3-9a0c-0305e82c3301'));
// 3. Storage that throws still gives this run an id.
{
  const broken = { get: async () => { throw new Error('db'); }, set: async () => { throw new Error('db'); } };
  const c = new ListenerIdCore(broken, make);
  const id = await c.load();
  ok('storage failure: still an id for this run', validListenerId(id) && c.query() === `&bid=${id}`);
}
// 5. Factory reset.
{
  const st = memStore();
  const d = new ListenerIdCore(st, make);
  const before = await d.load();
  delete st.m[LISTENER_ID_KEY];   // AsyncStorage.clear()
  d.forget();
  ok('forgotten: nothing sent until reloaded', d.query() === '');
  const after = await d.load();
  ok('a fresh id after a reset', after !== before && st.m[LISTENER_ID_KEY] === after);
}

// 6. Source: every VibeServer socket carries it, exactly as the web's does.
const ws = readFileSync(new URL('../src/services/VibeServerWsClient.ts', import.meta.url), 'utf8');
const specUrls = ws.match(/`\/ws\/user-spectrum\?[^`]*`/g) || [];   // ★ template literals only, not the comments
ok(`both app spectrum URLs found (${specUrls.length})`, specUrls.length === 2);
ok('every app spectrum URL carries the listener id', specUrls.every((u) => u.includes('${listenerIdQuery()}')));
ok('connect() awaits the id before the preflight', /await loadListenerId\(\);[\s\S]{0,120}await this\._checkConnection\(\)/.test(ws));
const lap = readFileSync(new URL('../src/components/LocalAudioPlayer.tsx', import.meta.url), 'utf8');
ok('the audio socket suffix carries bid', /combinedSuffix = [^;]*&bid=\$\{encodeURIComponent\(listenerId\)\}/.test(lap));
ok('the audio socket waits for the id', /if \(!listenerId\) \{[^}]*return; \}/.test(lap));
ok('listenerId is in the socket effect deps', /dualId, listenerId\]\);/.test(lap));
const web = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
ok('the web sends the same parameter on spectrum and audio', /'\/ws\/user-spectrum\?user_session_id=' \+ sid \+ '&bid=' \+ browserId\(\)/.test(web)
   && /'\/ws\/audio\?user_session_id=' \+ sid \+ '&bid=' \+ browserId\(\)/.test(web));
const shim = readFileSync(new URL('../android/app/src/main/cpp/local_sdr_shim.cpp', import.meta.url), 'utf8');
ok('the server reads `bid`', /queryParam\(reqLine, "bid"\)/.test(shim));

console.log(`listener id: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
